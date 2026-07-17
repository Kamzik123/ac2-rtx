#pragma once

// AC2 RTX - game lights -> Remix lights (ENTITY ROUTE)
//
// The problem: Remix cannot see AC2's lights. AC2 is forward-rendered with
// per-object light lists evaluated in the pixel shader; it never calls
// SetLight/LightEnable, so a stock Remix scene has nothing but the fallback light.
//
// The channel (this version): we walk the engine's own light nodes. The renderer
// hands each visible graphic node the set of lights that affect it as a
// PtrArray<scimitar::LightNode> passed to scimitar::LightingEnv::Update
// (Win32 RVA 0x1269E60). We hook that function, and for every node we read the
// light straight out of the engine structures:
//
//   LightNode (element of the PtrArray):
//     +0x40  world matrix, 4x4 row-major (row1 @ +0x50 = direction,
//                                         row3 @ +0x70 = world position)
//     +0xA8  -> light struct
//
//   light struct (*(node+0xA8)):
//     vtable[+0x14]() -> RTTI type magic (see the MAGIC_* constants in the .cpp)
//     [5]   +0x14   intensity (float)
//     [67..69] +0x10C  colour rgb
//     u16 @ +0x11C, bit 0x200  -> light is enabled
//     omni: near [72]+0x120, far [73]+0x124
//     spot: cone-outer(rad) [72]+0x120, cone-inner(rad) [73]+0x124,
//           near [75]+0x12C, far [76]+0x130
//
// All of that was re-derived on the Windows exe from the four vector writers
// LightingEnv::Set{Omni,Direct,Spot,Sun}Vectors (0x1268880 / 0x12681F0 /
// 0x1268970 / 0x12682B0), which are the authority on what each field means,
// cross-checked against the register decode documented in README "Lights".
//
// Why this beats the old register/constant path:
//   - IDENTITY: a light is keyed on its LightNode* (stable across frames), so a
//     static torch is ONE Remix light and never flickers. The old path hashed
//     quantised floats and flipped the key on jitter.
//   - POSITION: read from the node's own world matrix, not from a hardcoded and
//     actually-dynamic shader register (c40/c44 landed on someone else's data).
//   - THE SUN: it is just another node (magic 0x5EDC3E04); no special case.
//
// WORLD space, no transform needed - the writers take the position/direction
// straight from the node world matrix and hand it to the shader untransformed
// (the PS differences it against g_EyePosition). Verified in README "Lights".

namespace comp::ac2_lights
{
	// Master switch. Default ON. Gates the LightingEnv::Update walk.
	extern bool g_enabled;

	// Bring up the Remix API (deferred to the first Present - see the .cpp).
	// DEFAULT OFF: it needs `exposeRemixApi = True` in <game>\.trex\bridge.conf.
	// HOME toggles it; toggling OFF also destroys the live lights so ours can be
	// A/B'd against Remix's fallback sun.
	extern bool g_remix_api;

	// Radiance = (colour * intensity) * this. The game's colour and intensity are
	// in arbitrary units, so the mapping to Remix radiance is a tuning constant.
	extern float g_radiance_scale;

	// Sphere emitter radius in metres. NOT the light's range (that is a falloff
	// cutoff). Small = sharp shadows, large = soft.
	extern float g_emitter_radius;

	// Push this frame's unique lights to Remix and retire last frame's. Called
	// from Present. Also installs the LightingEnv::Update hook on first call.
	void frame_end();

	struct stats
	{
		std::uint32_t update_calls = 0;      // LightingEnv::Update hook invocations
		std::uint32_t nodes_walked = 0;      // light-node visits (with duplicates)
		std::uint32_t omni_seen = 0;         // unique, last submitted frame
		std::uint32_t direct_seen = 0;
		std::uint32_t spot_seen = 0;
		std::uint32_t sun_seen = 0;
		std::uint32_t unknown_magic = 0;     // nodes whose type magic we don't know
		std::uint32_t unique_this_frame = 0; // distinct LightNode* this frame
		std::uint32_t registry_size = 0;     // persistent lights currently tracked
		std::uint32_t matched_existing = 0;  // this frame's lights matched to registry
		std::uint32_t new_this_frame = 0;    // this frame's lights that were brand new
		std::uint32_t ptr_prev = 0;          // diagnostic: this frame's node-ptr count
		std::uint32_t ptr_carry = 0;         // ...of which were also present last frame
		std::uint32_t created = 0;           // lifetime CreateLight calls (churn)
		std::uint32_t drawn = 0;             // per-frame DrawLightInstance OK (live lights)
		std::uint32_t create_fail = 0;
		std::uint32_t draw_fail = 0;
		std::uint32_t frames = 0;            // frame_end() calls - 0 means Present isn't hooked
		std::uint32_t api_not_init = 0;      // frames where the Remix API was unavailable
		bool          hook_installed = false;
	};
	extern stats g_stats;

	// Print what we actually extracted - positions, colours, ranges - so the
	// numbers can be checked against the world instead of assumed correct.
	void dump(std::ostream& os);
	void shutdown();
}
