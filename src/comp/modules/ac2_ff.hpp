#pragma once

// AC2 RTX - fixed-function conversion (stage A)
//
// Goal: re-issue AC2's shader-based static-mesh draws through the D3D9
// fixed-function pipeline, which is what stock RTX Remix understands natively.
//
// Why a shadow VB is unavoidable:
//   FF requires D3DDECLUSAGE_POSITION to be FLOAT3. AC2 stores positions as
//   SHORT4. So we convert each source VB once into a float FVF buffer and cache
//   it (AC2's mesh VBs are static, so this is amortised to ~zero).
//
// The scale still saves us: the shadow VB stores the RAW xyz shorts as floats
// and we fold the per-draw scale into the World matrix instead of baking it in.
// That matters because one VB is shared by many meshes with different |w|.
//
//   position = (xyz / 32767) * (|w| / 8)  ==  xyz * (|w| / 262136)
//
// Stage A uses World = scale, View = identity, Projection = WVP (read back from
// c0..c3 and transposed). That rasterises correctly and proves the geometry +
// scale math. It is NOT yet a valid camera for Remix - stage B must feed the
// real View/Projection (available from GfxContext +608 / +544).

namespace comp::ac2_ff
{
	extern bool g_enabled;      // master switch (F7 toggles)
	extern bool g_wireframe;    // draw converted meshes in wireframe to spot them

	// "FF only" - drop every draw we can't convert, so the scene contains NOTHING
	// but our fixed-function geometry. DEFAULT ON; INSERT toggles it OFF.
	// (F4 collided with a mod debug menu.)
	//
	// Why: with FF on, only ~14% of draws are converted; the other 86% still go to
	// the game's shaders and get vertex-captured by Remix. So "it flickers with FF
	// on and off" does NOT prove FF is irrelevant - it's exactly what you'd see if
	// the VERTEX-CAPTURED geometry is the thing flickering and the FF geometry is
	// fine. This mode isolates the two: if the scene is stable with only FF
	// geometry present, FF conversion is the FIX (convert more), not a dead end.
	extern bool g_ff_only;

	// Skinned (character) conversion via CPU skinning. Toggle with F1.
	//
	// Why CPU and not FF indexed vertex blending: with D3DRS_INDEXEDVERTEXBLEND
	// Remix would still have to evaluate the blend itself to get final positions -
	// the same work that makes vertex capture flicker. CPU skinning hands Remix
	// plain pre-skinned static geometry, which we've proven is stable.
	//
	// From vs_872B45CB (130_-_Characters-Cloth-Tilling):
	//   mul r3, c5.x(=3), v2      -> bone index * 3
	//   mad r3, v0.xyzx, c5.yyyz(=16), c5.zzzw(=1)   -> pos = (xyz*16, 1)
	//   dp4 r4.x, r3, c120[a0.x] / c121 / c122       -> palette at c120 + 3*idx
	//   ... 4-bone weighted blend via v1.x/y/z/w
	extern bool g_skinning;
	extern std::uint32_t g_skinned_draws;

	// Game-side anti-culling. DEFAULT ON (applied on the first draw); DELETE
	// toggles it OFF.
	//
	// Remix's own rtx.antiCulling.* can only extend the life of objects the game
	// still SUBMITS. AC2 frustum-culls before submission, so Remix never sees the
	// geometry at all - which is why enabling anti-culling did nothing. We have to
	// stop the game culling in the first place.
	//
	// scimitar's culling job graph (registered in sub_118FD50):
	//   this[190] = sub_11B2550 -> "VisualsCulling"  -> sub_11B1FB0
	// and inside sub_11B1FB0 the submission gate is:
	//   11b206d  call sub_11B1710       ; eax = visibility flags (36 == culled)
	//   11b2072  mov  edx,[ebp+var_14]  ; edx = the pass mask
	//   11b2075  test edx,eax
	//   11b2077  jz   skip              ; <- skips the render-list add
	//   11b207f  call [ebp+var_1C]      ; add to render list (eax passed as flags)
	//
	// Patch `test edx,eax` -> `mov eax,edx` and NOP the jz. The object is then
	// always submitted, carrying exactly the flags the pass asked for (rather than
	// the culled sentinel 36, which the renderer would likely reject downstream).
	//   original: 85 C2 74 09
	//   patched : 8B C2 90 90
	extern bool g_anticull;
	void set_anticulling(bool enable);

