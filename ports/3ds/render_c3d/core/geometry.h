/*
	Aleph One for the Nintendo 3DS, citro3d back end: the pure geometry of the world draws
	(docs/3ds/CITRO3D_DESIGN.md, 2.1-2.4 and section 7, M1 specification). Plain C++, no 3DS or
	engine headers, host-tested against transliterations of the software mapper
	(scripts/3ds/tests/c3d-host/test_core.cpp):

	  Vertex           2.1's 32-byte vertex
	  wall_w*, clamp_w the wall row of 2.2, evaluated at the vertex's integer sx (SW's world_x of
	                   scottish_textures.cpp:906-954, not its fixed-point rounding), and 2.3's clamp
	  accept_polygon   the polygon acceptance test mirrored from SW (scottish_textures.cpp:440-505,
	                   build_y_table :1231-1290, low_level_textures.h:322-326), so that C3D never
	                   draws what SW skips
	  fan_indices      the u16 triangle fan of a convex n-gon
	  DrawKey          the batching key (M0's null-mode key) and c3d_supports() (M1's draw set)
	  shape_colour     the flat colour of a ShapeDesc (M1 draws no textures)
	  cmdbuf_words,    the command-buffer measurements and the reserve test of 2.4 and the M1
	  reserve_ok       "Reserves and the command buffer, measured" bullet

	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; either version 3 of the License, or
	(at your option) any later version.
*/
#ifndef A1_C3D_CORE_GEOMETRY_H
#define A1_C3D_CORE_GEOMETRY_H

#include <cstdint>

#include "classify.h"

