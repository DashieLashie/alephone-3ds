/*
	Aleph One for the Nintendo 3DS, citro3d back end: the overlay surface and texture of direct mode
	(docs/3ds/CITRO3D_DESIGN.md, 5.3 and section 7, M6 specification "Overlay bands", "Overlay surface and
	texture"). Back-internal: it names citro3d and SDL types, so only back/ includes it. Main thread only.

	The engine draws a direct frame's CPU overlays (crosshair, FPS, position, messages, input line) into a 400x240
	SDL surface whose pixels are a 512x256 RGBA8 linear buffer (the GPU's texel word, 0xRRGGBBAA): the composite
	frame uploads the frame's bands of it straight into the overlay texture (no copy, no channel reorder).

	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; either version 3 of the License, or
	(at your option) any later version.
*/
#ifndef A1_C3D_BACK_OVERLAY_H
#define A1_C3D_BACK_OVERLAY_H

#include "../core/overlay_bands.h"

#include <citro3d.h>

#include <cstdint>

struct SDL_Surface;

namespace a1_c3d {
namespace back {

// Hook 3b (a direct frame, after its view frame was ended): the surface for this frame's overlays, with the tiles of
// `bands` (normalised here) cleared where earlier frames wrote. Creates the buffer, surface and texture on first use.
// nullptr: no memory (logged once; the caller then must not treat the frame as direct). With `cal_row` >= 0
// (calibrate 1) the back end writes the frame-indexed band itself: one opaque white 8-row band at cal_row, nothing
// else, and `bands` is ignored.
SDL_Surface* overlay_begin(const Band* bands, int n, int cal_row);
// The surface handed out for the current frame (nullptr: none, or the hand-out was dropped).
const SDL_Surface* overlay_surface_now();
// A hand-out not consumed by a composite (the present was skipped by the gate, or never came): forget it.
void overlay_drop();
// The composite frame, before any draw: one display transfer per band (rows flushed first) into the texture.
// Returns the bands uploaded (and composited) into `out` (room for kMaxBands), 0 = none. Consumes the hand-out.
int overlay_upload(Band* out);
// The overlay texture (valid after a successful overlay_begin).
C3D_Tex* overlay_texture();
// overlay_audit: the surface's non-transparent pixels outside the hand-out's bands (call before overlay_upload).
unsigned overlay_leaks_now();
// The surface's pixels (RGBA8 words, pitch 512), for direct_check's reference; nullptr before creation.
const uint32_t* overlay_pixels();
// Hook 3b's counter of the bottom screen's pending swaps (critic, M6 round 2 minor 1): `bottom_frame`, now before the
// composite on a direct frame, then waits for a VBlank if it presents.
unsigned overlay_bottom_pending();
// direct_policy with a 0 mask: the buffer, surface and texture exist (made here, out of frame). False: they cannot be
// made (logged once): no direct frames.
bool overlay_ensure();
// After C3D_Fini (release_and_fini): the texture, the surface and the buffer are freed. Idempotent.
void overlay_free();
// "uploads=... bands=... clears=... linear_tex=... leaks=..." for the direct stats lines.
int overlay_stats_text(char* buf, unsigned size);

} // namespace back
} // namespace a1_c3d

#endif
