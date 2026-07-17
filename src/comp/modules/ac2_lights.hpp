#pragma once

// AC2 RTX - game lights -> Remix lights
//
// The problem: Remix cannot see AC2's lights. AC2 is forward-rendered with
// per-object light lists evaluated in the pixel shader; it never calls
// SetLight/LightEnable, so a stock Remix scene has nothing but the fallback light.
//
// The channel: AC2 uploads the complete per-draw light set as SHADER CONSTANTS.
// That matters enormously here. Engine memory read at D3D-draw time is STALE -
// AC2 records into a deferred D3D9 command buffer on worker threads and plays it
// back on the render thread, so anything that varies per object must be read from
// data that rides the command buffer WITH the draw. Shader constants are exactly
// that. So the lights are, by construction, in-sync with the draw that uses them.
// (There is no LightingEnv singleton to walk anyway - lighting state is only
// reachable through per-object chains.)
//
// ---- register map -----------------------------------------------------------
// Constant IDs resolve to registers through g_ShaderConstantNameTable @ 0x1de04b8
// (310 x 128-byte names, indexed by ID) and the switch in SetVectorVS @ 0x11fbbe0.
// The map below was VERIFIED INDEPENDENTLY of that switch, by reading the constant
// tables of all 7004 shader assets - the two agree:
//
//   g_OmniLights     c32  2 regs/light, max 4  -> c32..c39
//   g_DirectLights   c40  2 regs/light, max 2  -> c40..c43
//   g_SpotLights     c44  4 regs/light, max 2  -> c44..c51
//   g_ShadowedDirect c52  3 regs (the sun)     -> c52..c54
//   g_NumLights      c31  VS ONLY
//
// The (4 omni, 2 direct, 2 spot, 1 sun) budget is confirmed twice over: it is the
// clamp passed to the selector in LightingEnv::SelectAndSetLights (0x1269db0), and
// it is exactly the largest declared size across the asset shaders (VS declares
// g_OmniLights@c32x8 = 4 lights x 2 regs).
//
// ---- register contents (verified from the shader disassembly itself) ---------
// From PixelShader_GEN_Standard_0xD023CC0DECF52EE3:
//     add     r5.xyz, c32, -v3        ; L = lightPos - worldPos
//     add     r2.z, -r1.w, c32.w      ; far^2 - dist^2
//     mul_sat r2.z, r2.z, c33.w       ; * 1/(far^2 - near^2)
//     mul     r5.xyz, r1.w, c33       ; colour from c33.xyz
//
//   omni   reg0 = { pos.xyz, far^2 }   reg1 = { colour*intensity, 1/(far^2-near^2) }
//   direct reg0 = { -dir.xyz, ? }      reg1 = { colour*intensity, 0 }
//   spot   reg0/reg1 as omni, reg2 = { -dir.xyz, ? },
//          reg3 = { cos(outer), cos(inner), 1/(cosOuter-cosInner), ... }
//
// WORLD SPACE - verified, not assumed. The PS subtracts the light position from
// v3, and the matching VS writes v3 as the world position:
//     dp4 r0.x, r3, c8   ; * g_World
//     mul r3, r0, r1.w   ; / w   -> world position
//     mov o4.xyz, r3     ; -> texcoord2 = v3 in the PS
// and g_EyePosition (c12) is differenced against that same value. So light
// positions need NO transform before going to Remix.
//
// Two things the constants do NOT give us, by construction:
//   - colour is PRE-MULTIPLIED by intensity and by a 0..255 fade byte, so colour
//     and intensity cannot be separated. That's fine: Remix wants radiance.
//   - reg0.w is the light's RANGE squared, which is a falloff cutoff, NOT the
//     emitter size. Feeding sqrt(far^2) in as a sphere radius would produce a
//     hugely oversized soft light. The emitter radius is a separate tunable.

namespace comp::ac2_lights
{
	// Master switch. Default ON.
	extern bool g_enabled;

	// Bring up the Remix API (deferred to the first Present - see ac2_lights.cpp).
	// DEFAULT OFF: it needs `exposeRemixApi = True` in <game>\.trexridge.conf, and
	// enabling that is what surfaced a startup crash, because the stock code
	// initialised the API at ASI load time. HOME toggles it, so lights can be turned
	// on deliberately rather than at startup where a crash is unrecoverable.
	extern bool g_remix_api;

	// Radiance = constant colour * this. The game's colour is premultiplied by
	// intensity but is in its own arbitrary units, so the mapping to Remix's
	// radiance (W/sr/m^2) is a tuning constant, not a derivation. Tunable at
	// runtime rather than baked in.
	extern float g_radiance_scale;

	// Sphere emitter radius in metres. NOT derived from the light's range - see
	// the header comment. Small = sharp shadows, large = soft.
	extern float g_emitter_radius;

	// Shadow every SetPixelShaderConstantF. Same reasoning as the VS constant
	// shadow in ac2_ff: under Remix the game talks to a 32-bit bridge that IPCs to
	// a 64-bit host, and Get* readback across it is not trustworthy. Our own copy
	// is authoritative.
	void on_set_ps_constant_f(UINT start_register, const float* data, UINT vec4_count);

	// How many of each light type a given pixel shader actually consumes.
	//
	// This CANNOT be read from a constant at draw time: g_NumLights is VS-only,
	// because the light counts are baked into the pixel shader as literals - the PS
	// variant IS the light count (GfxLightingKey). So the count is recovered from
	// the PS constant table at creation: g_OmniLights@c32xN means N/2 omni lights.
	void register_ps_lights(IDirect3DPixelShader9* ps, int num_omni, int num_direct,
		int num_spot, bool has_sun);

	// Collect the lights feeding the current draw. Cheap and deduped - one light
	// typically lights many objects, so the same constants are seen over and over.
	void on_draw(IDirect3DDevice9* dev);

	// Push this frame's unique lights to Remix and retire last frame's. Called from
	// Present.
	void frame_end();

	struct stats
	{
		std::uint32_t draws_with_lights = 0;
		std::uint32_t omni_seen = 0;
		std::uint32_t direct_seen = 0;
		std::uint32_t spot_seen = 0;
		std::uint32_t ps_with_lights = 0;   // distinct PS variants that declare lights
		std::uint32_t unique_this_frame = 0;
		std::uint32_t created = 0;
		std::uint32_t drawn = 0;            // DrawLightInstance OK - a created light is
		                                    // NOT visible until it is submitted per frame
		std::uint32_t create_fail = 0;
		std::uint32_t draw_fail = 0;
		std::uint32_t no_ps_entry = 0;      // PS never registered (should be ~0)
		std::uint32_t const_miss = 0;       // constants not yet written by the game
		std::uint32_t frames = 0;           // frame_end() calls - 0 means Present isn't hooked
		std::uint32_t api_not_init = 0;     // frames where the Remix API was unavailable
	};
	extern stats g_stats;

	// Print what we actually extracted - positions, colours, radii - so the numbers
	// can be checked against the world instead of assumed correct.
	void dump(std::ostream& os);
	void shutdown();
}
