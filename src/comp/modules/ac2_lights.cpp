#include "std_include.hpp"
#include "ac2_lights.hpp"
#include "shared/common/remix_api.hpp"

#include <cmath>

namespace comp::ac2_lights
{
	bool  g_enabled = true;
	float g_radiance_scale = 20.0f;
	float g_emitter_radius = 0.06f;
	stats g_stats{};

	namespace
{
		// ---- pixel-shader constant shadow -----------------------------------
		// ps_3_0 has 224 float4 constants; the light block lives at c32..c54.
		constexpr UINT PS_CONST_MAX = 224;
		float      s_ps_const[PS_CONST_MAX][4]{};
		bool       s_ps_const_set[PS_CONST_MAX]{};
		std::mutex s_const_mtx;   // device is D3DCREATE_MULTITHREADED

		bool read_ps_const(UINT start, float* out, UINT count)
		{
			if (start + count > PS_CONST_MAX) return false;
			std::lock_guard<std::mutex> lk(s_const_mtx);
			for (UINT i = 0; i < count; ++i)
				if (!s_ps_const_set[start + i]) return false;   // game hasn't written it yet
			memcpy(out, &s_ps_const[start][0], count * 4 * sizeof(float));
			return true;
		}

		// ---- per-PS light counts --------------------------------------------
		struct ps_lights
		{
			int  omni = 0, direct = 0, spot = 0;
			bool sun = false;
		};
		std::map<IDirect3DPixelShader9*, ps_lights> s_ps_lights;

		// ---- collected lights ------------------------------------------------
		enum class ltype { omni, spot, direct };

		struct light
		{
			ltype type = ltype::omni;
			float pos[3]{};
			float dir[3]{};
			float colour[3]{};
			float range = 0.0f;          // metres (sqrt of the stored far^2)
			float cone_outer_deg = 0.0f;
			float cone_inner_deg = 0.0f;
		};

		std::mutex s_mtx;
		std::map<std::uint64_t, light> s_frame;   // deduped by stable key
		std::vector<remixapi_LightHandle> s_handles;  // live handles, retired next frame
		std::map<std::uint64_t, light> s_last_dump;  // snapshot for the report

		// Dedupe/identity key.
		//
		// One light lights many objects, so the same light arrives on hundreds of
		// draws per frame; and Remix needs a light's hash to be STABLE ACROSS FRAMES
		// or the light is destroyed and recreated every frame, which reads as a
		// flickering light. AC2's lights (torches, braziers) are essentially static,
		// so quantised world position + colour is both a good deduper and a stable
		// identity. Quantise at 1cm - finer than that and float jitter in the
		// constants would split one light into several.
		std::uint64_t light_key(const light& l)
		{
			auto q = [](float v) { return static_cast<std::int64_t>(std::lround(v * 100.0f)); };
			std::uint64_t h = 1469598103934665603ull;   // FNV-1a
			auto mix = [&h](std::int64_t v)
			{
				const auto* b = reinterpret_cast<const std::uint8_t*>(&v);
				for (int i = 0; i < 8; ++i) { h ^= b[i]; h *= 1099511628211ull; }
			};
			mix(static_cast<std::int64_t>(l.type));
			mix(q(l.pos[0])); mix(q(l.pos[1])); mix(q(l.pos[2]));
			// Colour at coarse resolution: a torch that flickers in INTENSITY should
			// stay ONE light, not spawn a new one per brightness step.
			mix(static_cast<std::int64_t>(std::lround(l.colour[0])));
			mix(static_cast<std::int64_t>(std::lround(l.colour[1])));
			mix(static_cast<std::int64_t>(std::lround(l.colour[2])));
			if (l.type == ltype::direct)
			{
				mix(q(l.dir[0])); mix(q(l.dir[1])); mix(q(l.dir[2]));
			}
			return h;
		}

		bool is_black(const float* c)
		{
			return c[0] <= 1e-4f && c[1] <= 1e-4f && c[2] <= 1e-4f;
		}

		void add(const light& l)
		{
			// A zero-radiance light contributes nothing but still costs Remix a light
			// slot. AC2 leaves disabled slots zeroed, so this is also how we tell a
			// real light from an unused register.
			if (is_black(l.colour)) return;

			const auto k = light_key(l);
			std::lock_guard<std::mutex> lk(s_mtx);
			s_frame.emplace(k, l);
		}
	}

