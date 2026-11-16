/*
	Aleph One for the Nintendo 3DS, citro3d back end: stereo 3D, the pure part (see stereo.h).

	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; either version 3 of the License, or
	(at your option) any later version.
*/
#include "stereo.h"

#include "geometry.h"

#include <cmath>
#include <limits>

namespace a1_c3d {

float stereo_step(StereoRamp& r, bool capable, float slider, unsigned ramp_frames)
{
	if (!capable)
	{
		r.p = 0.0f;
		return 0.0f;
	}
	const float t = slider < kStereoSliderFloor ? 0.0f : (slider > 1.0f ? 1.0f : slider);
	if (t == 0.0f)
	{
		r.p = 0.0f;
		return 0.0f;
	}
	const float step = 1.0f / static_cast<float>(ramp_frames < 1 ? 1 : ramp_frames);
	const float d = t - r.p;
	// A small tolerance: p is a sum of float steps, and a ramp of N steps must end on T in its N-th frame.
	constexpr float kEps = 1e-5f;
	if (d > step + kEps)
		r.p += step;
	else if (d < -step - kEps)
		r.p -= step;
	else
		r.p = t;
	return r.p;
}

void stereo_reset(StereoRamp& r)
{
	r.p = 0.0f;
}

float stereo_shift(float w, int e, float P, float w0, float near)
{
	float k = 1.0f - w0 / w;
	if (k < -near)
		k = -near;
	return static_cast<float>(e) * P * k;
}

float stereo_weapon_w(float w0, float t)
{
	if (t >= 0.99f)
		return kWPerspMax;
	float w = w0 / (1.0f - t);
	if (w < w0 * 0.5f)
		w = w0 * 0.5f;
	return w > kWPerspMax ? kWPerspMax : w;
}

float stereo_w(StereoWKind kind, int32_t depth, bool foreground, float weapon_w, float geometric_w)
{
	switch (kind)
	{
		case StereoWKind::Wall:
		case StereoWKind::Static:
			return geometric_w;
		case StereoWKind::Rectangle:
			return foreground ? weapon_w : clamp_wp(static_cast<float>(depth));
		case StereoWKind::Landscape:
			return kWPerspMax;
	}
	return geometric_w;
}

namespace {
const int kCalCol[kStereoCalQuads] = {100, 150, 200, 250, 2};
const uint32_t kCalRgb[kStereoCalQuads] = {0xFF0000u, 0x00FF00u, 0x0000FFu, 0xFFFF00u, 0xFF00FFu};
} // namespace

int stereo_cal_col(int q)
{
	return kCalCol[q];
}

float stereo_cal_w(int q, float w0)
{
	switch (q)
	{
		case 0:
		case 4:
			return w0 * 0.5f;
		case 1:
			return w0;
		case 2:
			return w0 * 2.0f;
		default:
			return kWPerspMax;
	}
}

uint32_t stereo_cal_rgb(int q)
{
	return kCalRgb[q];
}

void stereo_cal_expect(int q, int e, float w0, int& first, int& last)
{
	const double sh = static_cast<double>(stereo_shift(stereo_cal_w(q, w0), e, kStereoCalP, w0, kStereoCalNear));
	const double x0 = kCalCol[q] + sh, x1 = kCalCol[q] + kStereoCalSize + sh;
	// the first pixel whose centre k + 0.5 lies right of x0, the last whose centre lies left of x1
	first = static_cast<int>(std::ceil(x0 - 0.5));
	last = static_cast<int>(std::ceil(x1 - 0.5)) - 1;
	if (first < 0)
		first = 0;
}

int stereo_cal_check(StereoCalPixel px, const void* ctx, int e, float w0, int width)
{
	int ok = 0;
	for (int q = 0; q < kStereoCalQuads; ++q)
	{
		int first, last;
		stereo_cal_expect(q, e, w0, first, last);
		bool good = true;
		for (int y = kStereoCalRow; good && y < kStereoCalRow + kStereoCalSize; ++y)
			for (int x = 0; good && x < width; ++x)
				good = (px(ctx, x, y) == kCalRgb[q]) == (x >= first && x <= last);
		ok += good;
	}
	return ok;
}

int stereo_edge_px(unsigned stereo_k, unsigned stereo_near)
{
	const float n = static_cast<float>(stereo_near) / 100.0f;
	return static_cast<int>(std::ceil(stereo_pixels(1.0f, stereo_k) * (n > 1.0f ? n : 1.0f)));
}

bool stereo_edge_strip(const Point2* v, int n, int i, int w, int b, int ylo, int yhi, Point2 out[4], const double* ys)
{
	if (n < 3 || b <= 0)
		return false;
	const int j = i + 1 == n ? 0 : i + 1;
	const Point2 A = v[i], B = v[j];
	if (A.x != B.x || A.y == B.y || (A.x != 0 && A.x != w))
		return false;
	const int dir = A.x == 0 ? -1 : 1;
	// A clockwise polygon inside the view runs down its right edge and up its left one.
	if ((dir > 0) != (B.y > A.y))
		return false;
	// `upper`: this corner is the strip's upper one (smaller y)
	auto ext = [&](const Point2& p, const Point2& q, const double* given, bool upper) {
		double y = p.y;
		const int dx = (p.x - q.x) * dir;   // > 0: q is inside the view
		if (given)
			y = *given;
		else if (dx > 0)
			y += static_cast<double>(p.y - q.y) * b / dx;
		if (!(upper ? y <= p.y : y >= p.y))   // also a NaN
			y = p.y;
		y = y < ylo ? ylo : (y > yhi ? yhi : y);
		return Point2{p.x + dir * b, static_cast<int>(std::lround(y))};
	};
	const bool a_upper = A.y < B.y;
	out[0] = A;
	out[1] = ext(A, v[i ? i - 1 : n - 1], ys ? ys : nullptr, a_upper);
	out[2] = ext(B, v[j + 1 == n ? 0 : j + 1], ys ? ys + 1 : nullptr, !a_upper);
	out[3] = B;
	return true;
}

// wall_vertex's depth before its fabs (the same t, the same den guard): the sign says which side of the eye plane.
static double wall_depth_signed(const TexView& v, const WallSurf& o, double ex)
{
	const double s = ex - v.hw;
	double den = s * o.vi - static_cast<double>(v.wx) * o.vj;
	if (den == 0)
		den = 1;
	const double t = (static_cast<double>(v.wx) * o.oy - s * o.ox) / den;
	return o.ox + t * o.vi;
}

double stereo_wall_edge_y(const TexView& v, const WallSurf& o, int px, int py, int x)
{
	const double d0 = wall_depth_signed(v, o, px), d1 = wall_depth_signed(v, o, x);
	// Through the eye plane, or a vanishing point within the strip: the wall's attributes past it are not the wall's.
	if (!(d0 * d1 > 0) || std::fabs(d1) > kWallEdgeMaxRatio * std::fabs(d0))
		return std::numeric_limits<double>::quiet_NaN();
	const double y0 = static_cast<double>(v.hh) + v.dtp;
	return y0 - (y0 - py) * d0 / d1;
}

} // namespace a1_c3d
