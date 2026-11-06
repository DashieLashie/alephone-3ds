/*
	Aleph One for the Nintendo 3DS, citro3d back end: the rectangles of M4 (docs/3ds/CITRO3D_DESIGN.md, section 7,
	M4 specification, "SW's rectangle, exactly" and "Rectangles on the GPU"). Plain C++, no 3DS or engine headers,
	host-tested against the transliterated SW code (scripts/3ds/tests/c3d-host/test_core.cpp).

	rect_setup is Rasterizer_SW_Class::texture_rectangle's set-up (scottish_textures.cpp:656-723) on the engine's
	types: the clip, texture_dx/x/dy/y0 in 16.16 fixed point, the horizontal flip. The rectangle is then drawn as
	one quad over [cl, cr) x [ct, cb) whose texcoords at a pixel centre are SW's own fixed-point positions:
	column t(X + 1/2) = (tx0 + (X - cl) * dx) / 65536, row s(Y + 1/2) = (ty0 + (Y - ct) * dy) / 65536.

	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; either version 3 of the License, or
	(at your option) any later version.
*/
#ifndef A1_C3D_CORE_RECT_H
#define A1_C3D_CORE_RECT_H

#include <cstdint>

namespace a1_c3d {

// A rectangle_definition's fields (int16 in the engine) and its bitmap's width (columns) and height.
struct RectIn
{
	int x0, y0, x1, y1;
	int clip_left, clip_right, clip_top, clip_bottom;
	int W, H;
	bool flip_h;
};

// What SW draws: the box [cl, cr) x [ct, cb) and, in 16.16 fixed point, the texel column at column cl (tx0) and
// its step (dx, negative when flipped), the texel row at row ct (ty0) and its step (dy).
struct RectBox
{
	int cl, cr, ct, cb;
	int32_t tx0, dx, ty0, dy;
};

// False: SW draws nothing (empty or off-screen box, x0 >= x1 or y0 >= y1, or a zero step).
bool rect_setup(const RectIn& r, int screen_w, int screen_h, RectBox& out);

// The texel column / row (in texels, unbiased) at screen x / y of the box; at a pixel centre (X + 1/2) they are
// SW's positions exactly. Linear in x / y, so a quad's corners carry them.
inline double rect_t(const RectBox& b, double x) { return (b.tx0 + (x - b.cl - 0.5) * b.dx) / 65536.0; }
inline double rect_s(const RectBox& b, double y) { return (b.ty0 + (y - b.ct - 0.5) * b.dy) / 65536.0; }

// SW's rows of column k (0-based from cl) after its per-column trim (scottish_textures.cpp:763-783) for a run
// [first, last) of the column: rows [*y0, *y1) of the screen. Exposed for the tests.
void rect_sw_column_rows(const RectBox& b, unsigned first, unsigned last, int* y0, int* y1);

} // namespace a1_c3d

#endif
