/*
	Aleph One for the Nintendo 3DS, citro3d back end: texture tiling (see tiling.h).

	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; either version 3 of the License, or
	(at your option) any later version.
*/
#include "tiling.h"

#include <cstring>

namespace a1_c3d {

void tile_image(const void* src, unsigned W, unsigned texel_bytes, void* dst)
{
	const uint8_t* in = static_cast<const uint8_t*>(src);
	uint8_t* out = static_cast<uint8_t*>(dst);
	for (unsigned m = 0; m < W; ++m)
		for (unsigned s = 0; s < W; ++s)
			std::memcpy(out + tile_index(W, m, s) * texel_bytes,
			            in + (static_cast<size_t>(m) * W + s) * texel_bytes, texel_bytes);
}

} // namespace a1_c3d
