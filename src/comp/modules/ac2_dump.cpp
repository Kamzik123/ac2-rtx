#include "std_include.hpp"
#include "ac2_dump.hpp"
#include "ac2_ff.hpp"
#include "ac2_lights.hpp"

#include <cfloat>
#include <cmath>

namespace comp::ac2_dump
{
	bool g_enabled = true;

	namespace
	{
		// ---- D3DX, resolved dynamically -------------------------------------
		// AC2 imports d3dx9_42.dll, so it is already in the process. Resolving
		// from it avoids linking d3dx9.lib and dragging in a d3dx9_43 dependency.
		using PFN_Disassemble = HRESULT(WINAPI*)(const DWORD*, BOOL, LPCSTR, LPD3DXBUFFER*);
		using PFN_GetConstantTable = HRESULT(WINAPI*)(const DWORD*, LPD3DXCONSTANTTABLE*);

		PFN_Disassemble      p_disassemble = nullptr;
		PFN_GetConstantTable p_get_constant_table = nullptr;

		std::once_flag        s_init_once;
		std::mutex            s_mutex;
		std::filesystem::path s_dump_dir;

		std::unordered_set<std::uint32_t> s_seen_shaders;

		// ---- pos.w statistics ------------------------------------------------
		// IMPORTANT: many distinct meshes share one vertex format, so |w| is only
		// expected to be constant PER DRAW (per mesh), not per format. Classify
		// each draw on its own, then aggregate the verdicts.
		struct w_stats
		{
			std::string   format;
			std::uint32_t draws = 0;
			std::uint32_t draws_w_const = 0;    // every w identical within the draw
			std::uint32_t draws_absw_const = 0; // |w| identical, sign flips
			std::uint32_t draws_varying = 0;    // genuinely varies
			std::uint32_t samples = 0;
			std::uint32_t negative = 0;
			std::uint32_t positive = 0;
			std::uint32_t zero = 0;
			float         min_abs = FLT_MAX;
			float         max_abs = 0.0f;
			std::set<int> per_draw_absw; // one |w| per constant draw -> shows spread across meshes
			std::set<int> example_varying_w;
		};

		std::map<std::uint32_t, w_stats> s_w_stats;
		std::uint32_t s_total_draws = 0;   // proves the hook is live even if nothing samples
		std::uint32_t s_reports_written = 0;

		constexpr std::uint32_t MAX_DRAWS_PER_FORMAT = 512;
		constexpr std::uint32_t MAX_VERTS_PER_DRAW = 512;
		constexpr std::size_t   MAX_SET = 48;

		void ensure_init()
		{
			std::call_once(s_init_once, []
			{
				HMODULE d3dx = GetModuleHandleA("d3dx9_42.dll");
				if (!d3dx) d3dx = GetModuleHandleA("d3dx9_43.dll");
				if (!d3dx) d3dx = LoadLibraryA("d3dx9_42.dll");

				if (d3dx)
				{
					p_disassemble = reinterpret_cast<PFN_Disassemble>(
						GetProcAddress(d3dx, "D3DXDisassembleShader"));
					p_get_constant_table = reinterpret_cast<PFN_GetConstantTable>(
						GetProcAddress(d3dx, "D3DXGetShaderConstantTable"));
				}

				std::error_code ec;
				s_dump_dir = std::filesystem::current_path(ec) / "ac2_rtx_dump";
				std::filesystem::create_directories(s_dump_dir / "shaders", ec);
			});
		}

