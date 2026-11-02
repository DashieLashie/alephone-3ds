/*
	Aleph One for the Nintendo 3DS, citro3d back end: the texture cache instance
	(docs/3ds/CITRO3D_DESIGN.md, 3.3 and section 7, M2 specification "Generations and the deferred
	flush", "Upload model"). The pure cache is core/texcache; this file owns the one instance and the
	citro3d allocator behind it (one C3D_Tex per texture: C3D_TexInit in linear memory, C3D_TexInitVRAM
	for a promotion, C3D_TexDelete). Main thread only. Plain header: no <3ds.h>, no <citro3d.h>.
	The pure cache can be created, bumped and torn down before the first present and after C3D_Fini
	(C3D_TexDelete only frees memory: libcitro3d.a texture.o C3D_TexDelete = vramFree / linearFree).

	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; either version 3 of the License, or
	(at your option) any later version.
*/
#ifndef A1_C3D_BACK_TEXCACHE_H
#define A1_C3D_BACK_TEXCACHE_H

#include "../core/tex_convert.h"
#include "../core/texcache.h"

#include <cstdint>

namespace a1_c3d {
namespace back {

// What the front end hands over with a textured draw: the engine bitmap (its address is the key), a view
// of its rows, the 32-bit shading tables and the conversion kind (normal_keyed for a _TRANSPARENT_BIT
// bitmap on a wall, else normal).
struct TexSource
{
	const void* id = nullptr;
	TexBitmap bitmap;
	const uint32_t* tables = nullptr;
	TexKind kind = TexKind::Normal;
};

// The cache, created on first use from the debug-c3d caps (vram_cap, linear_cap, promote_max).
TexCache& texcache();
// A view frame's start, after frame_begin() returned and before the clear (M2 "Upload model"): frees
// what the retired frames left (on_retired), starts cache frame `frame`, then issues this frame's VRAM
// promotions (each one C3D_SyncTextureCopy, its GX entry taken by the cache's rule). Zone c3d_tex.
void texcache_frame_start(uint32_t frame);
enum class TexGet { Ok, Discard, Unsupported };
// Inside a view frame: the C3D_Tex* (opaque) to sample for `src`. A hit; or a miss converted straight into
// a new linear texture and flushed (exactly its bytes, GSPGPU_FlushDataCache). Discard: no room even after
// eviction (the check is "discarded (tex)"); Unsupported: a bitmap the converter refuses. Zone c3d_tex on a miss.
TexGet texcache_get(const TexSource& src, void** out);
// Hooks 2a/2b's body (a1_c3d::collections_changed()): bumps the generation and logs
// "[c3d] tex: collections changed (gen N, M entries retire)" when the C3D renderer is selected.
void texcache_collections_changed();
// "[c3d] tex: ... [tag]" (the stats line), if the cache exists.
void texcache_log(const char* tag);
// After C3D_Fini (release_and_fini): frees every slot and block. Idempotent.
void texcache_release_all();

} // namespace back
} // namespace a1_c3d

#endif