	// scimitar::CameraFrustum::CullAABB (0x119be50) forced to return 1
	// ("intersecting"). Fixes the building flicker.
	//
	// DEFAULT OFF - it can CRASH DURING LOADING. Enable with PAGE DOWN once you are
	// in-world. That's why it is a separate toggle from the DELETE patches rather
	// than folded in with them.
	//
	// Patched IN-BODY (jz -> jmp at 0x119be74, and the epilogue's `mov eax,2` ->
	// `mov eax,1`), NOT by overwriting the prologue - the prologue overwrite
	// crashes. See the comment in ac2_ff.cpp before changing this.
	extern bool g_cullaabb;
	void set_cullaabb(bool enable);

	// Prints module base + the ACTUAL bytes at every patch site. A patch whose
	// memcmp guard misses is a silent no-op, which looks identical to "applied but
	// had no effect" - this tells the two apart.
	void dump_patch_status(std::ostream& os);

	// ---- vertex-shader constant shadow ------------------------------------
	// We must NOT use dev->GetVertexShaderConstantF(): under RTX Remix the game
	// talks to a 32-bit BRIDGE that IPCs to the 64-bit host. Bridges are built for
	// one-way command submission; Get* state readback is exactly the sort of thing
	// that is stubbed or unreliable. Symptoms of trusting it: T-posed characters
	// (bone palette read back as identity/garbage), teleporting cloth, flickering
	// buildings - all of which vanish when Remix isn't in the chain, which is the
	// tell. The skinning path reads 126 registers in one call and broke worst.
	//
	// So: shadow every SetVertexShaderConstantF in the d3d9 proxy and read from
	// our own copy. This is authoritative regardless of what the bridge does.
	void on_set_vs_constant_f(UINT start_register, const float* data, UINT vec4_count);
	bool get_vs_constant_f(UINT start_register, float* out, UINT vec4_count);
	extern bool g_use_const_shadow;   // F12 toggles (compare against the bridge)

	// F5: stage B (real View/Proj, needed by Remix) vs stage A (View=identity,
	// Proj=WVP). The two are algebraically identical but NOT numerically: stage B
	// inverts View*Proj (which contains the projection's huge dynamic range) and
	// multiplies back, losing precision in clip-space Z. Since decals with no
	// diffuse sampler are NOT converted, they keep the game's exact WVP - so any
	// extra error we introduce on the base geometry shows up as z-fighting
	// against them. Use this to A/B whether flicker is ours.
	extern bool g_stage_b;

	// Worst RELATIVE |W*V*P - WVP| seen. Must be relative: WVP elements run into
	// the thousands, so an absolute error of ~0.0156 is just ~8 ULPs of float32.
	extern float g_max_wvp_error;

	// Where the World matrix actually came from on the last converted draw.
	enum class world_src { none, constant_c8, engine_ctx, derived_inverse };
	extern world_src g_world_source;
	const char* world_source_name();

	// One-shot diagnostic snapshot of a real converted draw, so we can read the
	// actual matrices instead of guessing which source is wrong.
	struct diag_snapshot
	{
		bool       valid = false;
		D3DXMATRIX wvp{};          // from c0 (in-sync, known good - stage A proves it)
		D3DXMATRIX world_c8{};     // from c8 (in-sync)
		D3DXMATRIX engine_view{};  // GfxContext+608 (record-time - may be stale)
		D3DXMATRIX engine_proj{};  // GfxContext+544 (record-time - may be stale)
		D3DXMATRIX vp_from_data{}; // inverse(world_c8) * wvp  -> the TRUE View*Proj
		D3DXMATRIX vp_engine{};    // engine_view * engine_proj
		float      err_c8 = -1.0f;
		float      err_engine = -1.0f;
		float      err_derived = -1.0f;
		float      err_vp = -1.0f; // |vp_from_data - vp_engine| relative
	};
	extern diag_snapshot g_diag;
	void arm_diagnostic();   // capture on the next converted draw

