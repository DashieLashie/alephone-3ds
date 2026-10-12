/*
	Aleph One for the Nintendo 3DS: the direct present path (docs/3ds/PERF.md,
	experiment a). See present_screen() in ports/3ds/include/a1_3ds_platform.h.

	SDL's path, per frame: SDL_UpdateTexture copies the 640x480 screen surface
	into a texture (1.2 MB); the software renderer clears its 400x240 window
	surface and scales the texture into it; SDL_RenderPresent then has the
	N3DS driver copy the window surface into the framebuffer one pixel at a
	time, rotated (the LCD scans columns), and flush the data cache. This
	file does the scale, the rotation and the format conversion in one pass,
	straight into the framebuffer, and touches each output pixel once.

	The result is the same picture. The engine asks SDL for linear scaling
	(SDL_HINT_RENDER_SCALE_QUALITY, screen.cpp), and SDL's bilinear filter
	puts each output pixel's centre at source 2x + 0.5 (SDL_stretch.c,
	get_scaler_datas): at exactly 2:1 that is the average of a 2x2 block,
	which this computes with the ARMv6 halving add (UHADD8, four channels
	at once; it rounds down, so a channel can be 1 lower than SDL's). The
	surface's XRGB8888 becomes the framebuffer's RGBA8 (the LCD ignores A).

	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; either version 3 of the License, or
	(at your option) any later version.
*/
#include "a1_3ds_platform.h"

#include <3ds.h>
#include <SDL2/SDL.h>
#include <arm_acle.h>   // __uhadd8

#include <stdio.h>
#include <string.h>

namespace a1_3ds {

namespace {

// The top screen as the LCD scans it: 400 columns of 240 pixels, each column
// stored bottom to top. Screen pixel (x, y) is fb[x * 240 + (239 - y)].
constexpr int kScreenW = 400, kScreenH = 240;
// Logical 640x480 halved, centred: 40-pixel bars left and right.
constexpr int kSrcW = 640, kSrcH = 480;
constexpr int kOutW = kSrcW / 2, kOutH = kSrcH / 2;
constexpr int kBarW = (kScreenW - kOutW) / 2;
// Output rows per pass: one 32-byte cache line of a framebuffer column.
constexpr int kBlock = 8;
static_assert(kOutH % kBlock == 0, "rows per pass must divide the height");
static_assert(kBlock == 8, "the inner loop is unrolled for 8 rows");

// The two framebuffers of the top screen, once their bars are black.
const void* s_bars_cleared[2] = {nullptr, nullptr};
bool s_logged = false;

inline u32 to_fb(u32 xrgb)
{
	return xrgb << 8;   // 0x00RRGGBB -> 0xRRGGBB00 (RGBA8; the LCD ignores A)
}

// The 2x2 block at column sx of two source rows, averaged per channel.
inline u32 box(const u32* r0, const u32* r1, int sx)
{
	return __uhadd8(__uhadd8(r0[sx], r0[sx + 1]), __uhadd8(r1[sx], r1[sx + 1]));
}

void clear_bars(u32* fb)
{
	if (s_bars_cleared[0] == fb || s_bars_cleared[1] == fb)
		return;
	memset(fb, 0, kBarW * kScreenH * sizeof(u32));
	memset(fb + (kBarW + kOutW) * kScreenH, 0, kBarW * kScreenH * sizeof(u32));
	s_bars_cleared[s_bars_cleared[0] ? 1 : 0] = fb;
}

} // namespace

bool present_screen(const void* pixels, int pitch, int width, int height, uint32_t sdl_format)
{
	if (width != kSrcW || height != kSrcH ||
	    (sdl_format != SDL_PIXELFORMAT_RGB888 && sdl_format != SDL_PIXELFORMAT_ARGB8888) ||
	    gfxGetScreenFormat(GFX_TOP) != GSP_RGBA8_OES)
	{
		if (!s_logged)
		{
			s_logged = true;
			printf("[3ds] video: direct present not used (%dx%d %s); SDL presents\n", width, height,
			       SDL_GetPixelFormatName(sdl_format));
		}
		return false;
	}
	if (!s_logged)
	{
		s_logged = true;
		printf("[3ds] video: direct present (640x480 %s -> 320x240 RGBA8 framebuffer)\n",
		       SDL_GetPixelFormatName(sdl_format));
	}

	// The buffer gfxGetFramebuffer() returns is the one not being shown, but
	// only once the previous swap took effect: that happens at the next
	// VBlank. Drawing before then would tear.
	if (gspIsPresentPending(GSP_SCREEN_TOP))
		gspWaitForVBlank();

	u16 fb_w = 0, fb_h = 0;
	u32* fb = reinterpret_cast<u32*>(gfxGetFramebuffer(GFX_TOP, GFX_LEFT, &fb_w, &fb_h));
	if (fb_w != kScreenH || fb_h != kScreenW)
		return false;
	clear_bars(fb);

	// kBlock output rows at a time (2 * kBlock source rows, read left to
	// right); each output column gets one 32-byte run (a cache line) per pass.
	const u8* src = static_cast<const u8*>(pixels);
	u32* col0 = fb + kBarW * kScreenH;
	for (int y0 = 0; y0 < kOutH; y0 += kBlock)
	{
		const u32* rows[2 * kBlock];
		for (int i = 0; i < 2 * kBlock; ++i)
			rows[i] = reinterpret_cast<const u32*>(src + (2 * y0 + i) * pitch);
		// Output row y0 + i lands at column offset 239 - (y0 + i).
		u32* d = col0 + (kScreenH - kBlock - y0);
		for (int x = 0; x < kOutW; ++x, d += kScreenH)
		{
			const int sx = 2 * x;
			d[7] = to_fb(box(rows[0], rows[1], sx));
			d[6] = to_fb(box(rows[2], rows[3], sx));
			d[5] = to_fb(box(rows[4], rows[5], sx));
			d[4] = to_fb(box(rows[6], rows[7], sx));
			d[3] = to_fb(box(rows[8], rows[9], sx));
			d[2] = to_fb(box(rows[10], rows[11], sx));
			d[1] = to_fb(box(rows[12], rows[13], sx));
			d[0] = to_fb(box(rows[14], rows[15], sx));
		}
	}

	GSPGPU_FlushDataCache(fb, kScreenW * kScreenH * sizeof(u32));
	gfxScreenSwapBuffers(GFX_TOP, false);
	return true;
}

} // namespace a1_3ds
