/*
	Aleph One for the Nintendo 3DS, citro3d back end: the overlay bands of a direct frame
	(docs/3ds/CITRO3D_DESIGN.md, 5.3 "Bands, not boxes", and section 7, M6 specification "Overlay bands").
	Plain C++, host-tested.

	The overlay surface is uploaded into a tiled texture by display transfers, which write whole texture rows in
	8-row tiles: every overlay region is a full-width band of rows, rounded out to tiles. A frame has at most
	kMaxBands bands (one GX queue entry each).

	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; either version 3 of the License, or
	(at your option) any later version.
*/
#ifndef A1_C3D_CORE_OVERLAY_BANDS_H
#define A1_C3D_CORE_OVERLAY_BANDS_H

#include <cstdint>

namespace a1_c3d {

constexpr int kMaxBands = 4;
constexpr int kBandTile = 8;

struct Band
{
	int y0, y1;   // rows [y0, y1)
};

// Clamps each input band to [0, height), drops empty ones, rounds y0 down and y1 up to multiples of kBandTile
// (height itself is a multiple of kBandTile), sorts, merges bands that overlap or touch, and while more than
// kMaxBands remain merges the neighbouring pair with the smallest gap. Writes the result to `out` (room for
// kMaxBands) and returns its count (0..kMaxBands).
int bands_normalise(const Band* in, int n, int height, Band* out);

// The tiles (bit t = rows [8t, 8t + 8)) the bands cover; height <= 32 tiles.
uint32_t bands_tile_mask(const Band* b, int n);

} // namespace a1_c3d

#endif
