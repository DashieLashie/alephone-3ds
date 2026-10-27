/*
	Aleph One for the Nintendo 3DS, citro3d back end: readback calibration check (see calibrate.h).

	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; either version 3 of the License, or
	(at your option) any later version.
*/
#include "calibrate.h"

#include <cstdio>

namespace a1_c3d {

const char* orientation_name(Orientation o)
{
	switch (o)
	{
	case Orientation::Ok: return "ok";
	case Orientation::RowsFlipped: return "rows flipped";
	case Orientation::ColsFlipped: return "columns flipped";
	case Orientation::Rotated180: return "rotated 180";
	case Orientation::Transposed: return "transposed";
	case Orientation::Unknown: break;
	}
	return "unknown";
}

CalResult cal_check(const uint32_t* argb, int w, int h, int pitch_px, unsigned k)
{
	CalResult r;
	if (w < 10 || h < 10)   // no room for the four sample points
		return r;
	auto at = [&](int x, int y) { return argb[static_cast<size_t>(y) * static_cast<size_t>(pitch_px) + x]; };
	r.corner[0] = at(4, 4);
	r.corner[1] = at(w - 5, 4);
	r.corner[2] = at(4, h - 5);
	r.corner[3] = at(w - 5, h - 5);
	r.centre = at(w / 2, h / 2);

	const uint32_t R = kCalTopLeft, G = kCalTopRight, B = kCalBottomLeft, W = kCalBottomRight;
	auto is = [&](uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
		return r.corner[0] == a && r.corner[1] == b && r.corner[2] == c && r.corner[3] == d;
	};
	if (is(R, G, B, W))
		r.orientation = Orientation::Ok;
	else if (is(B, W, R, G))
		r.orientation = Orientation::RowsFlipped;
	else if (is(G, R, W, B))
		r.orientation = Orientation::ColsFlipped;
	else if (is(W, B, G, R))
		r.orientation = Orientation::Rotated180;
	else if (is(R, B, G, W))
		r.orientation = Orientation::Transposed;
	else
		r.orientation = Orientation::Unknown;

	// The colour of each corner quad under the detected orientation (TL, TR, BL, BR).
	static const uint32_t layout[5][4] = {
		{R, G, B, W}, {B, W, R, G}, {G, R, W, B}, {W, B, G, R}, {R, B, G, W}};
	const int li = r.orientation == Orientation::Unknown ? 0 : static_cast<int>(r.orientation);
	const uint32_t clear = cal_clear_argb(k), sentinel = cal_sentinel_argb(k);
	for (int y = 0; y < h; ++y)
		for (int x = 0; x < w; ++x)
		{
			uint32_t want = clear;
			if ((x < kCalQuad || x >= w - kCalQuad) && (y < kCalQuad || y >= h - kCalQuad))
				want = layout[li][(y >= h - kCalQuad ? 2 : 0) + (x >= w - kCalQuad ? 1 : 0)];
			const uint32_t got = at(x, y);
			if (got != want)
			{
				++r.mismatched;
				r.sentinel += got == sentinel;
			}
		}

	r.current_frame_ok = r.centre == clear && r.mismatched == 0;
	if (((r.centre >> 8) & 0xFF) == 0x40 && (r.centre & 0xFF) == 0x80)
		r.observed_k = static_cast<int>((r.centre >> 16) & 0xFF);
	// Red arrived as red. Only judged when the corners form a known arrangement: the four colours are
	// closed under an R/B swap (red <-> blue), so with an unknown arrangement a swapped red could sit
	// where a true one would (a swapped "ok" image has red at the bottom left). Known arrangements
	// hold the exact colours by construction, so this is the same as "some corner is 0xFFFF0000".
	r.channels_ok = false;
	if (r.orientation != Orientation::Unknown)
		for (uint32_t c : r.corner)
			if (c == kCalTopLeft)
				r.channels_ok = true;
	return r;
}

std::string cal_line(unsigned k, const CalResult& r)
{
	char frame[48];
	if (r.current_frame_ok)
		std::snprintf(frame, sizeof frame, "ok");
	else if (r.observed_k >= 0)
		std::snprintf(frame, sizeof frame, "NO (centre k=%d)", r.observed_k);
	else
		std::snprintf(frame, sizeof frame, "NO (centre 0x%08X)", static_cast<unsigned>(r.centre));
	char buf[320];
	std::snprintf(buf, sizeof buf,
	              "[c3d] readback_cal k=%u orientation %s current frame %s mismatched %u (sentinel %u) "
	              "channels %s corners 0x%08X 0x%08X 0x%08X 0x%08X",
	              k, orientation_name(r.orientation), frame, r.mismatched, r.sentinel, r.channels_ok ? "ok" : "NO",
	              static_cast<unsigned>(r.corner[0]), static_cast<unsigned>(r.corner[1]),
	              static_cast<unsigned>(r.corner[2]), static_cast<unsigned>(r.corner[3]));
	return std::string(buf);
}

} // namespace a1_c3d
