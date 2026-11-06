/*
	Aleph One for the Nintendo 3DS, citro3d back end: the rectangles of M4 (see rect.h).

	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; either version 3 of the License, or
	(at your option) any later version.
*/
#include "rect.h"

namespace a1_c3d {

// scottish_textures.cpp:656-723, on int16 (short) and int32 (_fixed) as there.
bool rect_setup(const RectIn& r, int screen_w, int screen_h, RectBox& out)
{
	const int16_t x0 = static_cast<int16_t>(r.x0), x1 = static_cast<int16_t>(r.x1);
	const int16_t y0 = static_cast<int16_t>(r.y0), y1 = static_cast<int16_t>(r.y1);
	if (!(x0 < x1 && y0 < y1))
		return false;
	int16_t cl = static_cast<int16_t>(r.clip_left), cr = static_cast<int16_t>(r.clip_right);
	int16_t ct = static_cast<int16_t>(r.clip_top), cb = static_cast<int16_t>(r.clip_bottom);
	// :664-673: the screen, then the rectangle.
	if (cl < 0) cl = 0;
	if (cr > screen_w) cr = static_cast<int16_t>(screen_w);
	if (ct < 0) ct = 0;
	if (cb > screen_h) cb = static_cast<int16_t>(screen_h);
	if (cl < x0) cl = x0;
	if (cr > x1) cr = x1;
	if (ct < y0) ct = y0;
	if (cb > y1) cb = y1;
	// :676-678
	if (!(cl < cr && ct < cb && cr > 0 && cl < screen_w && cb > 0 && ct < screen_h))
		return false;
	const int16_t screen_width = static_cast<int16_t>(x1 - x0);
	const int16_t screen_height = static_cast<int16_t>(y1 - y0);
	// :688-693 (INTEGER_TO_FIXED = << 16; _fixed is int32)
	int32_t dx = static_cast<int32_t>(static_cast<uint32_t>(r.W) << 16) / screen_width;
	int32_t tx = dx >> 1;
	const int32_t dy = static_cast<int32_t>(static_cast<uint32_t>(r.H) << 16) / screen_height;
	int32_t ty = 0;
	if (!(dx && dy))
		return false;
	// :696-700
	if (r.flip_h)
	{
		dx = -dx;
		tx = static_cast<int32_t>(static_cast<uint32_t>(r.W) << 16) + (dx >> 1);
	}
	// :703-716 (left and top clipping move the start; right and bottom only shorten)
	int16_t delta = static_cast<int16_t>(cl - x0);
	if (delta > 0)
		tx += delta * dx;
	delta = static_cast<int16_t>(ct - y0);
	if (delta > 0)
		ty += delta * dy;
	out.cl = cl;
	out.cr = cr;
	out.ct = ct;
	out.cb = cb;
	out.tx0 = tx;
	out.dx = dx;
	out.ty0 = ty;
	out.dy = dy;
	return true;
}

// :759-783 for one column: y0/y1 start at the clip, the run's start and end trim them.
void rect_sw_column_rows(const RectBox& b, unsigned first, unsigned last, int* y0_out, int* y1_out)
{
	const int32_t ty0 = b.ty0, dy = b.dy;
	const int16_t screen_height = static_cast<int16_t>(b.cb - b.ct);
	const int32_t ty1 = ty0 + screen_height * dy;
	int16_t y0 = static_cast<int16_t>(b.ct), y1 = static_cast<int16_t>(b.cb);
	if ((ty0 >> 16) < static_cast<int32_t>(first))
	{
		const int16_t delta = static_cast<int16_t>((static_cast<int32_t>(first << 16) - ty0) / dy + 1);
		y0 = static_cast<int16_t>(y1 < y0 + delta ? y1 : y0 + delta);
	}
	if ((ty1 >> 16) > static_cast<int32_t>(last))
	{
		const int16_t delta = static_cast<int16_t>((ty1 - static_cast<int32_t>(last << 16)) / dy + 1);
		y1 = static_cast<int16_t>(y0 > y1 - delta ? y0 : y1 - delta);
	}
	*y0_out = y0;
	*y1_out = y1;
}

} // namespace a1_c3d
