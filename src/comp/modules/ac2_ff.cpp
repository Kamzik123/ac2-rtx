#include "std_include.hpp"
#include "ac2_ff.hpp"

#include <cmath>

namespace comp::ac2_ff
{
	bool g_enabled = true;
	bool g_wireframe = false;
	bool g_ff_only = true;   // INSERT toggles OFF; default ON
	bool g_skinning = true;
	std::uint32_t g_skinned_draws = 0;
	bool g_anticull = false;
	bool g_use_const_shadow = true;

	// ---- vertex-shader constant shadow --------------------------------------
	// vs_3_0 has 256 float4 constants. AC2 uses c0..c245 (g_BoneMatrixArray ends
	// at c245), so 256 covers it.
	namespace
	{
		constexpr UINT VS_CONST_MAX = 256;
		float       s_vs_const[VS_CONST_MAX][4]{};
		bool        s_vs_const_set[VS_CONST_MAX]{};
		std::mutex  s_const_mtx;   // device is D3DCREATE_MULTITHREADED
	}

	void on_set_vs_constant_f(UINT start_register, const float* data, UINT vec4_count)
	{
		if (!data || start_register >= VS_CONST_MAX) return;
		const UINT n = (start_register + vec4_count > VS_CONST_MAX)
			? (VS_CONST_MAX - start_register) : vec4_count;

		std::lock_guard<std::mutex> lk(s_const_mtx);
		memcpy(&s_vs_const[start_register][0], data, n * 4 * sizeof(float));
		for (UINT i = 0; i < n; ++i) s_vs_const_set[start_register + i] = true;
	}

	bool get_vs_constant_f(UINT start_register, float* out, UINT vec4_count)
	{
		if (!out || start_register + vec4_count > VS_CONST_MAX) return false;

		std::lock_guard<std::mutex> lk(s_const_mtx);
		// Only trust it if the game has actually written these registers.
		for (UINT i = 0; i < vec4_count; ++i)
			if (!s_vs_const_set[start_register + i]) return false;

		memcpy(out, &s_vs_const[start_register][0], vec4_count * 4 * sizeof(float));
		return true;
	}

	// Single entry point so every read goes through the shadow (or, with F12, the
	// bridge - useful to prove which one is lying).
	static bool read_vs_const(IDirect3DDevice9* dev, UINT start, float* out, UINT count)
	{
		if (g_use_const_shadow) return get_vs_constant_f(start, out, count);
		return SUCCEEDED(dev->GetVertexShaderConstantF(start, out, count));
	}

	// ---- anti-culling patches -----------------------------------------------
	// (1) Submission gate inside sub_11B1FB0 ("VisualsCulling" job).
	//     11b2075: test edx,eax ; jz  ->  mov eax,edx ; nop nop
	//     Submits the object with the pass's own mask instead of the culled
	//     sentinel 36. Applied successfully but on its own changed nothing - the
	//     object list it iterates is already pre-filtered upstream.
	//
	// (2) Entity culling flag inside sub_126490 ("UpdateEntitiesCullingFlags").
	//     126674: setz al  ->  xor al,al ; nop
	//     Bit 1 of the entity flags at +96 is CULLED, computed as 2*(!visible)
	//     from sub_119C7C0. Forcing al=0 keeps that bit permanently clear.
	//     This is the interesting one: games skip ANIMATION updates for culled
	//     entities, so a culled-but-drawn character keeps its BIND POSE => the
	//     T-pose. It also feeds scimitar::Entity::UpdateLODLevel.
	//     (cdq/shld then propagate 0 into the upper flag dword harmlessly.)
	// (3) **scimitar::CameraFrustum::CullAABB** = sub_119BE50 (RVA 0x119be50).
	//     Identified by structural match against the Mac symbol
	//     `_ZNK8scimitar13CameraFrustum8CullAABBERKN4Gear7Vector4IfEES5_`:
	//     both loop 6 planes (16 bytes each) calling CullPlaneAABBoxRes
	//     (= sub_16F1F70), break on 2, OR the per-plane results.
	//     Return semantics are NOT the classic 0=inside/1=intersect/2=outside.
	//     Read what the callers DO with it - Visuals_VisibilityTest (0x11b1710):
	//         if (v20 != 1) { if (v20 == 3) v28 |= v27; goto skip; }
	//         v28 |= v27;
	//     i.e. ONLY 1 or 3 set the visibility bit; 0 and 2 both leave it CLEAR.
	//     So returning 0 ("fully inside") reads as NOT VISIBLE to this caller -
	//     forcing 0 made every character T-pose. Return 1 (intersecting) instead:
	//     it sets the bit, and it avoids any containment fast-path that a
	//     "fully inside" answer might trigger.
	//     It has SIX callers, so forcing it to 0 disables culling at the source for
	//     entity culling, visuals culling and everything else at once - this is the
	//     approach the user validated on AC4 (same Scimitar/Anvil engine family).
	//     __thiscall(this in ecx, 2 stack args) -> epilogue is `retn 8`, so the
	//     early-out must clean the stack itself: xor eax,eax ; retn 8.
	struct code_patch
	{
		std::uintptr_t rva;
		std::uint8_t   orig[8];
		std::uint8_t   patch[8];
		UINT           len;
	};

	// DELETE applies these. Narrow, local, and they have never crashed.
	static const code_patch s_cull_patches[] = {
		// VisualsCulling submission gate: test edx,eax ; jz  ->  mov eax,edx ; nop nop
		{ 0x11b2075, { 0x85, 0xC2, 0x74, 0x09 }, { 0x8B, 0xC2, 0x90, 0x90 }, 4 },
		// UpdateEntitiesCullingFlags (0x126490): force the entity's VISIBLE bit on.
		//
		//   126674  setz al          ; al = (v26 == 0)
		//   126684  add eax, eax     ; bit1 = al
		//   126686  or  eax, ecx     ; [esi+60h] = (flags & ~2) | (2*al)
		//
		// bit1 is VISIBLE, *not* culled - I had this exactly backwards. Proof, from
		// Entity_FrustumVisibilityTest (0x119c7c0), whose result is v26:
		//     while (!plane_test(...)) { if (++i >= 6) return 0; }   // inside all 6
		//     return 1;                                              // outside one
		// => 0 = visible, 1 = culled. So bit1 = (v26 == 0) = VISIBLE.
		// Corroborated in the same function: it latches bit2 = old bit1, then treats
		// (bit2==0 && bit1!=0) specially - a RISING EDGE OF VISIBLE. That only parses
		// if bit1 means visible.
		//
		// The old patch was `xor al,al` (al=0) => bit1 cleared => EVERY entity marked
		// NOT VISIBLE. That is what T-posed the whole world, player included, and it
		// is why even a loading-zone character T-posed with nothing to cull against.
		//     setz al  (0F 94 C0)  ->  mov al,1 ; nop  (B0 01 90)
		// The following `and al,1` keeps 1, so bit1 is forced set = always visible.
		{ 0x126674,  { 0x0F, 0x94, 0xC0 }, { 0xB0, 0x01, 0x90 }, 3 },
	};

	// ---- CullAABB -> always return 1 ("intersecting"). PAGE DOWN, default ON. ----
	// scimitar::CameraFrustum::CullAABB (0x119be50), 6 callers - THE low-level
	// frustum test for entities, visuals, shadow cascades and spatial traversal.
	// Returning 1 FIXES THE BUILDING FLICKER (user-confirmed). Return values: only
	// 1 or 3 set the vis bit in Visuals_VisibilityTest (0x11b1710); 0 falls through
	// the same path as 2, so 0 does NOT mean "visible".
	//
	// DON'T overwrite the prologue. `mov eax,1 ; retn 8` at 0x119be50 CRASHES the
	// game - an 8-byte write across instruction boundaries in a hot function that
	// 6 call sites hit from JOB THREADS. A thread already executing inside the
	// prologue runs torn bytes. (That's my theory for the crash and it is NOT
	// verified - what IS verified is that the in-body patch below works.)
	//
	// Instead: short-circuit the loop to the existing epilogue, which the user
	// worked out. The prologue and its matching pop edi/esi/ebx both still run.
	//
	//   119be71  cmp eax, 2
	//   119be74  jz  short 119be8c   ; 74 16  ->  EB 16  = ALWAYS jump
	//   ...
	//   119be8c  pop edi             ; <- the "culled" epilogue, now the only exit
	//   119be8d  pop esi
	//   119be8e  mov eax, 2          ; B8 02.. -> B8 01.. = return 1, not 2
	//   119be93  pop ebx
	//   119be94  pop ebp
	//   119be95  retn 8
	//
	// One CullPlaneAABBoxRes call still happens and its result is discarded; the
	// function then always returns 1. Both writes are small and instruction-aligned.
	static const code_patch s_cullaabb_patch[] = {
		{ 0x119be74, { 0x74, 0x16 }, { 0xEB, 0x16 }, 2 },                             // jz -> jmp
		{ 0x119be8e, { 0xB8, 0x02, 0x00, 0x00, 0x00 }, { 0xB8, 0x01, 0x00, 0x00, 0x00 }, 5 }, // ret 2 -> ret 1
	};
	bool g_cullaabb = false;