		// Edge-detected hotkeys + a timed auto-flush, so the dump never depends on
		// catching a keypress inside a sampled draw call.
		//   F8 / F10 : write report now (F8 preferred - Windows eats F10 for menus)
		//   F9       : toggle sampling
		void poll_hotkeys()
		{
			// Anti-culling is ON by default (DELETE toggles it OFF). Applied from the
			// first draw rather than DllMain: the patches need the game module
			// mapped, and a draw guarantees that. call_once so the byte writes
			// happen exactly once even though draws come from many threads.
			//
			// CullAABB is deliberately NOT applied here: it can crash during LOADING,
			// and it turns out to be UNNECESSARY - disabling the hardware occlusion
			// cull below brings the missing objects back on its own (user-confirmed).
			// Enable it with PAGE DOWN only if you have a reason to.
			//
			// The occlusion patch IS applied by default: it is the culling system that
			// actually mattered, and it's a data write, so it carries none of the
			// torn-byte risk that makes CullAABB dangerous.
			static std::once_flag s_patch_once;
			std::call_once(s_patch_once, []
			{
				ac2_ff::set_anticulling(true);
				ac2_ff::set_occlusion_disabled(true);
			});

			static std::uint32_t tick = 0;
			if (((++tick) & 0xFF) != 0) return; // ~every 256 draws is plenty

			static bool prev_dump = false, prev_toggle = false, prev_ff = false, prev_wire = false;
			const bool dump_now = (GetAsyncKeyState(VK_F8) & 0x8000) != 0
				|| (GetAsyncKeyState(VK_F10) & 0x8000) != 0;
			const bool toggle = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
			const bool ff = (GetAsyncKeyState(VK_F7) & 0x8000) != 0;
			const bool wire = (GetAsyncKeyState(VK_F6) & 0x8000) != 0;
			const bool stage = (GetAsyncKeyState(VK_F5) & 0x8000) != 0;
			static bool prev_stage = false;
			if (stage && !prev_stage)
			{
				ac2_ff::g_stage_b = !ac2_ff::g_stage_b;
				ac2_ff::g_max_wvp_error = 0.0f; // re-measure after switching
				Beep(ac2_ff::g_stage_b ? 1000 : 700, 60);
			}
			prev_stage = stage;


			// INSERT: FF-ONLY mode - drop everything we can't convert, so the scene
			// is purely our fixed-function geometry. Isolates FF vs vertex capture.
			// (Was F4, but that collides with a mod debug menu that can't be closed.)
			static bool prev_only = false;
			const bool only = (GetAsyncKeyState(VK_INSERT) & 0x8000) != 0;
			if (only && !prev_only)
			{
				ac2_ff::g_ff_only = !ac2_ff::g_ff_only;
				Beep(ac2_ff::g_ff_only ? 1800 : 600, 80);
			}
			prev_only = only;

			// F1: CPU skinning for characters (the 33% not_static gap)
			static bool prev_skin = false;
			const bool skin = (GetAsyncKeyState(VK_F1) & 0x8000) != 0;
			if (skin && !prev_skin)
			{
				ac2_ff::g_skinning = !ac2_ff::g_skinning;
				Beep(ac2_ff::g_skinning ? 2000 : 500, 80);
			}
			prev_skin = skin;

			// F3: route SKINNED meshes to Remix vertex capture instead of CPU
			// skinning + FF. Lets us A/B the GPU-skinned capture (look + perf) against
			// our CPU path. Characters animate under capture; the open question is
			// flicker. When ON, skinned draws pass through instead of being dropped.
			static bool prev_svc = false;
			const bool svc = (GetAsyncKeyState(VK_F3) & 0x8000) != 0;
			if (svc && !prev_svc)
			{
				ac2_ff::g_skin_vertex_capture = !ac2_ff::g_skin_vertex_capture;
				Beep(ac2_ff::g_skin_vertex_capture ? 2400 : 500, 80);
			}
			prev_svc = svc;

			// DELETE: toggle the frustum ANTI-CULL patches. Default ON forces every
			// object frustum-visible (so off-screen geometry reaches Remix). Toggling
			// OFF lets the game's own FRUSTUM culling run - deterministic (unlike the
			// occlusion queries that flicker), so this is the test for whether ANY
			// source-level culling can be stable under Remix. Big perf lever if it is.
			static bool prev_ac = false;
			const bool ac = (GetAsyncKeyState(VK_DELETE) & 0x8000) != 0;
			if (ac && !prev_ac)
			{
				ac2_ff::set_anticulling(!ac2_ff::g_anticull);
				Beep(ac2_ff::g_anticull ? 2600 : 500, 80);
			}
			prev_ac = ac;

			// END: CPU-generated vegetation trunks. Separates "the trunk port is
			// wrong" from "the rest of the FF path is wrong" in one keypress - the
			// same job INSERT does for coverage-vs-culling.
			static bool prev_veg = false;
			const bool veg = (GetAsyncKeyState(VK_END) & 0x8000) != 0;
			if (veg && !prev_veg)
			{
				ac2_ff::g_veg_trunks = !ac2_ff::g_veg_trunks;
				Beep(ac2_ff::g_veg_trunks ? 2000 : 500, 80);
			}
			prev_veg = veg;

			// HOME is taken (Remix API), so leaves get F11: separate from trunks so
			// the two ports can be blamed independently.
			static bool prev_leaf = false;
			const bool leaf = (GetAsyncKeyState(VK_F11) & 0x8000) != 0;
			if (leaf && !prev_leaf)
			{
				ac2_ff::g_veg_leaves = !ac2_ff::g_veg_leaves;
				Beep(ac2_ff::g_veg_leaves ? 2200 : 550, 80);
			}
			prev_leaf = leaf;

			// F2: normal-only materials (water / volume fog). Took over the key the
			// depth-bias nudge used to own - that existed to stop converted geometry
			// z-fighting coplanar decals, which turned out to be an UNTAGGED DECAL in
			// Remix's config, not a depth problem. It had sat at 0 ever since.
			static bool prev_no = false;
			const bool no = (GetAsyncKeyState(VK_F2) & 0x8000) != 0;
			if (no && !prev_no)
			{
				ac2_ff::g_normal_only_materials = !ac2_ff::g_normal_only_materials;
				Beep(ac2_ff::g_normal_only_materials ? 2600 : 600, 80);
			}
			prev_no = no;


			// PAGE DOWN: force CullAABB -> "intersecting". OFF by default; this
			// CRASHES once the whole world starts being submitted. Opt-in only.
			static bool prev_ca = false;
			const bool ca = (GetAsyncKeyState(VK_NEXT) & 0x8000) != 0;
			if (ca && !prev_ca)
			{
				ac2_ff::set_cullaabb(!ac2_ff::g_cullaabb);
				Beep(ac2_ff::g_cullaabb ? 2400 : 400, 90);
			}
			prev_ca = ca;

			// PAGE UP: disable the game's hardware OCCLUSION culling (zero the
			// 25-pixel threshold). Default off so it can be A/B'd separately from the
			// frustum patches - it is an independent culling system. Sits next to
			// PAGE DOWN because both are culling toggles.
			static bool prev_occ = false;
			const bool occ = (GetAsyncKeyState(VK_PRIOR) & 0x8000) != 0;
			if (occ && !prev_occ)
			{
				ac2_ff::set_occlusion_disabled(!ac2_ff::g_occlusion_off);
				Beep(ac2_ff::g_occlusion_off ? 2400 : 400, 90);
			}
			prev_occ = occ;

			// HOME: bring up the Remix API and start submitting lights. Deliberately
			// a manual toggle: it needs exposeRemixApi = True in .trexridge.conf,
			// and a bad init crashes at STARTUP, where you cannot toggle anything off.
			static bool prev_ra = false;
			const bool ra = (GetAsyncKeyState(VK_HOME) & 0x8000) != 0;
			if (ra && !prev_ra)
			{
				ac2_lights::g_remix_api = !ac2_lights::g_remix_api;
				Beep(ac2_lights::g_remix_api ? 2800 : 300, 90);
			}
			prev_ra = ra;

			// '[' / ']': dim / brighten the point+spot lights (g_point_scale) by
			// 1.5x per press, and rebuild the live handles so it shows up
			// immediately. Only the SPHERE lights - the sun/direct scale is a
			// different unit and is not touched, which is the whole point of
			// having two. Tuning by eye against a torch is the only way to set
			// this: the game's intensity units are arbitrary.
			static bool prev_dim = false, prev_bri = false;
			const bool dim = (GetAsyncKeyState(VK_OEM_4) & 0x8000) != 0;   // '['
			const bool bri = (GetAsyncKeyState(VK_OEM_6) & 0x8000) != 0;   // ']'
			if (dim && !prev_dim) { ac2_lights::nudge_point_scale(1.0f / 1.5f); Beep(500, 60); }
			if (bri && !prev_bri) { ac2_lights::nudge_point_scale(1.5f);        Beep(1800, 60); }
			prev_dim = dim;
			prev_bri = bri;

			// F12: read constants from our shadow (default) vs the bridge's
			// GetVertexShaderConstantF. Proves which one is lying.
			static bool prev_cs = false;
			const bool cs = (GetAsyncKeyState(VK_F12) & 0x8000) != 0;
			if (cs && !prev_cs)
			{
				ac2_ff::g_use_const_shadow = !ac2_ff::g_use_const_shadow;
				Beep(ac2_ff::g_use_const_shadow ? 2600 : 350, 90);
			}
			prev_cs = cs;

			// Write FIRST, then re-arm for the next flush. Arming before writing
			// cleared the snapshot and always produced an empty diagnostic block.
			if (dump_now && !prev_dump)
			{
				write_report();
				ac2_ff::arm_diagnostic();
				ac2_ff::arm_skin_diagnostic();
				ac2_ff::arm_tex_diagnostic();
				Beep(880, 60);
			}
			if (toggle && !prev_toggle) { g_enabled = !g_enabled; Beep(g_enabled ? 660 : 330, 60); }
			if (ff && !prev_ff) { ac2_ff::g_enabled = !ac2_ff::g_enabled; Beep(ac2_ff::g_enabled ? 1200 : 500, 60); }
			if (wire && !prev_wire) { ac2_ff::g_wireframe = !ac2_ff::g_wireframe; Beep(1500, 40); }
			prev_dump = dump_now;
			prev_toggle = toggle;
			prev_ff = ff;
			prev_wire = wire;

			// Belt and braces: flush every ~15s regardless of input.
			static ULONGLONG last = 0;
			const ULONGLONG now = GetTickCount64();
			if (now - last > 15000) { last = now; write_report(); }
		}

