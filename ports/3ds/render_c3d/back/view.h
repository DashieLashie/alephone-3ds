/*
	Aleph One for the Nintendo 3DS, citro3d back end: the view target and its readback, M0's
	calibration (docs/3ds/CITRO3D_DESIGN.md, section 7, M0 specification, "Readback
	calibration"). <3ds.h> / <citro3d.h> side only in view.cpp; this header is plain, so front/
	includes it. No engine headers.

	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; either version 3 of the License, or
	(at your option) any later version.
*/
#ifndef A1_C3D_BACK_VIEW_H
#define A1_C3D_BACK_VIEW_H

#include "../core/calibrate.h"

#include <cstdint>

namespace a1_c3d {
namespace back {

// Design 7, M0 "Readback calibration". One calibration frame k for a w x h view (main thread, from
// front/frame.cpp's render_frame, i.e. between two presents). False = skipped: citro3d is not up,
// a release is pending, or the gate is closed (5.6.1 a), or w/h unsupported (not multiples of 8, > 1024,
// more than 262144 pixels) or out of memory (logged once). True: `out` holds the check, and *pixels
// (if non-null) points to the converted w x h ARGB readback (pitch w), valid until the next call or
// view_free().
bool view_calibrate(unsigned k, int w, int h, CalResult& out, const uint32_t** pixels);

// After C3D_Fini (release_and_fini): forgets the view target pointer (C3D_Fini destroyed it) and
// frees the readback staging, the vertex buffer and the converted copy. Idempotent.
void view_free();

} // namespace back
} // namespace a1_c3d

#endif
