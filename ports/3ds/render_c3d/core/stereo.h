/*
	Aleph One for the Nintendo 3DS, citro3d back end: stereo 3D, the pure part
	(docs/3ds/CITRO3D_DESIGN.md, section 7, "M9 specification"). Plain C++, no 3DS or engine headers: host-tested.

	  the ramp          the parallax strength p (slider units) eases from 0 to the slider's value at a constant rate
	                    of 1/N per frame; going flat is immediate (stereo_step)
	  the model         a vertex of eye depth w moves horizontally by e * P * max(1 - w0/w, -near) pixels, e = -1 for
	                    the left eye, +1 for the right (stereo_shift; the vertex shader does the same sum)
	  the w a vertex    rectangles, landscapes and the weapon carry a constant w on stereo frames (stereo_w, weapon_w)
	  the VRAM rule     the right-eye objects cost 887 KiB: the texture pool's default cap drops with them
	                    (stereo_allocated, stereo_vram_cap)

	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; either version 3 of the License, or
	(at your option) any later version.
*/
#ifndef A1_C3D_CORE_STEREO_H
#define A1_C3D_CORE_STEREO_H

#include <cstdint>

#include "config.h"

namespace a1_c3d {

// --- the ramp ---

// A slider below this reads as "at rest": the 3D barrier is off, so the strength drops to 0 at once (UNVERIFIED on
// hardware: that the slider reads exactly 0 at rest; hence the floor).
constexpr float kStereoSliderFloor = 1.0f / 64.0f;

struct StereoRamp
{
	float p = 0.0f;   // slider units, 0..1
};

// One frame. !capable: p = 0. Else T = slider < 1/64 ? 0 : min(slider, 1); T == 0: p = 0 at once; else p moves toward
// T by at most 1/ramp_frames. Returns p. From 0, T is reached in ceil(T * ramp_frames) frames.
float stereo_step(StereoRamp& r, bool capable, float slider, unsigned ramp_frames);
void stereo_reset(StereoRamp& r);

// P in pixels for the strength p: K = stereo_k / 16 pixels per eye at slider 1.
inline float stereo_pixels(float p, unsigned stereo_k)
{
	return p * static_cast<float>(stereo_k) * (1.0f / 16.0f);
}

// --- the model ---

// The horizontal shift in pixels of a vertex of eye depth w (float reference of world.v.pica): e = -1 left, +1 right,
// P the strength in pixels, w0 the convergence depth, near the pop-out limit (>= 0).
float stereo_shift(float w, int e, float P, float w0, float near);

// The weapon's depth: its parallax is the share t (-1..1) of P, w0 / (1 - t) at least w0 / 2, kWPerspMax for t >= 0.99.
float stereo_weapon_w(float w0, float t);

enum class StereoWKind : uint8_t { Wall, Static, Rectangle, Landscape };
// The w a vertex carries on a stereo frame. Wall (walls, floors, liquids) and Static (an unmasked static polygon,
// attrs() or the mean): `geometric_w`, already clamped. Rectangle: weapon_w when `foreground`, else clamp_wp(depth).
// Landscape: kWPerspMax.
float stereo_w(StereoWKind kind, int32_t depth, bool foreground, float weapon_w, float geometric_w);

// --- VRAM ---

// The right-eye objects (right screen target, second view texture, the smaller pool cap) exist for the run: without a
// debug-c3d file always; with one only when it says stereo 1.
inline bool stereo_allocated(bool has_file, bool file_stereo)
{
	return !has_file || file_stereo;
}
constexpr unsigned kStereoVramCapKib = 2944;   // 3 584 - 640
constexpr unsigned kMonoVramCapKib = 3584;
// The texture pool's VRAM cap: an explicit vram_cap key wins, else 2 944 KiB with the right-eye objects, else 3 584.
inline unsigned stereo_vram_cap(const Config& cfg, bool allocated)
{
	return cfg.vram_cap_set ? cfg.vram_cap : allocated ? kStereoVramCapKib : kMonoVramCapKib;
}

} // namespace a1_c3d

#endif