	void on_set_ps_constant_f(UINT start_register, const float* data, UINT vec4_count)
	{
		if (!data || start_register >= PS_CONST_MAX) return;
		const UINT n = (start_register + vec4_count > PS_CONST_MAX)
			? (PS_CONST_MAX - start_register) : vec4_count;

		std::lock_guard<std::mutex> lk(s_const_mtx);
		memcpy(&s_ps_const[start_register][0], data, n * 4 * sizeof(float));
		for (UINT i = 0; i < n; ++i) s_ps_const_set[start_register + i] = true;
	}

	void register_ps_lights(IDirect3DPixelShader9* ps, int num_omni, int num_direct,
		int num_spot, bool has_sun)
	{
		if (!ps) return;
		if (!num_omni && !num_direct && !num_spot && !has_sun) return;

		std::lock_guard<std::mutex> lk(s_mtx);
		auto it = s_ps_lights.find(ps);
		// AddRef for the same reason ac2_ff does on the diffuse-stage map: D3D
		// recycles the addresses of released objects (7 confirmed reuses WITH a
		// different descriptor in one run), so a raw pointer key can silently start
		// referring to a different shader. Holding a reference keeps the address ours.
		if (it == s_ps_lights.end()) ps->AddRef();
		s_ps_lights[ps] = { num_omni, num_direct, num_spot, has_sun };
		g_stats.ps_with_lights = static_cast<std::uint32_t>(s_ps_lights.size());
	}

	void on_draw(IDirect3DDevice9* dev)
	{
		if (!g_enabled || !dev) return;

		IDirect3DPixelShader9* ps = nullptr;
		if (FAILED(dev->GetPixelShader(&ps)) || !ps) { if (ps) ps->Release(); return; }

		ps_lights pl;
		{
			std::lock_guard<std::mutex> lk(s_mtx);
			auto it = s_ps_lights.find(ps);
			if (it == s_ps_lights.end()) { ps->Release(); return; }  // no lights in this PS
			pl = it->second;
		}
		ps->Release();

		g_stats.draws_with_lights++;

		// ---- omni: c32 + 2*i,  reg0 = {pos, far^2}, reg1 = {colour, 1/(f^2-n^2)}
		for (int i = 0; i < pl.omni && i < 4; ++i)
		{
			float r[8];
			if (!read_ps_const(32 + 2 * i, r, 2)) { g_stats.const_miss++; continue; }

			light l;
			l.type = ltype::omni;
			l.pos[0] = r[0]; l.pos[1] = r[1]; l.pos[2] = r[2];
			l.range = (r[3] > 0.0f) ? sqrtf(r[3]) : 0.0f;    // stored SQUARED
			l.colour[0] = r[4]; l.colour[1] = r[5]; l.colour[2] = r[6];
			add(l);
			g_stats.omni_seen++;
		}

		// ---- direct: c40 + 2*i, reg0 = {-dir, ?}, reg1 = {colour, 0}
		for (int i = 0; i < pl.direct && i < 2; ++i)
		{
			float r[8];
			if (!read_ps_const(40 + 2 * i, r, 2)) { g_stats.const_miss++; continue; }

			light l;
			l.type = ltype::direct;
			// The register holds the NEGATED direction (the shader wants a vector
			// pointing at the light); Remix wants the direction the light travels.
			l.dir[0] = -r[0]; l.dir[1] = -r[1]; l.dir[2] = -r[2];
			l.colour[0] = r[4]; l.colour[1] = r[5]; l.colour[2] = r[6];
			add(l);
			g_stats.direct_seen++;
		}

		// ---- spot: c44 + 4*i, reg0/1 as omni, reg2 = {-dir,?}, reg3 = cones
		for (int i = 0; i < pl.spot && i < 2; ++i)
		{
			float r[16];
			if (!read_ps_const(44 + 4 * i, r, 4)) { g_stats.const_miss++; continue; }

			light l;
			l.type = ltype::spot;
			l.pos[0] = r[0]; l.pos[1] = r[1]; l.pos[2] = r[2];
			l.range = (r[3] > 0.0f) ? sqrtf(r[3]) : 0.0f;
			l.colour[0] = r[4]; l.colour[1] = r[5]; l.colour[2] = r[6];
			l.dir[0] = -r[8]; l.dir[1] = -r[9]; l.dir[2] = -r[10];

			// reg3.x/.y are the COSINES of the cone half-angles, so convert back.
			const float co = (std::max)(-1.0f, (std::min)(1.0f, r[12]));
			const float ci = (std::max)(-1.0f, (std::min)(1.0f, r[13]));
			l.cone_outer_deg = acosf(co) * (180.0f / 3.14159265f);
			l.cone_inner_deg = acosf(ci) * (180.0f / 3.14159265f);
			add(l);
			g_stats.spot_seen++;
		}
	}

