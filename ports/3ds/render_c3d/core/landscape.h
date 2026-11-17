/*
	Aleph One for the Nintendo 3DS, citro3d back end: the landscapes of M5 (docs/3ds/CITRO3D_DESIGN.md, section 7,
	M5 specification, "SW's landscape, exactly" and "Landscapes on the GPU"). Plain C++, no 3DS or engine headers,
	host-tested against the transliterated SW code (scripts/3ds/tests/c3d-host/test_core.cpp).

	SW (scottish_textures.cpp:1102-1170, low_level_textures.h:245-262): pixel x of a line reads texel
	s(x) = floor(V / 65536) mod 2^bits, V = first + x * hpd; line y reads row R - 1 - PIN(k' + (R >> 1), 0, R - 1),
	k = (y - hh - dtp) * vpd >> 16, k' = k (or folded into (-h2, h2] with VertRepeat). Both are affine in the
	screen before the floor, so the GPU draws a landscape with w = 1 and texcoords at the vertices:
	u = V / 65536 (+ eps), v = (R - (R >> 1)) - f' - eps + delta (f' = k' before the floor, eps = 1/131072); the clamp
	of the padded rows (R .. T - 1 repeat row R - 1) is SW's PIN. delta (landscape_row_offset) centres v in the
	feasible interval of the polygon's lines: SW's row positions are often exact integers, which float32 corners cannot
	hold to 1/131072 (M5 specification, "The row offset").

	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; either version 3 of the License, or
	(at your option) any later version.
*/
#ifndef A1_C3D_CORE_LANDSCAPE_H
#define A1_C3D_CORE_LANDSCAPE_H

#include <cstdint>

#include "geometry.h"

namespace a1_c3d {

// The view_data fields the landscape mapper reads.
struct LandView
{
	int hh, dtp;                  // half_screen_height, dtanpitch (shorts in view_data)
	int landscape_yaw, half_cone; // angles
	int standard_screen_width;
};

// LandscapeOptions (ViewControl.h:81-106) of the polygon's ShapeDesc.
struct LandOpts
{
	int horiz_exp = 1, vert_exp = 1, asp_rat_exp = 0, azimuth = 0;
	bool vert_repeat = false;
};

// The mapper's constants for one polygon.
struct LandParams
{
	int32_t first = 0, hpd = 0, vpd = 0;   // first_horizontal_pixel, horizontal / vertical_pixel_delta (16.16)
	int bits = 0;                          // NextLowerExponent(texture->height): texels per row = 2^bits
	int rows = 0;                          // R = texture->width (row_addresses)
	int h2 = 0;                            // VertRepeat: height_repeat_shift (the fold's half period); 0 without
	int hh = 0, dtp = 0;
	// The texture (M5 revision: Marathon 2's landscapes are 540 rows of 1024): 2^bits texels per memory row; rows
	// [0, 512) in part 0 and rows [512, R) in part 1 when R > 512 (else one part of all R rows), each padded to a
	// power of two of rows by repeating its last row.
	unsigned S = 0;
	unsigned T[2] = {0, 0};
	int parts = 1;
};

constexpr int kLandPartRows = 512;
// The row the texture is cut at: kLandPartRows, or another for the test key land_cut (critic, M5 code review major 2:
// Marathon 2's pitch never shows rows past 512, so the test moves the cut into the sky just above the horizon, row 269
// of 540, to run part 1 and the cut-crossing span path on the GPU). Part 0 is padded to pot8(cut) rows. Set once,
// before any landscape texture exists.
int landscape_part_rows();
void landscape_set_part_rows(int rows);   // 64..512; anything else is ignored

constexpr double kLandEps = 1.0 / 131072.0;

// The POT textures of a landscape bitmap of `width` rows (row_addresses) of `height` texels: S = height (a power
// of two in [8, 1024]); `width` in [1, 1024]; part 0 = rows [0, min(width, 512)) and, when width > 512, part 1 =
// rows [512, width); T[i] = pot(max(rows of part i, 8)); *parts = 1 or 2. False: RLE or anything else.
bool landscape_tex_size(int width, int height, bool rle, unsigned& S, unsigned* T, int* parts);

// The constants of :1113-1153 (int32 arithmetic as SW; the shift of `first` wraps as an unsigned shift would).
// False when landscape_tex_size refuses the bitmap or standard_screen_width <= 0.
bool landscape_setup(const LandView& v, const LandOpts& o, int width, int height, bool rle, LandParams& out);

// SW, transliterated: the texel column of screen column x, the line's k (short y0, arithmetic shift), k', the row.
int landscape_sw_col(const LandParams& p, int x);
int landscape_sw_k(const LandParams& p, int y);
int landscape_sw_kfold(const LandParams& p, int k);
int landscape_sw_row(const LandParams& p, int y);

// The band of line y (VertRepeat: floor((k + h2) / 2h2); 0 without), and its part (SW's row >= 512: 1, else 0).
int landscape_band(const LandParams& p, int y);
int landscape_part(const LandParams& p, int y);
// band * 2 + part: lines of one piece share their texture and their v formula.
inline int landscape_piece(const LandParams& p, int y) { return landscape_band(p, y) * 2 + landscape_part(p, y); }

// The GPU's texcoords at the screen point (ex, ey) for band n, in texels (u) and rows of the whole bitmap (v), before
// the bias on u and the normalisation: u = (first + ex * hpd) / 65536, v = (R - (R >> 1)) - (f(ey) - 2 h2 n) - eps +
// row_off.
void landscape_uv(const LandParams& p, double ex, double ey, int band, double& u, double& v, double row_off = 0.0);

// The row offset of the lines [y0, y1) of band `band` and part `part`: the middle of [max(r - g), min(r + 1 - g)) over
// those lines whose row is not pinned (g = v at the line with row_off 0, r = SW's row); 0 if every such line is pinned
// (any offset in (-1, 1) keeps a pinned line's row: its part's clamp reads it) or the interval is empty (never for
// SW's own rows: the offset 0 is in it).
double landscape_row_offset(const LandParams& p, int y0, int y1, int band, int part);

// The n integer vertices' attributes at (X + delta, Y + delta) for band `band` and part `part`: w = 1,
// s = (u + eps - 2^bits m) / S with m = floor(min u / 2^bits) (whole repeats: GPU_REPEAT keeps the phase),
// t = (v - 512 part) / T[part] (with row_off). NonFinite: a non-finite value. (Never Degenerate.)
TexResult landscape_attrs(const LandParams& p, const Point2* pts, int n, double delta, int band, int part,
                          double row_off, TexAttr* out);

} // namespace a1_c3d

#endif
