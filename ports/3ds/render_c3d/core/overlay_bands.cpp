/*
	Aleph One for the Nintendo 3DS, citro3d back end: the overlay bands of a direct frame (see overlay_bands.h).

	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; either version 3 of the License, or
	(at your option) any later version.
*/
#include "overlay_bands.h"

#include <algorithm>

namespace a1_c3d {

int bands_normalise(const Band* in, int n, int height, Band* out)
{
	// At most 4 inputs from the engine (crosshair, top, bottom, calibration); a few more are tolerated.
	constexpr int kMaxIn = 16;
	Band b[kMaxIn];
	int m = 0;
	for (int i = 0; i < n && m < kMaxIn; ++i)
	{
		int y0 = std::max(in[i].y0, 0), y1 = std::min(in[i].y1, height);
		if (y0 >= y1)
			continue;
		y0 = y0 / kBandTile * kBandTile;
		y1 = (y1 + kBandTile - 1) / kBandTile * kBandTile;
		b[m++] = Band{y0, std::min(y1, height)};
	}
	std::sort(b, b + m, [](const Band& a, const Band& c) { return a.y0 < c.y0; });
	int k = 0;
	for (int i = 0; i < m; ++i)
	{
		if (k > 0 && b[i].y0 <= b[k - 1].y1)
			b[k - 1].y1 = std::max(b[k - 1].y1, b[i].y1);
		else
			b[k++] = b[i];
	}
	while (k > kMaxBands)
	{
		int best = 0;
		for (int i = 1; i + 1 < k; ++i)
			if (b[i + 1].y0 - b[i].y1 < b[best + 1].y0 - b[best].y1)
				best = i;
		b[best].y1 = b[best + 1].y1;
		for (int i = best + 1; i + 1 < k; ++i)
			b[i] = b[i + 1];
		--k;
	}
	for (int i = 0; i < k; ++i)
		out[i] = b[i];
	return k;
}

uint32_t bands_tile_mask(const Band* b, int n)
{
	uint32_t mask = 0;
	for (int i = 0; i < n; ++i)
		for (int t = b[i].y0 / kBandTile; t < b[i].y1 / kBandTile && t < 32; ++t)
			mask |= 1u << t;
	return mask;
}

} // namespace a1_c3d
