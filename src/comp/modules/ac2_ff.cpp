#include "std_include.hpp"
#include "ac2_ff.hpp"

#include <cmath>

namespace comp::ac2_ff
{
	bool g_enabled = true;
	bool g_wireframe = false;
	bool g_ff_only = true;   // INSERT toggles OFF; default ON
	bool g_skinning = true;
	bool g_veg_trunks = true;
	bool g_veg_leaves = true;
	bool g_normal_only_materials = true;
	std::uint32_t g_normal_only_draws = 0;
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

	// ---- hardware OCCLUSION culling -> disabled. END, default OFF. ------------
	// A SECOND, independent culling system, separate from the frustum work above.
	// scimitar::OGLBaseRenderer::RenderElements (0x1202950) runs two passes:
	//   a6 == 1 : issue the occlusion queries (sub_12679F0 / sub_1267A20)
	//   a6 == 2 : consume the results and DROP the draw if too few pixels showed
	//
	//   1202a81  call sub_1267A60          ; GetVisiblePixels()
	//   1202a90  cmp  eax, dword_1DD1828   ; the threshold, = 25
	//   1202a96  jnb  short 1202ae1        ; >= 25 -> RENDER
	//                                      ; fall through -> CULLED
	//
	// The cull path skips the draw call at 0x1202c30 (sub_1270D70) entirely, so an
	// occluded object NEVER REACHES D3D9 and Remix cannot see it. rtx.antiCulling.*
	// is powerless here for the same reason it is powerless against frustum culling:
	// it can only extend the life of objects the game still submits.
	//
	// VERIFIED FROM THE DECOMPILATION, not from the mnemonic: `jnb` TAKEN means
	// render; FALL-THROUGH means culled. So the intuitive "NOP the branch" fix would
	// cull EVERYTHING - the same inversion that has bitten this project twice.
	//
	// Patch the THRESHOLD instead of the branch. The compare is unsigned, so a
	// threshold of 0 makes `eax < 0` impossible => the cull never fires. Two reasons
	// this is the better site:
	//   - It is a DATA write, not a code write. No torn-byte hazard in a hot function
	//     that job threads are inside - which is exactly what crashed the CullAABB
	//     prologue patch.
	//   - dword_1DD1828 has exactly ONE reader (this compare), so there are no
	//     side effects elsewhere.
	//
	// Blast radius is naturally small: the gate only applies to objects that opted in
	// via (*(a4+171) & 1), and GetVisiblePixels already returns 1000 ("assume
	// visible") when an object has no query, so the no-query fallback renders anyway.
	static const code_patch s_occlusion_patch[] = {
		{ 0x1dd1828, { 0x19, 0x00, 0x00, 0x00 }, { 0x00, 0x00, 0x00, 0x00 }, 4 },
	};
	bool g_occlusion_off = false;

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

	void set_occlusion_disabled(bool enable)
	{
		apply_patches(s_occlusion_patch, std::size(s_occlusion_patch), enable, g_occlusion_off);
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
		for (const auto& cp : s_cull_patches)     dump_one(os, cp, "DELETE  ");
		for (const auto& cp : s_cullaabb_patch)   dump_one(os, cp, "PAGEDOWN");
		for (const auto& cp : s_occlusion_patch)  dump_one(os, cp, "PAGEUP  ");
	}

	bool g_stage_b = true;
	float g_max_wvp_error = 0.0f;
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

	// Two independent arms: a stage==0 draw and a stage!=0 draw are both needed
	// before the pair means anything, and they arrive in whatever order the
	// frame happens to submit them.
	tex_diag g_tex_diag[2]{};
	static volatile bool s_tex_diag_armed[2] = { true, true };
	void arm_tex_diagnostic() { s_tex_diag_armed[0] = true; s_tex_diag_armed[1] = true; }

	// Cumulative across the run - deliberately NOT reset by arming, because the
	// whole point is a population big enough that no single draw sways it.
	diffuse_size_stats g_diffuse_sizes{};

	void dump_diffuse_sizes(std::ostream& os)
	{
		static const char* const BUCKET[diffuse_size_stats::NBUCKET] = {
			"<=8", "<=32", "<=64", "<=128", "<=256", "<=512", ">512" };
		static const char* const RANK[6] = {
			"0 name has 'diffuse' ", "1 'basetexture'      ", "2 'layer0'           ",
			"3 GUESS (unrecognised)", "4 NORMAL-ONLY (water)", "5 never registered   " };

		os << "\n---- diffuse level-0 WIDTH by matcher rank (all converted draws) ----\n";
		os << "  Rank 3 is a GUESS: 'lowest sampler not provably wrong'. If rank 0 is big\n";
		os << "  and rank 3 is tiny, we are binding detail/mask maps on the guessed ones.\n";
		os << "  rank                    total";
		for (int b = 0; b < diffuse_size_stats::NBUCKET; ++b) os << "  " << BUCKET[b];
		os << "\n";
		os << "  Rank 4 is water/volume fog: no albedo exists, so the bound texture is the\n";
		os << "  NORMAL MAP, present only to give Remix something to hash and categorise.\n";
		for (int r = 0; r < 6; ++r)
		{
			if (!g_diffuse_sizes.rank_total[r]) continue;
			os << "  " << RANK[r] << " " << g_diffuse_sizes.rank_total[r];
			for (int b = 0; b < diffuse_size_stats::NBUCKET; ++b)
			{
				const std::uint32_t n = g_diffuse_sizes.by_rank[r][b];
				const double pct = 100.0 * n / g_diffuse_sizes.rank_total[r];
				os << "   " << n << "(" << static_cast<int>(pct + 0.5) << "%)";
			}
			os << "\n";
		}
	}

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

		// SHORT2N TEXCOORD0 -> real UV.
		//
		// D3D normalizes SHORT2N to short/32767, and AC2's vertex shaders then
		// scale that by a literal 16 before writing the UV output. Verified in the
		// disassembly for BOTH paths we convert:
		//   static  (VertexShader_..._0x0B6C41CD70642B52, SHORT4 position):
		//       def c5, 16, 0, 0, 0
		//       mul o2, c5.xxyy, v1.xyxx      ; uv = 16 * v1.xy
		//   skinned (VertexShader_Characters-Skin_0x05101CD460392E08_...):
		//       def c5, 3, 16, 0, 1          ; x = bone stride, y = 16
		//       mul o1, c5.yyzz, v3.xyxx      ; uv = 16 * v3.xy
		// Across the assets, every SHORT4-static VS that samples a diffuse uses
		// 16 (889 of them) and NONE uses 1 - the scale-1 shaders are FLOAT2-UV
		// variants, which take the uv_float2 path below and must NOT be scaled.
		//
		// Net: uv = short * 16/32767, i.e. the "2048 quantization scale"
		// (32768/16 = 2048). We previously stopped at short/32767, making every
		// UV 16x too small: the texture was magnified 16x, which looks SOFT and
		// was long mistaken for a mip/LOD clamp. It never was - sampler state and
		// GetLOD were measured clean.
		//
		// The 16 is a literal in the shader, not an uploaded constant, so there is
		// nothing per-draw to read back; it is genuinely a constant of the format.
		constexpr float UV_SHORT2N_SCALE = 16.0f / 32767.0f;
		static_assert(sizeof(ff_vertex) == 32, "ff_vertex must be 32 bytes");

		struct shadow_vb
		{
			IDirect3DVertexBuffer9* vb = nullptr;
			UINT vertex_count = 0;

			// ---- staleness / mismatch detection (diagnostic) ------------------
			// The cache is keyed ONLY on the source VB pointer, but the conversion
			// also depends on the STRIDE and the DECLARATION, and on the source
			// CONTENTS at build time. Two ways that can silently serve wrong
			// geometry, both of which look like "faces randomly disappear":
			//   1. same VB drawn with a different decl/stride -> we hand back a
			//      copy decoded with the wrong layout
			//   2. the game re-fills a MANAGED VB (streaming) -> our copy is frozen
			//      at the old contents. is_dynamic_vb() only bypasses DYNAMIC /
			//      D3DPOOL_DEFAULT, so a re-filled MANAGED buffer slips through.
			//      This is the cloth "static VB" lesson wearing a second costume.
			UINT stride = 0;
			std::uint64_t decl_fp = 0;   // fingerprint of the decl it was built with
			std::uint64_t src_hash = 0;  // checksum of the source bytes at build time
			UINT src_bytes = 0;
		};

		// Cheap order-dependent checksum. Not cryptographic - it only has to notice
		// that a buffer's contents changed.
		std::uint64_t hash_bytes(const void* p, std::size_t n)
		{
			const auto* b = static_cast<const std::uint8_t*>(p);
			std::uint64_t h = 1469598103934665603ull;          // FNV-1a
			for (std::size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 1099511628211ull; }
			return h;
		}

		// How much of the source we checksum. Full-buffer hashing every hit would
		// cost more than the conversion it guards; the head of the buffer is enough
		// to notice a re-fill.
		constexpr UINT SRC_HASH_BYTES = 256;

		struct cache_diag
		{
			std::uint32_t hits = 0;
			std::uint32_t decl_mismatch = 0;   // >0 => key is missing decl/stride
			std::uint32_t stale_checked = 0;
			std::uint32_t stale_found = 0;     // >0 => source was re-filled under us
		};
		cache_diag g_cache_diag{};

		std::mutex s_mutex;
		// keyed by source VB pointer (we AddRef the source to keep the key valid)
		std::map<IDirect3DVertexBuffer9*, shadow_vb> s_shadow;
		// per-draw |w|, keyed by (source VB, first vertex)
		std::map<std::pair<IDirect3DVertexBuffer9*, UINT>, float> s_scale;
		// pixel shader -> diffuse texture stage (-1 = none identified)
		std::map<IDirect3DPixelShader9*, ps_diffuse_info> s_ps_diffuse;
		// vertex shader -> family + bytecode hash + per-shader draw accounting
		// (AddRef'd, like s_ps_diffuse: D3D recycles the addresses of released
		// objects)
		struct vs_info
		{
			veg_kind      kind = veg_kind::none;
			std::uint32_t hash = 0;
			std::uint32_t seen = 0;
			std::uint32_t converted = 0;
		};
		std::map<IDirect3DVertexShader9*, vs_info> s_vs_veg;

		// AC2 only ever uses stream 0 and 1, but bound the array anyway - classify()
		// rejects a declaration that reaches past this so the rest of the code can
		// index the stream arrays without re-checking.
		constexpr UINT MAX_STREAMS = 4;

		// The locked source data for each stream a declaration actually uses.
		// Attributes may live on different streams, so a vertex is GATHERED across
		// streams rather than read from one flat record.
		struct vtx_streams
		{
			const std::uint8_t* base[MAX_STREAMS]{};   // locked VB bytes + the stream's own offset
			UINT                stride[MAX_STREAMS]{};
		};