		std::size_t shader_size_bytes(const DWORD* fn)
		{
			if (!fn) return 0;
			const DWORD* p = fn;
			for (std::size_t i = 0; i < 0x40000; ++i, ++p)
			{
				if (*p == 0x0000FFFF) {
					return (static_cast<std::size_t>(p - fn) + 1) * sizeof(DWORD);
				}
			}
			return 0;
		}

		const char* decl_type_name(BYTE t)
		{
			switch (t)
			{
			case D3DDECLTYPE_FLOAT1:    return "FLOAT1";
			case D3DDECLTYPE_FLOAT2:    return "FLOAT2";
			case D3DDECLTYPE_FLOAT3:    return "FLOAT3";
			case D3DDECLTYPE_FLOAT4:    return "FLOAT4";
			case D3DDECLTYPE_D3DCOLOR:  return "D3DCOLOR";
			case D3DDECLTYPE_UBYTE4:    return "UBYTE4";
			case D3DDECLTYPE_SHORT2:    return "SHORT2";
			case D3DDECLTYPE_SHORT4:    return "SHORT4";
			case D3DDECLTYPE_UBYTE4N:   return "UBYTE4N";
			case D3DDECLTYPE_SHORT2N:   return "SHORT2N";
			case D3DDECLTYPE_SHORT4N:   return "SHORT4N";
			case D3DDECLTYPE_USHORT2N:  return "USHORT2N";
			case D3DDECLTYPE_USHORT4N:  return "USHORT4N";
			case D3DDECLTYPE_UDEC3:     return "UDEC3";
			case D3DDECLTYPE_DEC3N:     return "DEC3N";
			case D3DDECLTYPE_FLOAT16_2: return "FLOAT16_2";
			case D3DDECLTYPE_FLOAT16_4: return "FLOAT16_4";
			default:                    return "?";
			}
		}

		// AC2's mesh declarations use REAL semantics (confirmed by the shader
		// disassembly: dcl_position v0, dcl_normal, dcl_blendweight, ...).
		const char* decl_usage_name(BYTE u)
		{
			switch (u)
			{
			case D3DDECLUSAGE_POSITION:     return "POSITION";
			case D3DDECLUSAGE_BLENDWEIGHT:  return "BLENDWEIGHT";
			case D3DDECLUSAGE_BLENDINDICES: return "BLENDINDICES";
			case D3DDECLUSAGE_NORMAL:       return "NORMAL";
			case D3DDECLUSAGE_PSIZE:        return "PSIZE";
			case D3DDECLUSAGE_TEXCOORD:     return "TEXCOORD";
			case D3DDECLUSAGE_TANGENT:      return "TANGENT";
			case D3DDECLUSAGE_BINORMAL:     return "BINORMAL";
			case D3DDECLUSAGE_TESSFACTOR:   return "TESSFACTOR";
			case D3DDECLUSAGE_POSITIONT:    return "POSITIONT";
			case D3DDECLUSAGE_COLOR:        return "COLOR";
			case D3DDECLUSAGE_FOG:          return "FOG";
			case D3DDECLUSAGE_DEPTH:        return "DEPTH";
			case D3DDECLUSAGE_SAMPLE:       return "SAMPLE";
			default:                        return "?";
			}
		}
	}

	void initialize() { ensure_init(); }

	// -------------------------------------------------------------------------
	// Which vegetation family is this VS, if any? Read straight off the constant
	// table - the same trick the PS diffuse stage and the PS light counts use.
	//
	// Marker verified across all 5642 asset VS: no dcl_position <=> declares
	// CompressionParams, 141 == 141, no exceptions. Within those, TrunkStencil
	// and LeavesEquations are mutually exclusive and split the set 47/47/47.
	static ac2_ff::veg_kind veg_kind_of(LPD3DXCONSTANTTABLE ct)
	{
		D3DXCONSTANTTABLE_DESC desc{};
		if (!ct || FAILED(ct->GetDesc(&desc))) return ac2_ff::veg_kind::none;

		bool compression = false, trunk = false, leaves = false;
		for (UINT i = 0; i < desc.Constants; ++i)
		{
			D3DXHANDLE h = ct->GetConstant(nullptr, i);
			if (!h) continue;
			D3DXCONSTANT_DESC cd{};
			UINT n = 1;
			if (FAILED(ct->GetConstantDesc(h, &cd, &n)) || !cd.Name) continue;

			const std::string nm = shared::utils::str_to_lower(cd.Name);
			if (nm == "compressionparams")  compression = true;
			else if (nm == "trunkstencil")  trunk = true;
			else if (nm == "leavesequations") leaves = true;
		}

		// CompressionParams is the gate: without it the shader has a real
		// dcl_position and the ordinary static path already handles it.
		if (!compression) return ac2_ff::veg_kind::none;
		if (trunk)  return ac2_ff::veg_kind::trunk;
		if (leaves) return ac2_ff::veg_kind::leaves;
		return ac2_ff::veg_kind::clutter;
	}

