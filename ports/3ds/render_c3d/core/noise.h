/*
	Aleph One for the Nintendo 3DS, citro3d back end: the static noise of M5 (docs/3ds/CITRO3D_DESIGN.md, section 7,
	M5 specification, "SW's static, exactly" and "Static on the GPU"). Plain C++, no 3DS or engine headers,
	host-tested (scripts/3ds/tests/c3d-host/test_core.cpp).

	SW (low_level_textures.h:590-645), per written texel position: if (seed >= drop) the pixel becomes
	seed ^ (seed << 8) (32 bit: R = high byte, G = high ^ low, B = low, top byte 0); then the Galois step
	seed = (seed >> 1) ^ (seed & 1 ? 0xb400 : 0). The GPU samples a 256 x 256 texture of every LFSR state, laid out
	down columns as SW walks them (RGB as SW's pixel, A = min(255, high byte + 1)), under the alpha test
	GEQUAL min(255, (drop >> 8) + 1): exact at byte level but for drop >= 0xFF00.

	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; either version 3 of the License, or
	(at your option) any later version.
*/
#ifndef A1_C3D_CORE_NOISE_H
#define A1_C3D_CORE_NOISE_H

#include <cstddef>
#include <cstdint>

namespace a1_c3d {

constexpr unsigned kNoiseSide = 256;
constexpr size_t kNoiseBytes = static_cast<size_t>(kNoiseSide) * kNoiseSide * 4;   // RGBA8
constexpr uint16_t kNoiseSeed = 1;

// SW's step and pixel (randomize_vertical_polygon_lines, randomize_vertical_polygon_lines_write<pixel32>).
inline uint16_t sw_lfsr_step(uint16_t s)
{
	return (s & 1u) ? static_cast<uint16_t>((s >> 1) ^ 0xb400u) : static_cast<uint16_t>(s >> 1);
}
inline uint32_t sw_static_pixel(uint16_t seed)
{
	return static_cast<uint32_t>(seed) ^ (static_cast<uint32_t>(seed) << 8);
}

// The RGBA8 word (R << 24 | G << 16 | B << 8 | A) of the noise texel for LFSR state `seed`.
inline uint32_t noise_word(uint16_t seed)
{
	const uint32_t px = sw_static_pixel(seed);   // 0x00RRGGBB
	const unsigned hi = seed >> 8;
	const unsigned a = hi < 255 ? hi + 1 : 255u;
	return (px << 8) | a;
}

// The alpha-test reference of a static draw: a texel is written when its alpha >= ref (SW: seed >= drop).
inline unsigned noise_ref(uint16_t drop)
{
	const unsigned r = (drop >> 8) + 1u;
	return r > 255 ? 255u : r;
}

// The noise texture, tiled (8x8 Morton, the memory-row order of tex_convert) into `out` (kNoiseBytes, 4-byte
// aligned): texel (x, y) (column x, source row y) holds the state 256 * x + y after kNoiseSeed (down columns).
void noise_build(void* out);

// The fraction of the texture's texels an alpha test GEQUAL `ref` keeps (the host test's density), and SW's
// expected density for `drop` over the LFSR's period (the 65 535 non-zero states).
double noise_density(unsigned ref);
inline double sw_static_density(uint16_t drop)
{
	const unsigned d = drop ? drop : 1u;
	return static_cast<double>(65536u - d) / 65535.0;
}

} // namespace a1_c3d

#endif
