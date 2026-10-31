/*
	Aleph One for the Nintendo 3DS, citro3d back end: world geometry (see geometry.h).

	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; either version 3 of the License, or
	(at your option) any later version.
*/
#include "geometry.h"

#include "tex_convert.h"

#include <algorithm>
#include <utility>

#include <cmath>

namespace a1_c3d {

float wall_w(int hw, int wx, int ox, int oy, int vi, int vj, int sx)
{
	// Exact in 64 bits; the float conversions below round once each, like the vertex shader's input.
	const int64_t s = static_cast<int64_t>(sx) - hw;
	const int64_t num = static_cast<int64_t>(wx) * oy - s * ox;
	int64_t den = s * vi - static_cast<int64_t>(wx) * vj;
	if (!den)
		den = 1;   // SW's guard (:943, 950)
	const float t = static_cast<float>(num) / static_cast<float>(den);
	const float x = static_cast<float>(ox) + t * static_cast<float>(vi);
	return std::fabs(x);   // "mostly unclear what to do with negative x": SW negates (:955)
}

Accept accept_polygon(const Point2* v, int n, int w, int h, Orient orient)
{
	if (n < 3)
		return Accept::Degenerate;
	// The "major" axis is the one the walk advances along: x for a vertical polygon (columns), y for a
	// horizontal one (rows, scottish_textures.cpp:213-300). Vertical: the left table walks to higher
	// vertex indices and the right table to lower ones; horizontal: the other way round (:262, :276).
	const bool horiz = orient == Orient::Horizontal;
	auto major = [&](int i) { return horiz ? v[i].y : v[i].x; };
	const int left_step = horiz ? n - 1 : 1;    // index + step, modulo n
	const int right_step = horiz ? 1 : n - 1;

	int highest = 0, lowest = 0;
	for (int i = 1; i < n; ++i)
	{
		if (major(i) < major(highest))
			highest = i;
		if (major(i) > major(lowest))
			lowest = i;
	}
	for (int i = 0; i < n; ++i)
		if (!(v[i].x >= 0 && v[i].x <= w && v[i].y >= 0 && v[i].y <= h))
			return Accept::Bounds;
	if (highest == lowest)
		return Accept::Degenerate;

	// The two walks, reduced to their only refusal: the table builder returns NULL for a negative step
	// along the major axis (build_y_table :1231-1290, build_x_table :1173-1223). Same loop structure
	// (zero-length edges are skipped on each side).
	int total = major(lowest) - major(highest);
	int left_count = 0, right_count = 0;
	int left = highest, right = highest;
	while (total > 0)
	{
		if (left_count <= 0)
		{
			do
			{
				const int next = (left + left_step) % n;
				left_count = major(next) - major(left);
				if (left_count < 0)
					return Accept::Walk;
				left = next;
			} while (!left_count);
		}
		if (right_count <= 0)
		{
			do
			{
				const int next = (right + right_step) % n;
				right_count = major(next) - major(right);
				if (right_count < 0)
					return Accept::Walk;
				right = next;
			} while (!right_count);
		}
		const int delta = left_count < right_count ? left_count : right_count;
		total -= delta;
		left_count -= delta;
		right_count -= delta;
	}

	// Signed area (shoelace) in y-down space: positive when the vertex order runs clockwise on screen. That
	// is the order both mappers need (vertical: the walk from the leftmost vertex by +1 is the upper edge;
	// horizontal: the walk from the topmost vertex by +1 is the right edge); otherwise no row/column writes.
	int64_t area2 = 0;
	for (int i = 0; i < n; ++i)
	{
		const Point2& a = v[i];
		const Point2& b = v[i + 1 == n ? 0 : i + 1];
		area2 += static_cast<int64_t>(a.x) * b.y - static_cast<int64_t>(b.x) * a.y;
	}
	if (area2 <= 0)
		return Accept::Area;
	return Accept::Draw;
}

int fan_indices(uint16_t* out, int n, unsigned base)
{
	if (n < 3)
		return 0;
	int k = 0;
	for (int i = 1; i + 1 < n; ++i)
	{
		out[k++] = static_cast<uint16_t>(base);
		out[k++] = static_cast<uint16_t>(base + i);
		out[k++] = static_cast<uint16_t>(base + i + 1);
	}
	return k;
}

bool c3d_supports(Entry e, Transfer t, int width, int height, bool rle, bool shadeless, bool transparent_texture,
                  bool textures)
{
	if (t != Transfer::Textured || (e != Entry::Vertical && e != Entry::Horizontal))
		return false;
	if (!tex_supported(width, height, rle) || shadeless)
		return false;
	if (e == Entry::Vertical && transparent_texture && !textures)
		return false;
	return true;
}

TexAttrD wall_vertex(const TexView& v, const WallSurf& o, double ex, double ey)
{
	const double s = ex - v.hw;
	double den = s * o.vi - static_cast<double>(v.wx) * o.vj;
	if (den == 0)
		den = 1;   // SW's guard (:943, 950)
	const double t = (static_cast<double>(v.wx) * o.oy - s * o.ox) / den;
	const double x = std::fabs(o.ox + t * o.vi);   // "mostly unclear what to do with negative x": SW negates (:955)
	const double sy0 = v.hh + v.dtp - ey;           // screen_y0 = half_screen_height - y0 + dtanpitch (:926)
	const double den_s = static_cast<double>(v.wy) * o.vk;
	TexAttrD r;
	r.w = x;
	r.s = (x * sy0 - static_cast<double>(v.wy) * o.oz) / (den_s == 0 ? 1.0 : den_s);
	r.t = t;
	return r;
}

bool floor_vertex(const TexView& v, const FloorSurf& o, double ex, double ey, TexAttrD& out)
{
	if (o.oz == 0)
		return false;
	const double sx = ex - v.hw;
	double sy = v.hh - ey + v.dtp;
	if (sy == 0)
		sy = 1;   // :1044
	if (sy * o.oz <= 0)
		sy = o.oz > 0 ? 0.5 : -0.5;   // the sign guard: a vertex on or past the horizon (round 1 finding 3)
	const double kS = 1024.0 * 1024.0 * static_cast<double>(1 << o.scale);   // 1024 * S, S = 1024 * 2^scale
	const double dz = static_cast<double>(o.oz);
	out.s = (dz * (static_cast<double>(v.wy) * v.cos_t - sx * v.sin_t) / sy + 1024.0 * o.ox) / kS;
	out.t = (dz * (sx * v.cos_t + static_cast<double>(v.wy) * v.sin_t) / sy + 1024.0 * o.oy) / kS;
	out.w = std::fabs(dz * v.wy / sy);
	return true;
}

namespace {

constexpr double kMaxRepeats = 1048576.0;

// Subtracts floor(min) of each texcoord and converts; false on a non-finite value or a huge spread.
bool finish_attrs(const TexAttrD* raw, int n, TexAttr* out)
{
	double min_s = raw[0].s, min_t = raw[0].t;
	for (int i = 0; i < n; ++i)
	{
		if (!std::isfinite(raw[i].w) || !std::isfinite(raw[i].s) || !std::isfinite(raw[i].t))
			return false;
		min_s = std::min(min_s, raw[i].s);
		min_t = std::min(min_t, raw[i].t);
	}
	const double fs = std::floor(min_s), ft = std::floor(min_t);
	for (int i = 0; i < n; ++i)
	{
		const double s = raw[i].s - fs, t = raw[i].t - ft;
		if (s > kMaxRepeats || t > kMaxRepeats)
			return false;
		out[i].w = clamp_wp(static_cast<float>(raw[i].w));
		out[i].s = static_cast<float>(s);
		out[i].t = static_cast<float>(t);
	}
	return true;
}

} // namespace

TexResult wall_attrs(const TexView& v, const WallSurf& o, const Point2* pts, int n, double delta, TexAttr* out)
{
	TexAttrD raw[16];
	if (n < 1 || n > 16)
		return TexResult::NonFinite;
	for (int i = 0; i < n; ++i)
		raw[i] = wall_vertex(v, o, pts[i].x + delta, pts[i].y + delta);
	return finish_attrs(raw, n, out) ? TexResult::Ok : TexResult::NonFinite;
}

TexResult floor_attrs(const TexView& v, const FloorSurf& o, const Point2* pts, int n, double delta, TexAttr* out)
{
	TexAttrD raw[16];
	if (o.oz == 0)
		return TexResult::Degenerate;
	if (n < 1 || n > 16)
		return TexResult::NonFinite;
	for (int i = 0; i < n; ++i)
		if (!floor_vertex(v, o, pts[i].x + delta, pts[i].y + delta, raw[i]))
			return TexResult::Degenerate;
	return finish_attrs(raw, n, out) ? TexResult::Ok : TexResult::NonFinite;
}

Rgb shape_colour(uint16_t shape)
{
	// A multiplicative (Knuth) hash of the descriptor; the three top bytes are the channels.
	const uint32_t h = (static_cast<uint32_t>(shape) + 1u) * 2654435761u;
	Rgb c{static_cast<uint8_t>(h >> 24), static_cast<uint8_t>(h >> 16), static_cast<uint8_t>(h >> 8)};
	if (c.g == 0xC3 && c.b == 0xA5)   // the readback sentinel's G and B (calibrate.h)
		c.b = 0xA4;
	return c;
}

CmdBufWords cmdbuf_words(const uint32_t* p0, const uint32_t* p1, bool offset_nonzero, float usage,
                         uint32_t size_words)
{
	CmdBufWords r;
	r.frame_words = p1 > p0 ? static_cast<uint32_t>(p1 - p0) : 0;
	if (offset_nonzero)
	{
		const long total = std::lroundf(usage * static_cast<float>(size_words));
		r.end_words = total > static_cast<long>(r.frame_words) ? static_cast<uint32_t>(total) - r.frame_words : 0;
	}
	return r;
}

bool reserve_ok(uint32_t size, uint32_t offset, uint32_t draw_reserve, uint32_t end_reserve, uint32_t extra)
{
	if (offset > size)
		return false;
	return static_cast<uint64_t>(size - offset) >=
	       static_cast<uint64_t>(draw_reserve) + end_reserve + extra;
}

int tie_pixels(const Point2* v, int n, int w, int h, void (*fn)(void* ctx, int k, int m), void* ctx)
{
	int count = 0;
	for (int i = 0; i < n; ++i)
	{
		Point2 p0 = v[i], p1 = v[(i + 1) % n];
		if (p0.x > p1.x)
			std::swap(p0, p1);
		const long dx = p1.x - p0.x, dy = p1.y - p0.y;
		if (dx == 0 || dy == 0)
			continue;
		for (int k = std::max(p0.x, 0); k < p1.x && k < w; ++k)
		{
			const long a = dy * (2L * k + 1 - 2L * p0.x);
			if (a % dx != 0)
				continue;
			const long q = a / dx;   // 2m + 1 - 2*y0, must be odd
			if ((q & 1) == 0)
				continue;
			const long m = (q - 1 + 2L * p0.y) / 2;
			if (m < 0 || m >= h)
				continue;
			++count;
			if (fn)
				fn(ctx, k, static_cast<int>(m));
		}
	}
	return count;
}

} // namespace a1_c3d