	void on_create_vertex_shader(const DWORD* pFunction, IDirect3DVertexShader9* shader)
	{
		if (!g_enabled || !pFunction) return;
		ensure_init();

		const std::size_t size = shader_size_bytes(pFunction);
		if (!size) return;

		// Register the vegetation family BEFORE the dump's dedup below.
		//
		// The dedup is keyed on the BYTECODE hash and exists to avoid writing the
		// same .asm twice. Registration is keyed on the SHADER OBJECT, and the game
		// creates many objects from identical bytecode - so registering after the
		// dedup would register only the first object of each shader and silently
		// leave every later tree unconverted, while the counters happily reported
		// "trunk shaders: registered". Different key, different lifetime, must not
		// share the early-out.
		const std::uint32_t hash = shared::utils::data_hash32(pFunction, size);
		if (shader)
		{
			ac2_ff::veg_kind kind = ac2_ff::veg_kind::none;
			if (p_get_constant_table)
			{
				LPD3DXCONSTANTTABLE vct = nullptr;
				if (SUCCEEDED(p_get_constant_table(pFunction, &vct)) && vct)
				{
					kind = veg_kind_of(vct);
					vct->Release();
				}
			}
			// The hash rides along so the coverage table can be named offline.
			ac2_ff::register_vs_info(shader, kind, hash);
		}
		{
			std::lock_guard<std::mutex> lk(s_mutex);
			if (!s_seen_shaders.insert(hash).second) return;
		}

		const auto base = s_dump_dir / "shaders";
		char name[64];
		sprintf_s(name, "vs_%08X", hash);

		{
			std::ofstream f(base / (std::string(name) + ".cso"), std::ios::binary);
			if (f) f.write(reinterpret_cast<const char*>(pFunction), static_cast<std::streamsize>(size));
		}

		std::ofstream txt(base / (std::string(name) + ".asm"));
		if (!txt) return;

		txt << "; AC2 vertex shader dump\n";
		txt << "; hash  : 0x" << std::hex << hash << std::dec << "\n";
		txt << "; bytes : " << size << "\n;\n";

		if (p_get_constant_table)
		{
			LPD3DXCONSTANTTABLE ct = nullptr;
			if (SUCCEEDED(p_get_constant_table(pFunction, &ct)) && ct)
			{
				D3DXCONSTANTTABLE_DESC desc{};
				if (SUCCEEDED(ct->GetDesc(&desc)))
				{
					txt << "; ---- constant table (" << desc.Constants << " entries) ----\n";
					for (UINT i = 0; i < desc.Constants; ++i)
					{
						D3DXHANDLE h = ct->GetConstant(nullptr, i);
						if (!h) continue;

						D3DXCONSTANT_DESC cd{};
						UINT cnt = 1;
						if (SUCCEEDED(ct->GetConstantDesc(h, &cd, &cnt)) && cd.Name)
						{
							const char* set = "c";
							if (cd.RegisterSet == D3DXRS_SAMPLER) set = "s";
							else if (cd.RegisterSet == D3DXRS_BOOL) set = "b";
							else if (cd.RegisterSet == D3DXRS_INT4) set = "i";

							txt << ";   " << set << cd.RegisterIndex
								<< " .. " << set << (cd.RegisterIndex + cd.RegisterCount - 1)
								<< "  (" << cd.RegisterCount << ")  " << cd.Name << "\n";
						}
					}
					txt << ";\n";
				}
				ct->Release();
			}
		}

		if (p_disassemble)
		{
			LPD3DXBUFFER buf = nullptr;
			if (SUCCEEDED(p_disassemble(pFunction, FALSE, nullptr, &buf)) && buf)
			{
				txt.write(static_cast<const char*>(buf->GetBufferPointer()),
					static_cast<std::streamsize>(buf->GetBufferSize()));
				buf->Release();
			}
		}
		else
		{
			txt << "; (D3DXDisassembleShader unavailable - raw .cso still written)\n";
		}
	}

	// ---- texture lifecycle --------------------------------------------------
	namespace
	{
		struct tex_info
		{
			UINT w = 0, h = 0, levels = 0;
			DWORD usage = 0;
			D3DFORMAT fmt = D3DFMT_UNKNOWN;
			D3DPOOL pool = D3DPOOL_DEFAULT;
			std::uint32_t created_count = 0; // >1 == this pointer was handed out again
		};
		std::map<IDirect3DTexture9*, tex_info> s_textures;
		std::uint32_t s_tex_creates = 0;
		std::uint32_t s_tex_ptr_reuses = 0;   // same pointer created more than once
		std::uint32_t s_tex_reuse_diff_desc = 0; // ...and with a DIFFERENT description
		std::map<D3DPOOL, std::uint32_t> s_tex_pools;
		std::map<DWORD, std::uint32_t> s_tex_usages;
	}

	void on_create_texture(UINT w, UINT h, UINT levels, DWORD usage, D3DFORMAT fmt,
		D3DPOOL pool, IDirect3DTexture9* tex)
	{
		if (!tex) return;
		ensure_init();
		std::lock_guard<std::mutex> lk(s_mutex);

		s_tex_creates++;
		s_tex_pools[pool]++;
		s_tex_usages[usage]++;

		auto it = s_textures.find(tex);
		if (it != s_textures.end())
		{
			// The allocator handed back a pointer we've already seen: the previous
			// texture must have been released. If the description differs too, then
			// anything keyed on the pointer is now conflating two different textures.
			s_tex_ptr_reuses++;
			const auto& o = it->second;
			if (o.w != w || o.h != h || o.fmt != fmt || o.levels != levels)
				s_tex_reuse_diff_desc++;
		}

		tex_info ti;
		ti.w = w; ti.h = h; ti.levels = levels; ti.usage = usage; ti.fmt = fmt; ti.pool = pool;
		ti.created_count = (it != s_textures.end()) ? it->second.created_count + 1 : 1;
		s_textures[tex] = ti;
	}

	void on_set_texture(DWORD, IDirect3DBaseTexture9*) {}

	// -------------------------------------------------------------------------
	void on_create_pixel_shader(const DWORD* pFunction, IDirect3DPixelShader9* shader)
	{
		if (!pFunction || !shader) return;
		ensure_init();
		if (!p_get_constant_table) return;

		LPD3DXCONSTANTTABLE ct = nullptr;
		if (FAILED(p_get_constant_table(pFunction, &ct)) || !ct)
		{
			ac2_ff::register_ps_diffuse_stage(shader, -1);
			return;
		}

		int best = -1;
		int best_rank = 99;
		std::string best_name;

		// Remember the normal map instead of just discarding it. If the shader turns
		// out to have NO diffuse at any stage, a normal map is the difference between
		// a real surface with no albedo (water/fog) and a genuine AO/depth pass. The
		// former must still reach Remix; the latter must not.
		int normal_reg = -1;
		std::string normal_name;

		D3DXCONSTANTTABLE_DESC desc{};
		if (SUCCEEDED(ct->GetDesc(&desc)))
		{
			for (UINT i = 0; i < desc.Constants; ++i)
			{
				D3DXHANDLE h = ct->GetConstant(nullptr, i);
				if (!h) continue;
				D3DXCONSTANT_DESC cd{};
				UINT n = 1;
				if (FAILED(ct->GetConstantDesc(h, &cd, &n)) || !cd.Name) continue;

				if (cd.RegisterSet != D3DXRS_SAMPLER) continue;

				const std::string nm = shared::utils::str_to_lower(cd.Name);

				// Provably NOT a diffuse. Everything else stays a candidate -
				// AC2's material editor emits procedural sampler names like
				// Operator6_0 / Operator143_0, and rejecting those threw away 49%
				// of all draws (they fell back to the shader path, which Remix
				// can't consume -> most of the world went missing).
				if (nm.find("normal") != std::string::npos)
				{
					// Lowest normal sampler wins, same tie-break as the diffuse.
					if (normal_reg < 0 || static_cast<int>(cd.RegisterIndex) < normal_reg)
					{
						normal_reg = static_cast<int>(cd.RegisterIndex);
						normal_name = cd.Name;
					}
					continue;
				}
				if (nm.find("specular") != std::string::npos) continue;
				if (nm.find("depth") != std::string::npos) continue;
				if (nm.find("shadow") != std::string::npos) continue;
				if (nm.find("cookie") != std::string::npos) continue;
				if (nm.find("lightmap") != std::string::npos) continue;
				if (nm.find("reflection") != std::string::npos) continue;
				if (nm.find("cubemap") != std::string::npos) continue;
				if (nm.find("ao") == 0) continue; // AO*, but not "shadow" etc already caught

				// Rank candidates; lower is better. Rank 3 = "unrecognised but not
				// excluded" - prefer the lowest register among those.
				int rank = 3;
				if (nm.find("diffuse") != std::string::npos)          rank = 0;
				else if (nm.find("basetexture") != std::string::npos) rank = 1;
				else if (nm.find("layer0") != std::string::npos)      rank = 2;

				if (rank < best_rank
					|| (rank == best_rank && best >= 0 && static_cast<int>(cd.RegisterIndex) < best))
				{
					best_rank = rank;
					best = static_cast<int>(cd.RegisterIndex);
					best_name = cd.Name;
				}
			}
		}
		ct->Release();

		// No diffuse anywhere, but there IS a normal map => a real surface with no
		// albedo (AC2's water and volume fog), not an AO/depth pass. Bind the normal
		// map as rank 4 so the draw reaches Remix at all and Remix has a texture to
		// hash and categorise. See ps_diffuse_info in the header for why we do not
		// try to tell water and fog apart here.
		if (best < 0 && normal_reg >= 0)
		{
			ac2_ff::register_ps_diffuse_stage(shader, normal_reg, 4, normal_name.c_str(), true);
			return;
		}

		// Carry HOW the stage was chosen, not just which one won - rank 3 is a
		// guess and the report needs to tell guesses apart from real matches.
		ac2_ff::register_ps_diffuse_stage(shader, best,
			(best >= 0) ? best_rank : -1, best_name.c_str());
	}

