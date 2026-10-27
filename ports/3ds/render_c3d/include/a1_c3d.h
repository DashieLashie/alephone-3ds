/*
	Aleph One for the Nintendo 3DS, citro3d back end (docs/3ds/CITRO3D_DESIGN.md, 1.4 "Engine-facing"):
	what the engine calls. Included by RenderMain/render.cpp under
	#if defined(A1_PLATFORM_3DS) && defined(A1_RENDER_C3D) only. No <3ds.h>.

	Only what exists in M0 is declared. collections_changed() (hooks 2a, 2b) arrives with M2;
	overlay_surface(), overlay_dirty() and direct_frame() (hook 3) with M6.

	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; either version 3 of the License, or
	(at your option) any later version.
*/
#ifndef A1_C3D_H
#define A1_C3D_H

class RasterizerClass;
struct view_data;
struct bitmap_definition;

namespace a1_c3d {

using DrawFn = void (*)(void* ctx, RasterizerClass* ras);

// One world view. true: frame done (in `dest`, or on the GPU in direct mode).
// false: caller draws it with `sw`; nothing was written to `dest`.
bool render_frame(view_data& view, bitmap_definition* dest, RasterizerClass* sw,
                  DrawFn draw, void* ctx);

} // namespace a1_c3d

#endif