		struct decl_info
		{
			bool  ok = false;
			UINT  pos_stream = 0;
			UINT  pos_off = 0;
			bool  pos_float3 = false;  // FLOAT3 = uncompressed, no |w| scale
			UINT  nrm_stream = 0;  UINT nrm_off = 0;  bool has_nrm = false;  bool nrm_float3 = false;
			UINT  uv_stream = 0;   UINT uv_off = 0;   bool has_uv = false;   bool uv_float2 = false;

			// True when every attribute we use lives on stream 0. That's the case the
			// cached shadow VB can serve (one source VB -> one converted VB, 1:1 by
			// vertex index). Anything else has to be converted per-draw.
			bool single_stream0 = false;
		};

		// Fingerprint the fields the conversion actually reads. If any differ from
		// what a cached copy was decoded under, that copy is wrong for this draw.
		std::uint64_t decl_fingerprint(const decl_info& d, UINT stride)
		{
			const std::uint32_t a[] = {
				stride, d.pos_stream, d.pos_off, d.pos_float3,
				d.nrm_stream, d.nrm_off, d.has_nrm, d.nrm_float3,
				d.uv_stream, d.uv_off, d.has_uv, d.uv_float2, d.single_stream0
			};
			return hash_bytes(a, sizeof(a));
		}

