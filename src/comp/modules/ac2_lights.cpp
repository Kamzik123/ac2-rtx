#include "std_include.hpp"
#include "ac2_lights.hpp"
#include "shared/common/remix_api.hpp"

#include <cmath>

namespace comp::ac2_lights
{
	bool  g_enabled = true;
	bool  g_remix_api = true;    // lights ON by default (HOME toggles); needs
	                             // exposeRemixApi=True in bridge.conf (already set)
	float g_radiance_scale = 20.0f;   // distant lights: radiance == irradiance
	float g_point_scale    = 20.0f;   // sphere lights: illuminance at 1 m (see .hpp)
	float g_emitter_radius = 0.06f;
	stats g_stats{};

	namespace
	{
		// ---- engine addresses / layout (Win32, IDB rebased to 0 => RVAs) -----
		// scimitar::LightingEnv::Update(this=node+0x90, PtrArray<LightNode>*,
		// GraphicNode*), __thiscall. Hook point for the whole entity route.
		constexpr std::uintptr_t RVA_LIGHTING_UPDATE = 0x1269E60;

		// Light-struct RTTI magic returned by the virtual at vtable+0x14. These are
		// class-name hashes, so they are identical on the Mac binary; but the field
		// OFFSETS below were re-derived on Windows and differ from the Mac ones.
		constexpr std::uint32_t MAGIC_DIRECT = 0x7E15FD50;   // directional
		constexpr std::uint32_t MAGIC_OMNI   = 0x344780D6;   // point
		constexpr std::uint32_t MAGIC_SPOT   = 0x80320FB8;
		constexpr std::uint32_t MAGIC_SUN    = 0x5EDC3E04;   // shadowed directional

		// LightNode offsets.
		constexpr std::size_t NODE_MATRIX = 0x40;   // 4x4 float, row-major
		constexpr std::size_t NODE_ROW1   = 0x50;   // matrix row 1 -> direction
		constexpr std::size_t NODE_ROW3   = 0x70;   // matrix row 3 -> world position
		constexpr std::size_t NODE_LIGHT  = 0xA8;   // -> light struct

		// light-struct offsets (bytes).
		constexpr std::size_t L_INTENSITY     = 0x14;    // [5]
		constexpr std::size_t L_COLOUR        = 0x10C;   // [67..69] rgb
		constexpr std::size_t L_ENABLE        = 0x11C;   // u16, bit 0x200
		constexpr std::size_t L_OMNI_FAR      = 0x124;   // [73]  (near @ [72] +0x120)
		constexpr std::size_t L_SPOT_CONE_OUT = 0x120;   // [72] radians
		constexpr std::size_t L_SPOT_CONE_IN  = 0x124;   // [73] radians
		constexpr std::size_t L_SPOT_FAR      = 0x130;   // [76]  (near @ [75] +0x12C)

		template <typename T>
		T rd(const void* base, std::size_t off)
		{
			return *reinterpret_cast<const T*>(
				reinterpret_cast<const std::uint8_t*>(base) + off);
		}

		void read3(const void* base, std::size_t off, float out[3])
		{
			const auto* p = reinterpret_cast<const float*>(
				reinterpret_cast<const std::uint8_t*>(base) + off);
			out[0] = p[0]; out[1] = p[1]; out[2] = p[2];
		}

		void normalize3(float v[3])
		{
			const float m = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
			if (m > 1e-8f) { v[0] /= m; v[1] /= m; v[2] /= m; }
		}

		// ---- collected light --------------------------------------------------
		enum class ltype { omni, spot, direct, sun };

		struct light
		{
			ltype type = ltype::omni;
			float pos[3]{};
			float dir[3]{};
			float colour[3]{};      // already premultiplied by intensity (see collect)
			float range = 0.0f;     // metres (far), for the report only
			float cone_outer_deg = 0.0f;
			float cone_inner_deg = 0.0f;
		};

		// ---- lock-free per-thread collection ---------------------------------
		// LightingEnv::Update runs on the engine's WORKER threads, hundreds of times
		// per frame. A single shared mutex here serialises those workers and stalls
		// the whole record phase. Instead each thread accumulates into its OWN bucket
		// (no cross-thread contention); frame_end drains them all on the render thread.
		struct tls_bucket
		{
			std::mutex mtx;   // only ever contended between its owner thread and the
			                  // once-per-frame drain, so effectively uncontended
			std::map<const void*, light> m;
		};
		std::mutex s_buckets_mtx;                 // guards the bucket LIST (first touch only)
		std::vector<tls_bucket*> s_buckets;

