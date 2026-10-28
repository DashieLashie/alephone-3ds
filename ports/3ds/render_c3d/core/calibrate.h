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

// M1: the same conversion keeping the alpha byte (0xRRGGBBAA -> 0xAARRGGBB), so that the coverage of
// a C3D side is alpha != 0 (design 8.1).
inline uint32_t rgba_to_argb_keep(uint32_t rgba) { return (rgba >> 8) | (rgba << 24); }

// Corner colours as ARGB, in screen position (y down).
constexpr uint32_t kCalTopLeft = 0xFFFF0000u;       // red
constexpr uint32_t kCalTopRight = 0xFF00FF00u;      // green
constexpr uint32_t kCalBottomLeft = 0xFF0000FFu;    // blue
constexpr uint32_t kCalBottomRight = 0xFFFFFFFFu;   // white
constexpr int kCalQuad = 16;                        // corner quads are 16x16 pixels
// M1's alpha check: a fifth 16x16 quad, ARGB 0x80206090 (alpha 0x80, RGB as drawn with blend ONE, ZERO), at
// the top centre, x in [w/2 - 8, w/2 + 8), y in [0, 16); the detected orientation moves it (rows flipped
// or rotated 180: bottom centre; transposed: left centre, x in [0, 16), y in [h/2 - 8, h/2 + 8)). Every
// other pixel of the image is expected with alpha 0xFF, so the readback is converted with
// rgba_to_argb_keep.
constexpr uint32_t kCalAlphaQuad = 0x80206090u;

// The clear colour that encodes frame index k: R = k & 0xFF, G = 0x40, B = 0x80 (never a corner colour).
inline uint32_t cal_clear_argb(unsigned k) { return 0xFF000000u | ((k & 0xFFu) << 16) | (0x40u << 8) | 0x80u; }
// The same colour as 0xRRGGBBAA, the form C3D_RenderTargetClear takes.
inline uint32_t cal_clear_rgba(unsigned k) { return ((k & 0xFFu) << 24) | (0x40u << 16) | (0x80u << 8) | 0xFFu; }

// What the back end fills the staging buffer with before each readback transfer (0xRRGGBBAA, as read
// back, and as ARGB after rgba_to_argb): neither the clear colour of frame k (G 0x40, B 0x80) nor any
// corner colour (G, B in {0x00, 0xFF}), and different for every k, so a pixel the transfer did not
// write is told from the previous frame's too.
inline uint32_t cal_sentinel_rgba(unsigned k) { return ((~k & 0xFFu) << 24) | (0xC3u << 16) | (0xA5u << 8) | 0xFFu; }
inline uint32_t cal_sentinel_argb(unsigned k) { return rgba_to_argb(cal_sentinel_rgba(k)); }

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
	// The full-image check: every pixel must be the clear colour of k, or, inside a 16x16 corner quad,
	// the colour that the detected orientation puts there (the "ok" layout while Unknown), or, inside the
	// alpha quad, kCalAlphaQuad.
	// `mismatched` counts the pixels that are neither; `sentinel` how many of those still hold
	// cal_sentinel_argb(k) (the transfer never wrote them). current_frame_ok needs both centre and 0.
	unsigned mismatched = 0;
	unsigned sentinel = 0;
	bool alpha_ok = false;   // all 256 pixels of the alpha quad are exactly kCalAlphaQuad
};

// `argb` is the converted readback (w x h, `pitch_px` pixels per row, top row first as displayed).
// Orientation, channel order and observed_k come from the samples (4,4), (w-5,4), (4,h-5), (w-5,h-5)
// and (w/2,h/2); the whole image is then checked as described at CalResult::mismatched. `argb` is the
// alpha-keeping conversion (rgba_to_argb_keep).
CalResult cal_check(const uint32_t* argb, int w, int h, int pitch_px, unsigned k);

// "[c3d] readback_cal k=K orientation <name> current frame <ok|NO (centre k=X)|NO (centre 0x........)>
//  mismatched N (sentinel S) channels <ok|NO> alpha <ok|NO> corners 0x........ 0x........ 0x........ 0x........", no newline.
std::string cal_line(unsigned k, const CalResult& r);

} // namespace a1_c3d

#endif
