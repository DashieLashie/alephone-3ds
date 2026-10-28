/*
	Aleph One for the Nintendo 3DS, citro3d back end: world geometry (see geometry.h).

	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; either version 3 of the License, or
	(at your option) any later version.
*/
#include "geometry.h"

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

Accept accept_polygon(const Point2* v, int n, int w, int h)
{
	if (n < 3)
		return Accept::Degenerate;
	int highest = 0, lowest = 0;
	for (int i = 1; i < n; ++i)
	{
		if (v[i].x < v[highest].x)
			highest = i;
		if (v[i].x > v[lowest].x)
			lowest = i;
	}
	for (int i = 0; i < n; ++i)
		if (!(v[i].x >= 0 && v[i].x <= w && v[i].y >= 0 && v[i].y <= h))
			return Accept::Bounds;
	if (highest == lowest)
		return Accept::Degenerate;

	// The two walks of texture_vertical_polygon, reduced to their only refusal: build_y_table returns
	// NULL for a negative dx. Same loop structure (zero-width edges are skipped on each side).
	int total = v[lowest].x - v[highest].x;
	int left_count = 0, right_count = 0;
	int left = highest, right = highest;
	while (total > 0)
	{
		if (left_count <= 0)
		{
			do
			{
				const int next = (left == n - 1) ? 0 : left + 1;
				left_count = v[next].x - v[left].x;
				if (left_count < 0)
					return Accept::Walk;
				left = next;
			} while (!left_count);
		}
		if (right_count <= 0)
		{
			do
			{
				const int next = right ? right - 1 : n - 1;
				right_count = v[next].x - v[right].x;
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

	// Signed area (shoelace) in y-down space: positive when the vertex order runs clockwise on screen,
	// i.e. the "clockwise" walk from the leftmost vertex is the upper edge (y0 table) and the other the
	// lower (y1 table).
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