	// One-shot dump of a real skinned draw. T-pose means EITHER the palette is
	// identity (not uploaded where we think) OR our blend math is wrong - those
	// need opposite fixes and only the raw numbers can tell them apart.
	struct skin_diag
	{
		bool  valid = false;
		float bones[6][12]{};      // first 6 bones, 3 rows x 4 floats each
		int   idx[4]{};            // BLENDINDICES of vertex 0
		float wgt[4]{};            // BLENDWEIGHT of vertex 0
		float src_pos[4]{};        // raw SHORT4N position of vertex 0 (as ints)
		float out_pos[3]{};        // skinned result for vertex 0
		UINT  max_bone_index = 0;  // highest bone index seen this draw
		bool  shadow_had_all = false;
	};
	extern skin_diag g_skin_diag;
	void arm_skin_diagnostic();

	// F3/F2 nudge this. Even with exact matrices FF concatenates W*V*P itself
	// while the game's shader uses a pre-multiplied WVP; they differ by ~1 ULP,
	// enough to z-fight coplanar decals we don't convert.
	extern float g_depth_bias;

	// stats for the on-screen/report side
	extern std::uint32_t g_converted_draws;
	extern std::uint32_t g_shadow_vbs;

	// Where draws actually go. Stop guessing; count them.
	struct reject_stats
	{
		std::uint32_t seen = 0;
		std::uint32_t no_decl = 0;
		std::uint32_t not_static = 0;   // skinned / not SHORT4 position
		std::uint32_t no_diffuse = 0;   // lighting/AO/projector pass
		std::uint32_t no_stream = 0;
		std::uint32_t no_scale_or_vb = 0;
		std::uint32_t no_c8 = 0;
		std::uint32_t world_singular = 0;
		std::uint32_t ortho = 0;        // decompose_vp said the pass is orthographic
		std::uint32_t bad_reconstruct = 0; // decomposed but W*V*P != WVP
		std::uint32_t converted = 0;

		// Skinned path: 112k draws match the character declaration but still end up
		// in not_static, i.e. try_render_skinned() is bailing. Find out where.
		std::uint32_t skin_seen = 0;
		std::uint32_t skin_no_diffuse = 0;
		std::uint32_t skin_no_stream = 0;
		std::uint32_t skin_no_wvp = 0;
		std::uint32_t skin_no_camera = 0;   // ortho / decompose failed
		std::uint32_t skin_no_dynvb = 0;
		std::uint32_t skin_lock_fail = 0;
		std::uint32_t skin_ok = 0;
	};
	extern reject_stats g_rej;

	// Histogram of the vertex declarations we REJECT as not_static, so we can see
	// what those ~37% of draws actually are instead of guessing. (Suspect: the
	// FP32 float3 position format - classify() only accepts SHORT4.)
	void dump_rejected_formats(std::ostream& os);

	void shutdown();

	// True once the engine's real View/Projection have been captured from
	// GfxContext (stage B). Until then we fall back to the stage-A camera.
	bool g_have_matrices();

	// Pixel-shader -> diffuse texture stage registry.
	// AC2 does NOT put the diffuse at s0 consistently: across the 1362 asset
	// pixel shaders, s0 is DiffuseMap_0 (185), Diffuse_0 (102), Layer0Diffuse_0
	// (86) ... but also NormalMap_0 (99) and Layer0Normal_0 (26). Lighting/AO/
	// projector passes have no diffuse sampler at all (g_WorldLightMapSampler,
	// g_DepthSampler, g_ProjectorShadow, ...).
	// ac2_dump parses each PS constant table at creation and registers the stage
	// here; a PS with no diffuse registers -1 and we then refuse to convert that
	// draw, which is what keeps AO/lighting passes out of the FF path.
	void register_ps_diffuse_stage(IDirect3DPixelShader9* ps, int stage);
	int  get_ps_diffuse_stage(IDirect3DPixelShader9* ps); // -1 = unknown / none

	// Returns true if the draw was fully handled (caller must NOT draw again).
	bool try_render_fixed_function(IDirect3DDevice9* dev,
		D3DPRIMITIVETYPE PrimitiveType, INT BaseVertexIndex, UINT MinVertexIndex,
		UINT NumVertices, UINT startIndex, UINT primCount);
}
