<h1 align="center">Assassin's Creed II — RTX Remix Compatibility Mod</h1>

<div align="center">

A work-in-progress compatibility mod that makes **Assassin's Creed II (PC)** render through
NVIDIA's [RTX Remix](https://github.com/NVIDIAGameWorks/rtx-remix).

Built on [xoxor4d/remix-comp-base](https://github.com/xoxor4d/remix-comp-base).

**Status: Remix path-traces the game. Geometry, camera, textures, characters, vegetation and
water all reach the path tracer. Lighting is flat — Remix's legacy material has no normal-map slot
at all, which is now a measured dead end rather than a guess. The game's own lights are decoded but
have never been seen on screen. Not finished — see [Open problems](#open-problems).**

</div>

---

## What this is

AC2 is a 32-bit, shader-based Direct3D 9 game (Ubisoft's *Scimitar* engine, later renamed Anvil).
RTX Remix natively understands **fixed-function** D3D9 draw calls. It can also try to capture
vertices out of vertex shaders, but on AC2 that path **flickers** and, when mixed with the
fixed-function draws, **puts geometry in the wrong place** — vertex-captured draws carry the game's
shader-space transform while our FF draws use the analytically-reconstructed camera, and Remix's
single scene camera can't satisfy both (static meshes shift vertically). (Note: characters captured
this way *do* animate correctly — an earlier claim that they came out T-posed was wrong; Remix
captures the GPU-skinned output. The blockers are flicker + the camera-space mismatch, not T-pose.)

So this mod hooks D3D9 and **re-issues AC2's draws through the fixed-function pipeline**: it
decodes the game's compressed vertex formats into plain float vertices, reconstructs a real
View/Projection camera, skins characters on the CPU, and hands Remix ordinary static geometry.

The same approach GTA IV's Remix mod uses. Their README frames fixed-function as a *performance*
choice over vertex capture; on AC2 it turned out to be a *correctness* requirement.

### What works

- Static world geometry — correct placement, scale, and diffuse texture
- A real, stable per-pass View/Projection camera (analytically decomposed, exact to ~1e-7)
- Characters, via CPU skinning (bone palette decoded, 4-bone blend)
- Cloth / soft-body (dynamic vertex buffers converted per-frame)
- Game-side anti-culling patches (always on), so off-screen geometry still reaches the path tracer
- **Occlusion-cull defeat** — the thing that was actually making objects disappear
- **Multi-stream declarations** — meshes whose attributes span stream 0 and 1
- **Vegetation** — trunks and leaves, CPU ports of the RealTree vertex paths (no `dcl_position` at
  all; the VB holds a skeleton the shader sweeps). Both confirmed in-world, leaves animate.
- **Water** — reaches Remix as of the *normal-only material* fix. It renders **purple** (its normal
  map is being used as albedo) until it is tagged Remix-side; see [Water](#water--a-real-surface-with-no-albedo-rank-4-normal-only).

### What doesn't, yet

Most of what is still rejected is rejected *correctly*: `no_diffuse` is ~45% of all draws and is
genuinely depth/shadow/AO passes that must not be path-traced. **"% of draws converted" is a
misleading number** — pick a denominator that maps to what you see on screen.

The vegetation gap is **closed**. What remains:

- **The game's lights have never been seen.** Extraction works (79 PS variants, 4.6k omni + 1.4k
  direct slots, `const_miss: 0`); the Remix API has simply never been initialised successfully.
  This is the next task.
- **Lighting is flat.** Not fixable from this side: Remix's legacy material holds exactly one
  texture (albedo) — measured, not assumed.
- **Whatever the coverage table finds.** Eyeballing no longer works; use
  [Finding what's still missing](#finding-whats-still-missing).

Textures are **sharp** as of the UV-scale fix (see
[Texture coordinates](#texture-coordinates--short2n-and-the-shader-applies-a-16-you-must-copy)).

---

## Install

1. Install [RTX Remix](https://github.com/NVIDIAGameWorks/rtx-remix) for AC2 (32-bit bridge).
2. Copy everything in `assets/` into the game directory. Rename the Ultimate ASI Loader DLL to
   match a DLL the game imports if needed.
3. Put `remix-comp-base.asi` in a `plugins/` folder in the game directory.

Tested against `AssassinsCreedIIGame.exe`, md5 `2dd4d3c196fa270945826d2faab42007`.
**The code patches are byte-verified before writing**, so on a different build they no-op
rather than corrupting the game.

## Hotkeys

| Key | Does |
|---|---|
| `INSERT` | FF-only mode — drop every draw we can't convert. **Default ON** |
| `PAGE UP` | **Disable the game's hardware occlusion culling** (visible-pixel threshold 25 → 0). **Default ON.** This is the one that matters — see below |
| `PAGE DOWN` | CullAABB → always "intersecting". **Default OFF** — *can crash during loading*, and is **no longer necessary**: `PAGE UP` brings the objects back on its own |
| `HOME` | Submit game lights to Remix — a true **ON/OFF** (toggling off destroys live lights, so it A/Bs against Remix's fallback sun). **Default OFF** — needs `exposeRemixApi = True` in `<game>/.trex/bridge.conf` |
| `END` | CPU-generated vegetation **trunks**. **Default ON** |
| `F11` | CPU-generated vegetation **leaves**. **Default ON** |
| `F2` | Convert **normal-only** materials (water / volume fog). **Default ON** |
| `F1` | CPU skinning |
| `F7` | Fixed-function conversion |
| `F6` | Wireframe (spot converted meshes) |
| `F5` | Camera stage A/B |
| `F8` | Write report now (also auto-flushes every 15s) |
| `F9` | Toggle sampling |
| `F12` | Read constants from our shadow vs. the bridge |

> `F4` is unusable — it opens an un-closable game debug menu. Don't bind it.

The report lands in `<game>\ac2_rtx_dump\posw_report.txt`, alongside dumped shaders. It prints
which toggles were active, draw dispositions, matrix/skinning diagnostics, and the live bytes at
every patch site. **Read the header before drawing conclusions from a run** — see
[Lessons](#lessons-learned-the-hard-way).

## Building

```
git clone --recurse-submodules <this repo>
generate-buildfiles_vs22.bat
msbuild build\remix-comp-base.sln /p:Configuration=Release /p:Platform=Win32
```

Optionally set `REMIX_COMP_ROOT` (game dir) and `REMIX_COMP_ROOT_EXE` before generating to
auto-deploy. Release builds treat warnings as errors. Premake globs `src/comp/**.cpp`.

The AC2-specific code is entirely in:

- `src/comp/modules/ac2_ff.{hpp,cpp}` — the fixed-function conversion (the mod proper)
- `src/comp/modules/ac2_dump.{hpp,cpp}` — instrumentation, shader dumping, the report
- `tools/shaderdis/` — offline x86 shader disassembler for the extracted game assets
- `tools/name_live_shaders.py` — names the shaders the game ACTUALLY runs, by hashing the
  MaterialTemplate assets (see [Finding what's still missing](#finding-whats-still-missing))

---

# Reverse engineering notes

This is the part worth reading if you want to improve on this. All addresses are **RVAs** — the
IDB is based at 0, so runtime address = `GetModuleHandleA(nullptr) + RVA`.

Two IDA databases were used: the Windows exe (stripped; ~11.5k named symbols, mostly Wwise audio
exports) and the **Mac/Feral port** (`scimitar_sf`), which is fully symbolized with
Itanium-mangled `scimitar::` names. The Mac build reuses the same class names as the D3D9 build
(including, confusingly, `OGLState` / `OGLBaseRenderer` on Windows), so Mac symbols map onto the
Windows binary almost 1:1. That's how most of the below was found.

## Engine architecture

AC2 renders through a **deferred D3D9 command buffer**: worker threads record, the render thread
plays back. `D3D9DeviceWrapper+0x04` is the record-vs-direct flag.

**This has a major consequence:** engine memory read at D3D-draw time is *stale* — the recording
already moved on. Anything that changes per-object must come from data that rides the command
buffer *with* the draw (i.e. shader constants), not from engine structs.

Playback dispatch table: `g_D3D9PlaybackDispatchTable` @ `0x1dd1c30`, ~26 opcodes, every D3D9
call funnels through it. Notably **`SetStreamSourceFreq` is absent** ⇒ no hardware instancing ⇒
**one character = one draw call**. That's unusual and good for Remix; HW instancing is normally
what breaks it.

## Shader constants

Constant names resolve to registers through a two-tier static system:

- `g_ShaderConstantNameTable` @ `0x1de04b8` — 310 fixed 128-byte name strings, **indexed by
  semantic constant ID**
- `scimitar::OGLState::SetVectorVS` @ `0x11fbbf0` — a big switch mapping constant ID → hardware
  register

The registers that matter, all verified against live shader constant tables:

| Constant | ID | Register |
|---|---|---|
| `g_WorldViewProj` | 149 | **VS c0..c3** |
| `g_World` | 305 | **VS c8..c11** |
| `g_BoneMatrixArray` | 19 | **VS c120..c245** (126 regs = 42 bones × 3) |
| `g_WorldToView` (View) | 307 | VS c6 |
| `g_ViewToWorld` | 298 | PS c2 |

Registers **alias** across IDs (per-shader-variant allocation) — a slot is only meaningful for
shaders that declare that constant. There is no Projection constant at all; it's CPU-side only.

Matrices are **transposed on upload** (`Matrix44_TransposeCopy` @ `0x11fc820`), so a constant
read back needs transposing again for D3D's row-vector `SetTransform`.

> Verified across the whole game asset set: **all 5501 vertex shaders with `dcl_position v0`
> bind `g_World`, always at c8** — 100% coverage. And **all 1134 skinned VS use c120, size 126.**

## Vertex formats

Three position encodings. All three confirmed against shader disassembly *and* live data:

**1. Generic static meshes — `SHORT4`**

```asm
def c4, 3.81481368e-006, -127, 0.00787401572, 1   ; c4.x = 1/262136 = 1/(32767*8)
mul r0.x,   c4.x, v0.w         ; K * w
mul r0.xyz, r0_abs.x, v0       ; position = |K*w| * v0.xyz    <- note ABS
```

⇒ `position = (xyz / 32767) * (|w| / 8)`. `w` is stored pre-multiplied by 8, so every observed
`|w|` is a multiple of 8. **`sign(w)` is tangent handedness only** — recovered via two `slt`s and
multiplied into `cross(N,T)` to make the binormal. Position uses `abs`, so the sign never affects
geometry. Since `|w|` is constant per mesh, the scale folds into the World matrix.

**2. Characters — `SHORT4N`**

```asm
def c5, 3, 16, 0, 1
mul r3, c5.x, v2 / mova a0, r3          ; a0 = boneIndex * 3
mad r3, v0.xyzx, c5.yyyz, c5.zzzw       ; pos = (xyz*16, 1)   <- fixed scale 16
dp4 r4.x, r3, c120[a0.x] / c121[a0.x] / c122[a0.x]
```

⇒ hardware normalizes `/32768`, shader applies a **fixed scale of 16**, `w` is forced to 1.
**`v0.w` is ambient occlusion, not position.** Bone indices (`UBYTE4`) and weights (`UBYTE4N`)
are separate, later elements.

**3. FP32 `FLOAT3`** — uncompressed, scale 1.

### Texture coordinates — `SHORT2N`, and the shader applies a ×16 you must copy

Positions are not the only thing AC2 quantizes. **`TEXCOORD0` is `SHORT2N`, and the vertex
shader multiplies it by a literal `16`** before writing the UV output:

```asm
; static  (VertexShader_..._0x0B6C41CD70642B52, SHORT4 position)
def c5, 16, 0, 0, 0
mul o2, c5.xxyy, v1.xyxx        ; uv = 16 * v1.xy

; skinned (VertexShader_Characters-Skin_0x05101CD460392E08_...)
def c5, 3, 16, 0, 1             ; x = bone stride, y = 16
mul o1, c5.yyzz, v3.xyxx        ; uv = 16 * v3.xy
```

D3D normalizes `SHORT2N` to `short/32767`, so the real UV is `short * 16/32767` — the
**2048 quantization scale** (`32768/16 = 2048`). UVs legitimately run to ±16, i.e. tiled
surfaces genuinely tile up to 16×.

Decoding to `short/32767` and stopping makes every UV **16× too small**: the texture is
magnified 16×, one small patch stretched over the whole surface. That looks SOFT, and it was
recorded for two sessions as "textures stuck at the lowest mip" — see
[Lessons](#lessons-learned-the-hard-way), because the misnomer is what made it unsolvable.

> **Do not apply this scale blanket.** Across the assets the UV scale is bimodal: **2017 VS use
> ×16, 1569 use ×1**, and no shader mixes them. The ×1 shaders are the **`FLOAT2`-UV variants** —
> variants are compiled per vertex declaration, so the scale is baked to match the UV *type*.
> Scale `SHORT2N` only; leave `FLOAT2` alone. The decisive count: of static SHORT4 meshes that
> actually sample a diffuse, **889 use ×16 and none uses ×1**. The 423 apparent exceptions are
> `mov o1, r1` depth passes that output clip position to TEXCOORD0 and have no UV at all — the
> same passes `no_diffuse` already rejects.

> **The `16` is overloaded — read the register, not the number.** In character shaders
> `def c5, 3, 16, 0, 1` uses `c5.y = 16` as the **position** scale and `c5.x = 3` as the bone
> stride. An asset-wide grep for "×16" therefore "confirms" a UV scale on skinned shaders using
> the *position* math, without the UV question ever being asked. The UV ×16 on characters is real
> (`mul o1, c5.yyzz, v3.xyxx`) but has to be established from the texcoord output, independently.

**4. Multi-stream.** AC2 splits a mesh's attributes across stream 0 and stream 1. `classify()`
originally filtered `if (e[i].Stream != 0) continue`, so these were rejected as `not_static` —
and in FF-ONLY mode that means **deleted from the scene**, which reads as a culling bug.
Fixing it recovered **~165k draws/run**. Attributes are now GATHERED across streams
(`vtx_streams` / `with_streams`), and such draws take the per-draw path because the shadow-VB
cache is keyed on a single source buffer.

> **The trap:** `note_rejected()` had the *same* stream-0 filter, so it logged those draws
> **minus their position element** — making them look like exotic position-less declarations.
> The instrumentation was reproducing the very blind spot that caused the rejection. The tell:
> every VS dumped at runtime declares `dcl_position`, so D3D *must* have been feeding POSITION
> from somewhere.

**5. Vegetation / RealTree — genuinely position-less (STILL UNCONVERTED).**
`AC2_Tex1_DistanceClutter` (and the same vertex path in `AC2_Tex2_PixelLit`,
`AC2_Tex1_PixelLit*`, `AC2_Tex1_VertexLit`, `Default Material Template` — 141 VS in total, one
shared path). There is **no `RealTree` material template**; `.RealTreeMesh` is a geometry asset.
Real runtime declarations:

```
[s0 D3DCOLOR TC0][s0 D3DCOLOR TC1][s0 FLOAT1 TC2][s0 D3DCOLOR TC3][s1 SHORT4N TC4][s1 D3DCOLOR TC5]
[s0 SHORT4 TC0][s0 FLOAT4 TC1][s0 D3DCOLOR TC2][s0 SHORT2N TC3][s1 SHORT4N TC4][s1 SHORT4N TC5]
```

**Every element is TEXCOORD — there is no `D3DDECLUSAGE_POSITION` at all.** The geometry is
generated procedurally in the VS from a compressed skeleton, decompressed by `CompressionParams`
(c120):

```asm
mad r0.xyz, vN, c120.y, c120.x     ; vN = base position (SHORT4N)
```

**The marker is exact.** Measured over all 5642 asset VS: the shaders with no `dcl_position` are
*exactly* the shaders that declare `CompressionParams` — 141 ≡ 141, zero exceptions either way.
That biconditional is what the runtime family detection keys on.

**It is THREE families, not one path** (an earlier note here claimed "one port covers all 141
shaders" — that is wrong). They split exactly 47 / 47 / 47:

| Family | Marker | Geometry | Base position |
|---|---|---|---|
| **trunk** | `TrunkStencil` @ c150x56 | swept cylinder around a skeleton | TEXCOORD4 |
| **leaves** | `LeavesEquations` @ c214x6 | camera-facing billboards from `v0.xy` corners | TEXCOORD4 |
| **clutter** | neither | rotated grass blades | **TEXCOORD9** |

> **Read the semantic, not the register.** The `mad` above reads `v3` in leaves, `v3`/`v4` in
> trunks, and `v4`/`v6`/`v8` in clutter — while the *semantic* is stable per family. An earlier
> note recorded "position = `v3` = TEXCOORD4" from a single sample; that happens to hold only for
> the two families that are live. Same lesson as `D3DDECLUSAGE_POSITION`: select by semantic.

**Only trunk and leaves are live.** Both runtime declarations carry `[s1 SHORT4N TEXCOORD4]` and
neither carries TEXCOORD9/10, so the 47 clutter shaders never run in what we've tested. The two
decls occur an **identical** number of times (797,960 each in report #40) ⇒ one trunk draw and one
leaves draw per tree. The real target is 94 shaders in 2 families.

The family cannot be guessed at draw time, and doesn't need to be: it is read off the **VS constant
table at `CreateVertexShader`**, exactly like the PS diffuse stage and the PS light counts.
`g_World` (c8) and `g_WorldViewProj` (c0) are present as usual, and the generated positions are
**model space** (the shader does `dp4 o0, r0, c0`), so the existing camera path works unchanged and
there is no `|w|` scale to fold — `CompressionParams` does that job here.

### Trunks — the vertex path, decoded

The VB holds no geometry, only a skeleton: a spine point plus a radius, swept into a ring whose
cross-section comes from `TrunkStencil[]`.

```
base   = TEXCOORD4.xyz * CompressionParams.y + CompressionParams.x
radius = TEXCOORD4.w   * CompressionParams.z + 1e-6
ring   = trunc(dot(TEXCOORD1.xyz, OffsetInStencil.xyz))     ; spelled out with frc/slt, not round
t1     = cross(TEXCOORD5, TEXCOORD2)                        ; TEXCOORD5 = spine axis
t2     = cross(TEXCOORD5, t1)
dir    = TrunkStencil[ring].x * t1 + TrunkStencil[ring].y * t2
pos    = base + radius * dir                                ; * distance morph + LOD gate
normal = normalize(radius * dir)                            ; the radial direction
uv     = TEXCOORD0.xy * TrunkUVDecompression.y + TrunkUVDecompression.x
```

`TEXCOORD0` is `SHORT4`, **not** `SHORT4N` — the VS reads raw integers and `TrunkUVDecompression`
(c211) carries the whole scale, so there is no `/32767` here. (Contrast the static path, where
`SHORT2N` UVs are hardware-normalised and the shader applies a literal ×16.)

> ### The live shader is the authority, not the asset corpus
>
> The first version of this port read **one** trunk shader out of the 47 in the assets — picked with
> `grep -l TrunkStencil | head -1` — and generalised it. That variant is one of only **6 of 47**
> that build a procedural world-space UV, and one of the few that apply a `VFalloff`/`VDistScale`
> distance fade. **All four trunk shaders the game actually creates do neither.** The cost was two
> visible bugs at once:
>
> - **Broken UVs** — a world-space planar projection where the real shader decompresses TEXCOORD0.
> - **Jitter** — the fade read `c100`/`c101`, which *no live trunk shader declares*. Registers
>   **alias** across variants, so those reads returned whatever unrelated shader ran last, and the
>   result was differenced against the eye position and subtracted from every vertex's Z. Geometry
>   that moves with the camera *and* with whatever drew before it: a jitter that comes and goes.
>
> The runtime dumps every shader it creates to `<game>\ac2_rtx_dump\shaders`. **Cross-check there
> before believing any variant-specific detail** — 47 assets can outvote the 4 that run. Corollary:
> only read constants the live shader *declares*. An undeclared register is not empty, it is
> someone else's data.

Implemented in `ac2_ff.cpp` (`try_render_trunk` / `decode_trunk_vertex`), toggled by `END`
(`g_veg_trunks`), counted in the report's **VEGETATION** section.

**Measured (report #19, trunk-only):** trunk draws 406,473 — *identical* to the leaves count, one
pair per tree; clutter **0**, confirming grass never runs; `trunk_no_decl` **0**, so the decl↔family
mapping is right; `stencil_clamped` **0**, so the ring index always lands in `[0,56)`; and
131,497 converted + 274,976 `no_diffuse` = 406,473 exactly, so every trunk draw is accounted for.
**Trunks are confirmed correct and stable in-world.**

### Leaves — the vertex path, decoded

A leaf is a card that **morphs with distance** between an object-space offset and a camera-facing
billboard, then has wind added. Read from the four *live* leaves shaders, which are unanimous:

```
base   = TEXCOORD4.xyz * CompressionParams.y + CompressionParams.x
eye_l  = the eye in tree-local space                     ; see the yaw-only note below
size   = clamp(dist, eq.x, eq.y) * eq.z + eq.w           ; eq = LeavesEquations[i], c214..c219
fixed  = (TEXCOORD1.xyz*2-1) * (TEXCOORD4.w * TEXCOORD2.x) * size*TEXCOORD4.w
card   = (TEXCOORD0.x*2-1) * normalize(-eye_l.y, eye_l.x, TEXCOORD0.y*2-1) * size*TEXCOORD4.w*TEXCOORD2.x
pos    = base + lerp(fixed, card, morph) + wind
uv     = TEXCOORD0.zw                                    ; raw D3DCOLOR components, no decompression
```

- **The equation index** is a raw byte in `TEXCOORD1.w`. Leaves with byte ≥ 99 skip the morph and
  index from 100 (`slt`/`lrp`); the rest morph. `LeavesEquations` is only **6** registers, so an
  index outside `[0,6)` would `mova` into unrelated constants — clamped and counted
  (`leaf eq clamped`).
- **The eye is transformed to local space assuming a YAW-ONLY world matrix**: the shader uses
  `(m00, m01)` for local X and the *perpendicular* `(-m01, m00)` for local Y rather than the
  matrix's real second row, and takes Z as a plain difference. The port replicates this exactly — a
  "more correct" full inverse would not match the game.
- **The wind is genuine per-frame animation** (two oscillators seeded by position and a time
  constant), so leaf geometry is *expected* to change every frame — unlike the trunk, where any
  per-frame change was a bug.
- **The `frc`/`mad` around each `sincos` is only range reduction.** `frac(x/2π + 0.5)*2π − π`
  differs from `x` by a multiple of 2π, so its `sin` **is** `sin(x)`. Nothing to reproduce on the CPU.
- **Leaf cards are alpha-tested, and the game does the test in the PIXEL SHADER** (against
  `g_AlphaTestValue`, PS c31) — which the FF pipeline cannot run. Without reproducing it as
  render state every leaf is an opaque quad and the canopy becomes a blob. `try_render_leaves`
  reads the game's own threshold and sets `ALPHATESTENABLE`/`ALPHAFUNC`/`ALPHAREF` plus
  `ALPHAOP = SELECTARG1(TEXTURE)`, restoring all of it afterwards.
- **The normal is ours, not the game's.** The live leaves VS emits *no* normal — it shades foliage
  unlit (outputs are uv, AO, fade colour). Remix path-traces it, so the port uses the per-vertex
  offset direction, which gives a canopy a rounded look. **If foliage lighting looks wrong, that
  line is the first suspect** — it is an invention, not a port.

Toggled by `F11` (`g_veg_leaves`). **Confirmed correct in-world — foliage renders and animates.**
Both vegetation families are therefore done; the RealTree coverage gap is closed.

Normals/tangents/binormals are `UBYTE4` decoded as `(v - 127) / 127`.

Real declarations use **real semantics** (`dcl_position v0`, `dcl_normal`, …). Select POSITION by
`D3DDECLUSAGE_POSITION`. (`GfxMesh_AppendVertexElements_Stream` sets everything to TEXCOORD
unconditionally, but that function does *not* build mesh declarations — it misled us for a while.)

## Camera

Remix reads **View and Projection separately** to build its camera. AC2 only uploads the product
(`g_WorldViewProj`), and the engine's own View/Proj in `GfxContext` are stale at draw time thanks
to the command buffer. So the camera is derived analytically per pass:

```
World   = transpose(c8)          // in-sync constant, rides the command buffer
VP_true = inv(World) * WVP       // World is affine -> stable inverse
View, Proj = decompose_vp(VP_true)
```

`decompose_vp()` exploits the structure of a row-vector `V*P` where V is rigid and P is
perspective: column 3 gives the view's third axis and `tz` directly; `a = |col0|`, `b = |col1|`
(basis vectors are unit); `c = dot(col2, r2)`; `d = VP[3][2] - c*tz`.

Orthographic passes give `col3 ≈ (0,0,0,1)` → decomposition returns false → **we refuse to
convert that draw** (a bogus camera is worse than letting the game draw it).

Results are **cached per pass** so every draw in a pass gets bit-identical V and P — a camera
that jitters a few ULPs per draw makes a path tracer flicker even though rasterization is exact.

**AC2's measured camera:** right-handed projection (`proj[2][3] = -1`), near 0.1, far ≈ 1250,
16:9, fovY ≈ 46°. `rtx.leftHandedCoordinateSystem` defaults to right-handed already — don't set it.

## Culling patches

A path tracer needs off-screen geometry for shadows, reflections and bounce light. AC2 culls
hard. **`rtx.antiCulling.*` cannot help** — it only extends the lifetime of objects the game
still *submits*, and AC2 culls *before* submission. Remix never sees them. So the game's culling
has to be patched.

### The one that actually mattered: hardware occlusion queries

**AC2 has TWO independent culling systems, and we spent a long time patching the wrong one.**

`scimitar::OGLBaseRenderer::RenderElements` @ `0x1202950` runs two passes: pass 1 issues D3D9
occlusion queries, pass 2 consumes the results and **drops the draw** for any object showing
fewer than 25 visible pixels — before submission, so Remix never sees it.

```
1202a81  call sub_1267A60          ; GetVisiblePixels()
1202a90  cmp  eax, dword_1DD1828   ; the threshold, = 25
1202a96  jnb  short 1202ae1        ; >= 25 -> RENDER
                                   ; fall through -> CULLED (skips the draw at 0x1202c30)
```

**Patch the threshold, not the branch:** `dword_1DD1828` (RVA) `19 00 00 00` → `00 00 00 00`.
The compare is *unsigned*, so a threshold of 0 makes `eax < 0` impossible and the cull never
fires. Three reasons this site is better than the branch:

- It is a **data write**. No torn-byte hazard in a hot function that job threads are executing —
  which is exactly what crashed the CullAABB prologue patch.
- `dword_1DD1828` has **exactly one reader**, so there are no side effects.
- Blast radius is small: the gate only applies to objects that opted in via `*(a4+171) & 1`, and
  `GetVisiblePixels` already returns 1000 ("assume visible") when an object has no query.

**Verified: `jnb` TAKEN means render; FALL-THROUGH means culled.** The intuitive "NOP the branch"
fix would have culled the entire world — nearly the third inverted culling patch on this project.
Read the decompilation, not the mnemonic.

**This makes the CullAABB patch unnecessary** (user-confirmed: with occlusion off, every object
reappears and `PAGE DOWN` changes nothing). That is a straight win — it retires the one patch that
crashes during loading.

### The frustum patches (still applied, lower value)

| RVA | Patch | Purpose |
|---|---|---|
| `0x11b2075` | `85 C2 74 09` → `8B C2 90 90` | VisualsCulling submission gate: `test edx,eax / jz` → `mov eax,edx / nop / nop` |
| `0x126674` | `0F 94 C0` → `B0 01 90` | `UpdateEntitiesCullingFlags`: force the entity **visible** bit on |
| `0x119be74` | `74 16` → `EB 16` | `CullAABB`: make the loop's early-out unconditional |
| `0x119be8e` | `B8 02 …` → `B8 01 …` | `CullAABB`: epilogue returns **1** instead of 2 |

**`scimitar::CameraFrustum::CullAABB` @ `0x119be50`** is the low-level frustum test, with 6
callers covering entity culling, visuals culling, shadow cascades and spatial traversal. Forcing
it to return 1 fixes building flicker.

Two traps here, both of which cost us real time:

**The return values are not the classic 0=inside / 1=intersect / 2=outside.** Read what the
callers *do* with the value. `Visuals_VisibilityTest` @ `0x11b1710` only sets the visibility bit
for **1 or 3** — a return of 0 falls through the same path as 2. Forcing 0 marks everything
culled.

**Don't overwrite the prologue.** `mov eax,1 ; retn 8` at the function entry is stack-correct
(`retn 8` balances the two stack args) but **crashes the game**. It's an 8-byte write spanning
several instructions in a hot function that six call sites hit from job threads; a thread already
executing inside the prologue runs torn bytes. Patching the *conditional* plus the *existing
epilogue's immediate* leaves the prologue and its matching `pop edi/esi/ebx` intact, and works.
That's a good general rule for forcing a return value in a hot multithreaded function.

Similarly, `Entity+0x60` **bit1 is VISIBLE, not culled** — `Entity_FrustumVisibilityTest` @
`0x119c7c0` returns **0 when inside all six planes**, and the caller does `bit1 = 2 * (v26 == 0)`.
Clearing that bit marks the whole world invisible, which T-poses every character including the
player.

### Culling gates animation

Culled entities appear to skip their animation update, so a culled-but-drawn character keeps its
bind pose — **a T-pose**. This is still the leading explanation for character T-posing and is
consistent with the entity-visible-bit bug above (universal "not visible" ⇒ universal T-pose).
It has not been cleanly isolated yet.

## Performance — geometry-bound, and it fights RT correctness

The frame is **GPU-bound in Remix's path tracer** (Remix's own HUD confirms it), and the cost scales
with the **instance count** Remix has to build into its BVH — which is exactly what the mod
maximises (anti-cull forces everything visible, occlusion off). This was established by elimination,
and the surprises are worth recording so nobody re-runs the experiments:

- **Optimising our CPU path does nothing for fps.** A per-phase profiler (frame-time + per-block
  QPC timers, since removed) put our draw-hook at ~18-20ms of a ~32ms frame — but cutting it moved
  fps **zero**: the camera-inverse fix (12ms → 0.2ms), a parsed-declaration cache, a skinned
  bind-pose cache. The render thread's work is not the bound. *(The camera-inverse and skin caches
  were kept — they're correct and cheap; the decl cache too.)*
- **Culling at the D3D layer does nothing either.** A cull that suppressed **half** the draws we
  forward to Remix changed fps by 0. By the time the game calls `DrawIndexedPrimitive`, it has
  already built the object and issued all its per-object state calls **across the 32→64-bit
  bridge**; suppressing the draw saves only the draw call itself. Remix's own `rtx.antiCulling.*`
  is powerless for the same reason — it "can only extend the life of objects the game still
  submits." **The only cull that helps is one that stops the game issuing the object at all.**
- **Remix quality settings (DLSS / resolution / bounces) don't change fps** — so it is not
  pixel-shading bound, it is geometry/instance bound.

That leaves **source-level culling**, and here RT correctness and performance are in direct tension:

- **HW occlusion queries** (the game's big lever — re-enabling gave ~30 → 100 fps) **flicker badly
  under Remix.** They are frame-delayed, and Remix re-hashes objects as they cull in and out, so the
  scene "freaks out and breaks." Unusable. This is the core reason the mod force-submits everything.
- **Frustum culling** (deterministic, `DELETE` toggles the anti-cull patches off) is stable and
  gives a **modest** improvement, at the cost of off-screen geometry in reflections/shadows.
  `rtx.antiCulling.object.enable` (now on) can retain some of it.
- **Remix's own `rtx.viewDistance` cull** (`rtx.conf`) drops far geometry from the BVH Remix-side,
  without the game-culling flicker — a clean knob to trade draw distance for fps.

**Bottom line:** a fully stable image wants every instance submitted, which is slow; every cull that
recovers the cost either flickers (occlusion) or removes geometry (frustum / view-distance). ~30fps
with everything submitted looks like a practical ceiling for dense city content on this hardware,
short of a Remix-side fix for instance stability under culling (the same class of problem GTA IV's
mod forked dxvk-remix for). Levers exposed: `DELETE` (frustum anti-cull), `PAGE UP` (occlusion —
flickers), and `rtx.viewDistance.*` / `rtx.antiCulling.*` in `rtx.conf`.

## Skinning

CPU skinning rather than FF indexed vertex blending: with `D3DRS_INDEXEDVERTEXBLEND`, Remix would
still have to evaluate the blend to get final positions — the same work that makes vertex capture
unreliable. CPU skinning hands Remix plain pre-skinned static geometry.

Palette at **c120 + 3×boneIndex**, 3 registers per bone (a 4×3 transposed matrix), 4 bones per
vertex. Rows `c120+3i / c121+3i / c122+3i` are the three rows of a 3×4 matrix M; the shader does
`skinned.x = dot(pos4, M[0])`. Weights are `UBYTE4N / 255`. Normals skin with `w = 0` and get
renormalized. Output goes to a round-robin dynamic VB pool in the same FVF as the static path.

Also present but **not handled**: `g_ClutterWorldMatrices`, a separate instanced-vegetation palette.

## Textures

`CreateTexture` pointer reuse is real and confirmed: the allocator recycles addresses of released
textures, and reuses were observed *with a different descriptor*. **Any map keyed on a raw D3D
pointer is unsafe unless you AddRef.** This bit us once already (`s_ps_diffuse` was keyed on a
non-AddRef'd `IDirect3DPixelShader9*`, so a recycled address returned the previous shader's
diffuse stage — wrong textures on meshes).

Diffuse texture stage is **not reliably s0**. Across the game's 1362 pixel shaders, s0 is
`DiffuseMap_0` (185), `Diffuse_0` (102), `Layer0Diffuse_0` (86) — but also `NormalMap_0` (99) and
`Layer0Normal_0` (26). So each PS's constant table is parsed at creation and the diffuse stage is
registered. The matcher **rejects only what is provably not a diffuse** (normal/specular/depth/
shadow/cookie/lightmap/reflection/cubemap/ao) and otherwise accepts the lowest sampler.

A pixel shader with no usable sampler is a genuine lighting/AO/depth pass and its draws are
refused — that's what keeps those passes out of the FF path. **But "no usable sampler" had a false
positive: water.**

### Water — a real surface with no albedo (rank 4, "normal-only")

**AC2's water was invisible, and the vertex path was never the problem.** The water VS
(`AC2_Water_DO_NOT_USE` — the name is a lie, it is what the game runs) is the *ordinary generic
static format*: SHORT4 position with `|w|/262136`, UBYTE4 normal/tangent, ×16 SHORT2N UVs,
`g_World`@c8. `classify()` already handled it. The chain was:

```
water PS declares ONLY  NormalMap_0@s0 + g_DepthSampler@s8 (+ reflection/projector/lightmap)
  -> matcher rejects normal/depth/reflection/shadow/cookie/lightmap = every sampler water has
  -> returns -1 -> `no_diffuse` -> draw refused -> FF-ONLY deletes it -> Remix never sees water
```

Water has **no diffuse texture at all** — it is normal map + reflection + screen-space refraction.
So the matcher now registers a **rank 4 "normal-only"** material: no diffuse at any stage *but* a
normal map present ⇒ a real surface with no albedo, not an AO pass. Measured blast radius across
the 1362 asset pixel shaders: **only 33 match, in 3 templates** — `AC2_VolumeFog_DO_NOT_USE` (20),
`AC2_Water_DO_NOT_USE` (11), `AC1_GFX_Water_Puddle` (2). The other **109** no-diffuse shaders have
no normal map either and stay refused; they are the genuine AO/depth passes.

> **Water and VolumeFog are indistinguishable by sampler signature** — identical down to the
> register (`NormalMap_0@s0` + `g_DepthSampler@s8`). They are one shader family; only their
> non-sampler constants differ (fog declares `WaterOpacity_*`/`DepthFadeControl_*`, water declares
> `g_PickingID`). We deliberately **do not** separate them in code: Remix categorises by **texture
> hash**, and their normal maps are different textures, so they get different hashes and can be
> tagged independently. Deciding in the ASI would only take that choice away.

**The bound texture is the NORMAL MAP — as an identity, not as a normal map.** Remix's legacy
material has exactly one texture slot (albedo) and no normal slot at all, so water's blue-ish
normal pixels become its albedo until a replacement material is authored. That is the accepted cost
of getting water geometry into the path tracer at all (it still contributes reflections and GI).

**The rest is Remix-side**, and the runtime's own help text prescribes it:

> *"Textures on draw calls to be treated as 'animated water'."*
> *"Objects with this flag applied will animate their normals to fake a basic water effect ... only
> when `rtx.opaqueMaterial.layeredWaterNormalEnable` is set to true."*
> *"**Should typically be used on static water planes that the original application may have relied
> on shaders to animate water on.**"*

So: tag water's normal-map hash into `rtx.animatedWaterTextures` and set
`rtx.opaqueMaterial.layeredWaterNormalEnable = True`. A nicer look later means a translucent
replacement (`AperturePBR_Translucent`) in `mod.usda` keyed on the same hash — *"Animated water with
Translucent materials will animate using Remix animation time."*

Toggled by `F2` (`g_normal_only_materials`); counted in the report's **NORMAL-ONLY materials**
section and as rank 4 in the diffuse-width histogram. **Built and deployed, NOT yet verified.**

Useful sampler map: `s7` = `g_ReflectionSampler`, `s8` = `g_DepthSampler`, `s9` =
`g_ProjectorCookies`, `s11` = `g_ProjectorShadow`, `s12` = `g_WorldLightMapSampler`.

## Lights

**AC2 never calls `SetLight`** — it is forward-rendered with per-object light lists evaluated in
the pixel shader.

**The live path is now the ENTITY ROUTE** (`ac2_lights.{hpp,cpp}`): we hook
`scimitar::LightingEnv::Update` and read each light straight out of the engine's own `LightNode`
structures, keyed on the `LightNode*` for stable cross-frame identity. This replaced the older
register/constant path, which reached the screen but flickered, mislocated lights, and never
extracted the sun — all because it reconstructed a *global* light set by hashing floats out of a
*per-object* shader channel. The register decode is preserved below as reference (it is still the
authority on what each field *means*); the entity route is documented at the end of this section.

### The register path (current implementation — works, but flawed)

Register map — **verified twice, independently**: once from `g_ShaderConstantNameTable` @
`0x1de04b8` + the switch in `SetVectorVS` @ `0x11fbbe0`, and once by reading the constant tables
of all 7004 shader assets. They agree on the register *names*.

| Constant | Reg | Layout | Max |
|---|---|---|---|
| `g_OmniLights` | **c32** | 2 regs/light | 4 |
| `g_DirectLights` | c40 (**not fixed** — see below) | 2 regs/light | 2 |
| `g_SpotLights` | c44 (**not fixed**) | 4 regs/light | 2 |
| `g_ShadowedDirect` (sun) | c52 (**not fixed**) | 3 regs | 1 |
| `g_NumLights` | c31 | **VS ONLY** | |

The `(4,2,2,1)` budget is confirmed twice: it's the clamp in `LightingEnv::SelectAndSetLights`,
and it's the largest declared size across the assets (VS declares `g_OmniLights@c32x8`).

Register *contents* were read out of the shader itself
(`PixelShader_GEN_Standard_0xD023CC0DECF52EE3`) and cross-checked against the engine writers
`LightingEnv::SetOmniVectors`/`SetSpotVectors` in the symbolized Mac binary. Both agree:

```
omni    reg0 = { pos.xyz, far^2 }    reg1 = { colour*intensity*fade, 1/(far^2-near^2) }
spot    reg0/reg1 as omni, reg2 = { -dir.xyz, ? }, reg3 = cones (mapping UNVERIFIED — see below)
direct  reg0 = { -dir.xyz, ? }       reg1 = { colour*intensity*fade, 0 }
```

**WORLD space — now confirmed by a second, independent source.** The engine writer
`SetOmniVectors` takes the position straight from the light node's world matrix (row 3 of the
`Matrix44` at `node+0x40`) and writes it to `reg0` untransformed; the PS then differences it
against `v3` (world position) and `g_EyePosition` (c12). Positions go to Remix untransformed.

**Load-bearing decode facts (all still valid for the entity route too):**

- **The light count cannot be read at draw time.** `g_NumLights` is VS-only; the counts are baked
  into the pixel shader as literals — the PS variant *is* the light count (`GfxLightingKey`).
  Recover it from the PS constant table at creation: `g_OmniLights@c32xN` ⇒ `N/2` omni.
- **`reg0.w` is the RANGE squared** — a falloff cutoff, *not* the emitter size. Feeding
  `sqrt(far^2)` as a sphere radius gives hugely oversized soft lights. Emitter radius is a tunable.
- **Colour is pre-multiplied** by intensity and a `fade/255` byte, in arbitrary units
  (`(fade * 0.00392) * intensity * colour`, confirmed in `SetOmniVectors`). The map to Remix
  radiance is a **tuning constant, not a derivation** (`g_radiance_scale`, currently 20).

### `CreateLight` does not make a light — `DrawLightInstance` does

`CreateLight` only *defines* the light and hands back a handle; it contributes nothing until
`DrawLightInstance(handle)` submits it, **every frame**. `frame_end()` created handles and never
submitted them, so the path had **two** independent blockers: (1) the API was never initialised
(`bridge.conf`) ⇒ `created: 0`, and (2) even initialised, nothing was drawn ⇒ `created: N`, screen
dark. Blocker 1 masked blocker 2 perfectly. The only precedent in the tree is the flashlight
(`remix_api.cpp`, `DrawLightInstance` every begin_scene). The report now prints **`Remix lights
DRAWN`** next to `created`; `created != drawn` is the signature and can no longer hide. **Fixed and
confirmed on screen.**

### Three faults of the register path (all fixed by the entity route, now live)

These are why the entity route replaced it, not more tuning. Kept as the rationale:

1. **Flicker with a stationary camera — `light_key()` hashes colour through `lround`.** The
   identity key mixes `lround(colour)`. The lights' actual colours cluster in 0.2–0.8, where
   `lround` is a **1-bit quantiser whose boundary sits at exactly 0.5** — and measured lights land
   right on it (Abstergo report: several at `0.5`, `0.468`, `0.583`). Float jitter flips the key,
   Remix destroys and recreates the light, and it flickers *by construction*. The comment claims
   the opposite intent ("a torch flickering in INTENSITY should stay ONE light"). Any float hash
   has such a boundary somewhere — dropping colour only moves it to the 1cm position quantum.
2. **Lights land at the camera — the register layout is DYNAMIC and we hardcoded it.** Only
   `g_OmniLights` sits at a fixed base (c32). In `LightingEnv::UpdateShaderConstants`, `direct`,
   `spot` and `sun` are written to offsets *computed from the light-count flags*: direct lands at
   c40 only when there are exactly 4 omni; with 2 omni it is at **c36**. Our code reads a fixed
   `40 + 2*i` / `44 + 4*i`, so on every non-4-omni variant it reads **another constant's data and
   submits it as a light position** — this repo's own documented trap ("an undeclared register is
   someone else's data"), and the trunk-jitter bug in a new place.
3. **The sun is never extracted, and the spot cone mapping is unverified.** `on_draw` handles
   omni/direct/spot; `has_sun` is stored and never read, so `g_ShadowedDirect` is never decoded —
   the visible sun comes from the ~2.6 direct lights/frame, not the sun path. (This corrects the
   old note that "PS variants don't declare `g_ShadowedDirect`" — measurement shows the opposite:
   nearly every lit variant declares it; it is the one light we skip.) Separately, the engine calls
   `cosf()` on cone angles stored in **radians**, and the decompiler dropped where `reg3.x/.y`
   land, so the inner/outer mapping in `on_draw` is a guess — consistent with the nonsensical
   `cone 180..72 deg` seen in the report.

### The entity route (the live path — Windows decode DONE)

We hook `scimitar::LightingEnv::Update` and walk its light-node list directly. **Every address and
offset below is re-derived on the Windows exe** (`ida-pro-mcp`, IDB rebased to 0 ⇒ addresses are
RVAs); the Mac binary was only the map of *what* to look for.

**Hook point — `LightingEnv::Update` @ Win32 `0x1269E60`.** `__thiscall(this = graphicNode+0x90,
PtrArray<LightNode>* lights, GraphicNode* node)`; modelled as `__fastcall(ecx, edx, lights, node)`
to catch `this` in ecx (edx is unused). Found as the sole caller of `SelectAndSetLights`
(`0x1269DB0`), whose Windows address the register work already had. It is called **per visible
graphic node, on worker threads** (single-threaded caller `0x11B2840`, job/work-stealing caller
`0x11B2C10`), so the light list is *per-object*, not one global list — but that no longer matters,
because we dedupe on the `LightNode*`. The hook takes one mutex per node-list and reads only engine
memory, so it holds it safely across the walk.

**`PtrArray<LightNode>`** (the `lights` arg): data pointer @ `+0`, count = `u16 @ +6 & 0x3FFF`,
elements are `LightNode*` (stride 4).

**`LightNode`:** world matrix @ `+0x40` (4×4 float, row-major) — **row 1 @ `+0x50` = direction**,
**row 3 @ `+0x70` = world position**; light-struct pointer @ `+0xA8`.

**light struct** (`*(node+0xA8)`), all authoritative from the four vector writers
`LightingEnv::Set{Omni,Direct,Spot,Sun}Vectors` (`0x1268880` / `0x12681F0` / `0x1268970` /
`0x12682B0`), dispatched by `WriteLightVectors` (`0x12691B0`):

| field | offset | notes |
|---|---|---|
| type magic | `vtable[+0x14]()` | virtual getter; call it like the engine does |
| intensity | `+0x14` `[5]` | float |
| colour rgb | `+0x10C..+0x114` `[67..69]` | |
| enabled | `u16 @ +0x11C`, bit `0x200` | `(w >> 9) & 1` |
| omni near/far | `+0x120`/`+0x124` `[72]/[73]` | writer stores `far²` as `reg0.w` |
| spot cone out/in | `+0x120`/`+0x124` `[72]/[73]` | **radians** (writer does `cos()`) |
| spot near/far | `+0x12C`/`+0x130` `[75]/[76]` | |

**Type magic → kind** (virtual @ `vtable+0x14`; identical values on Mac since they are class-name
hashes, but note the **decompiler mis-signs them — these are the raw disasm immediates**):

| magic | kind | Remix light |
|---|---|---|
| `0x7E15FD50` | direct (directional) | distant, dir = node row 1 |
| `0x344780D6` | omni (point) | sphere, pos = node row 3 |
| `0x80320FB8` | spot | sphere + shaping, pos row 3 / dir row 1 |
| `0x5EDC3E04` | sun (shadowed directional) | distant, dir = node row 1 |

The writers confirm the register semantics from the reference decode: omni `reg0 = {pos, far²}`;
direct/sun `reg0 = -row1` (so travel direction = row 1); spot adds `reg2 = -row1` and
`reg3 = {cos(outer), cos(inner), 1/(cosOut−cosIn)}`. Colour is emitted as
`colour · intensity · (fade/255)`; we fold `colour·intensity` at collect time and apply
`g_radiance_scale` at submit, so the magnitude matches the old premultiplied read.

Position comes from the node's own world matrix, not a guessed (and dynamic) register; and the sun
is just the node with magic `0x5EDC3E04` — no getters needed, it rides the same walk.

**Identity — the real fix was create-once + age-out, not the hash.** The first cut keyed the Remix
hash on the `LightNode*` and destroyed+recreated every light every frame; it flickered. The obvious
theory was "the pointer churns, so the hash churns" — but the report's `node-ptr carry-over` counter
later measured **3/3 stable**, refuting that. The actual cause was the **destroy-every-frame model
colliding with a one-frame collection gap**: the node walk runs on worker threads, and on any frame
where a light was momentarily absent from the collection (worker timing, or transient culling while
moving) it got destroyed and not recreated — a blink. The flashlight avoids this only because its
one light is always present to be re-created.

The fix is a **persistent registry** (`ac2_lights.cpp`) whose handles are **created once and merely
redrawn every frame**, recreated only when the light data changes or after it goes unseen for
`MAX_AGE` (4) frames — so a momentary gap no longer blinks. Entries are matched frame-to-frame by
position (5cm) / direction (~11°) rather than by pointer or a value hash: a tolerance match is
**boundary-free**, honouring this repo's rule that identity must not come from quantising a value
(any hard quantum has a boundary a jittering light can straddle). The spatial match turned out to be
belt-and-braces given the pointer was stable, but it costs nothing and is robust if that ever
changes. `created` is a lifetime counter that should **plateau** in a static scene; if it keeps
climbing, identity is still churning.

Needs `exposeRemixApi = True` in `<game>/.trex/bridge.conf` and `HOME` (a true on/off — it destroys
live lights when toggled off, so it can A/B against Remix's fallback sun). The report prints
`hook installed`, `light-node visits`, per-type unique counts, and `created`/`DRAWN` so a decode or
hook regression is visible without a debugger.

**Confirmed on screen:** lights show up correctly positioned and are **stable — the flicker is
gone** (persistent spatial registry, above). **Still unverified** (cheap to check next): direction
sign (row 1 assumed = travel direction, from the old working negate-the-register behaviour — flip
if the sun is backwards), spot inner/outer assignment (`[72]`=outer/`[73]`=inner — swap if cones
invert), and `g_radiance_scale` may need retuning now that colour and intensity are read separately.

## Remix-side tagging (not every bug is in this repo)

Some of what looks like a conversion bug is Remix not knowing what a texture *is*. Tags live in
`<game>\rtx.conf` and are written by the Remix developer menu. Confirmed relevant on AC2:

- `rtx.decalTextures` — **an untagged decal is treated as opaque world geometry.** Coplanar with a
  wall, it fights that wall and the wall's faces appear/disappear with camera angle. This was the
  real cause of the long-standing "faces of buildings randomly disappear" bug, after the FF path,
  the shadow-VB cache and the dynamic-VB pool had all been measured innocent.
- `rtx.ignoreTextures`, `rtx.uiTextures`, `rtx.skyBoxTextures`, `rtx.terrainTextures`,
  `rtx.particleTextures`, `rtx.worldSpaceUiTextures`, `rtx.playerModelBodyTextures`,
  `rtx.animatedWaterTextures`, `rtx.raytracedRenderTargetTextures`, `rtx.ignoreLights`.

> **The lists can contradict each other, silently.** A hash in `ignoreTextures` *and*
> `decalTextures` is a texture that is both suppressed and decal-shaded; a hash in
> `terrainTextures` *and* `ignoreTextures` is terrain that never renders. Repeat-tagging while
> hunting a texture in the dev menu is how this happens — one hash ended up in four lists at once.
> When a Remix tag "doesn't work", grep `rtx.conf` for that hash in the *other* lists before
> assuming the tag is broken.

## Finding what's still missing

Once coverage is good, **eyeballing stops working** — you cannot tell a missing material from one
that was never there. The report's totals cannot answer it either: `no_diffuse` is ~45% of all draws
and is *mostly correct*. **Water hid inside that counter for the entire project**: a real surface,
invisible, statistically indistinguishable from a legitimate depth-pass rejection.

So the report accounts for every draw's outcome **per vertex shader** (`PER-SHADER COVERAGE`):

```
  hash        seen        converted   pct   dropped     family
  0x1A2B3C4D      40213           0    0%       40213   -
```

**A shader with many draws and 0% converted is the signature of a gap** — and it surfaces without
anyone walking past it. Name the hashes offline:

```
python tools/name_live_shaders.py "<game-dir>"
```

This works because AC2's shaders live in **MaterialTemplate assets, not the exe**, and reach
`CreateVertexShader` byte-identical — so an FNV-1a 32 of each asset `.dxbc` maps a runtime hash back
to a template name. It is how `AC2_Water_DO_NOT_USE` was identified as the live water template
despite its name. Of the 185 live shaders, **94 resolve to exactly one template**.

> **Two traps, both paid for already.**
> **(1) Many templates compile to identical bytecode**, so one hash maps to a *list* of names — 91
> of the 185 are ambiguous (one matches 74 templates). A shader is only unambiguously template X if
> *every* name it matches is X. Counting each matched name as a hit inflates the totals absurdly.
> **(2) The live shader is the authority, not the corpus.** Use this tool to name what *runs*; read
> the **dumped** shader, never the asset, when porting one.

## Assets

Shaders live in **MaterialTemplate assets**, not the exe — 7004 `.dxbc` (5642 VS + 1362 PS)
across 138 templates, raw D3D9 bytecode with no container (first dword `0xFFFE0300` = vs_3_0).
They're byte-identical to what passes through `CreateVertexShader` at runtime, so asset sweeps and
live dumps can be cross-referenced by hash. `tools/shaderdis/` disassembles them offline.

There is **no "FakeMesh" material template** in AC2 — FakeMesh is a geometry concept; such
shaders may only exist in later titles.

---

## Open problems

**Normal maps are never used — everything looks flat.** The most visible remaining problem.
The FF path hands Remix an *unlit, straight-texture* material: the diffuse bound to stage 0 with
`D3DTSS_COLOROP = SELECTARG1` / `COLORARG1 = TEXTURE`, and nothing else. Surfaces therefore shade
off the **interpolated vertex normal** only, so every wall is geometrically flat to the path
tracer. Note this is *not* missing vertex normals — `FF_FVF` includes `D3DFVF_NORMAL` and the
normals are decoded (`UBYTE4` → `(v-127)/127`); it is specifically the missing normal **map**.

What we already have, which makes the first half cheap:

- **The game binds the normal map, and we already identify it and throw it away.** The PS
  constant-table parse in `ac2_dump.cpp` rejects any sampler whose name contains `normal` when
  hunting the diffuse (`if (nm.find("normal") != npos) continue`). Across the 1362 asset pixel
  shaders `s0` is `NormalMap_0` on 99 and `Layer0Normal_0` on 26, so the stage varies exactly like
  the diffuse does and must be registered per-PS the same way. Registering a `normal_stage`
  alongside `ps_diffuse_info::stage` is a small change.
- Tangents and binormals are present in the declarations (`UBYTE4`, decoded `(v-127)/127`) — a
  tangent basis is available if one is needed.

**The hand-off to Remix is the whole problem, and route 1 is now MEASURED DEAD:**

> **Remix's legacy/FF material holds exactly ONE texture (albedo). A normal map cannot reach
> Remix through a fixed-function draw call, at any stage, under any setting.** Established from
> the shipped runtime rather than guessed: the complete `rtx.legacyMaterial.*` option set was
> recovered from the option strings in `<game>\.trex\d3d9.dll` — 13 options
> (`useAlbedoTextureIfPresent`, `albedoConstant`, `opacityConstant`, `roughnessConstant`,
> `metallicConstant`, `anisotropy`, `emissiveIntensity`, `emissiveColorConstant`,
> `enableEmissive`, `ignoreAlphaChannel`, and three thin-film ones) — and **not one is a texture
> other than albedo**. There is no `normalTexture` on the legacy path and no
> `rtx.normalMapTextures` tag list. The runtime's texture baker says so out loud:
> `"Only single texture legacy materials are supported. Ignoring the second color texture."`
> So a normal map bound to stage 1 is not blended as colour — it is **silently discarded**.
> `rtx.opaqueMaterial.normalIntensity` only scales an *already-present* normal map; it sources none.

That leaves two routes, both of which take the normal map **off** the FF draw path entirely:

1. **USD replacement materials** keyed on the diffuse's texture hash, referencing the game's normal
   map dumped to disk. Fully offline and automatable (we already dump textures), but it is a
   per-material asset pipeline, not a runtime fix. **Open question before betting on it:** whether a
   *material-only* replacement applies by texture hash without also replacing the geometry.
2. **The Remix API** (`remixapi_MaterialInfo`) — confirmed from our own vendored header,
   `deps/bridge_api/remix/remix_c.h:250`: `normalTexture` and `tangentTexture` are on the **base**
   `remixapi_MaterialInfo` (not `OpaqueEXT`), and their type is `remixapi_Path` =
   `const wchar_t*` (`remix_c.h:166`) — a **file path**. There is no way to hand Remix a live
   `IDirect3DTexture9` as a normal map; it must be dumped to disk first. (`remix_api.cpp:174`
   already passes `.normalTexture = L""`.) Still unconfirmed: whether an API material can attach to
   a captured FF draw at all, rather than only to API-created meshes.

> Don't "fix" this by binding the normal map to stage 1 and hoping — the runtime drops it on the
> floor without a word.

**Faces of buildings randomly disappear/reappear as the camera moves — RESOLVED, and it was not
our code.** The cause was a **decal texture Remix had not been told was a decal**, so Remix treated
it as opaque world geometry; a decal plane coplanar with a wall then fights that wall, which reads
as faces winking in and out with camera angle. Fixed by tagging it in `rtx.decalTextures`
(`<game>\rtx.conf`, via the Remix developer menu). See
[Remix-side tagging](#remix-side-tagging-not-every-bug-is-in-this-repo).

Three code suspects were **measured dead** before that, and are recorded here so nobody re-derives
them from the same plausible reasoning:

| Suspect | Verdict |
|---|---|
| `get_dynamic_vb()`'s unlocked 64-buffer pool races on a `D3DCREATE_MULTITHREADED` device | **Impossible.** Exactly **one** thread ever reaches it. AC2 records on workers but plays back on one render thread, and our hook is on playback. A lock would be pure cost on a ~900k-draw path. |
| The pool is too small now multi-stream added ~165k draws/run | **No.** Both consumers lock with `D3DLOCK_DISCARD`, so the driver renames per lock and in-flight draws keep their data. One buffer would be safe; the pool is belt-and-braces. |
| The shadow-VB cache is keyed only on the source VB pointer, ignoring stride/decl, and never notices a re-filled source | **Sound.** `decl_mismatch` 0 across 649,905 hits; `STALE` 0 across 1269 sampled content checks. (The source is AddRef'd, so pointer recycling can't hit it either.) |

Those counters are still live in the report (`shadow-VB cache integrity`), and are cheap enough to
leave in as regression guards.

**Coverage (~41% of draws).** Every draw left to vertex capture is a flicker source. The one real
remaining gap is **vegetation**: `AC2_Tex1_DistanceClutter` (RealTree), 1.6M draws in two
declarations (797,960 each). Also still open: the `g_ClutterWorldMatrices` vegetation palette.

**Trunk port — trunks render; UV + jitter fix deployed, NOT yet re-verified.** Trunks appear in
world, which validates the whole position-less pipeline (family registration, constant reads, the
sweep). Two bugs then showed up and both traced to porting the wrong shader variant (broken UVs and
a camera-dependent jitter — see [Vertex formats §5](#vertex-formats)). The fix is deployed but the
next run has not been looked at. Still open:

- **The frame is built from a `D3DCOLOR`.** `t1 = cross(TEXCOORD5, TEXCOORD2)` where TEXCOORD2 is
  `D3DCOLOR` ⇒ its components expand to **0..1, unsigned**, and the shader never decodes them to
  ±1. So `|t1|` is not 1, and since the radius multiplies `dir` *unnormalised*, trunk thickness
  depends on `|cross(axis, ref)|`. Either the asset guarantees the magnitudes, or this is a bug in
  our reading. **If trunks come out too fat, too thin, or pinched, look here first.**
- **The distance morph is camera-dependent, and that is real shader behaviour**, not a bug:
  `t = saturate(DistanceFactors.x * dist(worldOrigin, eye) * g_FovDistanceScale + DistanceFactors.y)`
  feeds both the `MorphEnabled` radius morph and the `LevelLOD` gate. It makes a static tree's
  geometry change as the camera moves, which is fine for a rasteriser but may cost Remix instance
  stability. If a *residual* jitter survives the fade fix, this is the next suspect — but measure
  before removing it: it is what the game does.
- Trunk-only means **bare trees** until leaves are ported; foliage is still dropped in FF-ONLY mode.

**Texture hash stability.** The top unmitigated risk, and what GTA IV's mod needed a custom
dxvk-remix fork for. AC2 streams aggressively; Remix expects a texture's identity to be stable
after creation. Detecting content rewrites needs an `IDirect3DTexture9` proxy to observe
`LockRect`/`UnlockRect` — not yet written.

**CullAABB crashes during loading.** Hence default-off. Cause unknown — but it is now
**superseded by the occlusion patch** and probably should just be deleted.

**Remix API init crashes at startup.** `comp::main()` called `remix_api::initialize()` at ASI load
time, before the D3D9 device exists; `initialize()` runs `init_debug_lines()`, which creates Remix
meshes/materials. While `bridge.conf` was absent this simply FAILED and the path stayed dead (which
is why lights created zero); adding `bridge.conf` made it succeed at load time and crash. Init is
now deferred to the first Present and gated behind `HOME`. **The deferred init is a plausible fix,
not a verified one** — if `HOME` still crashes, suspect `init_debug_lines()` specifically and
bypass it with a minimal init that only does `CreateLight`/`DestroyLight`.

**Character T-posing.** Not fully resolved. See [Culling gates animation](#culling-gates-animation).

**Performance.** No hardware instancing, one draw call per NPC, plus CPU skinning. GTA IV's mod is
CPU-bound for the same reason.

**Baked-shadow decals** fight path-traced lighting. Don't blanket-suppress decals though — blood,
posters and gameplay decals should still work. Needs per-material categorisation.

---

## Lessons learned the hard way

Offered because they'd have saved us several sessions each, and they generalise beyond AC2.

**Creating a resource is not using it — and a known blocker will hide the next one.** The lights
path called `CreateLight` and never `DrawLightInstance`, so it was dead twice over: the API was
uninitialised *and* nothing was ever submitted. We knew about the first and had written it up as
*the* reason ("zero lights, purely because the API is never initialised"). That confidence is the
trap. A known blocker upstream of an unknown one is worse than no explanation at all, because it
predicts the symptom perfectly and so retires the search — you fix the config, the counter you
were watching (`created`) finally moves, and the screen stays dark, which now looks like a *decode*
bug in the one component that was verified twice. **When one blocker fully explains a dead path,
that is exactly when to check whether it is the only one** — the explanation was never tested
against a working path, only against a broken one. Counting `created` and not `drawn` encoded the
same assumption into the instrumentation (see also: don't let instrumentation share the filter's
assumption). Cheap general guard: when adopting an API, diff your call sequence against the one
working example in the tree, not against the struct definitions — `remixapi_LightInfo` gives no
hint that a second call exists, and the flashlight was the only precedent for what a *complete*
sequence looks like.

**The bug is not always in your code — and "plausible mechanism + matching symptom" is not
evidence.** The disappearing building faces had a *beautiful* suspect: an unlocked buffer pool, on
a multithreaded device, that a recent change had just added 165k draws/run to. "A race looks
exactly like this" was true and irrelevant — exactly one thread ever reaches that pool, so the race
was impossible, and the real cause was an untagged decal in Remix's config. Two counters (thread
IDs, a decl fingerprint) cost one build and refuted three suspects at once. **Measure the premise
before you fix the mechanism**: a lock added on that reasoning would have cost throughput, fixed
nothing, and left the bug wearing a "fixed" label — the worst outcome available, because it also
retires the suspect.

**A symptom's NAME can encode a false hypothesis, and then nothing can solve it.** "Textures are
stuck at their lowest mip" was written down early and believed for two sessions. It cost a
sampler-state investigation and a diffuse-matcher investigation, both of which measured clean —
*because there was no mip involved at all*. Our UVs were 16× too small, so the texture was
magnified 16×; magnification looks soft, and "soft" got named "low mip". Every measurement that
came back clean was answering a question nobody should have been asking. When instrumentation
keeps exonerating suspects, suspect the *description* — the word "mip" was a conclusion wearing
the costume of an observation. Name symptoms by what you SEE ("textures look soft"), not by the
mechanism you assume ("stuck at lowest mip").

**Under Remix, D3D9 sampler state is not the mechanism you think it is.** Remix path-traces; it
selects mips from UV gradients on the geometry we submit, not from `D3DSAMP_*`. So sampler state
was never going to explain a blur, and `F7`-off being sharp pointed at *our geometry* — the one
thing that differs between the two paths — rather than at our textures.

**A metric whose reference is the suspect quantity proves nothing.** We validated candidate World
matrices against a View/Proj we were simultaneously unsure of. A World derived from `WVP` always
validates *by construction*.

**`W*V*P == WVP` holds for any split of V and P.** It's self-validating and therefore worthless as
a check — and rasterization looks perfect no matter how wrong the split is. Remix reads V and P
*separately*. That property actively hid a real bug for a long time.

**Error metrics on matrices must be relative.** WVP elements run into the thousands, where a
float32 ULP is ~0.015. An absolute `> 1e-3` threshold fires always.

**"Cache it, the data is static" needs a dynamic escape hatch.** A comment in this codebase once
asserted "AC2's mesh VBs are static". Cloth is re-simulated every frame; it rendered frozen.

**A filter that rejects on "not recognised" silently deletes most of the world.** Prefer rejecting
only what's provably wrong.

**Instrumentation that shares the filter's blind spot will confirm the filter's story.**
`classify()` ignored streams != 0 and rejected the draw; `note_rejected()` ignored streams != 0 and
printed the draw *without its position element*. So the log "showed" position-less declarations and
we went looking for an exotic vertex format. The logger must not reuse the assumption under test.

**"X% of draws convert" is the wrong denominator.** 14% sounded catastrophic; ~66% of all draws are
depth/shadow/AO passes that *must not* be converted. Against drawable draws we were already at ~65%.
Pick a denominator that maps to what you actually see on screen, or you'll chase the wrong lever.

**When something is missing, check whether YOU deleted it before blaming the game.** FF-ONLY mode
turns "we can't convert this" into "this does not exist", so every coverage gap masquerades as a
culling bug. One keypress (`INSERT`) separates the two — that test should be the *first* move, not
the fifth.

**The LIVE shader is the authority, not the asset corpus.** The trunk port was derived from one
shader picked out of 47 with `grep -l TrunkStencil | head -1`, then generalised. It happened to be
one of only 6 that build a procedural UV, and one of the few with a distance fade. All FOUR trunk
shaders the game actually creates do neither - so we shipped broken UVs and a jitter at once. The
runtime dumps every shader it creates to `<game>c2_rtx_dump\shaders`; cross-check there before
believing any variant-specific detail. **47 assets can outvote the 4 that run.** Live dumps and
assets are byte-identical, so an FNV-1a 32 of each asset `.dxbc` NAMES every live shader - that is
how `AC2_Water_DO_NOT_USE` was identified as the live water template despite its name. (Beware:
many templates compile to identical bytecode, so one live shader matches a LIST of names; only
shaders whose every match is water are unambiguously water.)

**An undeclared constant register is not empty - it is someone else's data.** Registers alias
across shader variants. The trunk jitter was our port reading c100/c101 (VFalloff/VDistScale),
which no live trunk shader declares, so it got whatever unrelated shader ran last - then
differenced it against the eye position and moved every vertex. Only read constants the live
shader DECLARES, and refuse the draw when one is missing rather than substituting a default.

**A rejection rule needs its false positives measured, not assumed.** "A PS with no usable sampler
is an AO/depth pass" was true for 109 of 142 shaders - and wrong for water, which is a real
surface that genuinely has no albedo. It was invisible for the whole project. When a filter
rejects by absence, ask what a legitimate absence would look like.

**Don't resolve an ambiguity in code that the downstream tool resolves better.** Water and volume
fog are indistinguishable by sampler signature. Rather than guess, we let both through: Remix
categorises by texture hash, their normal maps differ, so they tag independently. Guessing in the
ASI would have removed a choice the dev menu makes trivially.

**Read what callers do with a return value; don't assume the convention.** Cost us two inverted
culling patches in a row.

**Ask which toggles were active when an observation was made.** We killed a correct hypothesis
using evidence produced by the very bug under investigation — a loading-zone character T-posing
"with nothing to cull against", while an inverted patch was marking the whole world invisible.
The report prints the active toggles for exactly this reason.

**A silent early-return is indistinguishable from "nothing found".** The light pipeline extracted
lights perfectly and created zero, because `frame_end()` bailed on `!api.is_initialized()` without
counting it. The report showed "0 lights" and looked like a decode bug. Every bail-out on a path
you are measuring needs a counter, or the measurement lies by omission.

**Ask what a config change ENABLES, not just what it sets.** Adding `bridge.conf` didn't create the
startup crash — it made a previously-failing init start succeeding, at ASI load time, before the
device existed. The bug was always there; the config just woke it up.

**In a bridge/Remix setup, be suspicious of reading D3D state back** — though note that in our
case `GetVertexShaderConstantF` turned out to be *fine*, and the constant shadow we built to work
around it made zero difference. It's kept behind F12 as a diagnostic. Don't assume the bridge is
lying just because something is broken.

---

## Credits

- [xoxor4d](https://github.com/xoxor4d) — [remix-comp-base](https://github.com/xoxor4d/remix-comp-base),
  which this is built on, and [gta4-rtx](https://github.com/xoxor4d/gta4-rtx), the reference
  implementation that proved the 32-bit shader-based D3D9 path works
- [NVIDIA — RTX Remix](https://github.com/NVIDIAGameWorks/rtx-remix)
- [Dear ImGui](https://github.com/ocornut/imgui) · [minhook](https://github.com/TsudaKageyu/minhook) ·
  [Ultimate-ASI-Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader)
- [momo5502](https://github.com/momo5502) — initial codebase

Licensed as the upstream codebase. See `LICENSE`.