		tls_bucket& my_bucket()
		{
			thread_local tls_bucket* b = nullptr;
			if (!b)
			{
				b = new tls_bucket();
				std::lock_guard<std::mutex> lk(s_buckets_mtx);
				s_buckets.push_back(b);
			}
			return *b;
		}

		std::mutex s_mtx;                           // guards s_last_dump only
		std::map<const void*, light> s_last_dump;

		// ---- persistent light registry ---------------------------------------
		// Keep a persistent registry and match each frame's lights to it SPATIALLY,
		// with a tolerance - boundary-free, so a light that jitters by a hair (or
		// whose node is reallocated) stays the same light. Each entry carries a
		// stable `id` used as the Remix hash, and is aged
		// out only after it goes unseen for a few frames, so a one-frame collection
		// gap (workers still building while Present swaps) does not blink every light.
		struct reg_entry
		{
			light                l;
			remixapi_LightHandle handle = nullptr;
			std::uint64_t        id = 0;          // stable Remix hash for this light
			std::uint32_t        last_seen = 0;   // frame number last matched
			bool                 dirty = true;    // data changed => recreate handle
		};
		std::vector<reg_entry> s_registry;
		std::uint64_t s_next_id = 1;
		std::uint32_t s_frame_no = 0;

		// A light survives this many frames without being re-seen before we drop it.
		constexpr std::uint32_t MAX_AGE = 4;
		// Spatial match tolerances: 5cm for position, ~11 deg for direction.
		constexpr float POS_TOL2 = 0.05f * 0.05f;
		constexpr float DIR_DOT_MIN = 0.98f;

		// pointer-stability diagnostic (informational only)
		std::vector<const void*> s_prev_ptrs;

		// Set when a tuning knob changes, so frame_end rebuilds every handle:
		// the scales are NOT part of the light data light_differs() compares, so
		// nothing would otherwise mark an existing light dirty.
		std::atomic<bool> s_tuning_dirty{ false };

		bool is_black(const float* c)
		{
			return c[0] <= 1e-4f && c[1] <= 1e-4f && c[2] <= 1e-4f;
		}

		bool is_distant(ltype t) { return t == ltype::direct || t == ltype::sun; }

		// Does registry entry `e` describe the same physical light as `l`?
		bool same_light(const reg_entry& e, const light& l)
		{
			if (e.l.type != l.type) return false;
			if (is_distant(l.type))
			{
				const float d = e.l.dir[0] * l.dir[0] + e.l.dir[1] * l.dir[1]
					+ e.l.dir[2] * l.dir[2];
				return d >= DIR_DOT_MIN;
			}
			const float dx = e.l.pos[0] - l.pos[0];
			const float dy = e.l.pos[1] - l.pos[1];
			const float dz = e.l.pos[2] - l.pos[2];
			return (dx * dx + dy * dy + dz * dz) <= POS_TOL2;
		}

		// Has the light data changed enough that Remix needs a fresh handle?
		bool light_differs(const light& a, const light& b)
		{
			auto df = [](float x, float y) { return fabsf(x - y) > 1e-3f; };
			if (df(a.pos[0], b.pos[0]) || df(a.pos[1], b.pos[1]) || df(a.pos[2], b.pos[2]))
				return true;
			if (df(a.dir[0], b.dir[0]) || df(a.dir[1], b.dir[1]) || df(a.dir[2], b.dir[2]))
				return true;
			if (df(a.colour[0], b.colour[0]) || df(a.colour[1], b.colour[1])
				|| df(a.colour[2], b.colour[2]))
				return true;
			if (df(a.cone_outer_deg, b.cone_outer_deg) || df(a.cone_inner_deg, b.cone_inner_deg))
				return true;
			return false;
		}