namespace a1_c3d {

// 2.1: v0 (sx, sy, w), texcoord0, texcoord1, colour (ubyte4, R first in memory).
struct Vertex
{
	float x, y, w;
	float u0, v0, u1, v1;
	uint8_t r, g, b, a;
};
static_assert(sizeof(Vertex) == 32, "design 2.1: the vertex is 32 bytes");

// 2.3: w clamps to [16, 32767] (SHRT_MAX as SW, scottish_textures.cpp:991, 1091).
constexpr float kWMin = 16.0f;
constexpr float kWMax = 32767.0f;
inline float clamp_w(float w) { return w < kWMin ? kWMin : (w > kWMax ? kWMax : w); }

// 2.2's wall row. hw = half the screen width, wx = world_to_screen_x, (ox, oy) = polygon->origin.x/y,
// (vi, vj) = polygon->vector.i/j, sx = the vertex's integer screen x. Returns |o.x + t*v.i| with
// t = (wx*o.y - s*o.x)/(s*v.i - wx*v.j), s = sx - hw (SW's tx, :930-954, in float), UNCLAMPED; a zero
// denominator becomes 1 as in SW (:943, 950).
float wall_w(int hw, int wx, int ox, int oy, int vi, int vj, int sx);
// The same, clamped (clamp_w).
inline float wall_w_clamped(int hw, int wx, int ox, int oy, int vi, int vj, int sx)
{
	return clamp_w(wall_w(hw, wx, ox, oy, vi, vj, sx));
}

struct Point2
{
	int x, y;
};

enum class Accept
{
	Draw,
	Degenerate,   // fewer than 3 vertices, or min x == max x (:459)
	Bounds,       // a vertex outside [0, w] x [0, h] (:449-456)
	Walk,         // build_y_table refuses (negative dx on either walk, :479-505, :1288)
	Area          // non-positive signed area in y-down space: SW writes no row (y1 <= y0 in every column)
};

// SW's checks in SW's order: the bounds test comes before the min == max test (:449 before :459).
Accept accept_polygon(const Point2* v, int n, int w, int h);
inline bool accepted(Accept a) { return a == Accept::Draw; }

// The tie pixels of a polygon (integer vertices): pixels (k, m) whose centre (k+1/2, m+1/2) lies exactly on
// an edge, within the edge's x-span (sorted endpoints x0 < x1, columns x0 <= k < x1). Vertical and horizontal
// edges and zero-length edges have none (centres are half-integers). Clipped to [0,w) x [0,h). Each is
// reported once through fn(ctx, k, m); returns the count. Exact integer test:
// dy*(2k+1-2*x0) == dx*(2m+1-2*y0). These are the pixels where SW (top ties in, bottom ties out) and
// another rasteriser may disagree; the design's PICA tie rule is UNVERIFIED.
int tie_pixels(const Point2* v, int n, int w, int h, void (*fn)(void* ctx, int k, int m), void* ctx);

// The triangle fan (base, base+i, base+i+1), i = 1..n-2, as u16 indices relative to the vertex index
// `base`. Writes 3(n-2) indices to `out` (0 and nothing written for n < 3) and returns the count.
int fan_indices(uint16_t* out, int n, unsigned base);

// M0's null-mode key (front/filter.h NullRasterizer::Key): consecutive polygons with an equal key merge
// into one draw.
struct DrawKey
{
	int cls;
	const void* texture;
	const void* shading_tables;
	int16_t transfer_mode, transfer_data;
	uint16_t shadeless;
	bool operator==(const DrawKey& o) const
	{
		return cls == o.cls && texture == o.texture && shading_tables == o.shading_tables &&
		       transfer_mode == o.transfer_mode && transfer_data == o.transfer_data && shadeless == o.shadeless;
	}
	bool operator!=(const DrawKey& o) const { return !(*this == o); }
};

// M1's draw set: texture_vertical_polygon with _textured_transfer whose texture is opaque (not
// _TRANSPARENT_BIT). `transparent_texture` is the already extracted flag.
inline bool c3d_supports(Entry e, Transfer t, bool transparent_texture)
{
	return e == Entry::Vertical && t == Transfer::Textured && !transparent_texture;
}

// A hash of a ShapeDesc (u16) to an RGB colour, alpha 0xFF by the caller. Never G == 0xC3 && B == 0xA5,
// so it never equals the RGB of the readback sentinel (calibrate.h cal_sentinel_*) for any frame index.
struct Rgb
{
	uint8_t r, g, b;
};
Rgb shape_colour(uint16_t shape);

// 2.4 / M1: the command-buffer reserves, 2x the worst case measured in the M1 acceptance runs (Azahar;
// command words are deterministic): 254 words for one C3D_DrawElements with citro3d's state flush (first
// draw of a view frame: world program upload, AttrInfo, BufInfo, TEV, uniforms); 10 words for the end
// (the readback's flushing split + transfer, and C3D_FrameEnd's own split on a discard path).
// docs/3ds/CITRO3D_DESIGN.md, M1 status.
constexpr uint32_t kDrawReserveWords = 508;
constexpr uint32_t kEndReserveWords = 20;

struct CmdBufWords
{
	uint32_t frame_words = 0;   // P1 - P0: the frame's words before C3D_FrameEnd
	uint32_t end_words = 0;     // C3D_FrameEnd's own words; 0 when the buffer was empty at P1
};

// p0 = gpuCmdBuf + gpuCmdBufOffset right after frame_begin(), p1 the same right before C3D_FrameEnd.
// If the offset was not 0 at p1, C3D_FrameEnd splits and C3D_GetCmdBufUsage() (read after it) is the
// end of that split over the buffer's `size_words`: its words are lroundf(usage * size) - (p1 - p0).
// With an empty buffer at p1 the usage is stale and 0 is recorded.
CmdBufWords cmdbuf_words(const uint32_t* p0, const uint32_t* p1, bool offset_nonzero, float usage,
                         uint32_t size_words);

// 2.4's reserve test: gpuCmdBufSize - gpuCmdBufOffset >= draw + end + extra (extra = reserve_extra).
bool reserve_ok(uint32_t size, uint32_t offset, uint32_t draw_reserve, uint32_t end_reserve, uint32_t extra);

} // namespace a1_c3d

#endif
