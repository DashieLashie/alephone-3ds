/*
	Aleph One for the Nintendo 3DS, citro3d back end: bitmap -> GPU texel conversion (see tex_convert.h).

	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; either version 3 of the License, or
	(at your option) any later version.
*/
#include "tex_convert.h"

#include "landscape.h"
#include "tiling.h"

namespace a1_c3d {

namespace {

constexpr uint32_t kRgb = 0x00FFFFFFu;

// pixel_format_32 entry 0xAARRGGBB, plus an alpha -> R<<24 | G<<16 | B<<8 | A.
uint32_t texel_word(uint32_t argb, unsigned a)
{
	return ((argb & 0x00FF0000u) << 8) | ((argb & 0x0000FF00u) << 8) | ((argb & 0x000000FFu) << 8) | (a & 0xFFu);
}

bool is_pot(unsigned v) { return v != 0 && (v & (v - 1)) == 0; }

bool kind_keyed(TexKind k) { return k == TexKind::NormalKeyed || k == TexKind::ShadelessKeyed || k == TexKind::Mask; }

unsigned pot_at_least(unsigned v)
{
	unsigned p = kRectTexMin;
	while (p < v)
		p <<= 1;
	return p;
}

// Writes palette[index] (RGBA8 little-endian, or the A8 byte) at texel t of the tiled image.
inline void put_texel(uint8_t* dst, size_t t, uint32_t w, bool a8)
{
	if (a8)
	{
		dst[t] = static_cast<uint8_t>(w);
		return;
	}
	uint8_t* p = dst + t * 4;   // little-endian u32: A, B, G, R
	p[0] = static_cast<uint8_t>(w);
	p[1] = static_cast<uint8_t>(w >> 8);
	p[2] = static_cast<uint8_t>(w >> 16);
	p[3] = static_cast<uint8_t>(w >> 24);
}

} // namespace

bool tex_supported(int width, int height, bool rle)
{
	return !rle && width == height && width >= static_cast<int>(kTexMin) && width <= static_cast<int>(kTexMax) &&
	       is_pot(static_cast<unsigned>(width));
}

uint32_t tex_palette_word(const uint32_t* t, TexKind kind, unsigned i)
{
	i &= 0xFFu;
	const bool transparent = kind_keyed(kind) && i == 0;
	switch (kind)
	{
		case TexKind::Mask:
			return transparent ? 0u : 255u;
		case TexKind::Shadeless:
		case TexKind::ShadelessKeyed:
			return texel_word(t[i] & kRgb, transparent ? 0u : 255u);
		case TexKind::Normal:
		case TexKind::NormalKeyed:
		default:
		{
			const unsigned a = transparent ? 0u : ((t[i] & kRgb) != 0 ? 128u : 255u);
			return texel_word(t[256u * 255u + i] & kRgb, a);
		}
	}
}

bool tex_convert(const TexBitmap& bitmap, const uint32_t* tables, TexKind kind, void* out)
{
	if (!tex_supported(bitmap) || !bitmap.rows || !out || (kind != TexKind::Mask && !tables))
		return false;
	const unsigned W = static_cast<unsigned>(bitmap.width);

	if (kind == TexKind::Mask)
	{
		uint8_t* dst = static_cast<uint8_t*>(out);
		const uint8_t lut[2] = {static_cast<uint8_t>(tex_palette_word(nullptr, kind, 0)),
		                        static_cast<uint8_t>(tex_palette_word(nullptr, kind, 1))};
		for (unsigned j = 0; j < W; ++j)
		{
			const uint8_t* row = bitmap.rows[j];
			const unsigned m = tile_memory_row(W, j);
			for (unsigned s = 0; s < W; ++s)
				dst[tile_index(W, m, s)] = lut[row[s] != 0];
		}
		return true;
	}

	uint32_t palette[256];
	for (unsigned i = 0; i < 256; ++i)
		palette[i] = tex_palette_word(tables, kind, i);
	uint8_t* dst = static_cast<uint8_t*>(out);
	for (unsigned j = 0; j < W; ++j)
	{
		const uint8_t* row = bitmap.rows[j];
		const unsigned m = tile_memory_row(W, j);
		for (unsigned s = 0; s < W; ++s)
		{
			const uint32_t w = palette[row[s]];
			uint8_t* p = dst + tile_index(W, m, s) * 4;   // little-endian u32: A, B, G, R
			p[0] = static_cast<uint8_t>(w);
			p[1] = static_cast<uint8_t>(w >> 8);
			p[2] = static_cast<uint8_t>(w >> 16);
			p[3] = static_cast<uint8_t>(w >> 24);
		}
	}
	return true;
}

bool rect_tex_size(int width, int height, bool rle, unsigned& S, unsigned& T)
{
	if (!rle || width < 1 || height < 1 || width > static_cast<int>(kRectTexMax) || height > static_cast<int>(kRectTexMax))
		return false;
	S = pot_at_least(static_cast<unsigned>(height));
	T = pot_at_least(static_cast<unsigned>(width));
	return true;
}

bool tex_convert_rle(const TexBitmap& bitmap, const uint32_t* tables, TexKind kind, unsigned S, unsigned T, void* out)
{
	unsigned s_want = 0, t_want = 0;
	if (!rect_tex_size(bitmap.width, bitmap.height, bitmap.rle, s_want, t_want) || S != s_want || T != t_want ||
	    !bitmap.rows || !out)
		return false;
	if (kind != TexKind::NormalKeyed && kind != TexKind::ShadelessKeyed && kind != TexKind::Mask)
		return false;
	const bool a8 = kind == TexKind::Mask;
	if (!a8 && !tables)
		return false;
	uint32_t palette[256];
	for (unsigned i = 0; i < 256; ++i)
		palette[i] = tex_palette_word(tables, kind, i);   // index 0: A 0 (keyed kinds)
	const unsigned W = static_cast<unsigned>(bitmap.width), H = static_cast<unsigned>(bitmap.height);
	uint8_t* dst = static_cast<uint8_t*>(out);
	for (unsigned c = 0; c < T; ++c)
	{
		const unsigned m = tile_memory_row(T, c);
		unsigned first = 0, last = 0;
		const uint8_t* data = nullptr;
		if (c < W)
		{
			// The column's header (scottish_textures.cpp:763-767; last exclusive, shapes.cpp:705-717).
			const uint8_t* p = bitmap.rows[c];
			first = (static_cast<unsigned>(p[0]) << 8) | p[1];
			last = (static_cast<unsigned>(p[2]) << 8) | p[3];
			data = p + 4;
			if (last > H)
				last = H;   // SW never samples a row >= H (texture_y < H << 16)
		}
		for (unsigned s = 0; s < S; ++s)
		{
			const unsigned i = (data && s >= first && s < last) ? data[s - first] : 0u;
			put_texel(dst, tile_index(S, m, s), palette[i], a8);
		}
	}
	return true;
}

} // namespace a1_c3d

