/*
	Aleph One for the Nintendo 3DS, citro3d back end: the landscapes of M5 (see landscape.h).

	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; either version 3 of the License, or
	(at your option) any later version.
*/
#include "landscape.h"

#include <cmath>

namespace a1_c3d {

namespace {

int next_lower_exponent(int n)   // low_level_textures.h:120-125
{
	int xp = 0;
	while (n > 1)
	{
		n >>= 1;
		++xp;
	}
	return xp;
}

unsigned pot8(int v)
{
	unsigned p = 8;
	while (p < static_cast<unsigned>(v))
		p <<= 1;
	return p;
}

// floor(a / b) for b > 0.
int floor_div(int a, int b)
{
	return a >= 0 ? a / b : -((-a + b - 1) / b);
}

} // namespace

bool landscape_tex_size(int width, int height, bool rle, unsigned& S, unsigned* T, int* parts)
{
	if (rle || width < 1 || width > 1024 || height < 8 || height > 1024 || (height & (height - 1)) != 0)
		return false;
	S = static_cast<unsigned>(height);
	const int n = width > kLandPartRows ? 2 : 1;
	if (T)
	{
		T[0] = pot8(width < kLandPartRows ? width : kLandPartRows);
		T[1] = n == 2 ? pot8(width - kLandPartRows) : 0;
	}
	if (parts)
		*parts = n;
	return true;
}

bool landscape_setup(const LandView& v, const LandOpts& o, int width, int height, bool rle, LandParams& out)
{
	LandParams p;
	if (!landscape_tex_size(width, height, rle, p.S, p.T, &p.parts) || v.standard_screen_width <= 0)
		return false;
	p.bits = next_lower_exponent(height);
	p.rows = width;
	p.hh = v.hh;
	p.dtp = v.dtp;
	// :1122-1124 (FIXED_FRACTIONAL_BITS 16, ANGULAR_BITS 9). The shift of `first` as unsigned: SW's int32 shift of a
	// small angle never overflows, and an unsigned one gives the same bits where it would.
	const int hshift = p.bits + o.horiz_exp + 16 - 9;
	p.first = static_cast<int32_t>(static_cast<uint32_t>(v.landscape_yaw + o.azimuth) << hshift);
	p.hpd = static_cast<int32_t>((static_cast<int32_t>(v.half_cone) << (1 + hshift)) / v.standard_screen_width);
	p.vpd = static_cast<int32_t>((static_cast<int32_t>(v.half_cone) << (1 + p.bits + o.vert_exp + 16 - 9)) /
	                             v.standard_screen_width);
	if (o.vert_repeat)
	{
		// :1141-1146: repeat_texture_height = texture->height >> OGL_AspRatExp, its half.
		const int repeat = height >> o.asp_rat_exp;
		p.h2 = repeat >> 1;
		if (p.h2 < 1)
			return false;
	}
	out = p;
	return true;
}

int landscape_sw_col(const LandParams& p, int x)
{
	// :1164-1165, :245-262: source_x = (first + x0 * hpd) << free_bits, advanced by hpd << free_bits per pixel (uint32),
	// read at source_x >> (32 - bits).
	const int free_bits = 32 - 16 - p.bits;
	const uint32_t v = static_cast<uint32_t>(p.first) + static_cast<uint32_t>(x) * static_cast<uint32_t>(p.hpd);
	return static_cast<int>((v << free_bits) >> (32 - p.bits));
}

int landscape_sw_k(const LandParams& p, int y)
{
	// :1151, :1157: y0 (short) -= half_screen_height + dtanpitch; FIXED_INTEGERAL_PART(y0 * vpd) into a short.
	const short y0 = static_cast<short>(y - (p.hh + p.dtp));
	return static_cast<short>(static_cast<int32_t>(y0 * p.vpd) >> 16);
}

int landscape_sw_kfold(const LandParams& p, int k)
{
	if (!p.h2)
		return k;
	// :1158-1160 on shorts.
	return static_cast<short>(((k + p.h2) & (2 * p.h2 - 1)) - p.h2);
}

int landscape_sw_row(const LandParams& p, int y)
{
	// :1161: source_y = texture_height - PIN(y_txtr_offset + height_shift, 0, height_reduced) - 1.
	const int k = landscape_sw_kfold(p, landscape_sw_k(p, y));
	int a = k + (p.rows >> 1);
	if (a < 0)
		a = 0;
	if (a > p.rows - 1)
		a = p.rows - 1;
	return p.rows - a - 1;
}

int landscape_part(const LandParams& p, int y)
{
	return p.parts == 2 && landscape_sw_row(p, y) >= kLandPartRows ? 1 : 0;
}

int landscape_band(const LandParams& p, int y)
{
	if (!p.h2)
		return 0;
	return floor_div(landscape_sw_k(p, y) + p.h2, 2 * p.h2);
}

void landscape_uv(const LandParams& p, double ex, double ey, int band, double& u, double& v, double row_off)
{
	u = (static_cast<double>(p.first) + ex * static_cast<double>(p.hpd)) / 65536.0;
	const double f = (ey - static_cast<double>(p.hh + p.dtp)) * static_cast<double>(p.vpd) / 65536.0;
	v = static_cast<double>(p.rows - (p.rows >> 1)) - (f - 2.0 * p.h2 * band) - kLandEps + row_off;
}

double landscape_row_offset(const LandParams& p, int y0, int y1, int band, int part)
{
	double lo = -1.0, hi = 1.0;
	bool any = false;
	for (int y = y0; y < y1; ++y)
	{
		if (landscape_band(p, y) != band || landscape_part(p, y) != part)
			continue;
		const int a = landscape_sw_kfold(p, landscape_sw_k(p, y)) + (p.rows >> 1);
		if (a < 0 || a > p.rows - 1)
			continue;   // pinned
		double u, g;
		landscape_uv(p, 0.0, y, band, u, g);
		const int r = landscape_sw_row(p, y);
		lo = std::fmax(lo, r - g);
		hi = std::fmin(hi, r + 1 - g);
		any = true;
	}
	return any && lo < hi ? 0.5 * (lo + hi) : 0.0;
}

TexResult landscape_attrs(const LandParams& p, const Point2* pts, int n, double delta, int band, int part,
                          double row_off, TexAttr* out)
{
	if (part < 0 || part >= p.parts)
		return TexResult::NonFinite;
	const double base = static_cast<double>(kLandPartRows) * part;
	double u[16], v[16];
	if (n < 1 || n > 16)
		return TexResult::NonFinite;
	double umin = 0.0;
	for (int i = 0; i < n; ++i)
	{
		landscape_uv(p, pts[i].x + delta, pts[i].y + delta, band, u[i], v[i], row_off);
		if (!std::isfinite(u[i]) || !std::isfinite(v[i]))
			return TexResult::NonFinite;
		umin = i ? std::fmin(umin, u[i]) : u[i];
	}
	const double period = static_cast<double>(1u << p.bits);
	const double shift = std::floor(umin / period) * period;
	for (int i = 0; i < n; ++i)
	{
		out[i].w = 1.0f;
		out[i].s = static_cast<float>((u[i] - shift + kLandEps) / p.S);
		out[i].t = static_cast<float>((v[i] - base) / p.T[part]);
	}
	return TexResult::Ok;
}

} // namespace a1_c3d