		// Read one LightNode into `out`. Returns false if it should be skipped.
		bool decode_node(const void* node, light& out)
		{
			const void* ls = rd<const void*>(node, NODE_LIGHT);
			if (!ls) return false;

			// Enabled bit (bit 9 of the u16 at +0x11C). The engine gates on this
			// exact bit before submitting a light.
			if (((rd<std::uint16_t>(ls, L_ENABLE) >> 9) & 1u) == 0) return false;

			// Type via the same virtual the engine calls: vtable[+0x14](this).
			const void* vtbl = rd<const void*>(ls, 0);
			if (!vtbl) return false;
			using PFN_Type = std::uint32_t(__thiscall*)(const void*);
			const auto get_type = rd<PFN_Type>(vtbl, 0x14);
			const std::uint32_t magic = get_type(ls);

			// intensity, colour (colour is premultiplied by intensity here so the
			// submit path matches the old register decode, which read the already-
			// premultiplied colour out of reg1).
			const float intensity = rd<float>(ls, L_INTENSITY);
			read3(ls, L_COLOUR, out.colour);
			out.colour[0] *= intensity;
			out.colour[1] *= intensity;
			out.colour[2] *= intensity;

			switch (magic)
			{
			case MAGIC_OMNI:
				out.type = ltype::omni;
				read3(node, NODE_ROW3, out.pos);
				out.range = rd<float>(ls, L_OMNI_FAR);   // far (linear, not squared)
				return true;

			case MAGIC_SPOT:
			{
				out.type = ltype::spot;
				read3(node, NODE_ROW3, out.pos);
				read3(node, NODE_ROW1, out.dir);          // travel direction
				normalize3(out.dir);
				out.range = rd<float>(ls, L_SPOT_FAR);
				const float outer = rd<float>(ls, L_SPOT_CONE_OUT);   // radians
				const float inner = rd<float>(ls, L_SPOT_CONE_IN);
				out.cone_outer_deg = outer * (180.0f / 3.14159265f);
				out.cone_inner_deg = inner * (180.0f / 3.14159265f);
				return true;
			}

			case MAGIC_DIRECT:
				out.type = ltype::direct;
				read3(node, NODE_ROW1, out.dir);
				normalize3(out.dir);
				return true;

			case MAGIC_SUN:
				out.type = ltype::sun;
				read3(node, NODE_ROW1, out.dir);
				normalize3(out.dir);
				return true;

			default:
				g_stats.unknown_magic++;
				return false;
			}
		}

		// ---- LightingEnv::Update hook ----------------------------------------
		// __thiscall(this, PtrArray<LightNode>* lights, GraphicNode* node) modelled
		// as __fastcall so we receive `this` in ecx; edx is unused by the engine.
		using PFN_Update = void(__fastcall*)(void* ecx, void* edx, void* lights, void* node);
		PFN_Update o_lighting_update = nullptr;

		void collect(void* lights)
		{
			if (!lights) return;

			// PtrArray<LightNode>: data ptr @ +0, count = u16 @ +6 & 0x3FFF,
			// elements are LightNode* (stride 4).
			const auto* const* data = rd<const void* const*>(lights, 0);
			const std::uint16_t count = rd<std::uint16_t>(lights, 6) & 0x3FFF;
			if (!data || !count) return;

			// Accumulate into THIS thread's bucket - no cross-thread contention.
			tls_bucket& b = my_bucket();
			std::lock_guard<std::mutex> lk(b.mtx);
			for (std::uint16_t i = 0; i < count; ++i)
			{
				const void* node = data[i];
				if (!node) continue;

				g_stats.nodes_walked++;

				light l;
				if (!decode_node(node, l)) continue;
				if (is_black(l.colour)) continue;   // zeroed / disabled slot

				b.m[node] = l;                      // dedupe by stable node identity
			}
		}

		// Merge every thread's bucket into `out` and clear them. Render thread only.
		void drain_buckets(std::map<const void*, light>& out)
		{
			std::lock_guard<std::mutex> lk(s_buckets_mtx);
			for (auto* b : s_buckets)
			{
				std::lock_guard<std::mutex> bl(b->mtx);
				for (auto& [k, v] : b->m) out[k] = v;
				b->m.clear();
			}
		}

		void __fastcall hk_lighting_update(void* ecx, void* edx, void* lights, void* node)
		{
			o_lighting_update(ecx, edx, lights, node);   // let the engine build/select first
			g_stats.update_calls++;
			if (g_enabled)
				collect(lights);
		}

		void install_hook()
		{
			const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleA(nullptr));
			MH_Initialize();   // no-op if the framework already did it