		// Find the generic static format on WHATEVER STREAM it lives on.
		//
		// This used to hard-filter `if (e[i].Stream != 0) continue`, so a mesh with
		// POSITION on stream 1 was rejected as "not_static" and never converted -
		// which in FF-ONLY mode means DELETED FROM THE SCENE. That reads as a culling
		// bug (objects missing) but is really a coverage bug, and it is why the
		// rejected-format histogram appeared to show position-less declarations: the
		// histogram was filtered to stream 0 too, so it printed the same blind spot
		// that caused the rejection. Every runtime-dumped VS declares dcl_position,
		// so POSITION is always SOMEWHERE in the declaration - go find it.
		//
		// Skinned meshes (BLENDINDICES/BLENDWEIGHT) are still excluded here; they are
		// handled by the CPU-skinning path in try_render_skinned().
		decl_info classify(const D3DVERTEXELEMENT9* e, UINT n)
		{
			decl_info d;
			bool skinned = false;
			for (UINT i = 0; i < n && e[i].Stream != 0xFF; ++i)
			{
				if (e[i].Usage == D3DDECLUSAGE_BLENDINDICES
					|| e[i].Usage == D3DDECLUSAGE_BLENDWEIGHT) skinned = true;

				switch (e[i].Usage)
				{
				case D3DDECLUSAGE_POSITION:
					// SHORT4 = compressed (|w|/262136 folded into World).
					// FLOAT3 = uncompressed, used as-is.
					// Only UsageIndex 0 is the real position.
					if (e[i].UsageIndex != 0) break;
					if (e[i].Type == D3DDECLTYPE_SHORT4)
					{
						d.pos_stream = e[i].Stream; d.pos_off = e[i].Offset; d.ok = true; d.pos_float3 = false;
					}
					else if (e[i].Type == D3DDECLTYPE_FLOAT3)
					{
						d.pos_stream = e[i].Stream; d.pos_off = e[i].Offset; d.ok = true; d.pos_float3 = true;
					}
					break;
				case D3DDECLUSAGE_NORMAL:
					if (e[i].UsageIndex != 0) break;
					if (e[i].Type == D3DDECLTYPE_UBYTE4)
					{
						d.nrm_stream = e[i].Stream; d.nrm_off = e[i].Offset; d.has_nrm = true; d.nrm_float3 = false;
					}
					else if (e[i].Type == D3DDECLTYPE_FLOAT3)
					{
						d.nrm_stream = e[i].Stream; d.nrm_off = e[i].Offset; d.has_nrm = true; d.nrm_float3 = true;
					}
					break;
				case D3DDECLUSAGE_TEXCOORD:
					if (e[i].UsageIndex != 0) break;
					if (e[i].Type == D3DDECLTYPE_SHORT2N)
					{
						d.uv_stream = e[i].Stream; d.uv_off = e[i].Offset; d.has_uv = true; d.uv_float2 = false;
					}
					else if (e[i].Type == D3DDECLTYPE_FLOAT2)
					{
						d.uv_stream = e[i].Stream; d.uv_off = e[i].Offset; d.has_uv = true; d.uv_float2 = true;
					}
					break;
				default: break;
				}
			}
			if (skinned) d.ok = false;

			// with_streams()/decode_vertex() index the stream arrays unchecked, so a
			// declaration reaching past MAX_STREAMS must be refused here, not there.
			if (d.pos_stream >= MAX_STREAMS
				|| (d.has_nrm && d.nrm_stream >= MAX_STREAMS)
				|| (d.has_uv && d.uv_stream >= MAX_STREAMS)) d.ok = false;

			d.single_stream0 = d.ok
				&& d.pos_stream == 0
				&& (!d.has_nrm || d.nrm_stream == 0)
				&& (!d.has_uv || d.uv_stream == 0);
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
		// Used for dynamic sources (caching would freeze them) and for multi-stream
		// declarations (the shadow cache is keyed on a single source VB).
		UINT convert_range(const vtx_streams& s, const decl_info& di,
			UINT first, UINT count, ff_vertex* out);
		void decode_vertex(const vtx_streams& s, UINT v, const decl_info& di, ff_vertex& o);

		// Build (once) a float FVF copy of the whole source buffer.
		shadow_vb* get_or_build_shadow(IDirect3DDevice9* dev, IDirect3DVertexBuffer9* src,
			UINT stride, const decl_info& di)
		{
			std::lock_guard<std::mutex> lk(s_mutex);
			auto it = s_shadow.find(src);
			if (it != s_shadow.end())
			{
				// DIAGNOSTIC ONLY - deliberately still returns the cached copy, so
				// this build measures the bug without also changing what renders.
				// Fixing and measuring in the same build confounds both.
				g_cache_diag.hits++;
				if (it->second.decl_fp != decl_fingerprint(di, stride))
					g_cache_diag.decl_mismatch++;

				// Re-read the head of the source and compare. Sampled: locking the
				// source on every hit would cost more than the cache saves.
				if ((g_cache_diag.hits & 0x1FF) == 0 && it->second.src_bytes)
				{
					void* sd = nullptr;
					if (SUCCEEDED(src->Lock(0, it->second.src_bytes, &sd, D3DLOCK_READONLY)) && sd)
					{
						const std::uint64_t now = hash_bytes(sd, it->second.src_bytes);
						src->Unlock();
						g_cache_diag.stale_checked++;
						if (now != it->second.src_hash) g_cache_diag.stale_found++;
					}
				}
				return &it->second;
			}

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

			// Only reached for single_stream0 declarations, so every attribute reads
			// out of this one buffer. Build a vtx_streams that says exactly that and
			// reuse the shared decoder rather than keeping a second copy of the
			// conversion rules that can drift out of sync with it.
			vtx_streams s{};
			s.base[0] = static_cast<const std::uint8_t*>(sdata);
			s.stride[0] = stride;

			auto* out = static_cast<ff_vertex*>(ddata);
			for (UINT i = 0; i < count; ++i)
				decode_vertex(s, i, di, out[i]);

			// Checksum the source while it is still LOCKED - sdata is invalid after
			// Unlock, and hashing it afterwards would read freed/remapped memory.
			shadow_vb sv; sv.vb = dst; sv.vertex_count = count;
			sv.stride = stride;
			sv.decl_fp = decl_fingerprint(di, stride);
			sv.src_bytes = (vd.Size < SRC_HASH_BYTES) ? vd.Size : SRC_HASH_BYTES;
			sv.src_hash = hash_bytes(sdata, sv.src_bytes);

			dst->Unlock();
			src->Unlock();

			src->AddRef(); // keep the key pointer alive
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

		// Which threads actually reach the dynamic pool? The pool is unlocked on a
		// D3DCREATE_MULTITHREADED device, which reads as an obvious race - but AC2
		// records on worker threads and PLAYS BACK on one render thread, and our
		// hook sits on playback. If only one thread ever appears here, the race is
		// impossible and a lock would be pure cost on a ~900k-draw path. Measure
		// before locking.
		std::uint32_t g_dyn_threads[4]{};
		std::uint32_t g_dyn_thread_count = 0;
		std::uint32_t g_dyn_thread_overflow = 0;

		void note_dyn_thread()
		{
			const std::uint32_t id = GetCurrentThreadId();
			for (std::uint32_t i = 0; i < g_dyn_thread_count; ++i)
				if (g_dyn_threads[i] == id) return;
			if (g_dyn_thread_count < 4) g_dyn_threads[g_dyn_thread_count++] = id;
			else g_dyn_thread_overflow++;
		}

		IDirect3DVertexBuffer9* get_dynamic_vb(IDirect3DDevice9* dev, UINT bytes)
		{
			note_dyn_thread();

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

		// Log EVERY stream, not just stream 0.
		//
		// This used to filter to `Stream != 0` - the same blind spot classify() had.
		// A mesh whose POSITION lives on stream 1 was rejected by classify() (which
		// never looked past stream 0) and then printed here MINUS its position
		// element, so the report showed it as an all-TEXCOORD declaration with no
		// position at all. That is a measurement artefact, and it sent us hunting for
		// an exotic "RealTree vertex format" that may not exist: every one of the 184
		// vertex shaders dumped at runtime declares dcl_position, so D3D *must* be
		// feeding POSITION from somewhere in the declaration.
		void note_rejected(const D3DVERTEXELEMENT9* e, UINT n)
		{
			std::string s;
			for (UINT i = 0; i < n && e[i].Stream != 0xFF; ++i)
			{
				s += "[s" + std::to_string(e[i].Stream)
					+ " +" + std::to_string(e[i].Offset) + " " + dtype_name(e[i].Type)
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

		// Shared decode: source vertex `v` -> one ff_vertex. Each attribute is fetched
		// from its own stream, so a declaration that splits POSITION and TEXCOORD
		// across two vertex buffers decodes correctly.
		void decode_vertex(const vtx_streams& s, UINT v, const decl_info& di, ff_vertex& o)
		{
			auto at = [&](UINT stream, UINT off) {
				return s.base[stream] + static_cast<std::size_t>(s.stride[stream]) * v + off;
			};

			const std::uint8_t* pp = at(di.pos_stream, di.pos_off);
			if (di.pos_float3)
			{
				float f[3]; memcpy(f, pp, sizeof(f));
				o.x = f[0]; o.y = f[1]; o.z = f[2];
			}
			else
			{
				std::int16_t ps[4]; memcpy(ps, pp, sizeof(ps));
				o.x = static_cast<float>(ps[0]);
				o.y = static_cast<float>(ps[1]);
				o.z = static_cast<float>(ps[2]);
			}

			if (di.has_nrm && di.nrm_float3)
			{
				float f[3]; memcpy(f, at(di.nrm_stream, di.nrm_off), sizeof(f));
				o.nx = f[0]; o.ny = f[1]; o.nz = f[2];
			}
			else if (di.has_nrm)
			{
				const std::uint8_t* nb = at(di.nrm_stream, di.nrm_off);
				o.nx = (nb[0] - 127.0f) * (1.0f / 127.0f);
				o.ny = (nb[1] - 127.0f) * (1.0f / 127.0f);
				o.nz = (nb[2] - 127.0f) * (1.0f / 127.0f);
			}
			else { o.nx = 0.0f; o.ny = 1.0f; o.nz = 0.0f; }

			if (di.has_uv && di.uv_float2)
			{
				float f[2]; memcpy(f, at(di.uv_stream, di.uv_off), sizeof(f));
				o.u = f[0]; o.v = f[1];
			}
			else if (di.has_uv)
			{
				std::int16_t t[2]; memcpy(t, at(di.uv_stream, di.uv_off), sizeof(t));
				o.u = t[0] * UV_SHORT2N_SCALE;
				o.v = t[1] * UV_SHORT2N_SCALE;
			}
			else { o.u = 0.0f; o.v = 0.0f; }
		}

		UINT convert_range(const vtx_streams& s, const decl_info& di,
			UINT first, UINT count, ff_vertex* out)
		{
			for (UINT v = 0; v < count; ++v)
				decode_vertex(s, first + v, di, out[v]);
			return count;
		}

		// Lock every stream a declaration uses, run `body`, then unlock exactly what
		// was locked. Streams are gathered by index, so two attributes sharing one
		// stream lock that buffer once - double-locking the same VB is legal but
		// double-unlocking is not.
		//
		// Returns false if any needed stream is missing or unlockable, in which case
		// nothing stays locked.
		// Lock exactly the streams in `need`, gather them, run `body`, unlock.
		// Takes the stream set rather than a decl_info because the vegetation path
		// reads FOUR attributes (TEXCOORD1/2/4/5) and decl_info only describes
		// three. Locking a stream twice (e.g. by calling this once per descriptor)
		// would be both redundant and fragile - the set has to be unioned first.
		template <typename F>
		bool with_stream_mask(IDirect3DDevice9* dev, const bool (&need)[MAX_STREAMS], F&& body)
		{
			IDirect3DVertexBuffer9* vb[MAX_STREAMS]{};
			bool locked[MAX_STREAMS]{};
			vtx_streams s{};
			bool ok = true;

			for (UINT i = 0; i < MAX_STREAMS && ok; ++i)
			{
				if (!need[i]) continue;
				UINT soff = 0, stride = 0;
				if (FAILED(dev->GetStreamSource(i, &vb[i], &soff, &stride)) || !vb[i] || !stride)
				{
					ok = false; break;
				}
				void* data = nullptr;
				if (FAILED(vb[i]->Lock(0, 0, &data, D3DLOCK_READONLY)) || !data)
				{
					ok = false; break;
				}
				locked[i] = true;
				s.base[i] = static_cast<const std::uint8_t*>(data) + soff;
				s.stride[i] = stride;
			}

			if (ok) ok = body(s);

			for (UINT i = 0; i < MAX_STREAMS; ++i)
			{
				if (locked[i]) vb[i]->Unlock();
				if (vb[i]) vb[i]->Release();
			}
			return ok;
		}

		template <typename F>
		bool with_streams(IDirect3DDevice9* dev, const decl_info& di, F&& body)
		{
			bool need[MAX_STREAMS]{};
			need[di.pos_stream] = true;
			if (di.has_nrm) need[di.nrm_stream] = true;
			if (di.has_uv)  need[di.uv_stream] = true;
			return with_stream_mask(dev, need, std::forward<F>(body));
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
				o.u = t[0] * UV_SHORT2N_SCALE;
				o.v = t[1] * UV_SHORT2N_SCALE;
			}
			else { o.u = 0.0f; o.v = 0.0f; }
		}

		s_skin_diag_capturing = false;   // this draw is done
		src->Unlock();
		return count;
	}

	// Defined here rather than beside the other dumps: it reads globals that live
	// in the anonymous namespace further down this file.
	void dump_cache_diag(std::ostream& os)
	{
		os << "\n---- shadow-VB cache integrity (disappearing geometry) ----\n";
		os << "  The cache is keyed ONLY on the source VB pointer, but the conversion\n";
		os << "  also depends on stride+declaration and on the source CONTENTS.\n";
		os << "    decl_mismatch > 0 => one VB is drawn with >1 layout; the key is wrong\n";
		os << "    STALE > 0         => the game re-fills a MANAGED VB (streaming) and our\n";
		os << "                         copy is frozen at the old contents\n";
		os << "    both 0            => the cache is sound; look elsewhere\n";
		os << "  cache hits         : " << g_cache_diag.hits << "\n";
		os << "  decl_mismatch      : " << g_cache_diag.decl_mismatch << "\n";
		os << "  staleness checked  : " << g_cache_diag.stale_checked << "  (sampled 1-in-512)\n";
		os << "  STALE (src changed): " << g_cache_diag.stale_found << "\n";

		os << "\n  dynamic-VB pool threads : " << g_dyn_thread_count;
		for (std::uint32_t i = 0; i < g_dyn_thread_count; ++i) os << "  " << g_dyn_threads[i];
		if (g_dyn_thread_overflow) os << "  (+" << g_dyn_thread_overflow << " more)";
		os << "\n";
		os << "    1 thread  => the unlocked pool CANNOT race; a lock would be pure cost\n";
		os << "    >1 thread => the pool race is real; lock it\n";
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

	void register_ps_diffuse_stage(IDirect3DPixelShader9* ps, int stage, int rank,
		const char* name, bool normal_only)
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

		ps_diffuse_info info{};
		info.stage = stage;
		info.rank = rank;
		info.normal_only = normal_only;
		if (name)
		{
			std::strncpy(info.name, name, sizeof(info.name) - 1);
			info.name[sizeof(info.name) - 1] = '\0';
		}
		s_ps_diffuse[ps] = info;
	}

	int get_ps_diffuse_stage(IDirect3DPixelShader9* ps)
	{
		if (!ps) return -1;
		std::lock_guard<std::mutex> lk(s_mutex);
		auto it = s_ps_diffuse.find(ps);
		return (it == s_ps_diffuse.end()) ? -1 : it->second.stage;
	}

	ps_diffuse_info get_ps_diffuse_info(IDirect3DPixelShader9* ps)
	{
		ps_diffuse_info miss{};
		if (!ps) return miss;
		std::lock_guard<std::mutex> lk(s_mutex);
		auto it = s_ps_diffuse.find(ps);
		return (it == s_ps_diffuse.end()) ? miss : it->second;
	}

	void dump_normal_only(std::ostream& os)
	{
		os << "\n---- NORMAL-ONLY materials (water / volume fog) ----\n";
		os << "  conversion         : " << (g_normal_only_materials ? "ON" : "off") << "   (F2)\n";
		os << "  draws converted    : " << g_normal_only_draws << "\n";
		os << "  These have NO diffuse sampler at any stage - only a normal map (+depth,\n";
		os << "  reflection). They were previously refused as `no_diffuse`, so FF-ONLY mode\n";
		os << "  deleted the water from the scene. The texture we bind is the NORMAL MAP,\n";
		os << "  purely as an identity for Remix to hash: tag that hash in rtx.conf\n";
		os << "  (rtx.animatedWaterTextures + rtx.opaqueMaterial.layeredWaterNormalEnable\n";
		os << "  for water). Volume fog shares the identical sampler signature and is NOT\n";
		os << "  separated here on purpose - its normal map is a different texture, so it\n";
		os << "  gets a different hash and can be tagged independently.\n";
	}

	void dump_veg(std::ostream& os)
	{
		os << "\n---- VEGETATION (RealTree: no dcl_position, VS-generated) ----\n";
		os << "  trunk conversion   : " << (g_veg_trunks ? "ON" : "off")
			<< "    leaf conversion: " << (g_veg_leaves ? "ON" : "off") << "\n";
		os << "  draws seen by family:\n";
		os << "    trunk   : " << g_veg.seen[static_cast<int>(veg_kind::trunk)] << "\n";
		os << "    leaves  : " << g_veg.seen[static_cast<int>(veg_kind::leaves)] << "\n";
		os << "    clutter : " << g_veg.seen[static_cast<int>(veg_kind::clutter)]
			<< "   (grass; expected 0 - no runtime decl carries TEXCOORD9)\n";
		os << "  trunk CONVERTED    : " << g_veg.trunk_converted << "\n";
		os << "  trunk rejects:\n";
		os << "    no_decl    : " << g_veg.trunk_no_decl
			<< "   (missing TEXCOORD4/5/2/1 of the expected type)\n";
		os << "    no_const   : " << g_veg.trunk_no_const
			<< "   (TrunkStencil/CompressionParams never uploaded)\n";
		os << "    no_diffuse : " << g_veg.trunk_no_diffuse << "   (depth/shadow pass - correct)\n";
		os << "    no_vb      : " << g_veg.trunk_no_vb << "\n";
		os << "    no_camera  : " << g_veg.trunk_no_camera << "   (ortho / decompose failed)\n";
		os << "  stencil ring clamped: " << g_veg.stencil_clamped
			<< "   (>0 means the ring index left [0,56) - trunk shape is deformed)\n";
		os << "  leaf CONVERTED     : " << g_veg.leaf_converted << "\n";
		os << "  leaf rejects:\n";
		os << "    no_decl    : " << g_veg.leaf_no_decl
			<< "   (missing TEXCOORD0/1/2/4 of the expected type)\n";
		os << "    no_const   : " << g_veg.leaf_no_const
			<< "   (LeavesEquations/oscillators never uploaded)\n";
		os << "    no_diffuse : " << g_veg.leaf_no_diffuse << "   (depth/shadow pass - correct)\n";
		os << "    no_vb      : " << g_veg.leaf_no_vb << "\n";
		os << "    no_camera  : " << g_veg.leaf_no_camera << "   (ortho / decompose failed)\n";
		os << "  leaf eq clamped    : " << g_veg.leaf_eq_clamped
			<< "   (>0 means the equation index left [0,6) - leaf size is wrong)\n";
	}

	void register_vs_info(IDirect3DVertexShader9* vs, veg_kind kind, std::uint32_t hash)
	{
		if (!vs) return;
		std::lock_guard<std::mutex> lk(s_mutex);
		// AddRef for the same reason s_ps_diffuse does: a released shader's address
		// gets handed back to a later CreateVertexShader, and we would then answer
		// for the wrong shader. Holding a ref keeps the address uniquely ours.
		auto it = s_vs_veg.find(vs);
		if (it == s_vs_veg.end()) vs->AddRef();
		// Keep any counts already accumulated against this object: the game can
		// re-create from identical bytecode, and zeroing here would quietly reset
		// the very coverage we are trying to measure.
		vs_info& info = s_vs_veg[vs];
		info.kind = kind;
		info.hash = hash;
	}

	// Every draw's outcome, attributed to the shader that issued it.
	static void note_vs_draw(IDirect3DVertexShader9* vs, bool converted)
	{
		if (!vs) return;
		std::lock_guard<std::mutex> lk(s_mutex);
		auto it = s_vs_veg.find(vs);
		if (it == s_vs_veg.end()) return;   // created before our hook: nothing to name it
		it->second.seen++;
		if (converted) it->second.converted++;
	}

	void dump_vs_coverage(std::ostream& os)
	{
		struct row { std::uint32_t hash = 0, seen = 0, conv = 0; veg_kind kind = veg_kind::none; };
		std::vector<row> rows;
		{
			std::lock_guard<std::mutex> lk(s_mutex);
			for (const auto& kv : s_vs_veg)
			{
				if (!kv.second.seen) continue;
				rows.push_back({ kv.second.hash, kv.second.seen, kv.second.converted, kv.second.kind });
			}
		}
		// Merge by BYTECODE hash: many shader objects share one bytecode, and the
		// template is a property of the bytecode, not of the object.
		std::map<std::uint32_t, row> byhash;
		for (const auto& r : rows)
		{
			auto& d = byhash[r.hash];
			d.hash = r.hash;
			if (r.kind != veg_kind::none) d.kind = r.kind;
			d.seen += r.seen; d.conv += r.conv;
		}
		std::vector<row> out;
		out.reserve(byhash.size());
		for (const auto& kv : byhash) out.push_back(kv.second);
		// Most-dropped first: that IS the ranked list of what is still missing.
		std::sort(out.begin(), out.end(), [](const row& a, const row& b)
		{
			return (a.seen - a.conv) > (b.seen - b.conv);
		});

		os << "\n---- PER-SHADER COVERAGE (what is still missing?) ----\n";
		os << "  The totals cannot answer this: no_diffuse is ~45% of draws and is MOSTLY\n";
		os << "  CORRECT (depth/shadow/AO). Water hid in that counter for the whole project.\n";
		os << "  A shader with many draws and 0% converted is the signature of a gap.\n";
		os << "  Name these offline:  python tools/name_live_shaders.py <game-dir> <assets-dir>\n";
		os << "  hash        seen        converted   pct   dropped     family\n";
		int n = 0;
		for (const auto& r : out)
		{
			if (++n > 40) break;
			const int pct = r.seen ? static_cast<int>(100.0 * r.conv / r.seen + 0.5) : 0;
			const char* fam = (r.kind == veg_kind::trunk) ? "trunk"
				: (r.kind == veg_kind::leaves) ? "leaves"
				: (r.kind == veg_kind::clutter) ? "clutter" : "-";
			os << "  0x" << std::hex << std::setw(8) << std::setfill('0') << r.hash
				<< std::dec << std::setfill(' ')
				<< "  " << std::setw(10) << r.seen
				<< "  " << std::setw(10) << r.conv
				<< "  " << std::setw(3) << pct << "%"
				<< "  " << std::setw(10) << (r.seen - r.conv)
				<< "  " << fam << "\n";
		}
	}

	veg_kind get_vs_veg(IDirect3DVertexShader9* vs)
	{
		if (!vs) return veg_kind::none;
		std::lock_guard<std::mutex> lk(s_mutex);
		auto it = s_vs_veg.find(vs);
		return (it == s_vs_veg.end()) ? veg_kind::none : it->second.kind;
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

	// =========================================================================
	// Vegetation: the trunk family (TrunkStencil)
	// =========================================================================
	// A CPU port of the RealTree trunk vertex path. The mesh in the VB is not
	// geometry at all - it is a SKELETON: TEXCOORD4 gives a compressed spine
	// point plus a radius in .w, and the shader sweeps a ring around it using a
	// per-ring cross-section from TrunkStencil[] (c150, 56 regs):
	//
	//   base   = TC4.xyz * CompressionParams.y + CompressionParams.x
	//   radius = TC4.w   * CompressionParams.z + 1e-6
	//   ring   = trunc(dot(TC1.xyz, OffsetInStencil.xyz))
	//   t1     = cross(TC5, TC2)      ; TC5 = spine axis
	//   t2     = cross(TC5, t1)
	//   dir    = TrunkStencil[ring].x * t1 + TrunkStencil[ring].y * t2
	//   pos    = base + radius * dir
	//   normal = normalize(radius * dir)
	//
	// Output is MODEL space (the shader does `dp4 o0, r0, c0` = WVP * pos), so
	// the existing World/View/Proj derivation applies unchanged and there is no
	// |w| scale to recover - CompressionParams does that job here.
	//
	//   uv     = TEXCOORD0.xy * TrunkUVDecompression.y + TrunkUVDecompression.x
	//
	// > The live shader is the authority, NOT the asset corpus. An earlier version
	// > of this port read ONE trunk shader out of the 47 in the assets (picked by
	// > `grep -l TrunkStencil | head -1`) and generalised it. That variant is one of
	// > only 6 that build a PROCEDURAL world-space UV and one of the few that apply
	// > a VFalloff/VDistScale distance fade. All FOUR trunk shaders the game
	// > actually creates do neither: they decompress a real TEXCOORD0 through
	// > TrunkUVDecompression (c211) and have no fade at all. The result was broken
	// > UVs plus a jitter - the fade read c100/c101, which no live trunk shader
	// > declares, so it was differencing ALIASED leftovers from other shaders
	// > against the eye position and moving every vertex a little each frame.
	// > Cross-check a runtime dump (`<game>\ac2_rtx_dump\shaders`) before believing
	// > any variant-specific detail; 47 assets can outvote the 4 that run.
	veg_stats g_veg{};

	namespace
	{
		constexpr int TRUNK_STENCIL_REG = 150;
		constexpr int TRUNK_STENCIL_COUNT = 56;

		struct veg_decl
		{
			bool ok = false;
			UINT base_stream = 0, base_off = 0;   // TEXCOORD4 SHORT4N: spine + radius
			UINT axis_stream = 0, axis_off = 0;   // TEXCOORD5 SHORT4N: spine axis
			UINT ref_stream = 0, ref_off = 0;     // TEXCOORD2 D3DCOLOR: frame reference
			UINT sten_stream = 0, sten_off = 0;   // TEXCOORD1 FLOAT4: ring index + LOD
			UINT uv_stream = 0, uv_off = 0;       // TEXCOORD0 SHORT4: compressed UV
			bool have_axis = false, have_ref = false, have_sten = false, have_uv = false;
		};

		// Select by SEMANTIC, never by register: the base position is TEXCOORD4 on
		// trunk/leaves but TEXCOORD9 on clutter, and the register carrying it moves
		// (v3/v4/v6/v8) between variants of the same family.
		veg_decl classify_veg(const D3DVERTEXELEMENT9* e, UINT n)
		{
			veg_decl d;
			for (UINT i = 0; i < n && e[i].Stream != 0xFF; ++i)
			{
				if (e[i].Usage != D3DDECLUSAGE_TEXCOORD) continue;
				if (e[i].Stream >= MAX_STREAMS) continue;
				switch (e[i].UsageIndex)
				{
				case 4:
					if (e[i].Type == D3DDECLTYPE_SHORT4N)
					{
						d.base_stream = e[i].Stream; d.base_off = e[i].Offset; d.ok = true;
					}
					break;
				case 5:
					if (e[i].Type == D3DDECLTYPE_SHORT4N)
					{
						d.axis_stream = e[i].Stream; d.axis_off = e[i].Offset; d.have_axis = true;
					}
					break;
				case 2:
					if (e[i].Type == D3DDECLTYPE_D3DCOLOR)
					{
						d.ref_stream = e[i].Stream; d.ref_off = e[i].Offset; d.have_ref = true;
					}
					break;
				case 1:
					if (e[i].Type == D3DDECLTYPE_FLOAT4)
					{
						d.sten_stream = e[i].Stream; d.sten_off = e[i].Offset; d.have_sten = true;
					}
					break;
				case 0:
					// SHORT4, NOT SHORT4N: the VS reads v0 as raw integers and
					// TrunkUVDecompression supplies the scale, so D3D must not be
					// normalising here and neither must we.
					if (e[i].Type == D3DDECLTYPE_SHORT4)
					{
						d.uv_stream = e[i].Stream; d.uv_off = e[i].Offset; d.have_uv = true;
					}
					break;
				default: break;
				}
			}
			// Every one of these is load-bearing for the sweep; without the axis or
			// the reference there is no frame, without TC1 there is no ring, and
			// without TC0 there is no UV.
			if (!d.have_axis || !d.have_ref || !d.have_sten || !d.have_uv) d.ok = false;
			return d;
		}

		// Everything the sweep needs that is constant across the draw.
		struct trunk_consts
		{
			D3DXVECTOR4 cp{};        // c120 CompressionParams
			D3DXVECTOR4 offset{};    // c208 OffsetInStencil
			D3DXVECTOR4 morph{};     // c207 MorphEnabled
			D3DXVECTOR4 lod{};       // c210 LevelLOD
			D3DXVECTOR4 uvdec{};     // c211 TrunkUVDecompression
			D3DXVECTOR4 stencil[TRUNK_STENCIL_COUNT]{};
			float t = 0.0f;          // saturate(DistanceFactors.x*d + DistanceFactors.y)
		};

		inline float sat(float v) { return (v < 0.0f) ? 0.0f : (v > 1.0f ? 1.0f : v); }

		inline D3DXVECTOR3 read_short4n_xyz(const std::uint8_t* p, float& w)
		{
			const std::int16_t* s = reinterpret_cast<const std::int16_t*>(p);
			w = static_cast<float>(s[3]) / 32767.0f;
			return D3DXVECTOR3(static_cast<float>(s[0]) / 32767.0f,
				static_cast<float>(s[1]) / 32767.0f,
				static_cast<float>(s[2]) / 32767.0f);
		}

		// D3DCOLOR expands to (R,G,B,A)/255 in the VS - memory order is B,G,R,A.
		inline D3DXVECTOR3 read_d3dcolor_rgb(const std::uint8_t* p)
		{
			return D3DXVECTOR3(p[2] / 255.0f, p[1] / 255.0f, p[0] / 255.0f);
		}

		bool read_trunk_consts(IDirect3DDevice9* dev, const D3DXMATRIX& world_c8,
			trunk_consts& c)
		{
			D3DXVECTOR4 eye{}, fov{}, dfac{};
			// Every one of these is declared by all four live trunk shaders, so a
			// failed read means the constant genuinely never arrived - refuse the
			// draw rather than substitute a default.
			//
			// Do NOT add reads for constants the live shaders don't declare. Registers
			// ALIAS across shader variants: c100/c101 (VFalloff/VDistScale) are not
			// declared by any live trunk shader, so reading them returns whatever the
			// last unrelated shader left there. An earlier version did exactly that
			// and used the result to displace vertices - producing a camera-dependent
			// jitter that came and went with whatever ran before.
			if (!read_vs_const(dev, TRUNK_STENCIL_REG,
				reinterpret_cast<float*>(c.stencil), TRUNK_STENCIL_COUNT)) return false;
			if (!read_vs_const(dev, 120, reinterpret_cast<float*>(&c.cp), 1)) return false;
			if (!read_vs_const(dev, 208, reinterpret_cast<float*>(&c.offset), 1)) return false;
			if (!read_vs_const(dev, 211, reinterpret_cast<float*>(&c.uvdec), 1)) return false;
			if (!read_vs_const(dev, 207, reinterpret_cast<float*>(&c.morph), 1)) return false;
			if (!read_vs_const(dev, 210, reinterpret_cast<float*>(&c.lod), 1)) return false;
			if (!read_vs_const(dev, 209, reinterpret_cast<float*>(&dfac), 1)) return false;
			if (!read_vs_const(dev, 12, reinterpret_cast<float*>(&eye), 1)) return false;
			if (!read_vs_const(dev, 96, reinterpret_cast<float*>(&fov), 1)) return false;

			// The shader measures distance from the eye to the object's ORIGIN -
			// the World translation, read out of the transposed c8..c11 as
			// (c8.w, c9.w, c10.w). world_c8 here is already un-transposed, so the
			// translation is its row 3.
			const D3DXVECTOR3 origin(world_c8.m[3][0], world_c8.m[3][1], world_c8.m[3][2]);
			const D3DXVECTOR3 d(origin.x - eye.x, origin.y - eye.y, origin.z - eye.z);
			c.t = sat(dfac.x * (D3DXVec3Length(&d) * fov.x) + dfac.y);
			return true;
		}

		// One skeleton vertex -> one swept ring vertex.
		void decode_trunk_vertex(const vtx_streams& s, UINT v, const veg_decl& d,
			const trunk_consts& c, ff_vertex& o)
		{
			const std::uint8_t* pb = s.base[d.base_stream] + v * s.stride[d.base_stream] + d.base_off;
			const std::uint8_t* pa = s.base[d.axis_stream] + v * s.stride[d.axis_stream] + d.axis_off;
			const std::uint8_t* pr = s.base[d.ref_stream] + v * s.stride[d.ref_stream] + d.ref_off;
			const std::uint8_t* ps = s.base[d.sten_stream] + v * s.stride[d.sten_stream] + d.sten_off;
			const std::uint8_t* pu = s.base[d.uv_stream] + v * s.stride[d.uv_stream] + d.uv_off;

			float bw = 0.0f;
			const D3DXVECTOR3 raw = read_short4n_xyz(pb, bw);
			D3DXVECTOR3 base(raw.x * c.cp.y + c.cp.x,
				raw.y * c.cp.y + c.cp.x,
				raw.z * c.cp.y + c.cp.x);

			float aw = 0.0f;
			const D3DXVECTOR3 axis = read_short4n_xyz(pa, aw);
			const D3DXVECTOR3 ref = read_d3dcolor_rgb(pr);
			const float* tc1 = reinterpret_cast<const float*>(ps);

			// ring = trunc-toward-zero(dot(TC1.xyz, OffsetInStencil.xyz)). The shader
			// spells that out with frc/slt rather than using a round instruction.
			const float f = tc1[0] * c.offset.x + tc1[1] * c.offset.y + tc1[2] * c.offset.z;
			int ring = static_cast<int>(f);
			if (ring < 0 || ring >= TRUNK_STENCIL_COUNT)
			{
				// mova would wrap into unrelated constants; refuse instead. Counted,
				// because a clamped ring silently deforms the trunk.
				g_veg.stencil_clamped++;
				ring = (ring < 0) ? 0 : (TRUNK_STENCIL_COUNT - 1);
			}
			const D3DXVECTOR4& st = c.stencil[ring];

			// radius, with the distance morph folded in
			const float radius0 = bw * c.cp.z + 1e-6f;
			const float m = c.t * c.morph.x * st.z + 1.0f;
			// LOD gate: `sge (LevelLOD.x / TC1.w), 1` -> shrink this ring away with
			// distance. A zero TC1.w would divide by zero in the shader too; guard.
			const float inv_w = (fabsf(tc1[3]) > 1e-20f) ? (c.lod.x / tc1[3]) : 0.0f;
			const float k = 1.0f - ((inv_w >= 1.0f) ? 1.0f : 0.0f) * c.t;
			const float R = radius0 * m * k;

			D3DXVECTOR3 t1, t2;
			D3DXVec3Cross(&t1, &axis, &ref);
			D3DXVec3Cross(&t2, &axis, &t1);

			const D3DXVECTOR3 dir(st.x * t1.x + st.y * t2.x,
				st.x * t1.y + st.y * t2.y,
				st.x * t1.z + st.y * t2.z);

			const D3DXVECTOR3 pos(base.x + R * dir.x, base.y + R * dir.y, base.z + R * dir.z);

			// The sweep direction IS the outward radial normal of the cylinder.
			// Two of the four live shaders emit no normal at all (they are unlit
			// passes) - but FF_FVF has one and Remix shades with it, so it is
			// generated here regardless. R only scales it; normalising removes R.
			D3DXVECTOR3 nrm(R * dir.x, R * dir.y, R * dir.z);
			if (D3DXVec3LengthSq(&nrm) > 1e-20f) D3DXVec3Normalize(&nrm, &nrm);
			else nrm = D3DXVECTOR3(0.0f, 0.0f, 1.0f);

			o.x = pos.x; o.y = pos.y; o.z = pos.z;
			o.nx = nrm.x; o.ny = nrm.y; o.nz = nrm.z;

			// uv = TEXCOORD0.xy * TrunkUVDecompression.y + TrunkUVDecompression.x.
			// SHORT4 (not SHORT4N): the VS sees raw integers, so no /32767 here -
			// c211.y carries the whole scale.
			const std::int16_t* uraw = reinterpret_cast<const std::int16_t*>(pu);
			o.u = static_cast<float>(uraw[0]) * c.uvdec.y + c.uvdec.x;
			o.v = static_cast<float>(uraw[1]) * c.uvdec.y + c.uvdec.x;
		}

		// ---- leaves (LeavesEquations) ------------------------------------
		// A leaf is a card that MORPHS with distance between an object-space
		// offset and a camera-facing billboard, then gets wind added:
		//
		//   base   = TEXCOORD4.xyz * CompressionParams.y + CompressionParams.x
		//   eye_l  = the eye in tree-local space (yaw-only; see below)
		//   size   = clamp(dist, eq.x, eq.y) * eq.z + eq.w      ; eq = LeavesEquations[i]
		//   fixed  = (TEXCOORD1.xyz*2-1) * (TEXCOORD4.w * TEXCOORD2.x) * size*TEXCOORD4.w
		//   card   = (TEXCOORD0.x*2-1) * normalize(-eye_l.y, eye_l.x, TEXCOORD0.y*2-1) * ...
		//   pos    = base + lerp(fixed, card, morph) + wind
		//   uv     = TEXCOORD0.zw          ; raw D3DCOLOR components, no decompression
		//
		// The wind is genuine per-frame animation (two oscillators with a time
		// seed), so leaf geometry is EXPECTED to change every frame - unlike the
		// trunk, where any per-frame change was a bug.
		struct leaf_decl
		{
			bool ok = false;
			UINT base_stream = 0, base_off = 0;     // TEXCOORD4 SHORT4N: centre + size
			UINT corner_stream = 0, corner_off = 0; // TEXCOORD0 D3DCOLOR: .xy corner, .zw uv
			UINT dir_stream = 0, dir_off = 0;       // TEXCOORD1 D3DCOLOR: .xyz offset, .w index
			UINT scale_stream = 0, scale_off = 0;   // TEXCOORD2 FLOAT1: per-leaf scale
			bool have_corner = false, have_dir = false, have_scale = false;
		};

		leaf_decl classify_leaves(const D3DVERTEXELEMENT9* e, UINT n)
		{
			leaf_decl d;
			for (UINT i = 0; i < n && e[i].Stream != 0xFF; ++i)
			{
				if (e[i].Usage != D3DDECLUSAGE_TEXCOORD) continue;
				if (e[i].Stream >= MAX_STREAMS) continue;
				switch (e[i].UsageIndex)
				{
				case 4:
					if (e[i].Type == D3DDECLTYPE_SHORT4N)
					{
						d.base_stream = e[i].Stream; d.base_off = e[i].Offset; d.ok = true;
					}
					break;
				case 0:
					if (e[i].Type == D3DDECLTYPE_D3DCOLOR)
					{
						d.corner_stream = e[i].Stream; d.corner_off = e[i].Offset; d.have_corner = true;
					}
					break;
				case 1:
					if (e[i].Type == D3DDECLTYPE_D3DCOLOR)
					{
						d.dir_stream = e[i].Stream; d.dir_off = e[i].Offset; d.have_dir = true;
					}
					break;
				case 2:
					if (e[i].Type == D3DDECLTYPE_FLOAT1)
					{
						d.scale_stream = e[i].Stream; d.scale_off = e[i].Offset; d.have_scale = true;
					}
					break;
				default: break;
				}
			}
			if (!d.have_corner || !d.have_dir || !d.have_scale) d.ok = false;
			return d;
		}

		constexpr int LEAF_EQ_REG = 214;
		constexpr int LEAF_EQ_COUNT = 6;

		struct leaf_consts
		{
			D3DXVECTOR4 cp{};                  // c120 CompressionParams
			D3DXVECTOR4 morphfact{};           // c213 LeavesMorphFact
			D3DXVECTOR4 eq[LEAF_EQ_COUNT]{};   // c214..c219 LeavesEquations
			D3DXVECTOR4 osc0_scale{};          // c100 Oscil0AnimScale
			D3DXVECTOR4 osc0_phase{};          // c101 Oscil0        (time seed)
			D3DXVECTOR4 osc0_seed{};           // c102 Oscil0PosSeedCoefficient
			D3DXVECTOR4 osc1_seed{};           // c103 Oscil1PosSeedCoefficient
			D3DXVECTOR4 osc1_phase{};          // c104 Oscil1        (time seed)
			D3DXVECTOR4 osc1_scale{};          // c105 Oscil1AnimScale
			D3DXVECTOR4 entity{};              // c13  g_WorldEntityPosition
			float dist = 0.0f;                 // |eye_local| * g_FovDistanceScale
			D3DXVECTOR3 eye_dir{};             // normalize(eye_local)
		};

		bool read_leaf_consts(IDirect3DDevice9* dev, const D3DXMATRIX& w, leaf_consts& c)
		{
			D3DXVECTOR4 eye{}, fov{};
			if (!read_vs_const(dev, LEAF_EQ_REG, reinterpret_cast<float*>(c.eq), LEAF_EQ_COUNT)) return false;
			if (!read_vs_const(dev, 120, reinterpret_cast<float*>(&c.cp), 1)) return false;
			if (!read_vs_const(dev, 213, reinterpret_cast<float*>(&c.morphfact), 1)) return false;
			if (!read_vs_const(dev, 100, reinterpret_cast<float*>(&c.osc0_scale), 1)) return false;
			if (!read_vs_const(dev, 101, reinterpret_cast<float*>(&c.osc0_phase), 1)) return false;
			if (!read_vs_const(dev, 102, reinterpret_cast<float*>(&c.osc0_seed), 1)) return false;
			if (!read_vs_const(dev, 103, reinterpret_cast<float*>(&c.osc1_seed), 1)) return false;
			if (!read_vs_const(dev, 104, reinterpret_cast<float*>(&c.osc1_phase), 1)) return false;
			if (!read_vs_const(dev, 105, reinterpret_cast<float*>(&c.osc1_scale), 1)) return false;
			if (!read_vs_const(dev, 13, reinterpret_cast<float*>(&c.entity), 1)) return false;
			if (!read_vs_const(dev, 12, reinterpret_cast<float*>(&eye), 1)) return false;
			if (!read_vs_const(dev, 96, reinterpret_cast<float*>(&fov), 1)) return false;

			// The eye, in the tree's local space. The shader builds this by hand and
			// assumes the tree's World is a YAW-ONLY rotation: it uses (m00, m01) for
			// local X and the perpendicular (-m01, m00) for local Y rather than the
			// matrix's real second row, and takes Z as a plain difference. Replicated
			// exactly - a "more correct" full inverse would not match the game.
			const float dx = eye.x - w.m[3][0];
			const float dy = eye.y - w.m[3][1];
			const D3DXVECTOR3 local(w.m[0][0] * dx + w.m[0][1] * dy,
				-w.m[0][1] * dx + w.m[0][0] * dy,
				eye.z - w.m[3][2]);

			const float len = D3DXVec3Length(&local);
			c.dist = len * fov.x;
			c.eye_dir = (len > 1e-20f)
				? D3DXVECTOR3(local.x / len, local.y / len, local.z / len)
				: D3DXVECTOR3(0.0f, 1.0f, 0.0f);
			return true;
		}

		void decode_leaf_vertex(const vtx_streams& s, UINT v, const leaf_decl& d,
			const leaf_consts& c, ff_vertex& o)
		{
			const std::uint8_t* pb = s.base[d.base_stream] + v * s.stride[d.base_stream] + d.base_off;
			const std::uint8_t* pc = s.base[d.corner_stream] + v * s.stride[d.corner_stream] + d.corner_off;
			const std::uint8_t* pd = s.base[d.dir_stream] + v * s.stride[d.dir_stream] + d.dir_off;
			const std::uint8_t* pS = s.base[d.scale_stream] + v * s.stride[d.scale_stream] + d.scale_off;

			float bw = 0.0f;
			const D3DXVECTOR3 raw = read_short4n_xyz(pb, bw);
			const D3DXVECTOR3 base(raw.x * c.cp.y + c.cp.x,
				raw.y * c.cp.y + c.cp.x,
				raw.z * c.cp.y + c.cp.x);

			const float scale = *reinterpret_cast<const float*>(pS);   // TEXCOORD2, FLOAT1

			// TEXCOORD1: .xyz is an offset direction (D3DCOLOR, decoded *2-1),
			// .w carries the leaf's equation index as a raw byte.
			const D3DXVECTOR3 dir_raw = read_d3dcolor_rgb(pd);
			const D3DXVECTOR3 n(dir_raw.x * 2.0f - 1.0f, dir_raw.y * 2.0f - 1.0f, dir_raw.z * 2.0f - 1.0f);
			const float idx_byte = static_cast<float>(pd[3]);   // D3DCOLOR alpha, 0..255

			// Leaves with a byte >= 99 skip the morph and index from 100; the shader
			// spells this out with slt/lrp.
			const float r3x = idx_byte + 0.5f;
			const bool big = (99.0f < r3x);
			const float morph = big ? 0.0f : sat(c.dist * c.morphfact.x + c.morphfact.y);
			const float idx_sel = big ? (idx_byte - 99.5f) : (idx_byte + 0.5f);

			int i = static_cast<int>(idx_sel - (r3x - floorf(r3x)));
			if (i < 0 || i >= LEAF_EQ_COUNT)
			{
				// mova would index past LeavesEquations into unrelated constants.
				g_veg.leaf_eq_clamped++;
				i = (i < 0) ? 0 : (LEAF_EQ_COUNT - 1);
			}
			const D3DXVECTOR4& eq = c.eq[i];

			// The camera-facing card axis: horizontal perpendicular to the eye
			// direction, tilted by the corner's y.
			const D3DXVECTOR3 corner = read_d3dcolor_rgb(pc);
			const float a = corner.x * 2.0f - 1.0f;   // TEXCOORD0.x
			const float b = corner.y * 2.0f - 1.0f;   // TEXCOORD0.y
			D3DXVECTOR3 right(-c.eye_dir.y, c.eye_dir.x, b);
			if (D3DXVec3LengthSq(&right) > 1e-20f) D3DXVec3Normalize(&right, &right);

			float size = c.dist;
			if (size < eq.x) size = eq.x;      // max, then min = clamp
			if (size > eq.y) size = eq.y;
			size = size * eq.z + eq.w;
			const float size_w = size * bw;

			// fixed = the object-space offset; card = the camera-facing one.
			const float s_leaf = bw * scale;
			const D3DXVECTOR3 fixed_off(n.x * s_leaf * size_w, n.y * s_leaf * size_w, n.z * s_leaf * size_w);
			const float sw = size_w * scale;
			const D3DXVECTOR3 card(sw * a * right.x - fixed_off.x,
				sw * a * right.y - fixed_off.y,
				sw * a * right.z - fixed_off.z);

			D3DXVECTOR3 pos(base.x + fixed_off.x + morph * card.x,
				base.y + fixed_off.y + morph * card.y,
				base.z + fixed_off.z + morph * card.z);

			// ---- wind ----------------------------------------------------
			// The shader's frc/mad around each sincos is only RANGE REDUCTION for
			// the hardware: frac(x/2pi + 0.5)*2pi - pi differs from x by a multiple
			// of 2pi, so sin() of it IS sin(x). Nothing to reproduce on the CPU.
			const D3DXVECTOR3 p(pos.x + c.entity.x, pos.y + c.entity.y, pos.z + c.entity.z);
			const float ph0 = (c.osc0_seed.x * p.x + c.osc0_seed.y * p.y + c.osc0_seed.z * p.z) + c.osc0_phase.x;
			const float ph1 = (c.osc1_seed.x * p.x + c.osc1_seed.y * p.y + c.osc1_seed.z * p.z) + c.osc1_phase.x;
			const float w0 = sinf(ph0), w1 = sinf(ph1);
			pos.x += c.osc0_scale.x * w0 + c.osc1_scale.x * w1;
			pos.y += c.osc0_scale.y * w0 + c.osc1_scale.y * w1;
			pos.z += c.osc0_scale.z * w0 + c.osc1_scale.z * w1;

			o.x = pos.x; o.y = pos.y; o.z = pos.z;

			// The live leaves VS emits NO normal - the game shades this foliage
			// unlit (its outputs are uv, AO and a fade colour). Remix path-traces
			// it, so a normal has to come from somewhere: the per-vertex offset
			// direction is the closest thing available and gives a canopy a rounded
			// look. This is OUR choice, not the game's - if foliage lighting looks
			// wrong, this is the line to revisit.
			D3DXVECTOR3 nrm = n;
			if (D3DXVec3LengthSq(&nrm) > 1e-20f) D3DXVec3Normalize(&nrm, &nrm);
			else nrm = D3DXVECTOR3(0.0f, 0.0f, 1.0f);
			o.nx = nrm.x; o.ny = nrm.y; o.nz = nrm.z;

			// uv = TEXCOORD0.zw, straight D3DCOLOR components (`mul o1, c4.zzww, v0.zwzz`).
			o.u = pc[0] / 255.0f;   // .z = B
			o.v = pc[3] / 255.0f;   // .w = A
		}
	}

	// ---- vegetation trunks: sweep the skeleton on the CPU -> static geometry --
	// Shape deliberately mirrors try_render_skinned(): both take a source the FF
	// pipeline cannot describe, generate plain float vertices into the dynamic
	// pool, and hand Remix ordinary static geometry.
	static bool try_render_trunk(IDirect3DDevice9* dev, const veg_decl& vd,
		D3DPRIMITIVETYPE PrimitiveType, INT BaseVertexIndex, UINT MinVertexIndex,
		UINT NumVertices, UINT startIndex, UINT primCount)
	{
		IDirect3DPixelShader9* cur_ps = nullptr;
		dev->GetPixelShader(&cur_ps);
		const int diffuse_stage = get_ps_diffuse_stage(cur_ps);
		if (cur_ps) cur_ps->Release();
		// A trunk draw with no diffuse sampler is a depth/shadow pass, exactly as
		// on the static path - it must NOT be path-traced.
		if (diffuse_stage < 0) { g_veg.trunk_no_diffuse++; return false; }

		D3DXMATRIX wvp_t, wvp, world_c8, view, proj;
		if (!read_vs_const(dev, 0, reinterpret_cast<float*>(&wvp_t), 4))
		{
			g_veg.trunk_no_camera++; return false;
		}
		D3DXMatrixTranspose(&wvp, &wvp_t);
		if (!compute_camera(dev, wvp, world_c8, view, proj))
		{
			g_veg.trunk_no_camera++; return false;
		}

		trunk_consts tc;
		if (!read_trunk_consts(dev, world_c8, tc)) { g_veg.trunk_no_const++; return false; }

		const UINT first = static_cast<UINT>(BaseVertexIndex) + MinVertexIndex;
		const UINT bytes = NumVertices * sizeof(ff_vertex);

		IDirect3DVertexBuffer9* dyn = get_dynamic_vb(dev, bytes);
		if (!dyn) { g_veg.trunk_no_vb++; return false; }

		void* dst = nullptr;
		if (FAILED(dyn->Lock(0, bytes, &dst, D3DLOCK_DISCARD)) || !dst)
		{
			g_veg.trunk_no_vb++; return false;
		}

		// The skeleton spans stream 0 (TEXCOORD1/2) and stream 1 (TEXCOORD4/5), so
		// the vertex is GATHERED across streams - same as the multi-stream static
		// path. Lock the union of the four attributes' streams in one pass.
		bool need[MAX_STREAMS]{};
		need[vd.base_stream] = true;
		need[vd.axis_stream] = true;
		need[vd.ref_stream] = true;
		need[vd.sten_stream] = true;
		need[vd.uv_stream] = true;

		const bool converted = with_stream_mask(dev, need, [&](const vtx_streams& s)
		{
			ff_vertex* out = static_cast<ff_vertex*>(dst);
			for (UINT i = 0; i < NumVertices; ++i)
				decode_trunk_vertex(s, first + i, vd, tc, out[i]);
			return true;
		});
		dyn->Unlock();
		if (!converted) { g_veg.trunk_no_vb++; return false; }

		// ---- save what we touch ------------------------------------------
		IDirect3DVertexShader9* old_vs = nullptr; IDirect3DPixelShader9* old_ps = nullptr;
		IDirect3DVertexDeclaration9* old_decl = nullptr;
		dev->GetVertexShader(&old_vs); dev->GetPixelShader(&old_ps); dev->GetVertexDeclaration(&old_decl);
		DWORD old_lighting = 0; dev->GetRenderState(D3DRS_LIGHTING, &old_lighting);
		DWORD old_fill = 0; dev->GetRenderState(D3DRS_FILLMODE, &old_fill);
		IDirect3DBaseTexture9* old_tex0 = nullptr; dev->GetTexture(0, &old_tex0);
		IDirect3DBaseTexture9* diffuse = nullptr;
		IDirect3DVertexBuffer9* old_s0 = nullptr; UINT old_s0_off = 0, old_s0_stride = 0;
		dev->GetStreamSource(0, &old_s0, &old_s0_off, &old_s0_stride);

		dev->SetVertexShader(nullptr);
		dev->SetPixelShader(nullptr);
		dev->SetFVF(FF_FVF);
		dev->SetStreamSource(0, dyn, 0, sizeof(ff_vertex));

		// The sweep emits MODEL-space positions (the shader applies WVP to them
		// directly), so World is plain g_World - CompressionParams already did the
		// decompression that |w| does on the SHORT4 static path.
		dev->SetTransform(D3DTS_WORLD, &world_c8);
		dev->SetTransform(D3DTS_VIEW, &view);
		dev->SetTransform(D3DTS_PROJECTION, &proj);
		dev->SetRenderState(D3DRS_LIGHTING, FALSE);
		if (g_wireframe) dev->SetRenderState(D3DRS_FILLMODE, D3DFILL_WIREFRAME);

		if (diffuse_stage != 0)
		{
			dev->GetTexture(static_cast<DWORD>(diffuse_stage), &diffuse);
			dev->SetTexture(0, diffuse);
		}
		dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
		dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);

		// out[0] holds source vertex `first`, so shift the base by -MinVertexIndex.
		const HRESULT hr = dev->DrawIndexedPrimitive(PrimitiveType,
			static_cast<INT>(-static_cast<INT>(MinVertexIndex)), 0, NumVertices,
			startIndex, primCount);

		// ---- restore -----------------------------------------------------
		if (diffuse_stage != 0) { dev->SetTexture(0, old_tex0); if (diffuse) diffuse->Release(); }
		if (old_tex0) old_tex0->Release();
		dev->SetRenderState(D3DRS_LIGHTING, old_lighting);
		if (g_wireframe) dev->SetRenderState(D3DRS_FILLMODE, old_fill);
		dev->SetStreamSource(0, old_s0, old_s0_off, old_s0_stride);
		if (old_s0) old_s0->Release();
		dev->SetVertexShader(old_vs); if (old_vs) old_vs->Release();
		dev->SetPixelShader(old_ps); if (old_ps) old_ps->Release();
		dev->SetVertexDeclaration(old_decl); if (old_decl) old_decl->Release();

		if (SUCCEEDED(hr)) { g_veg.trunk_converted++; g_rej.converted++; return true; }
		return false;
	}

	// ---- vegetation leaves: morphing billboards + wind -> static geometry ----
	static bool try_render_leaves(IDirect3DDevice9* dev, const leaf_decl& ld,
		D3DPRIMITIVETYPE PrimitiveType, INT BaseVertexIndex, UINT MinVertexIndex,
		UINT NumVertices, UINT startIndex, UINT primCount)
	{
		IDirect3DPixelShader9* cur_ps = nullptr;
		dev->GetPixelShader(&cur_ps);
		const int diffuse_stage = get_ps_diffuse_stage(cur_ps);

		// Leaf cards are ALPHA-TESTED, and the game does that test inside the pixel
		// shader against g_AlphaTestValue (PS c31) - which the FF pipeline cannot
		// run. Without reproducing it every leaf becomes an opaque quad and the tree
		// turns into a blob. Read the game's own threshold; fall back to a half-way
		// reference rather than disabling the test (an opaque canopy is far worse
		// than a slightly wrong cutoff).
		float atest[4] = { 0.5f, 0, 0, 0 };
		if (!cur_ps || FAILED(dev->GetPixelShaderConstantF(31, atest, 1)))
			atest[0] = 0.5f;
		if (cur_ps) cur_ps->Release();
		if (diffuse_stage < 0) { g_veg.leaf_no_diffuse++; return false; }

		D3DXMATRIX wvp_t, wvp, world_c8, view, proj;
		if (!read_vs_const(dev, 0, reinterpret_cast<float*>(&wvp_t), 4))
		{
			g_veg.leaf_no_camera++; return false;
		}
		D3DXMatrixTranspose(&wvp, &wvp_t);
		if (!compute_camera(dev, wvp, world_c8, view, proj))
		{
			g_veg.leaf_no_camera++; return false;
		}

		leaf_consts lc;
		if (!read_leaf_consts(dev, world_c8, lc)) { g_veg.leaf_no_const++; return false; }

		const UINT first = static_cast<UINT>(BaseVertexIndex) + MinVertexIndex;
		const UINT bytes = NumVertices * sizeof(ff_vertex);

		IDirect3DVertexBuffer9* dyn = get_dynamic_vb(dev, bytes);
		if (!dyn) { g_veg.leaf_no_vb++; return false; }

		void* dst = nullptr;
		if (FAILED(dyn->Lock(0, bytes, &dst, D3DLOCK_DISCARD)) || !dst)
		{
			g_veg.leaf_no_vb++; return false;
		}

		bool need[MAX_STREAMS]{};
		need[ld.base_stream] = true;
		need[ld.corner_stream] = true;
		need[ld.dir_stream] = true;
		need[ld.scale_stream] = true;

		const bool converted = with_stream_mask(dev, need, [&](const vtx_streams& s)
		{
			ff_vertex* out = static_cast<ff_vertex*>(dst);
			for (UINT i = 0; i < NumVertices; ++i)
				decode_leaf_vertex(s, first + i, ld, lc, out[i]);
			return true;
		});
		dyn->Unlock();
		if (!converted) { g_veg.leaf_no_vb++; return false; }

		// ---- save what we touch ------------------------------------------
		IDirect3DVertexShader9* old_vs = nullptr; IDirect3DPixelShader9* old_ps = nullptr;
		IDirect3DVertexDeclaration9* old_decl = nullptr;
		dev->GetVertexShader(&old_vs); dev->GetPixelShader(&old_ps); dev->GetVertexDeclaration(&old_decl);
		DWORD old_lighting = 0, old_fill = 0, old_atest = 0, old_aref = 0, old_afunc = 0;
		dev->GetRenderState(D3DRS_LIGHTING, &old_lighting);
		dev->GetRenderState(D3DRS_FILLMODE, &old_fill);
		dev->GetRenderState(D3DRS_ALPHATESTENABLE, &old_atest);
		dev->GetRenderState(D3DRS_ALPHAREF, &old_aref);
		dev->GetRenderState(D3DRS_ALPHAFUNC, &old_afunc);
		DWORD old_aop = 0, old_aarg1 = 0;
		dev->GetTextureStageState(0, D3DTSS_ALPHAOP, &old_aop);
		dev->GetTextureStageState(0, D3DTSS_ALPHAARG1, &old_aarg1);
		IDirect3DBaseTexture9* old_tex0 = nullptr; dev->GetTexture(0, &old_tex0);
		IDirect3DBaseTexture9* diffuse = nullptr;
		IDirect3DVertexBuffer9* old_s0 = nullptr; UINT old_s0_off = 0, old_s0_stride = 0;
		dev->GetStreamSource(0, &old_s0, &old_s0_off, &old_s0_stride);

		dev->SetVertexShader(nullptr);
		dev->SetPixelShader(nullptr);
		dev->SetFVF(FF_FVF);
		dev->SetStreamSource(0, dyn, 0, sizeof(ff_vertex));

		dev->SetTransform(D3DTS_WORLD, &world_c8);
		dev->SetTransform(D3DTS_VIEW, &view);
		dev->SetTransform(D3DTS_PROJECTION, &proj);
		dev->SetRenderState(D3DRS_LIGHTING, FALSE);
		if (g_wireframe) dev->SetRenderState(D3DRS_FILLMODE, D3DFILL_WIREFRAME);

		if (diffuse_stage != 0)
		{
			dev->GetTexture(static_cast<DWORD>(diffuse_stage), &diffuse);
			dev->SetTexture(0, diffuse);
		}
		dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
		dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);

		// Reproduce the shader's alpha test with fixed-function state.
		float ref = atest[0];
		if (!(ref >= 0.0f && ref <= 1.0f)) ref = 0.5f;   // NaN/garbage-proof
		dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
		dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
		dev->SetRenderState(D3DRS_ALPHATESTENABLE, TRUE);
		dev->SetRenderState(D3DRS_ALPHAFUNC, D3DCMP_GREATEREQUAL);
		dev->SetRenderState(D3DRS_ALPHAREF, static_cast<DWORD>(ref * 255.0f));

		const HRESULT hr = dev->DrawIndexedPrimitive(PrimitiveType,
			static_cast<INT>(-static_cast<INT>(MinVertexIndex)), 0, NumVertices,
			startIndex, primCount);

		// ---- restore -----------------------------------------------------
		if (diffuse_stage != 0) { dev->SetTexture(0, old_tex0); if (diffuse) diffuse->Release(); }
		if (old_tex0) old_tex0->Release();
		dev->SetRenderState(D3DRS_LIGHTING, old_lighting);
		if (g_wireframe) dev->SetRenderState(D3DRS_FILLMODE, old_fill);
		dev->SetRenderState(D3DRS_ALPHATESTENABLE, old_atest);
		dev->SetRenderState(D3DRS_ALPHAREF, old_aref);
		dev->SetRenderState(D3DRS_ALPHAFUNC, old_afunc);
		dev->SetTextureStageState(0, D3DTSS_ALPHAOP, old_aop);
		dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, old_aarg1);
		dev->SetStreamSource(0, old_s0, old_s0_off, old_s0_stride);
		if (old_s0) old_s0->Release();
		dev->SetVertexShader(old_vs); if (old_vs) old_vs->Release();
		dev->SetPixelShader(old_ps); if (old_ps) old_ps->Release();
		dev->SetVertexDeclaration(old_decl); if (old_decl) old_decl->Release();

		if (SUCCEEDED(hr)) { g_veg.leaf_converted++; g_rej.converted++; return true; }
		return false;
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

	// Thin wrapper: attribute the draw's outcome to its vertex shader. It wraps
	// rather than instrumenting the impl because the impl has ~15 return points, and
	// any one of them missed would silently under-count a rejection - which is the
	// exact failure this table exists to catch.
	static bool try_render_fixed_function_impl(IDirect3DDevice9* dev,
		D3DPRIMITIVETYPE PrimitiveType, INT BaseVertexIndex, UINT MinVertexIndex,
		UINT NumVertices, UINT startIndex, UINT primCount);

	bool try_render_fixed_function(IDirect3DDevice9* dev,
		D3DPRIMITIVETYPE PrimitiveType, INT BaseVertexIndex, UINT MinVertexIndex,
		UINT NumVertices, UINT startIndex, UINT primCount)
	{
		if (!g_enabled || !dev || BaseVertexIndex < 0) return false;

		IDirect3DVertexShader9* vs = nullptr;
		dev->GetVertexShader(&vs);
		const bool ok = try_render_fixed_function_impl(dev, PrimitiveType, BaseVertexIndex,
			MinVertexIndex, NumVertices, startIndex, primCount);
		note_vs_draw(vs, ok);
		if (vs) vs->Release();
		return ok;
	}

	static bool try_render_fixed_function_impl(IDirect3DDevice9* dev,
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

			// Vegetation has no POSITION element at all - the geometry is generated
			// in the VS from a compressed skeleton. Which family this is comes from
			// the VS constant table (registered at CreateVertexShader), not from a
			// draw-time guess.
			{
				IDirect3DVertexShader9* vs = nullptr;
				dev->GetVertexShader(&vs);
				const veg_kind vk = get_vs_veg(vs);
				if (vs) vs->Release();

				if (vk != veg_kind::none)
				{
					g_veg.seen[static_cast<int>(vk)]++;
					if (vk == veg_kind::trunk && g_veg_trunks)
					{
						const veg_decl vd = classify_veg(el, n);
						if (!vd.ok) g_veg.trunk_no_decl++;
						else if (try_render_trunk(dev, vd, PrimitiveType, BaseVertexIndex,
							MinVertexIndex, NumVertices, startIndex, primCount))
						{
							return true;
						}
					}
					else if (vk == veg_kind::leaves && g_veg_leaves)
					{
						const leaf_decl ld = classify_leaves(el, n);
						if (!ld.ok) g_veg.leaf_no_decl++;
						else if (try_render_leaves(dev, ld, PrimitiveType, BaseVertexIndex,
							MinVertexIndex, NumVertices, startIndex, primCount))
						{
							return true;
						}
					}
				}
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
		const ps_diffuse_info ps_info = get_ps_diffuse_info(cur_ps);
		if (cur_ps) cur_ps->Release();
		int diffuse_stage = ps_info.stage;

		// Normal-only materials (water / volume fog) have no albedo at all. They are
		// real surfaces, not AO passes, so they convert - but behind a toggle, since
		// what Remix does with them depends on rtx.conf tagging.
		if (ps_info.normal_only)
		{
			if (!g_normal_only_materials) { g_rej.no_diffuse++; return false; }
			g_normal_only_draws++;
		}
		if (diffuse_stage < 0) { g_rej.no_diffuse++; return false; }

		// The POSITION stream (which is NOT always stream 0) drives the |w| scale and
		// the static/dynamic decision.
		IDirect3DVertexBuffer9* src = nullptr;
		UINT soff = 0, stride = 0;
		if (FAILED(dev->GetStreamSource(di.pos_stream, &src, &soff, &stride)) || !src || !stride)
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

		// The cached shadow VB is a 1:1 copy of ONE source buffer, keyed on that
		// buffer's pointer, so it can only serve declarations whose attributes all
		// live on stream 0. A multi-stream declaration has to be gathered per draw.
		//
		// Dynamic sources (cloth/softbody: re-simulated and re-uploaded every frame)
		// must not use the cache either - caching freezes them at frame one, so they
		// render but never follow the character.
		IDirect3DVertexBuffer9* vb_to_draw = nullptr;
		UINT draw_base = BaseVertexIndex;
		UINT draw_minv = MinVertexIndex;

		if (!di.single_stream0 || is_dynamic_vb(src))
		{
			const UINT bytes = NumVertices * sizeof(ff_vertex);
			IDirect3DVertexBuffer9* dyn = get_dynamic_vb(dev, bytes);
			if (!dyn) { src->Release(); g_rej.no_scale_or_vb++; return false; }

			void* dst = nullptr;
			if (FAILED(dyn->Lock(0, bytes, &dst, D3DLOCK_DISCARD)) || !dst)
			{
				src->Release(); g_rej.no_scale_or_vb++; return false;
			}
			const bool converted = with_streams(dev, di, [&](const vtx_streams& s)
			{
				return convert_range(s, di, first, NumVertices,
					static_cast<ff_vertex*>(dst)) != 0;
			});
			dyn->Unlock();
			if (!converted) { src->Release(); g_rej.no_scale_or_vb++; return false; }

			// We compacted [first, first+NumVertices) to out[0..], so shift the base.
			vb_to_draw = dyn;
			draw_base = static_cast<UINT>(-static_cast<INT>(MinVertexIndex));
			draw_minv = 0;
			g_rej.multistream++;
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

		// We bind our converted buffer to STREAM 0, but `src` is the POSITION stream,
		// which may be stream 1. Restoring stream 0 from `src` would then rebind the
		// wrong buffer and corrupt the next draw, so save stream 0's real binding.
		IDirect3DVertexBuffer9* old_s0 = nullptr;
		UINT old_s0_off = 0, old_s0_stride = 0;
		dev->GetStreamSource(0, &old_s0, &old_s0_off, &old_s0_stride);

		DWORD old_lighting = 0, old_fill = 0;
		dev->GetRenderState(D3DRS_LIGHTING, &old_lighting);
		dev->GetRenderState(D3DRS_FILLMODE, &old_fill);

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

		// Bind the real diffuse into stage 0 (it may live at any stage).
		IDirect3DBaseTexture9* diffuse = nullptr;
		IDirect3DBaseTexture9* old_tex0 = nullptr;
		dev->GetTexture(0, &old_tex0);
		if (diffuse_stage != 0)
		{
			dev->GetTexture(static_cast<DWORD>(diffuse_stage), &diffuse);
			dev->SetTexture(0, diffuse);
		}

		// ---- texture/sampler diagnostic (see tex_diag in the header) --------
		// Read the sampler state the GAME left behind, before FF samples with it.
		// Our SetTexture above does not touch sampler state, so reading it here
		// is the same as reading it before the bind - and this is the only place
		// that has both stage numbers and the resolved texture in hand.
		{
			// Describe a texture without QueryInterface (the IID lives in
			// dxguid.lib, which we don't link) and without a refcount.
			const auto desc_of = [](IDirect3DBaseTexture9* t, UINT& w, UINT& h,
				DWORD& levels, DWORD& fmt) -> bool
			{
				if (!t || t->GetType() != D3DRTYPE_TEXTURE) return false;
				D3DSURFACE_DESC sd{};
				if (FAILED(static_cast<IDirect3DTexture9*>(t)->GetLevelDesc(0, &sd))) return false;
				w = sd.Width; h = sd.Height;
				levels = t->GetLevelCount(); fmt = sd.Format;
				return true;
			};

			const ps_diffuse_info info = get_ps_diffuse_info(cur_ps);

			// ---- histogram over EVERY converted draw, split by rank ----------
			// n=2 can't tell "tiny is typical" from "I sampled two odd draws".
			{
				IDirect3DBaseTexture9* eff = diffuse ? diffuse : old_tex0;
				UINT w = 0, h = 0; DWORD lv = 0, fm = 0;
				if (desc_of(eff, w, h, lv, fm))
				{
					const int r = (info.rank >= 0 && info.rank <= 4) ? info.rank : 5;
					const int b = (w <= 8) ? 0 : (w <= 32) ? 1 : (w <= 64) ? 2 : (w <= 128) ? 3
						: (w <= 256) ? 4 : (w <= 512) ? 5 : 6;
					g_diffuse_sizes.by_rank[r][b]++;
					g_diffuse_sizes.rank_total[r]++;
				}
			}

			const int slot = (diffuse_stage != 0) ? 1 : 0;
			if (s_tex_diag_armed[slot])
			{
				s_tex_diag_armed[slot] = false;
				tex_diag d{};
				d.diffuse_stage = diffuse_stage;
				d.rank = info.rank;
				std::strncpy(d.name, info.name, sizeof(d.name) - 1);

				// What was bound at every OTHER stage? A big texture sitting at a
				// stage we skipped is the whole diagnosis, visible at a glance.
				for (DWORD s = 0; s < 8; ++s)
				{
					IDirect3DBaseTexture9* st = nullptr;
					if (FAILED(dev->GetTexture(s, &st)) || !st) continue;
					// Stage 0 already holds OUR bind at this point; report what the
					// GAME had there instead, or the row would describe ourselves.
					IDirect3DBaseTexture9* show = (s == 0 && diffuse_stage != 0) ? old_tex0 : st;
					auto& row = d.stages[s];
					row.bound = desc_of(show, row.width, row.height, row.levels, row.format);
					st->Release();
				}

				const auto read_samp = [&](DWORD stage, tex_diag::samp& s)
				{
					DWORD v = 0;
					dev->GetSamplerState(stage, D3DSAMP_MAXMIPLEVEL, &v);   s.max_mip_level = v;
					dev->GetSamplerState(stage, D3DSAMP_MIPFILTER, &v);     s.mip_filter = v;
					dev->GetSamplerState(stage, D3DSAMP_MINFILTER, &v);     s.min_filter = v;
					dev->GetSamplerState(stage, D3DSAMP_MAGFILTER, &v);     s.mag_filter = v;
					dev->GetSamplerState(stage, D3DSAMP_MIPMAPLODBIAS, &v);
					s.mip_lod_bias = *reinterpret_cast<const float*>(&v);   // stored as a float bitpattern
				};
				read_samp(static_cast<DWORD>(diffuse_stage), d.src);
				read_samp(0, d.dst);

				// When the diffuse is already at stage 0 we never fetched it - the
				// texture under test is then whatever the game had bound there.
				IDirect3DBaseTexture9* eff = diffuse ? diffuse : old_tex0;
				if (eff)
				{
					d.have_tex = true;
					d.tex_lod = eff->GetLOD();
					d.tex_levels = eff->GetLevelCount();

					// GetType() rather than QueryInterface: the IID lives in
					// dxguid.lib which we don't link, and this needs no refcount.
					// Cubes/volumes have no GetLevelDesc(0) with this signature.
					if (eff->GetType() == D3DRTYPE_TEXTURE)
					{
						auto* t2d = static_cast<IDirect3DTexture9*>(eff);
						D3DSURFACE_DESC sd{};
						if (SUCCEEDED(t2d->GetLevelDesc(0, &sd)))
						{
							d.tex_width = sd.Width;
							d.tex_height = sd.Height;
							d.tex_pool = sd.Pool;
							d.tex_format = sd.Format;
						}
					}
				}

				d.valid = true;
				g_tex_diag[slot] = d;
			}
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
		dev->SetStreamSource(0, old_s0, old_s0_off, old_s0_stride);
		if (old_s0) old_s0->Release();
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
