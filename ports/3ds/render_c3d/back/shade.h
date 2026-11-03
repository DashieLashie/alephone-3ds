/*
	Aleph One for the Nintendo 3DS, citro3d back end: the shade LUT's texture (docs/3ds/CITRO3D_DESIGN.md,
	4.2 and section 7, M3 specification "The LUT's lifecycle"). One 256 x 256 L8 texture, built once by
	core/shade_lut into linear memory, promoted to VRAM in a later view frame through M2's in-frame
	TextureCopy, and bound to texture unit 1 by lit view frames (back/draw.cpp). Main thread only. Plain
	header: no <3ds.h>, no <citro3d.h>.

	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; either version 3 of the License, or
	(at your option) any later version.
*/
#ifndef A1_C3D_BACK_SHADE_H
#define A1_C3D_BACK_SHADE_H

namespace a1_c3d {
namespace back {

// A lit view frame's start: after frame_begin() returned, before the texture cache's promotions and the clear
// (the promotion needs an empty command buffer). None: builds the LUT in linear memory and flushes it. Linear:
// issues its VRAM promotion if the GX rule allows (still linear this frame). Copying: the copy retired (this
// frame_begin returned), so unit 1 is re-bound to the VRAM copy and the linear one deleted. Returns the C3D_Tex*
// (opaque) unit 1 samples this frame; nullptr = no memory for the LUT (the check is discarded tex). Zone c3d_tex.
void* shade_frame_start();
// After C3D_Fini (release_and_fini): deletes both copies, state None. Idempotent.
void shade_free();
// For the "[c3d] cmdbuf" line.
const char* shade_state_name();
unsigned shade_builds();

} // namespace back
} // namespace a1_c3d

#endif