	// Bring the Remix API up at the LAST safe moment rather than at ASI load.
	// See the comment in comp::main(): initialising at load time creates Remix
	// meshes before the device exists and crashes at startup. By the first Present
	// d3d9.dll is loaded and the device is live.
	//
	// Off by default: this needs `exposeRemixApi = True` in <game>\.trex\bridge.conf,
	// and without it initialize() fails harmlessly and we create no lights.
	bool g_remix_api = false;

	static void ensure_remix_api()
	{
		if (!g_remix_api) return;
		static std::once_flag once;
		std::call_once(once, []
		{
			shared::common::remix_api::initialize(nullptr, nullptr, nullptr, false);
		});
	}

	void frame_end()
	{
		g_stats.frames++;
		ensure_remix_api();

		auto& api = shared::common::remix_api::get();
		if (!api.is_initialized())
		{
			// Remix API not available => we can extract lights perfectly and still
			// create none. That failure is INVISIBLE unless it is counted: the report
			// would just show "0 lights" and look like a decode bug. It needs
			// `exposeRemixApi = True` in <game>\.trex\bridge.conf.
			g_stats.api_not_init++;
			std::lock_guard<std::mutex> lk(s_mtx);
			s_frame.clear();
			return;
		}

		// HOME must be able to turn the lights back OFF, not just decline to
		// initialise. ensure_remix_api() only gates init, and is_initialized() is
		// permanent, so without this HOME is a ONE-WAY switch that cannot A/B.
		//
		// It has to A/B, because `rtx.fallbackLightMode` defaults to NoLightsPresent:
		// Remix injects its own sun whenever no lights exist. For this whole project
		// there were none, so the fallback sun lit every scene. The instant we submit
		// a light it switches off and ours takes over - and BOTH look like "the sun
		// works". Killing our last light brings the fallback straight back, so
		// toggling HOME swings the sun between our direction and rtx.conf's. That
		// difference is the only thing that proves whose light is on screen.
		if (!g_remix_api)
		{
			for (auto h : s_handles) if (h) api.m_bridge.DestroyLight(h);
			s_handles.clear();
			std::lock_guard<std::mutex> lk(s_mtx);
			s_frame.clear();
			return;
		}

		std::map<std::uint64_t, light> frame;
		{
			std::lock_guard<std::mutex> lk(s_mtx);
			frame.swap(s_frame);
		}

		// Retire last frame's lights. Remix's model is create-per-frame: a light
		// handle lives for one frame, and the `hash` is what gives it identity
		// across frames - which is why light_key() must be stable.
		for (auto h : s_handles) if (h) api.m_bridge.DestroyLight(h);
		s_handles.clear();

		g_stats.unique_this_frame = static_cast<std::uint32_t>(frame.size());

		for (const auto& [key, l] : frame)
		{
			remixapi_LightInfo info{};
			info.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
			info.hash = key;
			info.radiance = remixapi_Float3D{
				l.colour[0] * g_radiance_scale,
				l.colour[1] * g_radiance_scale,
				l.colour[2] * g_radiance_scale };

			remixapi_LightInfoSphereEXT sphere{};
			remixapi_LightInfoDistantEXT distant{};

			if (l.type == ltype::direct)
			{
				distant.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_DISTANT_EXT;
				distant.pNext = nullptr;
				distant.direction = remixapi_Float3D{ l.dir[0], l.dir[1], l.dir[2] };
				distant.angularDiameterDegrees = 0.53f;   // ~the sun's angular size
				info.pNext = &distant;
			}
			else
			{
				sphere.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_SPHERE_EXT;
				sphere.pNext = nullptr;
				sphere.position = remixapi_Float3D{ l.pos[0], l.pos[1], l.pos[2] };
				sphere.radius = g_emitter_radius;   // emitter size, NOT l.range
				sphere.shaping_hasvalue = (l.type == ltype::spot) ? TRUE : FALSE;
				sphere.shaping_value = {};
				if (l.type == ltype::spot)
				{
					sphere.shaping_value.direction =
						remixapi_Float3D{ l.dir[0], l.dir[1], l.dir[2] };
					sphere.shaping_value.coneAngleDegrees = l.cone_outer_deg;
					// Soften across the inner->outer band the game itself defines.
					const float band = l.cone_outer_deg - l.cone_inner_deg;
					sphere.shaping_value.coneSoftness = (band > 0.0f)
						? (std::min)(1.0f, band / (std::max)(1.0f, l.cone_outer_deg)) : 0.0f;
					sphere.shaping_value.focusExponent = 0.0f;
				}
				info.pNext = &sphere;
			}

			remixapi_LightHandle h = nullptr;
			if (api.m_bridge.CreateLight(&info, &h) != REMIXAPI_ERROR_CODE_SUCCESS || !h)
			{
				g_stats.create_fail++;
				continue;
			}

			s_handles.push_back(h);
			g_stats.created++;

			// CreateLight only DEFINES the light; it contributes nothing until it is
			// submitted to the frame. Remix's own flashlight path does exactly this
			// (remix_api.cpp: DrawLightInstance every begin_scene). Without this call
			// the report happily counts hundreds "created" and the screen stays dark -
			// a created-but-never-drawn light is indistinguishable from a decode bug.
			if (api.m_bridge.DrawLightInstance(h) == REMIXAPI_ERROR_CODE_SUCCESS)
				g_stats.drawn++;
			else
				g_stats.draw_fail++;
		}

		std::lock_guard<std::mutex> lk(s_mtx);
		s_last_dump = std::move(frame);
	}

