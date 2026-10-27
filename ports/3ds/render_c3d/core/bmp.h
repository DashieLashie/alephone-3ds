/*
	Aleph One for the Nintendo 3DS, citro3d back end: 32-bit BMP encoder for the diff and
	calibration dumps (docs/3ds/CITRO3D_DESIGN.md, section 7, M0 specification). Plain C++,
	host-testable.

	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; either version 3 of the License, or
	(at your option) any later version.
*/
#ifndef A1_C3D_CORE_BMP_H
#define A1_C3D_CORE_BMP_H

#include <cstdint>
#include <vector>

namespace a1_c3d {

// px: w x h pixels 0xAARRGGBB, top row first, `pitch_px` pixels per row. Returns an uncompressed
// 32-bit BMP (BITMAPFILEHEADER + BITMAPINFOHEADER, BI_RGB, bottom-up rows, bytes B,G,R,A).
// Empty for w <= 0 or h <= 0.
std::vector<uint8_t> bmp_encode(const uint32_t* px, int w, int h, int pitch_px);

} // namespace a1_c3d

#endif