	// -------------------------------------------------------------------------
	void on_draw_indexed_prim(IDirect3DDevice9* dev, INT BaseVertexIndex,
		UINT MinVertexIndex, UINT NumVertices)
	{
		// Hotkeys/auto-flush run FIRST and unconditionally. Previously this sat
		// behind the g_enabled gate and the early-outs, so a single stray F9 made
		// F10 dead forever with no way back.
		ensure_init();
		poll_hotkeys();

		if (!g_enabled || !dev || !NumVertices) return;
		{
			std::lock_guard<std::mutex> lk(s_mutex);
			s_total_draws++;
		}
		if (BaseVertexIndex < 0) return; // negative base: skip rather than guess

		IDirect3DVertexDeclaration9* decl = nullptr;
		if (FAILED(dev->GetVertexDeclaration(&decl)) || !decl) return;

		D3DVERTEXELEMENT9 elems[MAX_FVF_DECL_SIZE]{};
		UINT elem_count = 0;
		if (FAILED(decl->GetDeclaration(elems, &elem_count)) || !elem_count) {
			decl->Release();
			return;
		}

		// AC2 uses real semantics - take POSITION on stream 0, nothing else.
		const D3DVERTEXELEMENT9* pos = nullptr;
		for (UINT i = 0; i < elem_count && elems[i].Stream != 0xFF; ++i)
		{
			if (elems[i].Stream == 0 && elems[i].Usage == D3DDECLUSAGE_POSITION) {
				pos = &elems[i];
				break;
			}
		}

		// Only 4-component positions carry a w worth studying.
		if (!pos || (pos->Type != D3DDECLTYPE_SHORT4 &&
			pos->Type != D3DDECLTYPE_SHORT4N &&
			pos->Type != D3DDECLTYPE_FLOAT4))
		{
			decl->Release();
			return;
		}

		IDirect3DVertexBuffer9* vb = nullptr;
		UINT stream_off = 0, stride = 0;
		if (FAILED(dev->GetStreamSource(0, &vb, &stream_off, &stride)) || !vb || !stride)
		{
			if (vb) vb->Release();
			decl->Release();
			return;
		}

		std::uint32_t key = shared::utils::data_hash32(elems, sizeof(D3DVERTEXELEMENT9) * elem_count);
		key = shared::utils::hash32_combine(key, static_cast<int>(stride));

		{
			std::lock_guard<std::mutex> lk(s_mutex);
			auto& st = s_w_stats[key];
			if (st.draws >= MAX_DRAWS_PER_FORMAT) {
				vb->Release(); decl->Release();
				return;
			}
			if (st.format.empty())
			{
				std::string s = "stride=" + std::to_string(stride) + "  ";
				for (UINT i = 0; i < elem_count && elems[i].Stream != 0xFF; ++i)
				{
					s += "[s" + std::to_string(elems[i].Stream)
						+ " +" + std::to_string(elems[i].Offset)
						+ " " + decl_type_name(elems[i].Type)
						+ " " + decl_usage_name(elems[i].Usage)
						+ std::to_string(elems[i].UsageIndex)
						+ "] ";
				}
				st.format = s;
			}
		}

		D3DVERTEXBUFFER_DESC vbd{};
		if (FAILED(vb->GetDesc(&vbd))) { vb->Release(); decl->Release(); return; }

		// This draw's vertices live at (BaseVertexIndex + MinVertexIndex), NOT at 0.
		const UINT first = static_cast<UINT>(BaseVertexIndex) + MinVertexIndex;
		const UINT verts = (NumVertices < MAX_VERTS_PER_DRAW) ? NumVertices : MAX_VERTS_PER_DRAW;
		const std::size_t start = static_cast<std::size_t>(stream_off) + pos->Offset
			+ static_cast<std::size_t>(stride) * first;
		const std::size_t need = start + static_cast<std::size_t>(stride) * verts;
		if (need > vbd.Size) { vb->Release(); decl->Release(); return; }

		void* data = nullptr;
		if (SUCCEEDED(vb->Lock(0, 0, &data, D3DLOCK_READONLY)) && data)
		{
			const auto* bytes = static_cast<const std::uint8_t*>(data) + start;

			// Per-draw pass first - this is the question that actually matters.
			std::set<int> draw_w;
			std::uint32_t neg = 0, po = 0, ze = 0;
			float mn = FLT_MAX, mx = 0.0f;

			for (UINT v = 0; v < verts; ++v)
			{
				const auto* p = bytes + static_cast<std::size_t>(stride) * v;
				int raw;
				float w;

				if (pos->Type == D3DDECLTYPE_FLOAT4)
				{
					float f[4]; memcpy(f, p, sizeof(f));
					w = f[3]; raw = static_cast<int>(w);
				}
				else
				{
					std::int16_t s[4]; memcpy(s, p, sizeof(s));
					w = static_cast<float>(s[3]); raw = s[3];
				}

				if (draw_w.size() < MAX_SET) draw_w.insert(raw);
				if (w < 0.0f) neg++; else if (w > 0.0f) po++; else ze++;
				const float a = fabsf(w);
				if (a < mn) mn = a;
				if (a > mx) mx = a;
			}
			vb->Unlock();

			// Classify this draw.
			std::set<int> abs_w;
			for (int v : draw_w) abs_w.insert(v < 0 ? -v : v);

			std::lock_guard<std::mutex> lk(s_mutex);
			auto& st = s_w_stats[key];
			st.draws++;
			st.samples += verts;
			st.negative += neg; st.positive += po; st.zero += ze;
			if (mn < st.min_abs) st.min_abs = mn;
			if (mx > st.max_abs) st.max_abs = mx;

			if (draw_w.size() == 1) {
				st.draws_w_const++;
				if (st.per_draw_absw.size() < MAX_SET) st.per_draw_absw.insert(*abs_w.begin());
			}
			else if (abs_w.size() == 1) {
				st.draws_absw_const++;
				if (st.per_draw_absw.size() < MAX_SET) st.per_draw_absw.insert(*abs_w.begin());
			}
			else {
				st.draws_varying++;
				if (st.example_varying_w.empty()) st.example_varying_w = draw_w;
			}
		}
		// If the Lock failed (static VB in D3DPOOL_DEFAULT), just skip this draw.

		vb->Release();
		decl->Release();
	}

