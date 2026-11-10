/*
	Aleph One for the Nintendo 3DS, citro3d back end: stereo 3D, the pure part (see stereo.h).

	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; either version 3 of the License, or
	(at your option) any later version.
*/
#include "stereo.h"

#include "geometry.h"

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

} // namespace a1_c3d
