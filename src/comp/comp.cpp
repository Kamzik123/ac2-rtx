#include "std_include.hpp"

#include "modules/imgui.hpp"
#include "modules/renderer.hpp"
#include "shared/common/remix_api.hpp"

// see comment in main()
//#include "shared/common/dinput_hook_v1.hpp"
//#include "shared/common/dinput_hook_v2.hpp"

namespace comp
{
	void on_begin_scene_cb()
	{
		if (!tex_addons::initialized) {
			tex_addons::init_texture_addons();
		}

		// fake camera

		const auto& im = imgui::get();
		if (im->m_dbg_use_fake_camera)
		{
			D3DXMATRIX view_matrix
			(
				1.0f, 0.0f, 0.0f, 0.0f,
				0.0f, 0.447f, 0.894f, 0.0f,
				0.0f, -0.894f, 0.447f, 0.0f,
				0.0f, 100.0f, -50.0f, 1.0f
			);

			D3DXMATRIX proj_matrix
			(
				1.359f, 0.0f, 0.0f, 0.0f,
				0.0f, 2.414f, 0.0f, 0.0f,
				0.0f, 0.0f, 1.001f, 1.0f,
				0.0f, 0.0f, -1.0f, 0.0f
			);

			// Construct view matrix
			D3DXMATRIX rotation, translation;
			D3DXMatrixRotationYawPitchRoll(&rotation,
				D3DXToRadian(im->m_dbg_camera_yaw),		// Yaw in radians
				D3DXToRadian(im->m_dbg_camera_pitch),	// Pitch in radians
				0.0f);									// No roll for simplicity

			D3DXMatrixTranslation(&translation,
				-im->m_dbg_camera_pos[0], // Negate for camera (moves world opposite)
				-im->m_dbg_camera_pos[1],
				-im->m_dbg_camera_pos[2]);

			D3DXMatrixMultiply(&view_matrix, &rotation, &translation);

			// Construct projection matrix
			D3DXMatrixPerspectiveFovLH(&proj_matrix,
				D3DXToRadian(im->m_dbg_camera_fov), // FOV in radians
				im->m_dbg_camera_aspect,
				im->m_dbg_camera_near_plane,
				im->m_dbg_camera_far_plane);

			shared::globals::d3d_device->SetTransform(D3DTS_WORLD, &shared::globals::IDENTITY);
			shared::globals::d3d_device->SetTransform(D3DTS_VIEW, &view_matrix);
			shared::globals::d3d_device->SetTransform(D3DTS_PROJECTION, &proj_matrix);
		}


		// Actual camera setup here if matrices are available
		{
			shared::globals::d3d_device->SetTransform(D3DTS_WORLD, &shared::globals::IDENTITY); // does not hurt

			// Example code if you managed to find some kind of matrix struct
				//if (const auto viewport = game::vp; viewport)
				//{
				//	shared::globals::d3d_device->SetTransform(D3DTS_VIEW, &viewport->view);
				//	shared::globals::d3d_device->SetTransform(D3DTS_PROJECTION, &viewport->proj);
				//}
		}
	}


	void main()
	{
		// #Step 2: init remix api if you want to use it or comment it otherwise
		// Requires "exposeRemixApi = True" in the "bridge.conf" that is located in the .trex folder
		//
		// NOT INITIALISED HERE. main() runs at ASI load time, which is too early:
		// bridge_initRemixApi() resolves remixapi_InitializeLibrary out of d3d9.dll,
		// and initialize() then calls init_debug_lines(), which creates Remix meshes
		// and materials before a D3D9 device exists. While bridge.conf was absent the
		// init simply FAILED and the whole path stayed dead - which is exactly why
		// the mod created zero lights. Adding bridge.conf made the init succeed at
		// load time and it crashed at startup.
		//
		// So defer it to the first draw, which guarantees both d3d9.dll and the device
		// are up. See ac2_lights::ensure_remix_api().

		// init modules which do not need to be initialized, before the game inits, here
		shared::common::loader::module_loader::register_module(std::make_unique<imgui>());
		shared::common::loader::module_loader::register_module(std::make_unique<renderer>());

		// #Step 3: hook dinput if your game uses direct input (for ImGui) - ONLY USE ONE
		//shared::common::loader::module_loader::register_module(std::make_unique<shared::common::dinput_v1>()); // v1: might cause issues with the Alt+X menu
		//shared::common::loader::module_loader::register_module(std::make_unique<shared::common::dinput_v2>()); // v2: better but might need further tweaks

		MH_EnableHook(MH_ALL_HOOKS);
	}
}
