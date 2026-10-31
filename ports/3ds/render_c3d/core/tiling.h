/*
	Aleph One for the Nintendo 3DS, citro3d back end: the PICA200 texture tiling
	(docs/3ds/CITRO3D_DESIGN.md, section 7, M2 specification "Tiling"). Plain C++, no 3DS or
	engine headers; checked byte for byte against /opt/devkitpro/tools/bin/tex3ds in the host tests
	(scripts/3ds/tests/c3d-host/test_core.cpp).

	An image of W x W texels (W a multiple of 8) is stored as 8 x 8 tiles in row order across the
	width, tile rows top-down, and inside a tile in Morton (Z) order: the in-tile column x and row y
	interleave as bit 0 = x0, bit 1 = y0, bit 2 = x1, bit 3 = y1, bit 4 = x2, bit 5 = y2.
	"Memory row" m is the row index of that layout (tex3ds writes the PNG's row 0 as memory row 0).
	The sampler's t = 0 reads the LAST memory row, so the converter stores source row j in memory row
	W - 1 - j (tile_memory_row); the tile functions themselves never flip.

	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; either version 3 of the License, or
	(at your option) any later version.
*/
#ifndef A1_C3D_CORE_TILING_H
#define A1_C3D_CORE_TILING_H

#include <cstddef>
#include <cstdint>

namespace a1_c3d {

// Offset (in texels) inside an 8 x 8 tile of the texel at column x, row y (both 0..7).
inline unsigned morton8(unsigned x, unsigned y)
{
	return (x & 1u) | ((y & 1u) << 1) | ((x & 2u) << 1) | ((y & 2u) << 2) | ((x & 4u) << 2) | ((y & 4u) << 3);
}

// Texel index in a tiled W x W image of the texel at memory row m, column s (W a multiple of 8).
inline size_t tile_index(unsigned W, unsigned m, unsigned s)
{
	return (static_cast<size_t>(m >> 3) * (W >> 3) + (s >> 3)) * 64 + morton8(s, m);
}

// The memory row that holds source row j of a W-row image (the orientation rule above).
inline unsigned tile_memory_row(unsigned W, unsigned j) { return W - 1 - j; }

// Tiles a linear W x W image (row m at src + m * W * texel_bytes, texel_bytes 1 or 4 or any size)
// into dst (W * W * texel_bytes bytes; src and dst must not overlap). No row flip.
void tile_image(const void* src, unsigned W, unsigned texel_bytes, void* dst);

} // namespace a1_c3d

#endif