	// ---- perf triage --------------------------------------------------------
	namespace
	{
		long long s_qpc_freq = 0;
		long long s_qpc_last_present = 0;
		long long s_hook_accum = 0;      // ticks in our draw hook, current frame
		std::uint32_t s_draw_accum = 0;  // draws this frame

		// exponential moving averages (per frame)
		double s_ema_frame_ms = 0.0;
		double s_ema_hook_ms = 0.0;
		double s_ema_draws = 0.0;
		bool   s_ema_primed = false;
	}

	void perf_add_hook_ticks(long long qpc_ticks)
	{
		s_hook_accum += qpc_ticks;
		s_draw_accum++;
	}

	void perf_present()
	{
		LARGE_INTEGER now{};
		QueryPerformanceCounter(&now);
		if (!s_qpc_freq)
		{
			LARGE_INTEGER f{};
			QueryPerformanceFrequency(&f);
			s_qpc_freq = f.QuadPart ? f.QuadPart : 1;
			s_qpc_last_present = now.QuadPart;
			return;
		}

		const double frame_ms = 1000.0 * double(now.QuadPart - s_qpc_last_present) / double(s_qpc_freq);
		const double hook_ms = 1000.0 * double(s_hook_accum) / double(s_qpc_freq);
		s_qpc_last_present = now.QuadPart;
		s_hook_accum = 0;
		const double draws = double(s_draw_accum);
		s_draw_accum = 0;

		// Ignore absurd frames (load hitches, alt-tab) so the EMA stays meaningful.
		if (frame_ms > 0.0 && frame_ms < 1000.0)
		{
			if (!s_ema_primed)
			{
				s_ema_frame_ms = frame_ms; s_ema_hook_ms = hook_ms;
				s_ema_draws = draws;
				s_ema_primed = true;
			}
			else
			{
				const double a = 0.05;
				s_ema_frame_ms = s_ema_frame_ms * (1 - a) + frame_ms * a;
				s_ema_hook_ms  = s_ema_hook_ms  * (1 - a) + hook_ms  * a;
				s_ema_draws    = s_ema_draws    * (1 - a) + draws    * a;
			}
		}
	}

