/*
	Aleph One for the Nintendo 3DS, citro3d back end: the shade LUT and texcoord1 (docs/3ds/CITRO3D_DESIGN.md,
	4.1-4.2 and section 7, M3 specification "Shade LUT", "texcoord1"). Plain C++, no 3DS or engine headers,
	host-tested against a transliteration of calculate_shading_table (scripts/3ds/tests/c3d-host/test_core.cpp):

	  sw_shade_index    SW's table index for (M, depth, ambient), exactly as scottish_textures.cpp:149-165
	                    computes it at 32 bits: the short index (it wraps), CEILING 255, no lower clamp
	  shade_lut_value   the LUT's level at column i (s bin) and row j (a bin), from bin representatives
	  shade_lut_build   the 256 x 256 L8 LUT, tiled as every texture (core/tiling, row j -> memory row 255 - j)
	  shade_polygon     a polygon's texcoord1 parameters: depth-shaded (u per vertex from w), constant
	                    (shadeless, negative ambient), or excluded (SW's index wraps: undefined output)

	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; either version 3 of the License, or
	(at your option) any later version.
*/
#ifndef A1_C3D_CORE_SHADE_LUT_H
#define A1_C3D_CORE_SHADE_LUT_H

#include <cstddef>
#include <cstdint>

namespace a1_c3d {

constexpr unsigned kLutSide = 256;
constexpr size_t kLutBytes = kLutSide * kLutSide;   // L8: one byte per texel

// SW's shading-table index (calculate_shading_table, 32-bit: shading_table_fractional_bits 8): M =
// view->maximum_depth_intensity, depth the short depth SW passes (already MIN(..., SHRT_MAX)), a =
// polygon->ambient_shade. a < 0: (short)((-a) >> 8) with the 32-bit negation wrap; else s = PIN(M - (depth << 3),
// 0, FIXED_ONE) and (short)((a > s ? a + (s >> 1) : s + (a >> 1)) >> 8). Then CEILING(index, 255) = MIN. The
// result can be negative (SW then reads a table before shading_tables).
int sw_shade_index(int32_t M, int16_t depth, int32_t a);

// The bin representatives: s_0 = 0, s_255 = FIXED_ONE, else 256 i + 128; a_j = 256 j + 128.
int32_t shade_lut_s(unsigned i);
int32_t shade_lut_a(unsigned j);
// SW's level at the representatives of column i and row j (0..255).
uint8_t shade_lut_value(unsigned i, unsigned j);

// Tiles a W x W L8 image whose source row j starts at src + j * W into dst (W * W bytes), source row j into
// memory row W - 1 - j (the orientation rule of core/tiling.h).
void tile_l8(unsigned W, const uint8_t* src, uint8_t* dst);
// The whole LUT, tiled, into dst (kLutBytes).
void shade_lut_build(uint8_t* dst);

// One polygon's texcoord1 (design 7, M3 "texcoord1"). Depth: u = m_u - w / 8192 per vertex (w = the vertex's
// perspective weight), v per polygon. Constant: (u, v) for every vertex. Excluded: SW's index is negative or can
// wrap the short for some pixel (a >= 2^23 - 32768): the filter drops the polygon on both sides. FlashOver:
// M > 262136, where SW's SHRT_MAX depth clamp would leave s > 0 (never in Marathon 2): the front end discards.
enum class ShadeKind : uint8_t { Depth, Constant, Excluded, FlashOver };
struct ShadePoly
{
	ShadeKind kind = ShadeKind::Constant;
	float m_u = 0.0f;   // Depth: M / 65536
	float u = 0.0f;     // Constant
	float v = 0.0f;     // Depth and Constant: the centre of the polygon's LUT row
	uint8_t row = 0;
};
constexpr int32_t kAmbientWrapMin = (1 << 23) - 32768;
constexpr int32_t kFlashMax = 32767 * 8;   // 262136
bool shade_ambient_wraps(int32_t a);       // the Excluded test (a < 0 with a negative index, or a too large)
ShadePoly shade_polygon(int32_t M, int32_t a, bool shadeless);
inline float shade_u(const ShadePoly& p, float w) { return p.kind == ShadeKind::Depth ? p.m_u - w / 8192.0f : p.u; }

} // namespace a1_c3d

#endif
