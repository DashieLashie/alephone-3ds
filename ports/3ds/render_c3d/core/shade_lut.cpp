/*
	Aleph One for the Nintendo 3DS, citro3d back end: the shade LUT and texcoord1 (see shade_lut.h).

	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; either version 3 of the License, or
	(at your option) any later version.
*/
#include "shade_lut.h"

#include "tiling.h"

namespace a1_c3d {

namespace {

constexpr int32_t kFixedOne = 65536;

// SW's index from a non-negative ambient and a pinned shade (:159-162), before CEILING. The sum in 32-bit
// two's complement as the _fixed arithmetic, the shift arithmetic, the store into a short.
int16_t index_of(int32_t a, int32_t s)
{
	const uint32_t sum = a > s ? static_cast<uint32_t>(a) + static_cast<uint32_t>(s >> 1)
	                           : static_cast<uint32_t>(s) + static_cast<uint32_t>(a >> 1);
	return static_cast<int16_t>(static_cast<int32_t>(sum) >> 8);
}

// (short)((-a) >> 8) for a < 0 (:153-154), the negation wrapping as the 32-bit _fixed does.
int16_t negative_index(int32_t a)
{
	return static_cast<int16_t>(static_cast<int32_t>(0u - static_cast<uint32_t>(a)) >> 8);
}

int ceiling255(int i) { return i < 255 ? i : 255; }

} // namespace

int sw_shade_index(int32_t M, int16_t depth, int32_t a)
{
	if (a < 0)
		return ceiling255(negative_index(a));
	int32_t s = M - (static_cast<int32_t>(depth) << 3);
	s = s < 0 ? 0 : (s > kFixedOne ? kFixedOne : s);
	return ceiling255(index_of(a, s));
}

int32_t shade_lut_s(unsigned i)
{
	if (i == 0)
		return 0;
	if (i >= kLutSide - 1)
		return kFixedOne;
	return static_cast<int32_t>(256 * i + 128);
}

int32_t shade_lut_a(unsigned j)
{
	return static_cast<int32_t>(256 * j + 128);
}

uint8_t shade_lut_value(unsigned i, unsigned j)
{
	return static_cast<uint8_t>(ceiling255(index_of(shade_lut_a(j), shade_lut_s(i))));
}

void tile_l8(unsigned W, const uint8_t* src, uint8_t* dst)
{
	for (unsigned j = 0; j < W; ++j)
	{
		const unsigned m = tile_memory_row(W, j);
		const uint8_t* row = src + static_cast<size_t>(j) * W;
		for (unsigned s = 0; s < W; ++s)
			dst[tile_index(W, m, s)] = row[s];
	}
}

void shade_lut_build(uint8_t* dst)
{
	for (unsigned j = 0; j < kLutSide; ++j)
	{
		const unsigned m = tile_memory_row(kLutSide, j);
		for (unsigned i = 0; i < kLutSide; ++i)
			dst[tile_index(kLutSide, m, i)] = shade_lut_value(i, j);
	}
}

bool shade_ambient_wraps(int32_t a)
{
	return a < 0 ? negative_index(a) < 0 : a >= kAmbientWrapMin;
}

ShadePoly shade_polygon(int32_t M, int32_t a, bool shadeless)
{
	ShadePoly p;
	auto centre = [](unsigned k) { return (static_cast<float>(k) + 0.5f) / static_cast<float>(kLutSide); };
	if (shadeless)
	{
		// LUT (255, 255) = 255: the texture's own colour (SW: table 0 of the tint table, :984-986).
		p.kind = ShadeKind::Constant;
		p.row = 255;
		p.u = p.v = centre(255);
		return p;
	}
	if (shade_ambient_wraps(a))
	{
		p.kind = ShadeKind::Excluded;
		return p;
	}
	if (a < 0)
	{
		// Column 0 (s = 0) of row k: exactly k (the representative a_k >> 8 = k).
		const int k = ceiling255(negative_index(a));
		p.kind = ShadeKind::Constant;
		p.row = static_cast<uint8_t>(k);
		p.u = centre(0);
		p.v = centre(static_cast<unsigned>(k));
		return p;
	}
	if (M > kFlashMax)
	{
		p.kind = ShadeKind::FlashOver;
		return p;
	}
	const int32_t r = a >> 8;
	p.kind = ShadeKind::Depth;
	p.row = static_cast<uint8_t>(r < 255 ? r : 255);
	p.m_u = static_cast<float>(M) / static_cast<float>(kFixedOne);
	p.v = centre(p.row);
	return p;
}

} // namespace a1_c3d