	// -------------------------------------------------------------------------
	void write_report()
	{
		ensure_init();
		std::lock_guard<std::mutex> lk(s_mutex);

		std::ofstream f(s_dump_dir / "posw_report.txt");
		if (!f) return;

		f << "AC2 pos.w analysis (per-DRAW classification)\n";
		f << "============================================\n";
		f << "Many meshes share one vertex format, so |w| is only expected to be\n";
		f << "constant PER DRAW. Each draw is classified on its own:\n";
		f << "  w-const     : every w identical in the draw -> fold w/32768 into World\n";
		f << "  |w|-const   : magnitude identical, sign flips -> sign is a flag bit\n";
		f << "  varying     : genuinely per-vertex -> needs CPU transform / vertex capture\n\n";
		f << "Confirmed from shader constant tables: g_WorldViewProj = c0..c3,\n";
		f << "g_BoneMatrixArray = c120..c245 (126 regs = 42 bones x 3).\n\n";
		f << "report #             : " << (++s_reports_written) << "  (auto-flush every 15s; F8/F10 forces)\n";
		{
			const double fps = (s_ema_frame_ms > 0.0) ? (1000.0 / s_ema_frame_ms) : 0.0;
			const double pct = (s_ema_frame_ms > 0.0) ? (100.0 * s_ema_hook_ms / s_ema_frame_ms) : 0.0;
			char buf[192];
			snprintf(buf, sizeof(buf),
				"PERF (EMA)           : frame %.2f ms (%.1f fps) | our draw-hook %.2f ms/frame (%.0f%%) | %.0f draws/frame\n",
				s_ema_frame_ms, fps, s_ema_hook_ms, pct, s_ema_draws);
			f << buf;
			f << "                       (GPU-bound in Remix's path tracer; hook is CPU-side only)\n";
		}
		f << "sampling enabled     : " << (g_enabled ? "yes" : "NO - press F9") << "\n";
		f << "total draws seen     : " << s_total_draws << "\n";
		f << "FF conversion (F7)   : " << (ac2_ff::g_enabled ? "ON" : "off")
			<< "   converted draws: " << ac2_ff::g_converted_draws
			<< "   shadow VBs: " << ac2_ff::g_shadow_vbs << "\n";
		f << "FF-ONLY mode (INSERT): " << (ac2_ff::g_ff_only ? "ON - unconvertible draws DROPPED" : "off")
			<< "\n";
		f << "CPU skinning (F1)    : " << (ac2_ff::g_skinning ? "ON" : "off")
			<< "   skinned draws: " << ac2_ff::g_skinned_draws << "\n";
		f << "anti-cull (DELETE)   : " << (ac2_ff::g_anticull
			? "ON - VisualsCulling gate + entity VISIBLE bit forced" : "off - game frustum-culls") << "\n";
		f << "CullAABB (PAGE DOWN) : " << (ac2_ff::g_cullaabb
			? "ON - always 'intersecting'" : "off") << "\n";
		f << "occlusion off (PAGE UP): " << (ac2_ff::g_occlusion_off
			? "ON - HW occlusion-query cull disabled (threshold 25 -> 0)" : "off") << "\n";
		f << "const source (F12)   : " << (ac2_ff::g_use_const_shadow
			? "OUR SHADOW (bridge-proof)" : "bridge GetVertexShaderConstantF") << "\n";
		f << "engine View/Proj     : " << (ac2_ff::g_have_matrices()
			? "captured" : "NOT captured") << "\n";
		f << "camera mode (F5)     : " << (ac2_ff::g_stage_b
			? "STAGE B (real View/Proj - Remix-correct)" : "stage A (View=I, Proj=WVP)") << "\n";
		f << "max RELATIVE mvp err : " << ac2_ff::g_max_wvp_error
			<< "   (<1e-6 = essentially exact)\n";
		f << "World source         : " << ac2_ff::world_source_name() << "\n";

		// ---- where do draws actually go? -------------------------------------
		{
			const auto& r = ac2_ff::g_rej;
			f << "\n---- FF draw disposition (why draws are/aren't converted) ----\n";
			f << "  seen              : " << r.seen << "\n";
			f << "  no_decl           : " << r.no_decl << "\n";
			f << "  not_static        : " << r.not_static << "   (skinned, or position not SHORT4)\n";
			f << "  no_diffuse        : " << r.no_diffuse << "   (lighting/AO/projector pass)\n";
			f << "  no_stream         : " << r.no_stream << "\n";
			f << "  no_scale_or_vb    : " << r.no_scale_or_vb << "\n";
			f << "  no_c8             : " << r.no_c8 << "   (g_World readback failed)\n";
			f << "  world_singular    : " << r.world_singular << "\n";
			f << "  ORTHO             : " << r.ortho << "   (decompose_vp: col3 ~ (0,0,0,1))\n";
			f << "  bad_reconstruct   : " << r.bad_reconstruct << "   (decomposed but W*V*P != WVP)\n";
			f << "  CONVERTED         : " << r.converted << "\n";
			f << "    ..of which multi-stream : " << r.multistream
				<< "   (POSITION not on stream 0, or attrs split across streams)\n";
			f << "  -- skinned path --\n";
			f << "    skin_seen       : " << r.skin_seen << "\n";
			f << "    skin_no_diffuse : " << r.skin_no_diffuse << "\n";
			f << "    skin_no_stream  : " << r.skin_no_stream << "\n";
			f << "    skin_no_wvp     : " << r.skin_no_wvp << "\n";
			f << "    skin_no_camera  : " << r.skin_no_camera << "   (ortho / decompose failed)\n";
			f << "    skin_no_dynvb   : " << r.skin_no_dynvb << "\n";
			f << "    skin_lock_fail  : " << r.skin_lock_fail << "\n";
			f << "    skin_OK         : " << r.skin_ok << "\n";
		}
		ac2_ff::dump_rejected_formats(f);
		ac2_ff::dump_vs_coverage(f);
		ac2_ff::dump_veg(f);
		ac2_ff::dump_normal_only(f);
		ac2_lights::dump(f);

		// ---- skinning diagnostic: is the palette real, or identity? ----------
		if (ac2_ff::g_skin_diag.valid)
		{
			const auto& s = ac2_ff::g_skin_diag;
			f << "\n---- SKIN diagnostic (vertex 0 of one real skinned draw) ----\n";
			f << "  shadow had all 126 regs : " << (s.shadow_had_all ? "yes" : "NO (fell back to device)") << "\n";
			f << "  max bone index this draw: " << s.max_bone_index << "  (>=42 would overflow our MAX_BONES)\n";
			f << "  v0 BLENDINDICES : " << s.idx[0] << " " << s.idx[1] << " " << s.idx[2] << " " << s.idx[3] << "\n";
			f << "  v0 BLENDWEIGHT  : " << s.wgt[0] << " " << s.wgt[1] << " " << s.wgt[2] << " " << s.wgt[3]
				<< "   (sum " << (s.wgt[0] + s.wgt[1] + s.wgt[2] + s.wgt[3]) << ", should be ~1)\n";
			f << "  v0 raw SHORT4   : " << s.src_pos[0] << " " << s.src_pos[1] << " " << s.src_pos[2]
				<< "  w(AO)=" << s.src_pos[3] << "\n";
			f << "  v0 skinned pos  : " << s.out_pos[0] << " " << s.out_pos[1] << " " << s.out_pos[2] << "\n";
			f << "  first 6 bone matrices (3 rows x 4 - IDENTITY-ish => palette not where we think):\n";
			for (int b = 0; b < 6; ++b)
			{
				f << "    bone" << b << ":";
				for (int k = 0; k < 12; ++k)
				{
					f << " " << s.bones[b][k];
					if (k % 4 == 3 && k != 11) f << "  |";
				}
				f << "\n";
			}
		}

		// ---- texture/sampler diagnostic: why are we stuck on the lowest mip? --
		{
			const auto tex_filter_name = [](DWORD v) -> const char*
			{
				switch (v)
				{
				case D3DTEXF_NONE:            return "NONE";
				case D3DTEXF_POINT:           return "POINT";
				case D3DTEXF_LINEAR:          return "LINEAR";
				case D3DTEXF_ANISOTROPIC:     return "ANISOTROPIC";
				case D3DTEXF_PYRAMIDALQUAD:   return "PYRAMIDALQUAD";
				case D3DTEXF_GAUSSIANQUAD:    return "GAUSSIANQUAD";
				default:                      return "?";
				}
			};
			const auto pool_name = [](DWORD p) -> const char*
			{
				switch (p)
				{
				case D3DPOOL_DEFAULT:   return "DEFAULT";
				case D3DPOOL_MANAGED:   return "MANAGED";
				case D3DPOOL_SYSTEMMEM: return "SYSTEMMEM";
				case D3DPOOL_SCRATCH:   return "SCRATCH";
				default:                return "?";
				}
			};
			const auto put_samp = [&](const char* tag, const ac2_ff::tex_diag::samp& s)
			{
				f << "    " << tag
					<< " MAXMIPLEVEL " << s.max_mip_level
					<< " | MIPFILTER " << tex_filter_name(s.mip_filter)
					<< " | LODBIAS " << s.mip_lod_bias
					<< " | MIN " << tex_filter_name(s.min_filter)
					<< " | MAG " << tex_filter_name(s.mag_filter) << "\n";
			};

			f << "\n---- TEXTURE/SAMPLER diagnostic (blurry-texture regression) ----\n";
			f << "  Run 1 settled the original question: sampler states were IDENTICAL and\n";
			f << "  GetLOD was 0 on a MANAGED texture, so nothing is clamping a mip. The\n";
			f << "  bound textures were simply TINY at level 0 (8x8, 64x64). So the question\n";
			f << "  is now: are we binding the WRONG SAMPLER? Read the stage table below -\n";
			f << "  a big texture at a stage we passed over IS the answer.\n";

			for (int i = 0; i < 2; ++i)
			{
				const auto& d = ac2_ff::g_tex_diag[i];
				const char* what = (i == 0) ? "[stage==0: diffuse already at 0, we move NOTHING]"
					: "[stage!=0: we move the texture to stage 0]";
				f << "\n  " << what << "\n";
				if (!d.valid)
				{
					// Not the same as "nothing wrong" - say so out loud.
					f << "    NOT CAPTURED - no converted draw of this kind since the last arm.\n";
					continue;
				}
				f << "    diffuse_stage : " << d.diffuse_stage
					<< "   picked by rank " << d.rank
					<< (d.rank == 3 ? " (GUESS - unrecognised name)" : "")
					<< "   sampler '" << d.name << "'\n";
				put_samp("src(diffuse_stage):", d.src);
				put_samp("dst(stage 0)     :", d.dst);

				const bool differs = d.src.max_mip_level != d.dst.max_mip_level
					|| d.src.mip_filter != d.dst.mip_filter
					|| d.src.mip_lod_bias != d.dst.mip_lod_bias
					|| d.src.min_filter != d.dst.min_filter
					|| d.src.mag_filter != d.dst.mag_filter;
				f << "    SAMPLER STATES  : " << (differs ? "DIFFER  <- FF samples with the WRONG one"
					: "identical") << "\n";

				if (!d.have_tex)
				{
					f << "    texture         : NONE BOUND (!?)\n";
					continue;
				}
				f << "    texture         : " << d.tex_width << "x" << d.tex_height
					<< "  levels " << d.tex_levels
					<< "  GetLOD " << d.tex_lod
					<< "  pool " << pool_name(d.tex_pool)
					<< "  fmt " << d.tex_format << "\n";
				if (d.tex_pool != D3DPOOL_MANAGED)
					f << "    (pool is not MANAGED => GetLOD is always 0 here; it proves nothing)\n";

				// The decisive table. '<- WE PICKED THIS' next to an 8x8 while a
				// 1024x1024 sits two rows down is the whole diagnosis.
				// NB: do NOT read "biggest texture wins" into this table. s7 is
				// g_ReflectionSampler and is a 512x288 render target - it is the
				// largest bound texture on many draws and is never the diffuse.
				// An earlier version flagged exactly that as "the matcher chose
				// wrong" and was simply wrong itself.
				f << "    all stages the GAME had bound at this draw:\n";
				for (int s = 0; s < 8; ++s)
				{
					const auto& st = d.stages[s];
					if (!st.bound) continue;
					f << "      s" << s << ": " << st.width << "x" << st.height
						<< "  levels " << st.levels << "  fmt " << st.format;
					if (s == d.diffuse_stage) f << "   <- WE PICKED THIS";
					f << "\n";
				}
			}

			ac2_ff::dump_diffuse_sizes(f);
		}
		ac2_ff::dump_cache_diag(f);

		// ---- texture lifecycle (Remix hash stability) ------------------------
		f << "\n---- textures (Remix hash stability) ----\n";
		f << "  CreateTexture calls   : " << s_tex_creates << "\n";
		f << "  distinct pointers     : " << s_textures.size() << "\n";
		f << "  POINTER REUSES        : " << s_tex_ptr_reuses
			<< "   (same ptr created again => a released texture's address recycled)\n";
		f << "  ...with DIFFERENT desc: " << s_tex_reuse_diff_desc
			<< "   (<- if >0, anything keyed on the pointer conflates two textures)\n";
		f << "  pools: ";
		for (const auto& [p, n] : s_tex_pools)
		{
			const char* pn = (p == D3DPOOL_DEFAULT) ? "DEFAULT" : (p == D3DPOOL_MANAGED) ? "MANAGED"
				: (p == D3DPOOL_SYSTEMMEM) ? "SYSTEMMEM" : (p == D3DPOOL_SCRATCH) ? "SCRATCH" : "?";
			f << pn << "=" << n << " ";
		}
		f << "\n  usages: ";
		for (const auto& [u, n] : s_tex_usages)
		{
			f << "0x" << std::hex << u << std::dec << "=" << n;
			if (u & D3DUSAGE_DYNAMIC) f << "(DYNAMIC)";
			if (u & D3DUSAGE_RENDERTARGET) f << "(RT)";
			f << " ";
		}
		f << "\n";

		// ---- matrix diagnostic ----------------------------------------------
		if (ac2_ff::g_diag.valid)
		{
			const auto& d = ac2_ff::g_diag;
			auto dump = [&](const char* name, const D3DXMATRIX& m)
			{
				f << "  " << name << ":\n";
				for (int i = 0; i < 4; ++i)
				{
					f << "    ";
					for (int j = 0; j < 4; ++j) f << m.m[i][j] << "\t";
					f << "\n";
				}
			};

			f << "\n---- matrix diagnostic (one real converted draw) ----\n";
			f << "  reconstruct err = " << d.err_c8 << "   (W*V*P vs uploaded WVP)\n";

			// Is the decomposed view basis right-handed? det(R) must be +1; -1 means
			// a mirrored camera, which rasterises fine but breaks Remix's rays.
			const auto& v = d.engine_view; // holds the DECOMPOSED view
			const float det =
				v.m[0][0] * (v.m[1][1] * v.m[2][2] - v.m[1][2] * v.m[2][1])
				- v.m[0][1] * (v.m[1][0] * v.m[2][2] - v.m[1][2] * v.m[2][0])
				+ v.m[0][2] * (v.m[1][0] * v.m[2][1] - v.m[1][1] * v.m[2][0]);
			f << "  det(view 3x3)   = " << det << "   (+1 = right-handed, -1 = MIRRORED)\n";
			f << "  proj[2][3]      = " << d.engine_proj.m[2][3] << "   (+1 = LH, -1 = RH)\n";
			f << "  proj diag       = " << d.engine_proj.m[0][0] << ", " << d.engine_proj.m[1][1]
				<< ", " << d.engine_proj.m[2][2] << "   proj[3][2] = " << d.engine_proj.m[3][2] << "\n\n";

			dump("WVP (c0, known good)", d.wvp);
			dump("World (c8)", d.world_c8);
			dump("vp_true = inv(World_c8)*WVP", d.vp_from_data);
			dump("DECOMPOSED View", d.engine_view);
			dump("DECOMPOSED Proj", d.engine_proj);
			dump("check: View*Proj (should == vp_true)", d.vp_engine);
			f << "\n";
		}
		f << "unique vertex formats : " << s_w_stats.size() << "\n";
		f << "vertex shaders dumped : " << s_seen_shaders.size() << "\n\n";

		for (const auto& [key, st] : s_w_stats)
		{
			f << "---- format 0x" << std::hex << key << std::dec << " ----\n";
			f << "  " << st.format << "\n";
			f << "  draws=" << st.draws << " samples=" << st.samples << "\n";
			f << "  per-draw verdict:  w-const=" << st.draws_w_const
				<< "  |w|-const(sign flips)=" << st.draws_absw_const
				<< "  varying=" << st.draws_varying << "\n";
			f << "  w sign totals: +" << st.positive << " / -" << st.negative << " / 0=" << st.zero << "\n";
			f << "  |w| range across all draws: " << st.min_abs << " .. " << st.max_abs << "\n";

			if (!st.per_draw_absw.empty())
			{
				f << "  per-draw |w| values (one per constant draw): ";
				for (int v : st.per_draw_absw) f << v << " ";
				f << "\n";
			}
			if (!st.example_varying_w.empty())
			{
				f << "  example of a VARYING draw's w set: ";
				for (int v : st.example_varying_w) f << v << " ";
				f << "\n";
			}

			const std::uint32_t good = st.draws_w_const + st.draws_absw_const;
			if (st.draws && good == st.draws) {
				f << "  => EVERY draw has constant |w|: fixed-function conversion works for this format.\n";
			}
			else if (st.draws && good > 0) {
				f << "  => MIXED: " << good << "/" << st.draws
					<< " draws foldable; the rest need CPU transform / vertex capture.\n";
			}
			else {
				f << "  => w varies per-vertex in every draw: not foldable into World.\n";
			}
			f << "\n";
		}

		ac2_ff::dump_patch_status(f);

		f << "Shader disassembly: .\\ac2_rtx_dump\\shaders\\*.asm\n";
	}

	void shutdown()
	{
		write_report();
	}
}
