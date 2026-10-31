/*
	Aleph One for the Nintendo 3DS, citro3d back end: bitmap -> GPU texel conversion
	(docs/3ds/CITRO3D_DESIGN.md, 3.2 and section 7, M2 specification "Conversion"). Plain C++, no 3DS
	or engine headers, host-tested with tables built like shapes.cpp's build_shading_tables32.

	Input: a bitmap view (8-bit indices, `rows[j][s]` = the byte of GPU texel (s, j); square) and the
	32-bit shading tables (pixel_format_32 is ARGB8888: entry = 0xAARRGGBB, 0xFF000000 | rgb; at least
	256 x 256 entries, `table[256 * L + i]` = colour of index i at level L, number_of_shading_tables
	being 256 at 32 bits). Output: the W x W texels already tiled (tiling.h), source row j in memory row
	W - 1 - j:

	  normal           RGBA8; RGB = table[256 * 255 + i] & 0xFFFFFF (the top table, what fullbright
	                   selects); A = 128 if (table[i] & 0xFFFFFF) != 0 (self-luminous: level 0 is not
	                   black only for self-luminous colours), else 255
	  normal_keyed     the same, A = 0 for index 0 (a _TRANSPARENT_BIT bitmap used on a wall)
	  shadeless        RGBA8; RGB = table[i] & 0xFFFFFF (infravision's single tint table), A = 255
	  shadeless_keyed  the same, A = 0 for index 0
	  mask             A8 (one byte per texel, tiled with the same Morton pattern): 0 for index 0, else
	                   255; no tables (the tint is the global one, applied at draw time)

	An RGBA8 texel is the little-endian u32 R<<24 | G<<16 | B<<8 | A, i.e. bytes A, B, G, R in memory.

	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; either version 3 of the License, or
	(at your option) any later version.
*/
#ifndef A1_C3D_CORE_TEX_CONVERT_H
#define A1_C3D_CORE_TEX_CONVERT_H

#include <cstddef>
#include <cstdint>

namespace a1_c3d {

enum class TexKind : uint8_t { Normal, NormalKeyed, Shadeless, ShadelessKeyed, Mask };

constexpr unsigned kTexMin = 128;   // M2 supports square power-of-two bitmaps in [128, 512]
constexpr unsigned kTexMax = 512;   // (1024 squared RGBA8 is 4 MiB: above vram_cap and linear_cap)
constexpr unsigned kShadingTableEntries = 256 * 256;

// A read-only view of an engine bitmap_definition.
struct TexBitmap
{
	int width = 0, height = 0;
	const uint8_t* const* rows = nullptr;   // row_addresses: `height` rows of `width` bytes
	bool rle = false;                       // bytes_per_row == NONE (sprites: M4)
};

// The M2 bitmap predicate: not RLE, square, a power of two in [kTexMin, kTexMax].
bool tex_supported(int width, int height, bool rle);
inline bool tex_supported(const TexBitmap& b) { return tex_supported(b.width, b.height, b.rle); }

inline unsigned tex_texel_bytes(TexKind k) { return k == TexKind::Mask ? 1 : 4; }
inline size_t tex_bytes(unsigned W, TexKind k) { return static_cast<size_t>(W) * W * tex_texel_bytes(k); }

// Converts and tiles into `out` (tex_bytes(W, kind) bytes, 4-byte aligned for RGBA kinds). False
// (nothing written) if the bitmap is unsupported, `rows` is null, or `tables` is null for a kind
// that needs it (every kind but Mask).
bool tex_convert(const TexBitmap& bitmap, const uint32_t* shading_tables, TexKind kind, void* out);

// The colour/alpha word of palette index i for an RGBA8 kind (the converter's per-index step;
// exposed for the tests). Mask: the A8 value, 0 or 255.
uint32_t tex_palette_word(const uint32_t* shading_tables, TexKind kind, unsigned i);

} // namespace a1_c3d

#endif
