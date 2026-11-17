/*
	Aleph One for the Nintendo 3DS, citro3d back end: the static noise of M5 (see noise.h).

	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; either version 3 of the License, or
	(at your option) any later version.
*/
#include "noise.h"

#include "tiling.h"

namespace a1_c3d {

void noise_build(void* out)
{
	uint8_t* dst = static_cast<uint8_t*>(out);
	uint16_t seed = kNoiseSeed;
	for (unsigned x = 0; x < kNoiseSide; ++x)
	{
		for (unsigned y = 0; y < kNoiseSide; ++y)
		{
			const uint32_t w = noise_word(seed);
			uint8_t* p = dst + tile_index(kNoiseSide, tile_memory_row(kNoiseSide, y), x) * 4;   // u32 LE: A, B, G, R
			p[0] = static_cast<uint8_t>(w);
			p[1] = static_cast<uint8_t>(w >> 8);
			p[2] = static_cast<uint8_t>(w >> 16);
			p[3] = static_cast<uint8_t>(w >> 24);
			seed = sw_lfsr_step(seed);
		}
	}
}

double noise_density(unsigned ref)
{
	unsigned keep = 0;
	uint16_t seed = kNoiseSeed;
	for (unsigned k = 0; k < kNoiseSide * kNoiseSide; ++k)
	{
		keep += (noise_word(seed) & 0xFFu) >= ref;
		seed = sw_lfsr_step(seed);
	}
	return static_cast<double>(keep) / (kNoiseSide * kNoiseSide);
}

} // namespace a1_c3d