			auto* target = reinterpret_cast<void*>(base + RVA_LIGHTING_UPDATE);
			if (MH_CreateHook(target, &hk_lighting_update,
				reinterpret_cast<void**>(&o_lighting_update)) == MH_OK
				&& MH_EnableHook(target) == MH_OK)
			{
				g_stats.hook_installed = true;
			}
		}
	}

	// Bring the Remix API up at the LAST safe moment rather than at ASI load.
	// Initialising at load time creates Remix meshes before the device exists and
	// crashes at startup; by the first Present d3d9.dll is loaded and the device
	// is live. Off by default: needs `exposeRemixApi = True` in bridge.conf.
	static void ensure_remix_api()
	{
		if (!g_remix_api) return;
		static std::once_flag once;
		std::call_once(once, []
		{
			shared::common::remix_api::initialize(nullptr, nullptr, nullptr, false);
		});
	}

	namespace
	{
		// Build a Remix light from `l` with the stable hash `id`, create it, and
		// return the handle (null on failure). The EXT structs must live across the
		// CreateLight call, so building and creating stay in one function.
		remixapi_LightHandle create_light(const light& l, std::uint64_t id)
		{
			auto& api = shared::common::remix_api::get();

			remixapi_LightInfo info{};
			info.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
			info.hash = id;

			remixapi_LightInfoSphereEXT sphere{};
			remixapi_LightInfoDistantEXT distant{};

			if (is_distant(l.type))
			{
				// A distant light's radiance IS the arriving irradiance - no
				// geometry term, so colour*intensity maps straight through.
				info.radiance = remixapi_Float3D{
					l.colour[0] * g_radiance_scale,
					l.colour[1] * g_radiance_scale,
					l.colour[2] * g_radiance_scale };

				distant.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_DISTANT_EXT;
				distant.pNext = nullptr;
				distant.direction = remixapi_Float3D{ l.dir[0], l.dir[1], l.dir[2] };
				distant.angularDiameterDegrees = 0.53f;   // ~the sun's angular size
				info.pNext = &distant;
			}
			else
			{
				// A sphere light's radiance is the EMITTER SURFACE radiance: the
				// illuminance it delivers at distance d is radiance*pi*r^2/d^2,
				// so the emitter area is baked into the brightness. Handing it the
				// distant-light number made every omni/spot ~88x too dim at r=0.06
				// AND made g_emitter_radius a hidden r^2 brightness control.
				// Dividing by the area cancels both: g_point_scale is now the
				// illuminance at 1 m, and the radius is pure softness.
				const float r    = (std::max)(1e-3f, g_emitter_radius);
				const float area = 3.14159265f * r * r;
				const float k    = g_point_scale / area;

				info.radiance = remixapi_Float3D{
					l.colour[0] * k, l.colour[1] * k, l.colour[2] * k };

				sphere.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_SPHERE_EXT;
				sphere.pNext = nullptr;
				sphere.position = remixapi_Float3D{ l.pos[0], l.pos[1], l.pos[2] };
				sphere.radius = r;                  // emitter size, NOT l.range
				sphere.shaping_hasvalue = (l.type == ltype::spot) ? TRUE : FALSE;
				sphere.shaping_value = {};
				if (l.type == ltype::spot)
				{
					sphere.shaping_value.direction =
						remixapi_Float3D{ l.dir[0], l.dir[1], l.dir[2] };
					sphere.shaping_value.coneAngleDegrees = l.cone_outer_deg;
					const float band = l.cone_outer_deg - l.cone_inner_deg;
					sphere.shaping_value.coneSoftness = (band > 0.0f)
						? (std::min)(1.0f, band / (std::max)(1.0f, l.cone_outer_deg)) : 0.0f;
					sphere.shaping_value.focusExponent = 0.0f;
				}
				info.pNext = &sphere;
			}

			remixapi_LightHandle h = nullptr;
			if (api.m_bridge.CreateLight(&info, &h) != REMIXAPI_ERROR_CODE_SUCCESS)
				return nullptr;
			return h;
		}

		// Tear down every live registry handle (HOME-off / shutdown / API loss).
		void destroy_all_handles()
		{
			auto& api = shared::common::remix_api::get();
			if (api.is_initialized())
				for (auto& e : s_registry)
					if (e.handle) { api.m_bridge.DestroyLight(e.handle); e.handle = nullptr; }
		}
	}

	void nudge_point_scale(float factor)
	{
		g_point_scale = (std::max)(0.01f, g_point_scale * factor);
		s_tuning_dirty.store(true);
	}

	void frame_end()
	{
		// Install the engine hook once, from a point where the game is fully up.
		static std::once_flag s_hook_once;
		std::call_once(s_hook_once, install_hook);

		g_stats.frames++;
		ensure_remix_api();

		auto& api = shared::common::remix_api::get();
		if (!api.is_initialized())
		{
			// Remix API unavailable => we can decode lights perfectly and still
			// create none. That failure is INVISIBLE unless counted. Needs
			// `exposeRemixApi = True` in <game>\.trex\bridge.conf.
			g_stats.api_not_init++;
			std::map<const void*, light> discard;
			drain_buckets(discard);   // keep the buckets from growing unbounded
			return;
		}

		// HOME must be able to turn the lights back OFF, not just decline to
		// initialise. is_initialized() is permanent, so without this HOME would be
		// a one-way switch. Destroying our lights brings Remix's fallback sun
		// straight back (rtx.fallbackLightMode = NoLightsPresent), which is the
		// only thing that proves whose light is on screen.
		if (!g_remix_api)
		{
			destroy_all_handles();
			s_registry.clear();
			std::map<const void*, light> discard;
			drain_buckets(discard);
			return;
		}

		std::map<const void*, light> frame;
		drain_buckets(frame);

		++s_frame_no;

		// --- pointer-stability diagnostic: how many of this frame's LightNode
		// pointers were also present last frame. ~100% here would mean the pointer
		// WAS a viable identity and the flicker was a timing gap; low means the
		// pointer churns and the spatial registry is doing the real work.
		{
			std::vector<const void*> cur;
			cur.reserve(frame.size());
			for (const auto& [node, l] : frame) cur.push_back(node);
			std::uint32_t carry = 0;
			for (auto* p : cur)
				if (std::find(s_prev_ptrs.begin(), s_prev_ptrs.end(), p) != s_prev_ptrs.end())
					++carry;
			g_stats.ptr_prev = static_cast<std::uint32_t>(cur.size());
			g_stats.ptr_carry = carry;
			s_prev_ptrs.swap(cur);
		}

		g_stats.unique_this_frame = static_cast<std::uint32_t>(frame.size());
		g_stats.omni_seen = g_stats.direct_seen = g_stats.spot_seen = g_stats.sun_seen = 0;
		g_stats.matched_existing = 0;
		g_stats.new_this_frame = 0;

		// --- reconcile this frame's lights against the persistent registry -----
		for (const auto& [node, l] : frame)
		{
			switch (l.type)
			{
			case ltype::omni:   g_stats.omni_seen++;   break;
			case ltype::spot:   g_stats.spot_seen++;   break;
			case ltype::direct: g_stats.direct_seen++; break;
			case ltype::sun:    g_stats.sun_seen++;    break;
			}

			reg_entry* match = nullptr;
			for (auto& e : s_registry)
				if (e.last_seen != s_frame_no && same_light(e, l)) { match = &e; break; }

			if (match)
			{
				g_stats.matched_existing++;
				if (light_differs(match->l, l)) match->dirty = true;
				match->l = l;
				match->last_seen = s_frame_no;
			}
			else
			{
				g_stats.new_this_frame++;
				s_registry.push_back(reg_entry{ l, nullptr, s_next_id++, s_frame_no, true });
			}
		}

		// --- submit: create-once, redraw every frame, age out the rest ---------
		// `drawn` is per-frame (== live lights on screen); `created` stays a lifetime
		// total so churn is visible - it should plateau in a static scene.
		g_stats.drawn = 0;
		g_stats.draw_fail = 0;
		// A tuning change ('[' / ']') alters radiance without altering the light
		// data, so nothing above marked anything dirty. Do it here, once.
		const bool retune = s_tuning_dirty.exchange(false);
		for (auto it = s_registry.begin(); it != s_registry.end(); )
		{
			reg_entry& e = *it;

			// Aged out: unseen for MAX_AGE frames => this light is really gone.
			if (s_frame_no - e.last_seen > MAX_AGE)
			{
				if (e.handle) api.m_bridge.DestroyLight(e.handle);
				it = s_registry.erase(it);
				continue;
			}

			// (Re)create the handle only when it does not exist or the light data
			// changed. A static light is created ONCE and merely redrawn each frame,
			// so Remix never sees it destroyed -> no flicker. The stable `id` keeps
			// identity across the rare recreate too.
			if (!e.handle || e.dirty || retune)
			{
				if (e.handle) api.m_bridge.DestroyLight(e.handle);
				e.handle = create_light(e.l, e.id);
				e.dirty = false;
				if (e.handle) g_stats.created++;
				else          g_stats.create_fail++;
			}

			// CreateLight only DEFINES the light; it contributes nothing until
			// submitted every frame (Remix's flashlight path does the same).
			if (e.handle)
			{
				if (api.m_bridge.DrawLightInstance(e.handle) == REMIXAPI_ERROR_CODE_SUCCESS)
					g_stats.drawn++;
				else
					g_stats.draw_fail++;
			}
			++it;
		}

		g_stats.registry_size = static_cast<std::uint32_t>(s_registry.size());

		std::lock_guard<std::mutex> lk(s_mtx);
		s_last_dump = std::move(frame);
	}

	void dump(std::ostream& os)
	{
		std::lock_guard<std::mutex> lk(s_mtx);
		os << "\n---- LIGHTS (entity route: LightingEnv::Update walk) ----\n";
		os << "  enabled              : " << (g_enabled ? "yes" : "no") << "\n";
		os << "  hook installed       : " << (g_stats.hook_installed ? "yes" : "NO") << "\n";
		// The two scales are in DIFFERENT units on purpose (a distant light's
		// radiance is irradiance; a sphere light's is emitter-surface radiance).
		// Print the area divisor too, so "why is the number we submit 300x the
		// number I tuned" is answerable from the report alone.
		const float r_dump = (std::max)(1e-3f, g_emitter_radius);
		const float area_dump = 3.14159265f * r_dump * r_dump;
		os << "  distant scale        : " << g_radiance_scale << "  (irradiance)\n";
		os << "  point/spot scale     : " << g_point_scale
			<< "  (illuminance @1m; submitted radiance = x" << (1.0f / area_dump)
			<< " for r=" << g_emitter_radius << ")\n";
		os << "  emitter radius (m)   : " << g_emitter_radius
			<< "  (softness only - brightness is area-normalised)\n";
		os << "  Update calls / frame : " << g_stats.update_calls
			<< " over " << g_stats.frames << " frames\n";
		os << "  light-node visits    : " << g_stats.nodes_walked
			<< " (unknown magic " << g_stats.unknown_magic << ")\n";
		os << "  unique last frame    : " << g_stats.unique_this_frame
			<< "  (omni=" << g_stats.omni_seen << " spot=" << g_stats.spot_seen
			<< " direct=" << g_stats.direct_seen << " sun=" << g_stats.sun_seen << ")\n";
		os << "  registry (persistent): " << g_stats.registry_size
			<< "  (matched " << g_stats.matched_existing
			<< " / new " << g_stats.new_this_frame << " last frame)\n";
		// node-ptr carry-over: if this is ~= ptr count, the LightNode pointer was a
		// stable identity after all and the spatial registry is redundant; if it is
		// much lower, the pointer churns frame-to-frame (why the pointer hash flickered).
		os << "  node-ptr carry-over  : " << g_stats.ptr_carry
			<< " / " << g_stats.ptr_prev << " last frame\n";
		os << "  Remix lights created : " << g_stats.created
			<< " lifetime (fail " << g_stats.create_fail << ")\n";
		// DRAWN is per frame == live lights on screen. created should PLATEAU in a
		// static scene; if it keeps climbing, identity is still churning (flicker).
		os << "  Remix lights DRAWN   : " << g_stats.drawn
			<< " this frame (fail " << g_stats.draw_fail << ")\n";
		os << "  api not init frames  : " << g_stats.api_not_init << "\n";

		os << "  sample (up to 12 of last frame's unique lights, WORLD space):\n";
		int n = 0;
		for (const auto& [node, l] : s_last_dump)
		{
			if (n++ >= 12) break;
			const char* t = (l.type == ltype::omni) ? "omni  "
				: (l.type == ltype::spot) ? "spot  "
				: (l.type == ltype::sun) ? "sun   " : "direct";
			os << "    " << t
				<< "  col " << l.colour[0] << "," << l.colour[1] << "," << l.colour[2];
			if (l.type == ltype::omni || l.type == ltype::spot)
				os << "  pos " << l.pos[0] << "," << l.pos[1] << "," << l.pos[2]
				   << "  range " << l.range;
			if (l.type == ltype::spot)
				os << "  cone " << l.cone_inner_deg << ".." << l.cone_outer_deg << " deg";
			if (l.type == ltype::direct || l.type == ltype::spot || l.type == ltype::sun)
				os << "  dir " << l.dir[0] << "," << l.dir[1] << "," << l.dir[2];
			os << "\n";
		}
	}

	void shutdown()
	{
		std::lock_guard<std::mutex> lk(s_mtx);
		destroy_all_handles();
		s_registry.clear();
	}
}
