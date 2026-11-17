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
#include "geometry.h"

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
// Landscape: kWLandStereo.
// M9 fix, the sky bleed (design 7, "M9 fix: the sky bleed"): the console's PICA draws nothing of a textured polygon whose
// vertices all carry w = kWPerspMax (2^24) once the shader scales clip by w (sky magenta in every stereo frame, 15 478
// pixels uncovered at L0's start; 0 at w = 32 767, measured 2026-11-15). Landscapes carry kWMax instead, the farthest a wall
// goes: the shift is P * (1 - w0 / 32767) = 0.969 P (5.81 px of 6), the same depth as the farthest walls.
constexpr float kWLandStereo = kWMax;
float stereo_w(StereoWKind kind, int32_t depth, bool foreground, float weapon_w, float geometric_w);

// --- the edge extension (M9 fix, design 7 "M9 fix: the edge strip") ---

// The engine clips every polygon at the view's left and right edges, so an eye's shifted pass has nothing to draw in a
// strip of up to P * max(1, near) pixels at one edge (far surfaces leave one edge, surfaces nearer than w0 the other).
// On stereo frames the front end continues each clipped polygon past the edge by this many pixels: the largest shift of
// the run (slider 1), whole pixels.
int stereo_edge_px(unsigned stereo_k, unsigned stereo_near);

// The strip that continues a clockwise (y-down) polygon past the edge i -> i+1 when that edge lies on the view's left
// (x == 0) or right (x == w) edge: out = {A, A', B', B}, clockwise, A' and B' at x = -b or w + b. Their y: ys[0] / ys[1]
// when given (the surface's own lines through A and B: stereo_wall_edge_y), else the lines through the polygon's two
// neighbouring edges (a neighbour that also lies on the view edge keeps its y). Then the strip never narrows (the upper
// corner goes no lower than its vertex, the lower one no higher: a polygon one or two pixels wide has rounded vertices
// that say nothing about its lines, and a strip that is too large is overdrawn by nearer surfaces while one that is too
// small leaves a gap), and y is rounded and clamped to [ylo, yhi]. The strip shares A and B with the polygon, so the
// polygon itself is drawn unchanged. False: not such an edge (or b <= 0).
bool stereo_edge_strip(const Point2* v, int n, int i, int w, int b, int ylo, int yhi, Point2 out[4],
                       const double* ys = nullptr);

// A wall's own line (constant height: its top, its bottom, any line along it) through the screen point (px, py), at
// screen column x: y = Y0 - (Y0 - py) * depth(px) / depth(x), Y0 = hh + dtp the horizon row, depth = wall_vertex's w
// before its fabs (signed). Exact whatever the polygon's width. NaN when the wall crosses the eye plane between px and x
// (the signs differ, or a depth is 0) or its depth grows more than kWallEdgeMaxRatio times (a vanishing point within the
// strip): wall_vertex's attributes there are not the wall's, so the caller does not draw that strip.
constexpr double kWallEdgeMaxRatio = 16.0;
double stereo_wall_edge_y(const TexView& v, const WallSurf& o, int px, int py, int x);

// --- the calibration (stereo_cal 1, test T1; design 7, "M9 specification", "Tests") ---

// Five 16x16 flat-colour quads at rows 100-115 of the view, drawn on every direct frame instead of the world with P = 8,
// near = 1, no fill. Quads 0-3 sit at columns 100, 150, 200, 250 with w = w0/2, w0, 2*w0, kWPerspMax; quad 4 at column 2
// with w = w0/2 (its right-eye image leaves the left edge: columns 0..9 only). Both eye textures start opaque black.
constexpr int kStereoCalQuads = 5;
constexpr int kStereoCalSize = 16;
constexpr int kStereoCalRow = 100;
constexpr float kStereoCalP = 8.0f;
constexpr float kStereoCalNear = 1.0f;
// The quad's left column, depth (of w0: 0.5, 1, 2; kWPerspMax for 3) and colour 0xRRGGBB.
int stereo_cal_col(int q);
float stereo_cal_w(int q, float w0);
uint32_t stereo_cal_rgb(int q);
// The first and last covered pixel column of quad q in eye e (-1 left, +1 right) by the model (stereo_shift) with the pixel
// centres rule (a pixel is covered when its centre is inside); first is clipped to 0 (the target's left edge). A model value,
// not what the shader does: that is what T1 compares it with.
void stereo_cal_expect(int q, int e, float w0, int& first, int& last);
// The pixel at (x, y) as 0xRRGGBB.
typedef uint32_t (*StereoCalPixel)(const void* ctx, int x, int y);
// Checks one eye's picture against the model: for every quad and every row 100-115, the pixels of the quad's colour are
// exactly the columns [first, last] of x in [0, width) (so a wrapped copy at the right edge, or a stray column, fails).
// Returns how many of the 5 quads match.
int stereo_cal_check(StereoCalPixel px, const void* ctx, int e, float w0, int width);

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
