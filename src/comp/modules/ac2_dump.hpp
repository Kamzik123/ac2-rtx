#pragma once

// AC2 RTX - runtime dump/analysis module
//
// Answers the two open questions blocking a fixed-function conversion spec:
//   1) What does the vertex shader actually do with pos.w?
//        -> dump every VS bytecode + disassembly + its D3DX constant table
//           (name -> register), since AC2's shaders live in MaterialTemplate
//           assets, not in the exe. They still pass through CreateVertexShader.
//   2) Is |pos.w| constant per mesh, and what does its sign mean?
//        -> sample stream-0 vertex buffers at draw time and histogram w.
//
// Reverse-engineering context (AssassinsCreedIIGame.exe md5 2dd4d3c1...):
//   - Vertex declarations carry NO semantics: every element is declared as
//     D3DDECLUSAGE_TEXCOORD with UsageIndex == the D3D9 vertex attribute slot.
//     Slot 0 = POSITION, 1 = BLENDWEIGHT, 2 = NORMAL, 3 = COLOR0,
//     7 = BLENDINDICES, 8/9 = TEXCOORD0/1, 14/15 = TANGENT/BINORMAL.
//   - Expected: g_WorldViewProj at c0..c3, g_BoneMatrixArray at c120.
//     The constant-table dump verifies this against the live game.

namespace comp::ac2_dump
{
	// Master switch. Toggle at runtime with F9.
	extern bool g_enabled;

	// Creates the dump directory and resolves D3DX entry points from the
	// copy of d3dx9_42.dll the game already has loaded. Safe to call repeatedly.
	void initialize();

	// Flushes the pos.w report. Called on F10 and at shutdown.
	void write_report();
	void shutdown();

	// Hook: IDirect3DDevice9::CreateVertexShader.
	// Dumps bytecode (.cso), disassembly + constant table (.asm). Deduped by hash.
	void on_create_vertex_shader(const DWORD* pFunction);

	// Hook: IDirect3DDevice9::CreatePixelShader (call AFTER the real create, so
	// the shader pointer exists). Dumps like the VS path, and additionally works
	// out which sampler slot holds the diffuse and registers it with ac2_ff.
	void on_create_pixel_shader(const DWORD* pFunction, IDirect3DPixelShader9* shader);

	// Texture lifecycle instrumentation.
	// Remix expects a texture's identity to stay stable after creation, but AC2
	// streams aggressively. We need to know WHICH reuse pattern it uses before we
	// can fix it:
	//   (a) Create once, then LockRect/UnlockRect new content into the same object
	//       -> Remix's hash goes stale; needs re-hash or recreate-on-write.
	//   (b) Release + CreateTexture, with the allocator handing back the SAME
	//       pointer -> anything keyed on the pointer confuses two distinct textures.
	// Pointer-reuse counts distinguish (b); lock counts distinguish (a).
	void on_create_texture(UINT w, UINT h, UINT levels, DWORD usage, D3DFORMAT fmt,
		D3DPOOL pool, IDirect3DTexture9* tex);
	void on_set_texture(DWORD stage, IDirect3DBaseTexture9* tex);

	// Hook: IDirect3DDevice9::DrawIndexedPrimitive (called BEFORE the real draw).
	// Samples stream 0 and accumulates pos.w statistics per unique format.
	// BaseVertexIndex/MinVertexIndex matter: AC2 packs many meshes into one VB,
	// so the draw's vertices start at (BaseVertexIndex + MinVertexIndex), not 0.
	void on_draw_indexed_prim(IDirect3DDevice9* dev, INT BaseVertexIndex,
		UINT MinVertexIndex, UINT NumVertices);
}
