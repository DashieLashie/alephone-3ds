/*
	Aleph One for the Nintendo 3DS, citro3d back end: the pure part of the readback calibration
	(docs/3ds/CITRO3D_DESIGN.md, section 7, M0 specification, "Readback calibration"; 5.2's
	orientation and channel order). The back end draws four coloured corner quads on a clear that
	encodes the frame index, reads the target back, and converts RGBA to ARGB; this decides what
	came out. Plain C++, host-testable.

	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; either version 3 of the License, or
	(at your option) any later version.
*/
#ifndef A1_C3D_CORE_CALIBRATE_H
#define A1_C3D_CORE_CALIBRATE_H

#include <cstdint>
#include <string>

namespace a1_c3d {

// 5.2's conversion of one readback pixel (0xRRGGBBAA as a big-endian RGBA8 word) to 0xAARRGGBB.
inline uint32_t rgba_to_argb(uint32_t rgba) { return (rgba >> 8) | 0xFF000000u; }

// Corner colours as ARGB, in screen position (y down).
constexpr uint32_t kCalTopLeft = 0xFFFF0000u;       // red
constexpr uint32_t kCalTopRight = 0xFF00FF00u;      // green
constexpr uint32_t kCalBottomLeft = 0xFF0000FFu;    // blue
constexpr uint32_t kCalBottomRight = 0xFFFFFFFFu;   // white
constexpr int kCalQuad = 16;                        // corner quads are 16x16 pixels

// The clear colour that encodes frame index k: R = k & 0xFF, G = 0x40, B = 0x80 (never a corner colour).
inline uint32_t cal_clear_argb(unsigned k) { return 0xFF000000u | ((k & 0xFFu) << 16) | (0x40u << 8) | 0x80u; }
// The same colour as 0xRRGGBBAA, the form C3D_RenderTargetClear takes.
inline uint32_t cal_clear_rgba(unsigned k) { return ((k & 0xFFu) << 24) | (0x40u << 16) | (0x80u << 8) | 0xFFu; }

enum class Orientation { Ok, RowsFlipped, ColsFlipped, Rotated180, Transposed, Unknown };

const char* orientation_name(Orientation o);

struct CalResult
{
	Orientation orientation = Orientation::Unknown;
	bool current_frame_ok = false;
	int observed_k = -1;   // the centre's R byte if its G and B are the clear's, else -1
	bool channels_ok = false;   // red arrived as 0xFFFF0000; false whenever the orientation is Unknown
	uint32_t corner[4] = {0, 0, 0, 0};   // TL, TR, BL, BR, ARGB
	uint32_t centre = 0;
};

// `argb` is the converted readback (w x h, `pitch_px` pixels per row, top row first as displayed).
// Samples (4,4), (w-5,4), (4,h-5), (w-5,h-5) and (w/2,h/2).
CalResult cal_check(const uint32_t* argb, int w, int h, int pitch_px, unsigned k);

// "[c3d] readback_cal k=K orientation <name> current frame <ok|NO (centre k=X)|NO (centre 0x........)>
//  channels <ok|NO> corners 0x........ 0x........ 0x........ 0x........", no newline.
std::string cal_line(unsigned k, const CalResult& r);

} // namespace a1_c3d

#endif
