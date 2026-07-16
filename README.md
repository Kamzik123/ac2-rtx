<h1 align="center">Assassin's Creed II — RTX Remix Compatibility Mod</h1>

<div align="center">

A work-in-progress compatibility mod that makes **Assassin's Creed II (PC)** render through
NVIDIA's [RTX Remix](https://github.com/NVIDIAGameWorks/rtx-remix).

Built on [xoxor4d/remix-comp-base](https://github.com/xoxor4d/remix-comp-base).

**Status: Remix path-traces the game. Geometry, camera, textures and characters work.
Not finished — see [Open problems](#open-problems).**

</div>

---

## What this is

AC2 is a 32-bit, shader-based Direct3D 9 game (Ubisoft's *Scimitar* engine, later renamed Anvil).
RTX Remix natively understands **fixed-function** D3D9 draw calls. It can also try to capture
vertices out of vertex shaders, but on AC2 that path produces flickering geometry and T-posed
characters.

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
- Game-side anti-culling patches, so off-screen geometry still reaches the path tracer

### What doesn't, yet

Roughly **14%** of draws are converted. Much of the remainder is correctly rejected (depth,
shadow and AO passes that have no diffuse texture and shouldn't be path-traced), but coverage
is the main lever left. See [Open problems](#open-problems).

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
| `DELETE` | Game-side anti-culling (VisualsCulling gate + entity visible bit). **Default ON** |
| `PAGE DOWN` | CullAABB → always "intersecting". **Default OFF** — fixes building flicker but *can crash during loading*. Enable once in-world |
| `F1` | CPU skinning |
| `F7` | Fixed-function conversion |
| `F6` | Wireframe (spot converted meshes) |
| `F5` | Camera stage A/B |
| `F3` / `F2` | Depth bias ± |
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
refused — that's what keeps those passes out of the FF path.

Useful sampler map: `s7` = `g_ReflectionSampler`, `s8` = `g_DepthSampler`, `s9` =
`g_ProjectorCookies`, `s11` = `g_ProjectorShadow`, `s12` = `g_WorldLightMapSampler`.

## Assets

Shaders live in **MaterialTemplate assets**, not the exe — 7004 `.dxbc` (5642 VS + 1362 PS)
across 138 templates, raw D3D9 bytecode with no container (first dword `0xFFFE0300` = vs_3_0).
They're byte-identical to what passes through `CreateVertexShader` at runtime, so asset sweeps and
live dumps can be cross-referenced by hash. `tools/shaderdis/` disassembles them offline.

There is **no "FakeMesh" material template** in AC2 — FakeMesh is a geometry concept; such
shaders may only exist in later titles.

---

## Open problems

**Coverage (~14% of draws).** The biggest lever. Every draw left to vertex capture is a flicker
source. Known gaps: the `g_ClutterWorldMatrices` vegetation palette, two-stream formats, and the
all-TEXCOORD RealTree/vegetation path (`[SHORT4 TC0][FLOAT4 TC1][D3DCOLOR TC2][SHORT2N TC3]` —
`v0` is *not* position there, beware false greps).

**Texture hash stability.** The top unmitigated risk, and what GTA IV's mod needed a custom
dxvk-remix fork for. AC2 streams aggressively; Remix expects a texture's identity to be stable
after creation. Detecting content rewrites needs an `IDirect3DTexture9` proxy to observe
`LockRect`/`UnlockRect` — not yet written.

**CullAABB crashes during loading.** Hence default-off. Cause unknown.

**Character T-posing.** Not fully resolved. See [Culling gates animation](#culling-gates-animation).

**Performance.** No hardware instancing, one draw call per NPC, plus CPU skinning. GTA IV's mod is
CPU-bound for the same reason.

**Baked-shadow decals** fight path-traced lighting. Don't blanket-suppress decals though — blood,
posters and gameplay decals should still work. Needs per-material categorisation.

---

## Lessons learned the hard way

Offered because they'd have saved us several sessions each, and they generalise beyond AC2.

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

**Read what callers do with a return value; don't assume the convention.** Cost us two inverted
culling patches in a row.

**Ask which toggles were active when an observation was made.** We killed a correct hypothesis
using evidence produced by the very bug under investigation — a loading-zone character T-posing
"with nothing to cull against", while an inverted patch was marking the whole world invisible.
The report prints the active toggles for exactly this reason.

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
