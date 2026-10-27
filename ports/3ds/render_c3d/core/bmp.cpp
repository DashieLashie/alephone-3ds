/*
	Aleph One for the Nintendo 3DS, citro3d back end: BMP encoder (see bmp.h).

	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; either version 3 of the License, or
	(at your option) any later version.
*/
#include "bmp.h"

namespace a1_c3d {

namespace {

void put16(uint8_t* p, uint32_t v)
{
	p[0] = static_cast<uint8_t>(v);
	p[1] = static_cast<uint8_t>(v >> 8);
}

void put32(uint8_t* p, uint32_t v)
{
	put16(p, v);
	put16(p + 2, v >> 16);
}

} // namespace

std::vector<uint8_t> bmp_encode(const uint32_t* px, int w, int h, int pitch_px)
{
	if (w <= 0 || h <= 0)
		return std::vector<uint8_t>();
	const uint32_t data = static_cast<uint32_t>(w) * 4u * static_cast<uint32_t>(h);
	std::vector<uint8_t> out(54 + static_cast<size_t>(data), 0);
	uint8_t* p = out.data();

	p[0] = 'B';
	p[1] = 'M';
	put32(p + 2, 54 + data);   // file size
	put32(p + 10, 54);         // pixel data offset
	put32(p + 14, 40);         // BITMAPINFOHEADER size
	put32(p + 18, static_cast<uint32_t>(w));
	put32(p + 22, static_cast<uint32_t>(h));   // positive: bottom-up
	put16(p + 26, 1);          // planes
	put16(p + 28, 32);         // bits per pixel
	put32(p + 30, 0);          // BI_RGB
	put32(p + 34, data);       // image size

	uint8_t* row = p + 54;
	for (int y = h - 1; y >= 0; --y)   // the file's first row is the image's last
	{
		const uint32_t* src = px + static_cast<size_t>(y) * static_cast<size_t>(pitch_px);
		for (int x = 0; x < w; ++x, row += 4)
		{
			const uint32_t v = src[x];
			row[0] = static_cast<uint8_t>(v);          // B
			row[1] = static_cast<uint8_t>(v >> 8);     // G
			row[2] = static_cast<uint8_t>(v >> 16);    // R
			row[3] = static_cast<uint8_t>(v >> 24);    // A
		}
	}
	return out;
}

} // namespace a1_c3d
