/*
	Aleph One for the Nintendo 3DS, citro3d back end: the shade LUT's texture and (M5) the static noise's (docs/3ds/CITRO3D_DESIGN.md,
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
// Unit 1 back to "no texture" after a lit view frame (design 7, M3 "Binding and the state the present relies
// on", revised after the acceptance): the present's shader has no texcoord1 output, and an enabled unit 1
// samples an undefined coordinate (Azahar's software backend segfaults on it: Pica::Texture::LookupTexelInTile).
// C3D_TexBind(1, NULL) cannot be used (it reads NULL + 12, texture.o C3D_TexBind+0x10-0x20), so this does what
// C3D_TexBind does for unit 0 (+0x24-0x38: the pointer into the context at +0x118 + 4 * unit, dirty bit 23 +
// unit), for unit 1 and NULL; then the state flush disables the unit (base.o C3Di_UpdateContext+0x1e0-0x230).
// Only after `shade_check_bound(lut)` has confirmed the context layout once (the slot holds what the real
// C3D_TexBind stored); otherwise unit 1 keeps the LUT, as before (logged once). Returns true if it unbound.
bool shade_unbind_unit1();
// M6b: the texture lit view frames leave on unit 1 when shade_unbind_unit1 cannot unbind (the fallback): the LUT's
// current copy, nullptr before it is built. The fade passes put it back after using unit 1.
void* shade_lut_bound();
// Right after __real_C3D_TexBind(1, lut): checks that citro3d 1.7.1's context holds `lut` where
// shade_unbind_unit1 writes. The first call decides.
void shade_check_bound(const void* lut);
// M5 (design 7, M5 specification "Static on the GPU"): the static noise (256 x 256 RGBA8, core/noise), the LUT's
// lifecycle. noise_frame_start: at every view frame's start after shade_frame_start (nothing until the noise is
// built; then its promotion, as the LUT's). noise_get: what unit 1 samples for a static draw this frame, building the
// noise in linear memory on the first call of the run (a CPU fill inside the frame); nullptr = no memory (the caller
// discards tex). Zone c3d_tex.
void noise_frame_start();
void* noise_get();
const char* noise_state_name();
// After C3D_Fini (release_and_fini): deletes both copies of the LUT and of the noise, state None. Idempotent.
void shade_free();
// For the "[c3d] cmdbuf" line.
const char* shade_state_name();
unsigned shade_builds();

} // namespace back
} // namespace a1_c3d

#endif
