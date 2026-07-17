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

	// F3 test toggle: route skinned meshes to Remix's VERTEX CAPTURE instead of our
	// CPU-skinning + fixed-function path. When ON, a skinned draw is neither
	// converted nor dropped - the original shader draw runs and Remix captures its
	// (GPU-skinned) output, so we can A/B how it looks and performs. g_skin_passthrough
	// is the per-draw signal to the renderer that THIS draw must pass through the
	// FF-ONLY drop; it is reset at the top of try_render_fixed_function.
	extern bool g_skin_vertex_capture;
	extern bool g_skin_passthrough;

	// Game-side anti-culling. ALWAYS ON (applied on the first draw).
	//
	// The DELETE toggle was removed - the key was needed elsewhere and it had not
	// been used in a long time. set_anticulling() is kept, so anyone wanting to A/B
	// the patch can call it; the report still prints whether it is applied, and
	// dump_patch_status() still prints the live bytes at each site.
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

	// Hardware OCCLUSION culling disabled by zeroing the visible-pixel threshold
	// (dword_1DD1828, normally 25). PAGE UP toggles; DEFAULT ON.
	//
	// THIS IS THE CULLING SYSTEM THAT ACTUALLY MATTERED. Turning it off brings the
	// missing objects back on its own, and makes the CullAABB patch (PAGE DOWN)
	// unnecessary - which is a straight win, because CullAABB is the one that
	// crashes during loading. Prefer this; leave CullAABB off.
	//
	// This is a SEPARATE system from the frustum culling above: AC2 issues real D3D9
	// occlusion queries and then drops the draw for objects showing fewer than 25
	// pixels, BEFORE submission - so Remix never sees anything the game thinks is
	// occluded (behind a building, inside geometry). That is the classic path-tracer
	// failure mode and rtx.antiCulling.* cannot reach it.
	//
	// It is a data write, so unlike the CullAABB patch it carries no torn-byte risk.
	// See the long comment at the patch site before changing the site or polarity -
	// the branch semantics are inverted from the intuitive reading.
	extern bool g_occlusion_off;
	void set_occlusion_disabled(bool enable);

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

	// ---- texture / sampler diagnostic ------------------------------------
	// Textures render at their LOWEST MIP through the FF path (F7 off => sharp,
	// so it is ours). The leading suspect is that we move the diffuse TEXTURE
	// from its stage to stage 0 but leave stage 0's SAMPLER STATE as the game
	// set it for its own stage-0 texture - a clamp upstream of anything Remix
	// can bias.
	//
	// DO NOT theorise further on this; a cycle was already burned that way.
	// These are the numbers that separate the three candidate fixes:
	//   sampler clamp   -> MAXMIPLEVEL/MIPFILTER differ between the two stages
	//   texture LOD     -> tex_lod > 0 (game-side streaming, a DIFFERENT problem)
	//   neither         -> look elsewhere
	//
	// Two snapshots are captured, and the PAIR is the actual experiment:
	//   [0] a draw whose diffuse already sits at stage 0 (we move nothing)
	//   [1] a draw whose diffuse sits elsewhere (we move the texture, not the state)
	// If only [1] is clamped, the sampler move is the bug. If [0] is clamped too,
	// the sampler theory is DEAD - the cause is common to every converted draw.
	struct tex_diag
	{
		bool  valid = false;
		int   diffuse_stage = -1;

		// Sampler state as the GAME left it, read live on a real converted draw.
		// src = the stage the diffuse actually lives on; dst = stage 0, where FF
		// samples from. Equal on snapshot [0] by definition.
		struct samp
		{
			DWORD max_mip_level = 0;   // D3DSAMP_MAXMIPLEVEL: >0 CLAMPS to a coarser mip
			DWORD mip_filter = 0;      // D3DSAMP_MIPFILTER: NONE => mip 0 only, no chain
			float mip_lod_bias = 0.0f; // D3DSAMP_MIPMAPLODBIAS (stored as a float bitpattern)
			DWORD min_filter = 0;
			DWORD mag_filter = 0;
		};
		samp src{};   // sampler on diffuse_stage
		samp dst{};   // sampler on stage 0

		// The diffuse texture itself. GetLOD() is only meaningful for MANAGED
		// textures (it returns 0 for every other pool), so the pool is printed
		// alongside it - otherwise "lod 0" reads as proof of nothing.
		bool  have_tex = false;
		DWORD tex_lod = 0;         // GetLOD(): most detailed mip the GAME allows
		DWORD tex_levels = 0;      // GetLevelCount(): mips that exist
		UINT  tex_width = 0;       // level 0 dimensions - a tiny level 0 means
		UINT  tex_height = 0;      // the sharp mips were never uploaded (streaming)
		DWORD tex_pool = 0;
		DWORD tex_format = 0;

		// How the stage was picked, so a bad choice is visible as a bad choice.
		int  rank = -1;
		char name[64]{};

		// EVERY stage the game had bound at this draw. The point: if a 1024x1024
		// sits at a stage we passed over while we bound an 8x8, we picked wrong -
		// and that is visible here without any further theorising.
		struct stage_tex
		{
			bool  bound = false;
			UINT  width = 0;
			UINT  height = 0;
			DWORD levels = 0;
			DWORD format = 0;
		};
		stage_tex stages[8]{};
	};
	extern tex_diag g_tex_diag[2];   // [0] stage==0 draw, [1] stage!=0 draw
	void arm_tex_diagnostic();

	// F3/F2 nudge this. Even with exact matrices FF concatenates W*V*P itself
	// while the game's shader uses a pre-multiplied WVP; they differ by ~1 ULP,
	// enough to z-fight coplanar decals we don't convert.

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

		// Draws whose declaration spreads POSITION/NORMAL/TEXCOORD across more than
		// one stream (or puts POSITION somewhere other than stream 0). These used to
		// be rejected as not_static and - in FF-ONLY mode - deleted outright, which
		// looked like a culling bug. Counted separately because they take the slower
		// per-draw gather path instead of the cached shadow VB.
		std::uint32_t multistream = 0;

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
	// How the diffuse stage was CHOSEN, not just which one won. The matcher ranks
	// 0 = name contains "diffuse", 1 = "basetexture", 2 = "layer0",
	// 3 = UNRECOGNISED - a guess ("lowest sampler that isn't provably wrong").
	// Rank 3 exists because AC2's material editor emits procedural sampler names
	// (Operator6_0, ...) and rejecting them cost 49% of draws. But a guess can
	// land on a detail/mask sampler, and then we bind an 8x8 texture and stretch
	// it over a wall - which reads as "blurry", not as "wrong texture".
	// Keeping the rank lets the report separate the two populations.
	// Rank 4 = NORMAL-ONLY material: the PS has no diffuse sampler at ANY stage,
	// but it does have a normal map. That is not a lighting/AO pass - it is a real
	// surface that genuinely has no albedo. AC2's water is exactly this:
	// `NormalMap_0@s0` + `g_DepthSampler@s8` (+ reflection/projector), and nothing
	// else. The matcher used to return -1 for it, so every water draw was refused
	// as `no_diffuse` and FF-ONLY mode deleted the water from the scene.
	//
	// Only 33 of the game's 1362 pixel shaders match, across 3 templates:
	// AC2_VolumeFog_DO_NOT_USE (20), AC2_Water_DO_NOT_USE (11) and
	// AC1_GFX_Water_Puddle (2). The other 109 no-diffuse shaders have no normal map
	// either and stay refused - they are the genuine AO/depth passes.
	//
	// > Water and VolumeFog are INDISTINGUISHABLE by sampler signature - identical
	// > down to the register. They are the same shader family (fog is a water-derived
	// > template); only their non-sampler constants differ (fog declares
	// > `WaterOpacity_*`/`DepthFadeControl_*`, water declares `g_PickingID`). We
	// > deliberately do NOT try to tell them apart here: Remix categorises by TEXTURE
	// > HASH, and their normal maps are different textures, so they can be tagged
	// > independently in `rtx.conf` (water -> `rtx.animatedWaterTextures`, fog ->
	// > whatever suits). Letting both through with a hashable texture is what makes
	// > that possible; deciding here would only take the choice away.
	//
	// The bound texture is the NORMAL MAP, used purely as an identity for Remix to
	// hash - NOT as a normal map. Remix's legacy material has no normal slot at all
	// (see "Open problems"), so its blue-ish pixels become the albedo until a
	// replacement material is authored. That is the accepted cost of getting water
	// geometry into the path tracer.
	struct ps_diffuse_info
	{
		int  stage = -1;
		int  rank = -1;      // -1 = never registered
		char name[64]{};     // the winning sampler's name
		bool normal_only = false;
	};

	// Convert normal-only (water/fog) materials. Default ON; F2 toggles.
	extern bool g_normal_only_materials;
	extern std::uint32_t g_normal_only_draws;
	void register_ps_diffuse_stage(IDirect3DPixelShader9* ps, int stage,
		int rank = -1, const char* name = nullptr, bool normal_only = false);
	int  get_ps_diffuse_stage(IDirect3DPixelShader9* ps); // -1 = unknown / none
	ps_diffuse_info get_ps_diffuse_info(IDirect3DPixelShader9* ps);

	// Level-0 width of the diffuse we actually bind, histogrammed over EVERY
	// converted draw and SPLIT BY RANK. Two arbitrary draws showing an 8x8
	// texture proves nothing on its own; this says whether tiny textures are
	// typical, and whether they track the GUESSED rank-3 matches specifically.
	// If rank 0 binds 512/1024 and rank 3 binds 8/64, the matcher is the bug.
	// If both are tiny, the matcher is exonerated - look elsewhere.
	struct diffuse_size_stats
	{
		// buckets by level-0 width: <=8, <=32, <=64, <=128, <=256, <=512, >512
		static constexpr int NBUCKET = 7;
		std::uint32_t by_rank[6][NBUCKET]{};  // [rank 0..4, 5 = unregistered][bucket]
		std::uint32_t rank_total[6]{};
	};
	extern diffuse_size_stats g_diffuse_sizes;
	void dump_diffuse_sizes(std::ostream& os);

	// ---- vegetation (RealTree / DistanceClutter) ----------------------------
	// 141 of the game's 5642 vertex shaders declare NO dcl_position at all: the
	// geometry is generated procedurally in the VS from a compressed skeleton.
	// classify() therefore rejects them as not_static, and in FF-ONLY mode that
	// DELETES every tree from the scene (~1.6M draws/run).
	//
	// The marker is exact, and was measured across the whole asset set rather
	// than assumed: the 141 shaders with no dcl_position are EXACTLY the 141
	// that declare `CompressionParams` (c120). Zero exceptions either way.
	//
	// They are NOT one shared vertex path - they split 47/47/47 into three
	// families with genuinely different geometry generation:
	//   trunk   - `TrunkStencil` (c150x56): a swept cylinder around a skeleton
	//   leaves  - `LeavesEquations` (c214x6): camera-facing billboards
	//   clutter - neither: rotated grass blades, base position on TEXCOORD9
	// The family is recovered from the VS constant table at CreateVertexShader,
	// exactly like the PS diffuse stage and the PS light counts - a draw-time
	// guess is not possible and not needed.
	//
	// Base position is TEXCOORD4 for trunk/leaves but TEXCOORD9 for clutter, and
	// the REGISTER carrying it varies (v3/v4/v6/v8) across variants. Select it by
	// SEMANTIC. (An earlier note recorded "position = v3 = TEXCOORD4" from one
	// sample; that is only true of the two families that happen to be live.)
	//
	// Only trunk and leaves appear at runtime: the two vegetation declarations in
	// the report carry [s1 SHORT4N TEXCOORD4] and no TEXCOORD9/10, and they occur
	// an IDENTICAL number of times (797,960 each) - one trunk draw and one leaves
	// draw per tree.
	enum class veg_kind : int
	{
		none = 0,     // not a vegetation shader (has a real dcl_position)
		trunk,        // TrunkStencil
		leaves,       // LeavesEquations
		clutter,      // CompressionParams but neither: grass, TEXCOORD9 base
	};
	void     register_vs_info(IDirect3DVertexShader9* vs, veg_kind kind, std::uint32_t hash);
	veg_kind get_vs_veg(IDirect3DVertexShader9* vs);

	// ---- per-shader coverage --------------------------------------------
	// "Is anything still missing?" cannot be answered from the totals: no_diffuse
	// is ~45% of all draws and is MOSTLY CORRECT (depth/shadow/AO passes that must
	// not be path-traced). Water hid inside that counter for the entire project -
	// a real surface, invisible, and indistinguishable from a legitimate rejection.
	//
	// So account for every draw's outcome PER VERTEX SHADER instead. A shader with
	// many draws and 0% converted is the signature of something missing, and it
	// surfaces without anyone having to walk past it.
	//
	// The bytecode hash makes the table nameable offline: live dumps are
	// byte-identical to the MaterialTemplate assets, so an FNV-1a 32 of each asset
	// .dxbc maps a hash back to a template name. See tools/name_live_shaders.py.
	void dump_vs_coverage(std::ostream& os);

	// Convert trunk draws on the CPU. Toggled so a trunk-shaped bug can be told
	// apart from the rest of the FF path with one keypress, the way INSERT
	// separates "can't convert" from "doesn't exist".
	extern bool g_veg_trunks;
	extern bool g_veg_leaves;

	// Per-family draw accounting, so "vegetation still missing" can be told apart
	// from "we never saw a trunk draw". Every bail-out on this path increments
	// something: a silent early-return is indistinguishable from "nothing found".
	struct veg_stats
	{
		std::uint32_t seen[4]{};        // indexed by veg_kind
		std::uint32_t trunk_converted = 0;
		std::uint32_t trunk_no_decl = 0;    // no TEXCOORD4/5/2/1 of the right type
		std::uint32_t trunk_no_const = 0;   // a required constant was never uploaded
		std::uint32_t trunk_no_diffuse = 0;
		std::uint32_t trunk_no_vb = 0;
		std::uint32_t trunk_no_camera = 0;
		std::uint32_t stencil_clamped = 0;  // stencil index outside [0,55]

		std::uint32_t leaf_converted = 0;
		std::uint32_t leaf_no_decl = 0;
		std::uint32_t leaf_no_const = 0;
		std::uint32_t leaf_no_diffuse = 0;
		std::uint32_t leaf_no_vb = 0;
		std::uint32_t leaf_no_camera = 0;
		std::uint32_t leaf_eq_clamped = 0;  // LeavesEquations index outside [0,6)
	};
	extern veg_stats g_veg;
	void dump_veg(std::ostream& os);
	void dump_normal_only(std::ostream& os);

	// Is the shadow-VB cache serving wrong geometry, and can the dynamic pool
	// actually race? Both are open suspects for "faces randomly disappear";
	// both are measured rather than assumed. See ac2_ff.cpp for what each counter
	// distinguishes.
	void dump_cache_diag(std::ostream& os);

	// Returns true if the draw was fully handled (caller must NOT draw again).
	bool try_render_fixed_function(IDirect3DDevice9* dev,
		D3DPRIMITIVETYPE PrimitiveType, INT BaseVertexIndex, UINT MinVertexIndex,
		UINT NumVertices, UINT startIndex, UINT primCount);
}