	static void apply_patches(const code_patch* first, size_t count, bool enable, bool& flag)
	{
		const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleA(nullptr));
		bool any = false;

		for (size_t i = 0; i < count; ++i)
		{
			const auto& cp = first[i];
			auto* p = reinterpret_cast<std::uint8_t*>(base + cp.rva);

			// Only write if the bytes are exactly what we reverse-engineered -
			// a different exe build must be a no-op, not corrupted code.
			const std::uint8_t* expect = enable ? cp.orig : cp.patch;
			if (memcmp(p, expect, cp.len) != 0) continue;

			DWORD old = 0;
			if (!VirtualProtect(p, cp.len, PAGE_EXECUTE_READWRITE, &old)) continue;
			memcpy(p, enable ? cp.patch : cp.orig, cp.len);
			VirtualProtect(p, cp.len, old, &old);
			FlushInstructionCache(GetCurrentProcess(), p, cp.len);
			any = true;
		}

		if (any) flag = enable;
	}

	void set_anticulling(bool enable)
	{
		apply_patches(s_cull_patches, std::size(s_cull_patches), enable, g_anticull);
	}

	void set_cullaabb(bool enable)
	{
		apply_patches(s_cullaabb_patch, std::size(s_cullaabb_patch), enable, g_cullaabb);
	}

	// Patch sites silently no-op when the memcmp guard misses, which is
	// indistinguishable from "applied but did nothing". Print the ACTUAL bytes so
	// the address and the expectation are both checkable instead of assumed.
	static void dump_one(std::ostream& os, const code_patch& cp, const char* tag)
	{
		const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleA(nullptr));
		auto* p = reinterpret_cast<const std::uint8_t*>(base + cp.rva);

		os << "  " << tag << " rva 0x" << std::hex << cp.rva << std::dec << "  ";

		MEMORY_BASIC_INFORMATION mbi{};
		if (!VirtualQuery(p, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT)
		{
			os << "UNMAPPED (base=0x" << std::hex << base << std::dec
			   << ") <- rva is wrong / not an RVA\n";
			return;
		}

		auto hex = [&os](const std::uint8_t* b, UINT n) {
			for (UINT i = 0; i < n; ++i)
				os << std::hex << (b[i] < 16 ? "0" : "") << int(b[i]) << std::dec << " ";
		};

		os << "actual: "; hex(p, cp.len);
		if (memcmp(p, cp.patch, cp.len) == 0)      os << " => PATCHED";
		else if (memcmp(p, cp.orig, cp.len) == 0)  os << " => original (not applied)";
		else
		{
			os << " => MISMATCH  expected orig: "; hex(cp.orig, cp.len);
		}
		os << "\n";
	}

	void dump_patch_status(std::ostream& os)
	{
		os << "\n---- code patch sites (actual bytes in memory) ----\n";
		os << "  module base: 0x" << std::hex
		   << reinterpret_cast<std::uintptr_t>(GetModuleHandleA(nullptr)) << std::dec << "\n";
		for (const auto& cp : s_cull_patches)    dump_one(os, cp, "DELETE  ");
		for (const auto& cp : s_cullaabb_patch)  dump_one(os, cp, "PAGEDOWN");
	}

	bool g_stage_b = true;
	float g_max_wvp_error = 0.0f;
	float g_depth_bias = 0.0f;
	world_src g_world_source = world_src::none;
	diag_snapshot g_diag{};
	static volatile bool s_diag_armed = true;

	// Do NOT clear g_diag.valid here - the report reads it. Just let the next
	// converted draw overwrite the snapshot.
	void arm_diagnostic() { s_diag_armed = true; }

	skin_diag g_skin_diag{};
	static volatile bool s_skin_diag_armed = true;
	// True only while walking the ONE draw the diagnostic captured, so
	// max_bone_index describes that draw and not whatever ran last.
	static volatile bool s_skin_diag_capturing = false;
	void arm_skin_diagnostic() { s_skin_diag_armed = true; }
	std::uint32_t g_converted_draws = 0;
	std::uint32_t g_shadow_vbs = 0;
	reject_stats g_rej{};

	namespace
	{
		// 32767 * 8 - the constant the game's own shaders use (c4.x = 1/262136).
		constexpr float POS_SCALE_DIV = 262136.0f;

		// ---- Stage B: real View/Projection from the engine -------------------
		// scimitar::GfxContext keeps World/View/Projection separately:
		//   +544 (idx 34) Projection, +608 (idx 38) View, +672 (idx 42) World
		// and each setter recomputes ViewProj = Proj*View, WVP = ViewProj*World.
		// The engine works column-vector (clip = P*V*W*pos) and transposes on
		// upload, so D3D's row-vector matrices are the transposes of these.
		constexpr std::uintptr_t RVA_SET_PROJ = 0x1273520; // scimitar::GfxContext::SetProjectionMatrix
		constexpr std::uintptr_t RVA_SET_VIEW = 0x12735a0; // scimitar::GfxContext::SetViewMatrix
		constexpr std::uintptr_t RVA_SET_WORLD = 0x1273620; // GfxContext_SetWorldMatrix
		constexpr std::uintptr_t OFF_PROJ = 544;
		constexpr std::uintptr_t OFF_VIEW = 608;
		constexpr std::uintptr_t OFF_WORLD = 672;

		using PFN_SetMat = void* (__fastcall*)(void* ecx, void* edx, const void* m);
		PFN_SetMat o_set_proj = nullptr;
		PFN_SetMat o_set_view = nullptr;
		PFN_SetMat o_set_world = nullptr;

		std::mutex     s_mtx_mat;
		D3DXMATRIX     s_view{};   // D3D convention (already transposed)
		D3DXMATRIX     s_proj{};
		D3DXMATRIX     s_world{};
		volatile bool  s_have_matrices = false; // view+proj
		volatile bool  s_have_world = false;

		// IMPORTANT: GfxContext matrices are ALREADY in D3D row-vector form
		// (translation in the last ROW). Proven by the diagnostic: transposing
		// them moved the translation into the last COLUMN, which is wrong.
		// Only the *constants* (c0/c8) need transposing - the engine transposes
		// those on upload via Matrix44_TransposeCopy.
		void capture_from_ctx(void* ctx)
		{
			if (!ctx) return;
			const auto* base = static_cast<const std::uint8_t*>(ctx);

			D3DXMATRIX p, v;
			memcpy(&p, base + OFF_PROJ, sizeof(D3DXMATRIX));
			memcpy(&v, base + OFF_VIEW, sizeof(D3DXMATRIX));

			std::lock_guard<std::mutex> lk(s_mtx_mat);
			s_proj = p;
			s_view = v;
			s_have_matrices = true;
		}

		void capture_world(void* ctx)
		{
			if (!ctx) return;
			D3DXMATRIX w;
			memcpy(&w, static_cast<const std::uint8_t*>(ctx) + OFF_WORLD, sizeof(D3DXMATRIX));

			std::lock_guard<std::mutex> lk(s_mtx_mat);
			s_world = w;
			s_have_world = true;
		}

		void* __fastcall hk_set_proj(void* ecx, void* edx, const void* m)
		{
			void* r = o_set_proj(ecx, edx, m);
			capture_from_ctx(ecx);
			return r;
		}

		void* __fastcall hk_set_view(void* ecx, void* edx, const void* m)
		{
			void* r = o_set_view(ecx, edx, m);
			capture_from_ctx(ecx);
			return r;
		}

		void* __fastcall hk_set_world(void* ecx, void* edx, const void* m)
		{
			void* r = o_set_world(ecx, edx, m);
			capture_world(ecx);
			return r;
		}

		// Split a true View*Proj into View and Projection ANALYTICALLY.
		//
		// Why this is necessary: W*V*P == WVP holds for ANY split of V and P, so
		// rasterisation looks perfect no matter how wrong the split is. Remix does
		// NOT use the product - it reads View and Projection separately to build
		// its camera. Deriving P from the engine's (stale, pass-dependent) View
		// therefore gives a camera that jitters per draw => flicker in Remix while
		// the raster output stays flawless. So V and P must come from VP itself.
		//
		// Row-vector D3D. V is rigid, P is perspective:
		//   V rows: v_i = (r_i0, r_i1, r_i2, 0), v_3 = (tx, ty, tz, 1)
		//   P      = [[a,0,0,0],[0,b,0,0],[0,0,c,1],[0,0,d,0]]
		// Multiplying out:
		//   VP[i][0] = a*r_i0      VP[i][1] = b*r_i1
		//   VP[i][2] = c*r_i2      VP[i][3] = r_i2       (i < 3)
		//   VP[3][0] = a*tx        VP[3][1] = b*ty
		//   VP[3][2] = c*tz + d    VP[3][3] = tz
		// So column 3 hands us the view's third axis and tz directly, and the rest
		// falls out because the view's basis vectors are unit length.
		bool decompose_vp(const D3DXMATRIX& vp, D3DXMATRIX& view, D3DXMATRIX& proj)
		{
			// Column 3 == view forward axis. Orthographic gives (0,0,0,1) here.
			D3DXVECTOR3 r2(vp.m[0][3], vp.m[1][3], vp.m[2][3]);
			const float r2len = D3DXVec3Length(&r2);
			if (r2len < 1e-4f) return false; // orthographic pass - not handled
			r2 /= r2len;

			D3DXVECTOR3 c0(vp.m[0][0], vp.m[1][0], vp.m[2][0]);
			D3DXVECTOR3 c1(vp.m[0][1], vp.m[1][1], vp.m[2][1]);
			D3DXVECTOR3 c2(vp.m[0][2], vp.m[1][2], vp.m[2][2]);

			const float a = D3DXVec3Length(&c0);
			const float b = D3DXVec3Length(&c1);
			if (a < 1e-8f || b < 1e-8f) return false;

			D3DXVECTOR3 r0 = c0 / a;
			D3DXVECTOR3 r1 = c1 / b;

			// SIGN AMBIGUITY. V*P == VP is preserved if we flip r2 and P[2][3]
			// together, so the algebra alone can't tell us which is real. Both
			// rasterise identically (which is why bad_reconstruct stays 0 and the
			// raster output is pristine) - but one of them is a MIRRORED,
			// left-handed camera basis, and Remix builds its rays from that basis.
			// A real D3D view matrix has an orthonormal basis with det(R) = +1, so
			// pick the sign that gives a right-handed frame.
			D3DXVECTOR3 cross01;
			D3DXVec3Cross(&cross01, &r0, &r1);
			const float handed = D3DXVec3Dot(&cross01, &r2); // == det(R)
			const float s = (handed < 0.0f) ? -1.0f : 1.0f;
			r2 *= s;

			const float p23 = s * r2len;                 // P[2][3], sign included
			const float c = D3DXVec3Dot(&c2, &r2);       // c2 == c * r2
			const float tz = vp.m[3][3] / p23;
			const float tx = vp.m[3][0] / a;
			const float ty = vp.m[3][1] / b;
			const float d = vp.m[3][2] - c * tz;

			D3DXMatrixIdentity(&view);
			view.m[0][0] = r0.x; view.m[0][1] = r1.x; view.m[0][2] = r2.x; view.m[0][3] = 0.0f;
			view.m[1][0] = r0.y; view.m[1][1] = r1.y; view.m[1][2] = r2.y; view.m[1][3] = 0.0f;
			view.m[2][0] = r0.z; view.m[2][1] = r1.z; view.m[2][2] = r2.z; view.m[2][3] = 0.0f;
			view.m[3][0] = tx;   view.m[3][1] = ty;   view.m[3][2] = tz;   view.m[3][3] = 1.0f;

			ZeroMemory(&proj, sizeof(proj));
			proj.m[0][0] = a;
			proj.m[1][1] = b;
			proj.m[2][2] = c;
			proj.m[2][3] = p23;
			proj.m[3][2] = d;

			return true;
		}

		// ---- camera cache ----------------------------------------------------
		// All draws in a pass share the same View*Proj, but each one goes through
		// its own inv(World) and its own decomposition, so V and P wobble by a few
		// ULPs from draw to draw. Rasterisation doesn't care (the product stays
		// exact), but Remix builds its CAMERA from V and P - and a camera that
		// jitters every draw makes a path tracer flicker. So decompose once per
		// distinct VP and hand out bit-identical matrices for the whole pass.
		std::mutex s_cam_mtx;
		D3DXMATRIX s_cam_vp{}, s_cam_view{}, s_cam_proj{};
		bool       s_cam_valid = false;

		bool same_vp(const D3DXMATRIX& a, const D3DXMATRIX& b)
		{
			float e = 0.0f, mag = 0.0f;
			for (int i = 0; i < 4; ++i)
				for (int j = 0; j < 4; ++j)
				{
					e = (std::max)(e, fabsf(a.m[i][j] - b.m[i][j]));
					mag = (std::max)(mag, fabsf(b.m[i][j]));
				}
			const float rel = (mag > 1e-6f) ? (e / mag) : e;
			return rel < 1e-5f;
		}

		void install_matrix_hooks()
		{
			const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleA(nullptr));
			// The IDB is rebased to 0, so the addresses we recorded are RVAs.
			MH_Initialize(); // no-op if the framework already did it

			auto* pp = reinterpret_cast<void*>(base + RVA_SET_PROJ);
			auto* pv = reinterpret_cast<void*>(base + RVA_SET_VIEW);
			auto* pw = reinterpret_cast<void*>(base + RVA_SET_WORLD);

			if (MH_CreateHook(pp, &hk_set_proj, reinterpret_cast<void**>(&o_set_proj)) == MH_OK)
				MH_EnableHook(pp);
			if (MH_CreateHook(pv, &hk_set_view, reinterpret_cast<void**>(&o_set_view)) == MH_OK)
				MH_EnableHook(pv);
			if (MH_CreateHook(pw, &hk_set_world, reinterpret_cast<void**>(&o_set_world)) == MH_OK)
				MH_EnableHook(pw);
		}

		// Our fixed-function vertex. Must match FF_FVF exactly.
		struct ff_vertex
		{
			float x, y, z;      // raw shorts widened; scale folded into World
			float nx, ny, nz;   // (UBYTE4 - 127) / 127
			float u, v;         // SHORT2N -> float
		};
		constexpr DWORD FF_FVF = D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_TEX1;
		static_assert(sizeof(ff_vertex) == 32, "ff_vertex must be 32 bytes");

		struct shadow_vb
		{
			IDirect3DVertexBuffer9* vb = nullptr;
			UINT vertex_count = 0;
		};

		std::mutex s_mutex;
		// keyed by source VB pointer (we AddRef the source to keep the key valid)
		std::map<IDirect3DVertexBuffer9*, shadow_vb> s_shadow;
		// per-draw |w|, keyed by (source VB, first vertex)
		std::map<std::pair<IDirect3DVertexBuffer9*, UINT>, float> s_scale;
		// pixel shader -> diffuse texture stage (-1 = none identified)
		std::map<IDirect3DPixelShader9*, int> s_ps_diffuse;

		struct decl_info
		{
			bool  ok = false;
			UINT  pos_off = 0;
			bool  pos_float3 = false;  // FLOAT3 = uncompressed, no |w| scale
			UINT  nrm_off = 0;  bool has_nrm = false;  bool nrm_float3 = false;
			UINT  uv_off = 0;   bool has_uv = false;   bool uv_float2 = false;
		};

		// We only take the generic static format: SHORT4 POSITION on stream 0.
		// Skinned meshes (SHORT4N + BLENDINDICES) are deliberately excluded -
		// they need bone handling that FF can't express the same way.
		decl_info classify(const D3DVERTEXELEMENT9* e, UINT n)
		{
			decl_info d;
			bool skinned = false;
			for (UINT i = 0; i < n && e[i].Stream != 0xFF; ++i)
			{
				if (e[i].Usage == D3DDECLUSAGE_BLENDINDICES
					|| e[i].Usage == D3DDECLUSAGE_BLENDWEIGHT) skinned = true;

				if (e[i].Stream != 0) continue;
				switch (e[i].Usage)
				{
				case D3DDECLUSAGE_POSITION:
					// SHORT4 = compressed (|w|/262136 folded into World).
					// FLOAT3 = uncompressed, used as-is. ~43k draws/run are FLOAT3
					// geometry we were silently dropping into vertex capture.
					if (e[i].Type == D3DDECLTYPE_SHORT4) { d.pos_off = e[i].Offset; d.ok = true; d.pos_float3 = false; }
					else if (e[i].Type == D3DDECLTYPE_FLOAT3) { d.pos_off = e[i].Offset; d.ok = true; d.pos_float3 = true; }
					break;
				case D3DDECLUSAGE_NORMAL:
					if (e[i].Type == D3DDECLTYPE_UBYTE4) { d.nrm_off = e[i].Offset; d.has_nrm = true; d.nrm_float3 = false; }
					else if (e[i].Type == D3DDECLTYPE_FLOAT3) { d.nrm_off = e[i].Offset; d.has_nrm = true; d.nrm_float3 = true; }
					break;
				case D3DDECLUSAGE_TEXCOORD:
					if (e[i].UsageIndex != 0) break;
					if (e[i].Type == D3DDECLTYPE_SHORT2N) { d.uv_off = e[i].Offset; d.has_uv = true; d.uv_float2 = false; }
					else if (e[i].Type == D3DDECLTYPE_FLOAT2) { d.uv_off = e[i].Offset; d.has_uv = true; d.uv_float2 = true; }
					break;
				default: break;
				}
			}
			if (skinned) d.ok = false;
			return d;
		}

		// Is this buffer re-written by the game each frame? Cloth/softbody is
		// simulated on the CPU and re-uploaded (scimitar: ClothLODInstance /
		// SoftBodyLODInstance::UpdateSubMesh), so a cached shadow copy freezes it
		// at its first-frame pose - it renders, but never follows the character.
		bool is_dynamic_vb(IDirect3DVertexBuffer9* vb)
		{
			D3DVERTEXBUFFER_DESC d{};
			if (FAILED(vb->GetDesc(&d))) return false;
			return (d.Usage & D3DUSAGE_DYNAMIC) != 0 || d.Pool == D3DPOOL_DEFAULT;
		}

		// Convert one draw's vertices straight into a caller-provided buffer.
		// Used for dynamic sources, where caching would freeze the geometry.
		UINT convert_range(IDirect3DVertexBuffer9* src, UINT stride, const decl_info& di,
			UINT first, UINT count, ff_vertex* out);

		// Build (once) a float FVF copy of the whole source buffer.
		shadow_vb* get_or_build_shadow(IDirect3DDevice9* dev, IDirect3DVertexBuffer9* src,
			UINT stride, const decl_info& di)
		{
			std::lock_guard<std::mutex> lk(s_mutex);
			auto it = s_shadow.find(src);
			if (it != s_shadow.end()) return &it->second;

			D3DVERTEXBUFFER_DESC vd{};
			if (FAILED(src->GetDesc(&vd)) || !stride) return nullptr;
			const UINT count = vd.Size / stride;
			if (!count) return nullptr;

			void* sdata = nullptr;
			if (FAILED(src->Lock(0, 0, &sdata, D3DLOCK_READONLY)) || !sdata) return nullptr;

			IDirect3DVertexBuffer9* dst = nullptr;
			if (FAILED(dev->CreateVertexBuffer(count * sizeof(ff_vertex), D3DUSAGE_WRITEONLY,
				FF_FVF, D3DPOOL_MANAGED, &dst, nullptr)) || !dst)
			{
				src->Unlock();
				return nullptr;
			}

			void* ddata = nullptr;
			if (FAILED(dst->Lock(0, 0, &ddata, 0)) || !ddata)
			{
				dst->Release(); src->Unlock();
				return nullptr;
			}

			const auto* sb = static_cast<const std::uint8_t*>(sdata);
			auto* out = static_cast<ff_vertex*>(ddata);

			for (UINT i = 0; i < count; ++i)
			{
				const auto* p = sb + static_cast<std::size_t>(stride) * i;
				ff_vertex& o = out[i];

				if (di.pos_float3)
				{
					float f[3];
					memcpy(f, p + di.pos_off, sizeof(f));
					o.x = f[0]; o.y = f[1]; o.z = f[2];
				}
				else
				{
					std::int16_t ps[4];
					memcpy(ps, p + di.pos_off, sizeof(ps));
					// raw shorts; the |w| scale is folded into World per draw
					o.x = static_cast<float>(ps[0]);
					o.y = static_cast<float>(ps[1]);
					o.z = static_cast<float>(ps[2]);
				}

				if (di.has_nrm && di.nrm_float3)
				{
					float f[3];
					memcpy(f, p + di.nrm_off, sizeof(f));
					o.nx = f[0]; o.ny = f[1]; o.nz = f[2];
				}
				else if (di.has_nrm)
				{
					const std::uint8_t* nb = p + di.nrm_off;
					o.nx = (static_cast<float>(nb[0]) - 127.0f) * (1.0f / 127.0f);
					o.ny = (static_cast<float>(nb[1]) - 127.0f) * (1.0f / 127.0f);
					o.nz = (static_cast<float>(nb[2]) - 127.0f) * (1.0f / 127.0f);
				}
				else { o.nx = 0.0f; o.ny = 1.0f; o.nz = 0.0f; }

				if (di.has_uv && di.uv_float2)
				{
					float f[2];
					memcpy(f, p + di.uv_off, sizeof(f));
					o.u = f[0]; o.v = f[1];
				}
				else if (di.has_uv)
				{
					std::int16_t t[2];
					memcpy(t, p + di.uv_off, sizeof(t));
					o.u = static_cast<float>(t[0]) * (1.0f / 32767.0f);
					o.v = static_cast<float>(t[1]) * (1.0f / 32767.0f);
				}
				else { o.u = 0.0f; o.v = 0.0f; }
			}

			dst->Unlock();
			src->Unlock();

			src->AddRef(); // keep the key pointer alive
			shadow_vb sv; sv.vb = dst; sv.vertex_count = count;
			g_shadow_vbs++;
			return &(s_shadow[src] = sv);
		}

		// ---- skinned (character) support -------------------------------------
		struct skin_info
		{
			bool ok = false;
			UINT pos_off = 0;   // SHORT4N
			UINT nrm_off = 0;   bool has_nrm = false;
			UINT uv_off = 0;    bool has_uv = false;
			UINT idx_off = 0;   // UBYTE4  BLENDINDICES
			UINT wgt_off = 0;   // UBYTE4N BLENDWEIGHT
		};

		skin_info classify_skinned(const D3DVERTEXELEMENT9* e, UINT n)
		{
			skin_info s;
			bool have_pos = false, have_idx = false, have_wgt = false;
			for (UINT i = 0; i < n && e[i].Stream != 0xFF; ++i)
			{
				if (e[i].Stream != 0) continue;
				switch (e[i].Usage)
				{
				case D3DDECLUSAGE_POSITION:
					if (e[i].Type == D3DDECLTYPE_SHORT4N) { s.pos_off = e[i].Offset; have_pos = true; }
					break;
				case D3DDECLUSAGE_BLENDINDICES:
					if (e[i].Type == D3DDECLTYPE_UBYTE4) { s.idx_off = e[i].Offset; have_idx = true; }
					break;
				case D3DDECLUSAGE_BLENDWEIGHT:
					if (e[i].Type == D3DDECLTYPE_UBYTE4N) { s.wgt_off = e[i].Offset; have_wgt = true; }
					break;
				case D3DDECLUSAGE_NORMAL:
					if (e[i].Type == D3DDECLTYPE_UBYTE4) { s.nrm_off = e[i].Offset; s.has_nrm = true; }
					break;
				case D3DDECLUSAGE_TEXCOORD:
					if (e[i].UsageIndex == 0 && e[i].Type == D3DDECLTYPE_SHORT2N) { s.uv_off = e[i].Offset; s.has_uv = true; }
					break;
				default: break;
				}
			}
			s.ok = have_pos && have_idx && have_wgt;
			return s;
		}

		constexpr UINT MAX_BONES = 42;   // 126 regs / 3, and (256-120)/3 confirms it

		// Bone palette rows as uploaded: c120+3i, c121+3i, c122+3i are the three
		// ROWS of a 3x4 matrix M, and the shader does skinned.x = dot(pos4, M[0]).
		struct bone_rows { D3DXVECTOR4 r0, r1, r2; };

		// Dynamic VB pool for skinned output (positions change every frame).
		std::vector<IDirect3DVertexBuffer9*> s_dyn_pool;
		std::size_t s_dyn_next = 0;

		IDirect3DVertexBuffer9* get_dynamic_vb(IDirect3DDevice9* dev, UINT bytes)
		{
			// Round-robin a small pool so we don't stall on a buffer still in flight.
			constexpr std::size_t POOL = 64;
			if (s_dyn_pool.size() < POOL) s_dyn_pool.resize(POOL, nullptr);

			const std::size_t i = (s_dyn_next++) % POOL;
			IDirect3DVertexBuffer9*& vb = s_dyn_pool[i];

			if (vb)
			{
				D3DVERTEXBUFFER_DESC d{};
				if (SUCCEEDED(vb->GetDesc(&d)) && d.Size >= bytes) return vb;
				vb->Release();
				vb = nullptr;
			}

			const UINT sz = (bytes + 4095) & ~4095u;
			if (FAILED(dev->CreateVertexBuffer(sz, D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY,
				FF_FVF, D3DPOOL_DEFAULT, &vb, nullptr)))
			{
				vb = nullptr;
			}
			return vb;
		}

		// What ARE the draws we reject? Record their declarations.
		std::map<std::string, std::uint32_t> s_rejected_fmts;

		const char* dtype_name(BYTE t)
		{
			switch (t)
			{
			case D3DDECLTYPE_FLOAT1: return "FLOAT1";
			case D3DDECLTYPE_FLOAT2: return "FLOAT2";
			case D3DDECLTYPE_FLOAT3: return "FLOAT3";
			case D3DDECLTYPE_FLOAT4: return "FLOAT4";
			case D3DDECLTYPE_D3DCOLOR: return "D3DCOLOR";
			case D3DDECLTYPE_UBYTE4: return "UBYTE4";
			case D3DDECLTYPE_SHORT2: return "SHORT2";
			case D3DDECLTYPE_SHORT4: return "SHORT4";
			case D3DDECLTYPE_UBYTE4N: return "UBYTE4N";
			case D3DDECLTYPE_SHORT2N: return "SHORT2N";
			case D3DDECLTYPE_SHORT4N: return "SHORT4N";
			case D3DDECLTYPE_FLOAT16_2: return "FLOAT16_2";
			case D3DDECLTYPE_FLOAT16_4: return "FLOAT16_4";
			default: return "?";
			}
		}
		const char* dusage_name(BYTE u)
		{
			switch (u)
			{
			case D3DDECLUSAGE_POSITION: return "POSITION";
			case D3DDECLUSAGE_BLENDWEIGHT: return "BLENDWEIGHT";
			case D3DDECLUSAGE_BLENDINDICES: return "BLENDINDICES";
			case D3DDECLUSAGE_NORMAL: return "NORMAL";
			case D3DDECLUSAGE_TEXCOORD: return "TEXCOORD";
			case D3DDECLUSAGE_TANGENT: return "TANGENT";
			case D3DDECLUSAGE_BINORMAL: return "BINORMAL";
			case D3DDECLUSAGE_COLOR: return "COLOR";
			case D3DDECLUSAGE_POSITIONT: return "POSITIONT";
			default: return "?";
			}
		}

		void note_rejected(const D3DVERTEXELEMENT9* e, UINT n)
		{
			std::string s;
			for (UINT i = 0; i < n && e[i].Stream != 0xFF; ++i)
			{
				if (e[i].Stream != 0) continue;
				s += "[+" + std::to_string(e[i].Offset) + " " + dtype_name(e[i].Type)
					+ " " + dusage_name(e[i].Usage) + std::to_string(e[i].UsageIndex) + "]";
			}
			if (s.empty()) return;
			std::lock_guard<std::mutex> lk(s_mutex);
			if (s_rejected_fmts.size() < 64 || s_rejected_fmts.count(s)) s_rejected_fmts[s]++;
		}

		// Shared by the static and skinned paths: recover World (c8) and a stable
		// View/Proj from the two in-sync constants. Returns false if this pass
		// can't be handled (orthographic, or the split doesn't reproduce the MVP).
		bool compute_camera(IDirect3DDevice9* dev, const D3DXMATRIX& wvp,
			D3DXMATRIX& world_c8, D3DXMATRIX& view, D3DXMATRIX& proj)
		{
			D3DXMATRIX w_t;
			if (!read_vs_const(dev, 8, reinterpret_cast<float*>(&w_t), 4))
				return false;
			D3DXMatrixTranspose(&world_c8, &w_t);

			D3DXMATRIX w_inv, vp_true;
			float wdet = 0.0f;
			if (!D3DXMatrixInverse(&w_inv, &wdet, &world_c8) || fabsf(wdet) <= 1e-20f) return false;
			D3DXMatrixMultiply(&vp_true, &w_inv, &wvp);

			bool cached = false;
			{
				std::lock_guard<std::mutex> lk(s_cam_mtx);
				if (s_cam_valid && same_vp(vp_true, s_cam_vp)) { view = s_cam_view; proj = s_cam_proj; cached = true; }
			}
			if (cached)
			{
				D3DXMATRIX chk;
				D3DXMatrixMultiply(&chk, &view, &proj);
				if (same_vp(chk, vp_true)) return true;
			}

			if (!decompose_vp(vp_true, view, proj)) return false;
			{
				std::lock_guard<std::mutex> lk(s_cam_mtx);
				s_cam_vp = vp_true; s_cam_view = view; s_cam_proj = proj; s_cam_valid = true;
			}
			return true;
		}

		// Shared decode: one source vertex -> one ff_vertex. (Same conversions the
		// cached shadow builder uses; factored out so the dynamic path can reuse it.)
		void decode_vertex(const std::uint8_t* p, const decl_info& di, ff_vertex& o)
		{
			if (di.pos_float3)
			{
				float f[3]; memcpy(f, p + di.pos_off, sizeof(f));
				o.x = f[0]; o.y = f[1]; o.z = f[2];
			}
			else
			{
				std::int16_t ps[4]; memcpy(ps, p + di.pos_off, sizeof(ps));
				o.x = static_cast<float>(ps[0]);
				o.y = static_cast<float>(ps[1]);
				o.z = static_cast<float>(ps[2]);
			}

			if (di.has_nrm && di.nrm_float3)
			{
				float f[3]; memcpy(f, p + di.nrm_off, sizeof(f));
				o.nx = f[0]; o.ny = f[1]; o.nz = f[2];
			}
			else if (di.has_nrm)
			{
				const std::uint8_t* nb = p + di.nrm_off;
				o.nx = (nb[0] - 127.0f) * (1.0f / 127.0f);
				o.ny = (nb[1] - 127.0f) * (1.0f / 127.0f);
				o.nz = (nb[2] - 127.0f) * (1.0f / 127.0f);
			}
			else { o.nx = 0.0f; o.ny = 1.0f; o.nz = 0.0f; }

			if (di.has_uv && di.uv_float2)
			{
				float f[2]; memcpy(f, p + di.uv_off, sizeof(f));
				o.u = f[0]; o.v = f[1];
			}
			else if (di.has_uv)
			{
				std::int16_t t[2]; memcpy(t, p + di.uv_off, sizeof(t));
				o.u = t[0] * (1.0f / 32767.0f);
				o.v = t[1] * (1.0f / 32767.0f);
			}
			else { o.u = 0.0f; o.v = 0.0f; }
		}

		UINT convert_range(IDirect3DVertexBuffer9* src, UINT stride, const decl_info& di,
			UINT first, UINT count, ff_vertex* out)
		{
			void* data = nullptr;
			if (FAILED(src->Lock(0, 0, &data, D3DLOCK_READONLY)) || !data) return 0;
			const auto* base = static_cast<const std::uint8_t*>(data);
			for (UINT v = 0; v < count; ++v)
				decode_vertex(base + static_cast<std::size_t>(stride) * (first + v), di, out[v]);
			src->Unlock();
			return count;
		}

		// |w| is constant per draw; read one vertex and cache it.
		bool get_scale(IDirect3DVertexBuffer9* src, UINT stride, UINT pos_off, UINT first, float& out)
		{
			const auto key = std::make_pair(src, first);
			{
				std::lock_guard<std::mutex> lk(s_mutex);
				auto it = s_scale.find(key);
				if (it != s_scale.end()) { out = it->second; return true; }
			}

			D3DVERTEXBUFFER_DESC vd{};
			if (FAILED(src->GetDesc(&vd))) return false;
			const std::size_t off = static_cast<std::size_t>(stride) * first + pos_off;
			if (off + 8 > vd.Size) return false;

			void* data = nullptr;
			if (FAILED(src->Lock(0, 0, &data, D3DLOCK_READONLY)) || !data) return false;
			std::int16_t ps[4];
			memcpy(ps, static_cast<const std::uint8_t*>(data) + off, sizeof(ps));
			src->Unlock();

			const float w = fabsf(static_cast<float>(ps[3]));
			if (w <= 0.0f) return false;
			out = w / POS_SCALE_DIV;

			std::lock_guard<std::mutex> lk(s_mutex);
			s_scale[key] = out;
			return true;
		}
	}

	// Skin a draw on the CPU and emit plain static geometry.
	// Returns the vertex count written, or 0 on failure. Output is indexed the
	// same as the source (index i -> vertex i), so the caller keeps the game's
	// index buffer and BaseVertexIndex.
	static UINT skin_to_buffer(IDirect3DDevice9* dev, IDirect3DVertexBuffer9* src,
		UINT stride, const skin_info& si, UINT first, UINT count, ff_vertex* out)
	{
		// Palette: c120..c245 = 42 bones x 3 registers (4x3 transposed).
		static thread_local D3DXVECTOR4 regs[MAX_BONES * 3];
		const bool had_all = read_vs_const(dev, 120, reinterpret_cast<float*>(regs), MAX_BONES * 3);
		if (!had_all)
		{
			// Fall back to whatever the device says, so a partially-written palette
			// doesn't silently kill every character draw.
			if (FAILED(dev->GetVertexShaderConstantF(120, reinterpret_cast<float*>(regs), MAX_BONES * 3)))
				return 0;
		}

		void* data = nullptr;
		if (FAILED(src->Lock(0, 0, &data, D3DLOCK_READONLY)) || !data) return 0;
		const auto* base = static_cast<const std::uint8_t*>(data);

		for (UINT v = 0; v < count; ++v)
		{
			const auto* p = base + static_cast<std::size_t>(stride) * (first + v);

			std::int16_t ps[4];
			memcpy(ps, p + si.pos_off, sizeof(ps));

			// SHORT4N: hardware would divide by 32768; then the shader scales by 16.
			// pos4 = (x*16, y*16, z*16, 1). w is AO, NOT part of the position.
			const float k = 16.0f / 32768.0f;
			const D3DXVECTOR4 pos4(ps[0] * k, ps[1] * k, ps[2] * k, 1.0f);

			const std::uint8_t* ib = p + si.idx_off;
			const std::uint8_t* wb = p + si.wgt_off;

			float nx = 0.0f, ny = 1.0f, nz = 0.0f;
			if (si.has_nrm)
			{
				const std::uint8_t* nb = p + si.nrm_off;
				nx = (nb[0] - 127.0f) * (1.0f / 127.0f);
				ny = (nb[1] - 127.0f) * (1.0f / 127.0f);
				nz = (nb[2] - 127.0f) * (1.0f / 127.0f);
			}
			const D3DXVECTOR4 nrm4(nx, ny, nz, 0.0f); // direction: w = 0

			D3DXVECTOR3 sp(0, 0, 0), sn(0, 0, 0);
			for (int j = 0; j < 4; ++j)
			{
				const float w = wb[j] * (1.0f / 255.0f); // UBYTE4N
				if (w <= 0.0f) continue;

				UINT bi = ib[j];
				if (bi >= MAX_BONES) bi = 0;
				const bone_rows& b = *reinterpret_cast<const bone_rows*>(&regs[bi * 3]);

				sp.x += w * D3DXVec4Dot(&pos4, &b.r0);
				sp.y += w * D3DXVec4Dot(&pos4, &b.r1);
				sp.z += w * D3DXVec4Dot(&pos4, &b.r2);

				sn.x += w * D3DXVec4Dot(&nrm4, &b.r0);
				sn.y += w * D3DXVec4Dot(&nrm4, &b.r1);
				sn.z += w * D3DXVec4Dot(&nrm4, &b.r2);
			}

			ff_vertex& o = out[v];
			o.x = sp.x; o.y = sp.y; o.z = sp.z;
			D3DXVec3Normalize(&sn, &sn);
			o.nx = sn.x; o.ny = sn.y; o.nz = sn.z;

			// ---- one-shot skin diagnostic (vertex 0 of one real draw) --------
			if (s_skin_diag_armed && v == 0)
			{
				for (int bn = 0; bn < 6; ++bn)
					memcpy(g_skin_diag.bones[bn], &regs[bn * 3], 12 * sizeof(float));
				for (int j = 0; j < 4; ++j)
				{
					g_skin_diag.idx[j] = ib[j];
					g_skin_diag.wgt[j] = wb[j] * (1.0f / 255.0f);
				}
				g_skin_diag.src_pos[0] = static_cast<float>(ps[0]);
				g_skin_diag.src_pos[1] = static_cast<float>(ps[1]);
				g_skin_diag.src_pos[2] = static_cast<float>(ps[2]);
				g_skin_diag.src_pos[3] = static_cast<float>(ps[3]);
				g_skin_diag.out_pos[0] = sp.x;
				g_skin_diag.out_pos[1] = sp.y;
				g_skin_diag.out_pos[2] = sp.z;
				g_skin_diag.shadow_had_all = had_all;
				g_skin_diag.max_bone_index = 0;
				g_skin_diag.valid = true;
				s_skin_diag_capturing = true;   // keep scanning THIS draw only
				s_skin_diag_armed = false;
			}

			// max_bone_index must describe the SAME draw as idx[]/bones[]. It used to
			// reset on v==0 of EVERY draw, so it reported the LAST skinned draw while
			// idx[] came from the armed one - which read as "max index 1 but vertex 0
			// uses bone 26" and looked like a real skinning bug. It wasn't.
			if (s_skin_diag_capturing)
			{
				for (int j = 0; j < 4; ++j)
					if (wb[j] && ib[j] > g_skin_diag.max_bone_index) g_skin_diag.max_bone_index = ib[j];
			}

			if (si.has_uv)
			{
				std::int16_t t[2];
				memcpy(t, p + si.uv_off, sizeof(t));
				o.u = t[0] * (1.0f / 32767.0f);
				o.v = t[1] * (1.0f / 32767.0f);
			}
			else { o.u = 0.0f; o.v = 0.0f; }
		}

		s_skin_diag_capturing = false;   // this draw is done
		src->Unlock();
		return count;
	}

	void dump_rejected_formats(std::ostream& os)
	{
		std::lock_guard<std::mutex> lk(s_mutex);
		std::vector<std::pair<std::string, std::uint32_t>> v(s_rejected_fmts.begin(), s_rejected_fmts.end());
		std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
		os << "\n---- REJECTED as not_static: what are they? (stream 0) ----\n";
		for (std::size_t i = 0; i < v.size() && i < 16; ++i)
			os << "  " << v[i].second << "  " << v[i].first << "\n";
	}

	const char* world_source_name()
	{
		switch (g_world_source)
		{
		case world_src::constant_c8:     return "g_World @ c8 (in-sync, exact)";
		case world_src::engine_ctx:      return "GfxContext+672 (exact, but can be stale)";
		case world_src::derived_inverse: return "derived via (View*Proj)^-1 (LOSSY - z-fights decals)";
		default:                         return "none (stage A fallback)";
		}
	}

	bool g_have_matrices()
	{
		std::lock_guard<std::mutex> lk(s_mtx_mat);
		return s_have_matrices;
	}

	void register_ps_diffuse_stage(IDirect3DPixelShader9* ps, int stage)
	{
		if (!ps) return;
		std::lock_guard<std::mutex> lk(s_mutex);
		// AddRef: D3D recycles addresses of released objects. The texture counters
		// proved it - 67 CreateTexture calls handed back a pointer we'd already
		// seen, with a DIFFERENT description. Without a reference, a released
		// pixel shader's address can be reused by a new shader and we'd hand back
		// the previous shader's diffuse stage => wrong texture on the mesh.
		// Holding a ref keeps the address uniquely ours.
		auto it = s_ps_diffuse.find(ps);
		if (it == s_ps_diffuse.end()) ps->AddRef();
		s_ps_diffuse[ps] = stage;
	}

	int get_ps_diffuse_stage(IDirect3DPixelShader9* ps)
	{
		if (!ps) return -1;
		std::lock_guard<std::mutex> lk(s_mutex);
		auto it = s_ps_diffuse.find(ps);
		return (it == s_ps_diffuse.end()) ? -1 : it->second;
	}

	void shutdown()
	{
		std::lock_guard<std::mutex> lk(s_mutex);
		for (auto& [src, sv] : s_shadow)
		{
			if (sv.vb) sv.vb->Release();
			src->Release();
		}
		s_shadow.clear();
		s_scale.clear();
	}

	// ---- skinned characters: CPU skin -> plain static geometry ---------------
	static bool try_render_skinned(IDirect3DDevice9* dev, const skin_info& si,
		D3DPRIMITIVETYPE PrimitiveType, INT BaseVertexIndex, UINT MinVertexIndex,
		UINT NumVertices, UINT startIndex, UINT primCount)
	{
		g_rej.skin_seen++;

		IDirect3DPixelShader9* cur_ps = nullptr;
		dev->GetPixelShader(&cur_ps);
		const int diffuse_stage = get_ps_diffuse_stage(cur_ps);
		if (cur_ps) cur_ps->Release();
		if (diffuse_stage < 0) { g_rej.skin_no_diffuse++; return false; }

		IDirect3DVertexBuffer9* src = nullptr;
		UINT soff = 0, stride = 0;
		if (FAILED(dev->GetStreamSource(0, &src, &soff, &stride)) || !src || !stride)
		{
			if (src) src->Release();
			g_rej.skin_no_stream++;
			return false;
		}

		D3DXMATRIX wvp_t, wvp, world_c8, view, proj;
		if (!read_vs_const(dev, 0, reinterpret_cast<float*>(&wvp_t), 4))
		{
			src->Release(); g_rej.skin_no_wvp++; return false;
		}
		D3DXMatrixTranspose(&wvp, &wvp_t);
		if (!compute_camera(dev, wvp, world_c8, view, proj))
		{
			src->Release(); g_rej.skin_no_camera++; return false;
		}

		const UINT first = static_cast<UINT>(BaseVertexIndex) + MinVertexIndex;
		const UINT bytes = NumVertices * sizeof(ff_vertex);

		IDirect3DVertexBuffer9* dyn = get_dynamic_vb(dev, bytes);
		if (!dyn) { src->Release(); g_rej.skin_no_dynvb++; return false; }

		void* dst = nullptr;
		if (FAILED(dyn->Lock(0, bytes, &dst, D3DLOCK_DISCARD)) || !dst)
		{
			src->Release(); g_rej.skin_lock_fail++; return false;
		}
		const UINT written = skin_to_buffer(dev, src, stride, si, first, NumVertices,
			static_cast<ff_vertex*>(dst));
		dyn->Unlock();
		if (!written) { src->Release(); g_rej.skin_lock_fail++; return false; }
		g_rej.skin_ok++;

		// Save state
		IDirect3DVertexShader9* old_vs = nullptr; IDirect3DPixelShader9* old_ps = nullptr;
		IDirect3DVertexDeclaration9* old_decl = nullptr;
		dev->GetVertexShader(&old_vs); dev->GetPixelShader(&old_ps); dev->GetVertexDeclaration(&old_decl);
		DWORD old_lighting = 0; dev->GetRenderState(D3DRS_LIGHTING, &old_lighting);
		IDirect3DBaseTexture9* old_tex0 = nullptr; dev->GetTexture(0, &old_tex0);
		IDirect3DBaseTexture9* diffuse = nullptr;

		dev->SetVertexShader(nullptr);
		dev->SetPixelShader(nullptr);
		dev->SetFVF(FF_FVF);
		dev->SetStreamSource(0, dyn, 0, sizeof(ff_vertex));

		// Skinning already applied the 16x scale and put the mesh in object space,
		// so World is just g_World - no extra scale matrix here.
		dev->SetTransform(D3DTS_WORLD, &world_c8);
		dev->SetTransform(D3DTS_VIEW, &view);
		dev->SetTransform(D3DTS_PROJECTION, &proj);
		dev->SetRenderState(D3DRS_LIGHTING, FALSE);

		if (diffuse_stage != 0)
		{
			dev->GetTexture(static_cast<DWORD>(diffuse_stage), &diffuse);
			dev->SetTexture(0, diffuse);
		}
		dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
		dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);

		// We wrote source vertex `first` to out[0]. The index buffer still yields
		// IB[j] in [MinVertexIndex, MinVertexIndex+NumVertices), so shift the base
		// by -MinVertexIndex to land on out[0..NumVertices).
		const HRESULT hr = dev->DrawIndexedPrimitive(PrimitiveType,
			-static_cast<INT>(MinVertexIndex), 0, NumVertices, startIndex, primCount);

		// Restore
		if (diffuse_stage != 0) { dev->SetTexture(0, old_tex0); if (diffuse) diffuse->Release(); }
		if (old_tex0) old_tex0->Release();
		dev->SetRenderState(D3DRS_LIGHTING, old_lighting);
		dev->SetStreamSource(0, src, soff, stride);
		if (old_decl) { dev->SetVertexDeclaration(old_decl); old_decl->Release(); }
		dev->SetVertexShader(old_vs); dev->SetPixelShader(old_ps);
		if (old_vs) old_vs->Release();
		if (old_ps) old_ps->Release();
		src->Release();

		if (SUCCEEDED(hr)) { g_skinned_draws++; g_rej.converted++; return true; }
		return false;
	}

	bool try_render_fixed_function(IDirect3DDevice9* dev,
		D3DPRIMITIVETYPE PrimitiveType, INT BaseVertexIndex, UINT MinVertexIndex,
		UINT NumVertices, UINT startIndex, UINT primCount)
	{
		if (!g_enabled || !dev || BaseVertexIndex < 0) return false;

		static std::once_flag s_hooks_once;
		std::call_once(s_hooks_once, install_matrix_hooks);

		g_rej.seen++;

		IDirect3DVertexDeclaration9* decl = nullptr;
		if (FAILED(dev->GetVertexDeclaration(&decl)) || !decl) { g_rej.no_decl++; return false; }

		D3DVERTEXELEMENT9 el[MAX_FVF_DECL_SIZE]{};
		UINT n = 0;
		if (FAILED(decl->GetDeclaration(el, &n)) || !n) { decl->Release(); g_rej.no_decl++; return false; }
		const decl_info di = classify(el, n);
		const skin_info si = classify_skinned(el, n);
		decl->Release();

		if (!di.ok)
		{
			// Skinned characters are 33% of all draws - the single biggest chunk
			// still going to vertex capture (and therefore still flickering).
			if (si.ok && g_skinning
				&& try_render_skinned(dev, si, PrimitiveType, BaseVertexIndex,
					MinVertexIndex, NumVertices, startIndex, primCount))
			{
				return true;
			}
			note_rejected(el, n);
			g_rej.not_static++;
			return false;
		}

		// Which stage holds the diffuse? s0 is NOT reliably the diffuse in AC2.
		// If the PS has no diffuse sampler at all it's a lighting/AO/projector
		// pass - leave it to the game rather than forcing its texture out opaque.
		IDirect3DPixelShader9* cur_ps = nullptr;
		dev->GetPixelShader(&cur_ps);
		const int diffuse_stage = get_ps_diffuse_stage(cur_ps);
		if (cur_ps) cur_ps->Release();
		if (diffuse_stage < 0) { g_rej.no_diffuse++; return false; }

		IDirect3DVertexBuffer9* src = nullptr;
		UINT soff = 0, stride = 0;
		if (FAILED(dev->GetStreamSource(0, &src, &soff, &stride)) || !src || !stride)
		{
			if (src) src->Release();
			g_rej.no_stream++;
			return false;
		}

		const UINT first = static_cast<UINT>(BaseVertexIndex) + MinVertexIndex;

		// FLOAT3 positions are already in model space - no |w| compression, so no
		// scale to recover or fold. SHORT4 needs the per-draw |w|/262136.
		float scale = 1.0f;
		if (!di.pos_float3 && !get_scale(src, stride, di.pos_off, first, scale))
		{
			src->Release(); g_rej.no_scale_or_vb++; return false;
		}

		// Dynamic source (cloth/softbody: re-simulated and re-uploaded every frame)
		// must NOT use the cached shadow VB - caching freezes it at frame one, so it
		// renders but never follows the character. Convert it fresh each draw.
		IDirect3DVertexBuffer9* vb_to_draw = nullptr;
		UINT draw_base = BaseVertexIndex;
		UINT draw_minv = MinVertexIndex;

		if (is_dynamic_vb(src))
		{
			const UINT bytes = NumVertices * sizeof(ff_vertex);
			IDirect3DVertexBuffer9* dyn = get_dynamic_vb(dev, bytes);
			if (!dyn) { src->Release(); g_rej.no_scale_or_vb++; return false; }

			void* dst = nullptr;
			if (FAILED(dyn->Lock(0, bytes, &dst, D3DLOCK_DISCARD)) || !dst)
			{
				src->Release(); g_rej.no_scale_or_vb++; return false;
			}
			const UINT w = convert_range(src, stride, di, first, NumVertices,
				static_cast<ff_vertex*>(dst));
			dyn->Unlock();
			if (!w) { src->Release(); g_rej.no_scale_or_vb++; return false; }

			// We compacted [first, first+NumVertices) to out[0..], so shift the base.
			vb_to_draw = dyn;
			draw_base = static_cast<UINT>(-static_cast<INT>(MinVertexIndex));
			draw_minv = 0;
		}
		else
		{
			shadow_vb* sv = get_or_build_shadow(dev, src, stride, di);
			if (!sv || !sv->vb) { src->Release(); g_rej.no_scale_or_vb++; return false; }
			vb_to_draw = sv->vb;
		}

		// ---- read back the MVP the game just uploaded --------------------
		// c0..c3 hold transpose(WorldViewProj) (the engine transposes on upload),
		// so transpose it back for D3D's row-vector SetTransform convention.
		D3DXMATRIX wvp_t{}, wvp{};
		if (!read_vs_const(dev, 0, reinterpret_cast<float*>(&wvp_t), 4))
		{
			src->Release();
			return false;
		}
		D3DXMatrixTranspose(&wvp, &wvp_t);

		// ---- build World / View / Projection -----------------------------
		// Everything here comes from the two IN-SYNC constants (c0 = WVP, c8 =
		// World) - they ride the command buffer with the draw. The engine's
		// View/Proj from GfxContext are deliberately NOT used: they are
		// pass-dependent and stale (observed as an orthographic shadow matrix at
		// 4x the wrong scale), and a per-draw-varying camera makes Remix flicker
		// even while the raster output stays perfect.
		//     VP_true = inv(World) * WVP     <- true View*Proj for THIS draw
		//     decompose VP_true -> View, Proj
		// World is affine, so its inverse is well conditioned.
		D3DXMATRIX world{}, view{}, proj{}, scale_m{};
		D3DXMatrixScaling(&scale_m, scale, scale, scale);
		{
			D3DXMATRIX w_t, world_c8;
			if (!read_vs_const(dev, 8, reinterpret_cast<float*>(&w_t), 4))
			{
				g_rej.no_c8++; src->Release(); return false;
			}
			{
				D3DXMatrixTranspose(&world_c8, &w_t);

				D3DXMATRIX w_inv, vp_true;
				float wdet = 0.0f;
				if (!D3DXMatrixInverse(&w_inv, &wdet, &world_c8) || fabsf(wdet) <= 1e-20f)
				{
					g_rej.world_singular++; src->Release(); return false;
				}
				{
					D3DXMatrixMultiply(&vp_true, &w_inv, &wvp);

					// Reuse the cached camera if this draw's VP matches the pass we
					// already decomposed - gives Remix a bit-stable camera instead of
					// one that wobbles a few ULPs per draw.
					D3DXMATRIX v_dec, p_dec;
					bool cached = false;
					{
						std::lock_guard<std::mutex> lk(s_cam_mtx);
						if (s_cam_valid && same_vp(vp_true, s_cam_vp))
						{
							v_dec = s_cam_view;
							p_dec = s_cam_proj;
							cached = true;
						}
					}

					if (!cached)
					{
						if (!decompose_vp(vp_true, v_dec, p_dec))
						{
							g_rej.ortho++; src->Release(); return false;
						}
						std::lock_guard<std::mutex> lk(s_cam_mtx);
						s_cam_vp = vp_true;
						s_cam_view = v_dec;
						s_cam_proj = p_dec;
						s_cam_valid = true;
					}
					else
					{
						// The cache traded accuracy for stability: reusing a camera
						// for a slightly different VP pushed max mvp error from
						// 3.5e-06 to ~1e-3 and started failing draws outright.
						// If the cached camera doesn't reproduce THIS draw's MVP,
						// decompose fresh rather than dropping the draw.
						D3DXMATRIX chk;
						D3DXMatrixMultiply(&chk, &v_dec, &p_dec);
						if (!same_vp(chk, vp_true))
						{
							if (!decompose_vp(vp_true, v_dec, p_dec))
							{
								g_rej.ortho++; src->Release(); return false;
							}
							std::lock_guard<std::mutex> lk(s_cam_mtx);
							s_cam_vp = vp_true;
							s_cam_view = v_dec;
							s_cam_proj = p_dec;
						}
					}
					{
						// Verify the split actually reproduces the MVP the game sent.
						D3DXMATRIX vp_chk, check;
						D3DXMatrixMultiply(&vp_chk, &v_dec, &p_dec);
						D3DXMatrixMultiply(&check, &world_c8, &vp_chk);
						float e = 0.0f, mag = 0.0f;
						for (int i = 0; i < 4; ++i)
							for (int j = 0; j < 4; ++j)
							{
								e = (std::max)(e, fabsf(check.m[i][j] - wvp.m[i][j]));
								mag = (std::max)(mag, fabsf(wvp.m[i][j]));
							}
						const float rel = (mag > 1e-6f) ? (e / mag) : e;

						if (rel < 1e-3f)
						{
							if (rel > g_max_wvp_error) g_max_wvp_error = rel;
							D3DXMatrixMultiply(&world, &scale_m, &world_c8);
							view = v_dec;
							proj = p_dec;
							g_world_source = world_src::constant_c8;

							if (s_diag_armed)
							{
								g_diag.wvp = wvp;
								g_diag.world_c8 = world_c8;
								g_diag.engine_view = v_dec;   // decomposed, not engine
								g_diag.engine_proj = p_dec;
								D3DXMatrixMultiply(&g_diag.vp_engine, &v_dec, &p_dec);
								g_diag.vp_from_data = vp_true;
								g_diag.err_c8 = rel;
								g_diag.err_engine = -1.0f;
								g_diag.err_vp = rel;
								g_diag.valid = true;
								s_diag_armed = false;
							}
							goto matrices_ready;
						}
					}
				}
			}
			// Decomposed, but W*V*P doesn't reproduce the MVP the game sent.
			g_rej.bad_reconstruct++;
			src->Release();
			return false;
		}

	matrices_ready:;

		// ---- save what we touch -----------------------------------------
		IDirect3DVertexShader9* old_vs = nullptr;
		IDirect3DPixelShader9* old_ps = nullptr;
		IDirect3DVertexDeclaration9* old_decl = nullptr;
		dev->GetVertexShader(&old_vs);
		dev->GetPixelShader(&old_ps);
		dev->GetVertexDeclaration(&old_decl);

		DWORD old_lighting = 0, old_fill = 0, old_bias = 0, old_sbias = 0;
		dev->GetRenderState(D3DRS_LIGHTING, &old_lighting);
		dev->GetRenderState(D3DRS_FILLMODE, &old_fill);
		dev->GetRenderState(D3DRS_DEPTHBIAS, &old_bias);
		dev->GetRenderState(D3DRS_SLOPESCALEDEPTHBIAS, &old_sbias);

		// ---- fixed function ---------------------------------------------
		dev->SetVertexShader(nullptr);
		dev->SetPixelShader(nullptr);
		dev->SetFVF(FF_FVF);
		dev->SetStreamSource(0, vb_to_draw, 0, sizeof(ff_vertex));

		dev->SetTransform(D3DTS_WORLD, &world);
		dev->SetTransform(D3DTS_VIEW, &view);
		dev->SetTransform(D3DTS_PROJECTION, &proj);

		dev->SetRenderState(D3DRS_LIGHTING, FALSE);
		if (g_wireframe) dev->SetRenderState(D3DRS_FILLMODE, D3DFILL_WIREFRAME);

		// Even with exact matrices, FF concatenates W*V*P itself while the game's
		// shader uses a pre-multiplied WVP - they differ by ~1 ULP, which is
		// enough to z-fight coplanar decals we don't convert. Nudging converted
		// geometry very slightly away lets the decals win consistently.
		if (g_depth_bias != 0.0f)
		{
			const float b = g_depth_bias;
			dev->SetRenderState(D3DRS_DEPTHBIAS, *reinterpret_cast<const DWORD*>(&b));
		}

		// Bind the real diffuse into stage 0 (it may live at any stage).
		IDirect3DBaseTexture9* diffuse = nullptr;
		IDirect3DBaseTexture9* old_tex0 = nullptr;
		dev->GetTexture(0, &old_tex0);
		if (diffuse_stage != 0)
		{
			dev->GetTexture(static_cast<DWORD>(diffuse_stage), &diffuse);
			dev->SetTexture(0, diffuse);
		}

		// unlit, straight texture
		dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
		dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);

		// Cached shadow VB mirrors the source 1:1, so BaseVertexIndex is preserved.
		// The dynamic path compacted the range to out[0..], so it shifts the base
		// by -MinVertexIndex instead (set above).
		const HRESULT hr = dev->DrawIndexedPrimitive(PrimitiveType,
			static_cast<INT>(draw_base), draw_minv, NumVertices, startIndex, primCount);

		// ---- restore ------------------------------------------------------
		if (diffuse_stage != 0)
		{
			dev->SetTexture(0, old_tex0);
			if (diffuse) diffuse->Release();
		}
		if (old_tex0) old_tex0->Release();
		dev->SetRenderState(D3DRS_LIGHTING, old_lighting);
		if (g_wireframe) dev->SetRenderState(D3DRS_FILLMODE, old_fill);
		if (g_depth_bias != 0.0f)
		{
			dev->SetRenderState(D3DRS_DEPTHBIAS, old_bias);
			dev->SetRenderState(D3DRS_SLOPESCALEDEPTHBIAS, old_sbias);
		}
		dev->SetStreamSource(0, src, soff, stride);
		if (old_decl) { dev->SetVertexDeclaration(old_decl); old_decl->Release(); }
		dev->SetVertexShader(old_vs);
		dev->SetPixelShader(old_ps);
		if (old_vs) old_vs->Release();
		if (old_ps) old_ps->Release();
		src->Release();

		if (SUCCEEDED(hr)) { g_converted_draws++; g_rej.converted++; return true; }
		return false;
	}
}