	void dump(std::ostream& os)
	{
		std::lock_guard<std::mutex> lk(s_mtx);
		os << "\n---- LIGHTS (game -> Remix) ----\n";
		os << "  enabled              : " << (g_enabled ? "yes" : "no") << "\n";
		os << "  radiance scale       : " << g_radiance_scale << "\n";
		os << "  emitter radius (m)   : " << g_emitter_radius << "\n";
		os << "  PS variants w/ lights: " << g_stats.ps_with_lights << "\n";
		os << "  draws with lights    : " << g_stats.draws_with_lights << "\n";
		os << "  light slots read     : omni=" << g_stats.omni_seen
			<< " direct=" << g_stats.direct_seen << " spot=" << g_stats.spot_seen << "\n";
		os << "  const not written yet: " << g_stats.const_miss << "\n";
		os << "  unique last frame    : " << g_stats.unique_this_frame << "\n";
		os << "  Remix lights created : " << g_stats.created
			<< " (fail " << g_stats.create_fail << ")\n";
		// created != drawn means the lights exist but were never submitted => invisible.
		os << "  Remix lights DRAWN   : " << g_stats.drawn
			<< " (fail " << g_stats.draw_fail << ")\n";
		os << "  frames / api not init: " << g_stats.frames
			<< " / " << g_stats.api_not_init << "\n";

		os << "  sample (up to 12 of last frame's unique lights, WORLD space):\n";
		int n = 0;
		for (const auto& [k, l] : s_last_dump)
		{
			if (n++ >= 12) break;
			const char* t = (l.type == ltype::omni) ? "omni  "
				: (l.type == ltype::spot) ? "spot  " : "direct";
			os << "    " << t
				<< "  pos " << l.pos[0] << "," << l.pos[1] << "," << l.pos[2]
				<< "  col " << l.colour[0] << "," << l.colour[1] << "," << l.colour[2]
				<< "  range " << l.range;
			if (l.type == ltype::spot)
				os << "  cone " << l.cone_inner_deg << ".." << l.cone_outer_deg << " deg";
			if (l.type == ltype::direct)
				os << "  dir " << l.dir[0] << "," << l.dir[1] << "," << l.dir[2];
			os << "\n";
		}
	}

	void shutdown()
	{
		auto& api = shared::common::remix_api::get();
		std::lock_guard<std::mutex> lk(s_mtx);
		if (api.is_initialized())
			for (auto h : s_handles) if (h) api.m_bridge.DestroyLight(h);
		s_handles.clear();
		for (auto& [ps, l] : s_ps_lights) ps->Release();
		s_ps_lights.clear();
	}
}