namespace a1_c3d {

bool tex_convert_landscape(const TexBitmap& bitmap, const uint32_t* tables, TexKind kind, int part, unsigned S,
                           unsigned T, void* out)
{
	unsigned s_want = 0, t_want[2] = {0, 0};
	int parts = 0;
	if (!landscape_tex_size(bitmap.width, bitmap.height, bitmap.rle, s_want, t_want, &parts) || part < 0 ||
	    part >= parts || S != s_want || T != t_want[part] || !bitmap.rows || !tables || !out ||
	    (kind != TexKind::Normal && kind != TexKind::Shadeless))
		return false;
	uint32_t palette[256];
	for (unsigned i = 0; i < 256; ++i)
		palette[i] = tex_palette_word(tables, kind, i);
	uint8_t* dst = static_cast<uint8_t*>(out);
	const unsigned cut = static_cast<unsigned>(landscape_part_rows());
	const unsigned base = cut * static_cast<unsigned>(part);
	const unsigned R = static_cast<unsigned>(bitmap.width);
	const unsigned count = part ? R - base : (R < cut ? R : cut);   // part 1 holds the rest (<= 512 rows)
	for (unsigned j = 0; j < T; ++j)
	{
		const uint8_t* row = bitmap.rows[base + (j < count ? j : count - 1)];
		const unsigned m = tile_memory_row(T, j);
		for (unsigned s = 0; s < S; ++s)
		{
			const uint32_t w = palette[row[s]];
			uint8_t* p = dst + tile_index(S, m, s) * 4;   // little-endian u32: A, B, G, R
			p[0] = static_cast<uint8_t>(w);
			p[1] = static_cast<uint8_t>(w >> 8);
			p[2] = static_cast<uint8_t>(w >> 16);
			p[3] = static_cast<uint8_t>(w >> 24);
		}
	}
	return true;
}

} // namespace a1_c3d
