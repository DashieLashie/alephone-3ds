/*
	Aleph One for the Nintendo 3DS, citro3d back end: the texture cache instance
	(docs/3ds/CITRO3D_DESIGN.md, 3.3 and section 7, M2 specification "Generations and the deferred
	flush"). The pure cache is core/texcache; this file owns the one instance and, in part 6, the
	citro3d allocator behind it. Main thread only. Does not need citro3d to be up: the pure cache
	can be created, bumped and torn down before the first present and after C3D_Fini.

	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; either version 3 of the License, or
	(at your option) any later version.
*/
#ifndef A1_C3D_BACK_TEXCACHE_H
#define A1_C3D_BACK_TEXCACHE_H

#include "../core/texcache.h"

namespace a1_c3d {
namespace back {

// The cache, created on first use from the debug-c3d caps (vram_cap, linear_cap, promote_max).
TexCache& texcache();
// Hooks 2a/2b's body (a1_c3d::collections_changed()): bumps the generation and logs
// "[c3d] tex: collections changed (gen N, M entries retire)" when the C3D renderer is selected.
void texcache_collections_changed();
// After C3D_Fini (release_and_fini): frees every slot and block. Idempotent.
void texcache_release_all();

} // namespace back
} // namespace a1_c3d

#endif
