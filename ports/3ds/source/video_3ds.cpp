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

	With the HUD or the map on the bottom screen (docs/3ds/STAGE7.md) the
	game screen is 400x240 and goes to the top screen 1:1, without the
	averaging. present_bottom() does the same for the bottom screen (RGB565).

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

#ifdef A1_RENDER_C3D
#include "a1_c3d_port.h"
#endif

namespace a1_3ds {

namespace {

// The top screen as the LCD scans it: 400 columns of 240 pixels, each column
// stored bottom to top. Screen pixel (x, y) is fb[x * 240 + (239 - y)].
constexpr int kScreenW = 400, kScreenH = 240;
// Logical 640x480 halved, centred: 40-pixel bars left and right.
constexpr int kSrcW = 640, kSrcH = 480;
constexpr int kOutW = kSrcW / 2, kOutH = kSrcH / 2;
constexpr int kBarW = (kScreenW - kOutW) / 2;
// Output rows per pass: 16 bytes (half a cache line) of a framebuffer column.
// The 2 * kBlock source row pointers stay in registers (ARM has 13 for
// general use); 8 rows per pass made GCC keep 16 of them on the stack.
constexpr int kBlock = 4;
static_assert(kOutH % kBlock == 0, "rows per pass must divide the height");
static_assert(kBlock == 4, "the inner loop is unrolled for 4 rows");

// The two framebuffers of the top screen, once their bars are black.
const void* s_bars_cleared[2] = {nullptr, nullptr};
bool s_logged = false;

inline u32 to_fb(u32 xrgb)
{
	return xrgb << 8;   // 0x00RRGGBB -> 0xRRGGBB00 (RGBA8; the LCD ignores A)
}

// The 2x2 block at the front of two source rows, averaged per channel; the
// rows then move on by two pixels. A pair of pixels is one 64-bit load (LDRD).
inline u32 box(const u32*& r0, const u32*& r1)
{
	u64 a, b;
	memcpy(&a, r0, sizeof a);
	memcpy(&b, r1, sizeof b);
	r0 += 2;
	r1 += 2;
	return __uhadd8(__uhadd8(static_cast<u32>(a), static_cast<u32>(a >> 32)),
	                __uhadd8(static_cast<u32>(b), static_cast<u32>(b >> 32)));
}

void clear_bars(u32* fb)
{
	if (s_bars_cleared[0] == fb || s_bars_cleared[1] == fb)
		return;
	memset(fb, 0, kBarW * kScreenH * sizeof(u32));
	memset(fb + (kBarW + kOutW) * kScreenH, 0, kBarW * kScreenH * sizeof(u32));
	s_bars_cleared[s_bars_cleared[0] ? 1 : 0] = fb;
}

// 1:1 into a 240-pixel-tall rotated framebuffer: `w` columns from `x0`,
// kBlock source rows per pass, each column getting one run of kBlock pixels.
// Out is u32 (RGBA8) or u16 (RGB565).
template <class Out, Out (*Convert)(u32)>
void rotate_copy(Out* fb, int x0, const u8* src, int pitch, int w, int h)
{
	for (int y0 = 0; y0 < h; y0 += kBlock)
	{
		const u8* row = src + y0 * pitch;
		const u32* r0 = reinterpret_cast<const u32*>(row);
		const u32* r1 = reinterpret_cast<const u32*>(row + pitch);
		const u32* r2 = reinterpret_cast<const u32*>(row + 2 * pitch);
		const u32* r3 = reinterpret_cast<const u32*>(row + 3 * pitch);
		Out* d = fb + x0 * kScreenH + (kScreenH - kBlock - y0);
		for (int x = 0; x < w; ++x, d += kScreenH)
		{
			d[3] = Convert(r0[x]);
			d[2] = Convert(r1[x]);
			d[1] = Convert(r2[x]);
			d[0] = Convert(r3[x]);
		}
	}
}

inline u16 to_rgb565(u32 xrgb)
{
	return static_cast<u16>(((xrgb >> 8) & 0xf800) | ((xrgb >> 5) & 0x07e0) | ((xrgb >> 3) & 0x001f));
}

bool s_logged_native = false;

bool present_native(const void* pixels, int pitch)
{
	if (!s_logged_native)
	{
		s_logged_native = true;
		printf("[3ds] video: direct present 1:1 (400x240 -> RGBA8 framebuffer)\n");
	}
	if (gspIsPresentPending(GSP_SCREEN_TOP))
		gspWaitForVBlank();
	u16 fb_w = 0, fb_h = 0;
	u32* fb = reinterpret_cast<u32*>(gfxGetFramebuffer(GFX_TOP, GFX_LEFT, &fb_w, &fb_h));
	if (fb_w != kScreenH || fb_h != kScreenW)
		return false;
	rotate_copy<u32, to_fb>(fb, 0, static_cast<const u8*>(pixels), pitch, kScreenW, kScreenH);
	// The 640x480 path's bars are gone from this buffer now.
	s_bars_cleared[0] = s_bars_cleared[1] = nullptr;
	GSPGPU_FlushDataCache(fb, kScreenW * kScreenH * sizeof(u32));
	gfxScreenSwapBuffers(GFX_TOP, false);
	return true;
}

// 640x480 halved into the 320 middle columns of a rotated 240x400 framebuffer
// (the bars are not touched). Used by present_screen_cpu() and by the GPU
// present's self-check (present_reference). Forced inline, with the caller's `src`
// local kept: present_screen_cpu() then compiles to exactly the code it had before
// this was a function (objdump of video_3ds.o, -O2, identical); plain `inline`
// changed its register allocation.
__attribute__((always_inline)) inline void half_rotate(u32* fb, const u8* src, int pitch)
{
	// kBlock output rows at a time (2 * kBlock source rows, read left to
	// right); each output column gets one 16-byte run per pass.
	u32* col0 = fb + kBarW * kScreenH;
	for (int y0 = 0; y0 < kOutH; y0 += kBlock)
	{
		const u8* row = src + 2 * y0 * pitch;
		const u32* r0 = reinterpret_cast<const u32*>(row);
		const u32* r1 = reinterpret_cast<const u32*>(row + pitch);
		const u32* r2 = reinterpret_cast<const u32*>(row + 2 * pitch);
		const u32* r3 = reinterpret_cast<const u32*>(row + 3 * pitch);
		const u32* r4 = reinterpret_cast<const u32*>(row + 4 * pitch);
		const u32* r5 = reinterpret_cast<const u32*>(row + 5 * pitch);
		const u32* r6 = reinterpret_cast<const u32*>(row + 6 * pitch);
		const u32* r7 = reinterpret_cast<const u32*>(row + 7 * pitch);
		// Output row y0 + i lands at column offset 239 - (y0 + i).
		u32* d = col0 + (kScreenH - kBlock - y0);
		u32* const end = d + kOutW * kScreenH;
		for (; d != end; d += kScreenH)
		{
			d[3] = to_fb(box(r0, r1));
			d[2] = to_fb(box(r2, r3));
			d[1] = to_fb(box(r4, r5));
			d[0] = to_fb(box(r6, r7));
		}
	}
}

} // namespace

namespace {

bool present_screen_cpu(const void* pixels, int pitch, int width, int height, uint32_t sdl_format)
{
	if (width == kScreenW && height == kScreenH && gfxGetScreenFormat(GFX_TOP) == GSP_RGBA8_OES &&
	    (sdl_format == SDL_PIXELFORMAT_RGB888 || sdl_format == SDL_PIXELFORMAT_ARGB8888))
		return present_native(pixels, pitch);
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

	const u8* src = static_cast<const u8*>(pixels);
	half_rotate(fb, src, pitch);

	GSPGPU_FlushDataCache(fb, kScreenW * kScreenH * sizeof(u32));
	gfxScreenSwapBuffers(GFX_TOP, false);
	return true;
}

} // namespace

bool present_screen(const void* pixels, int pitch, int width, int height, uint32_t sdl_format)
{
#ifdef A1_RENDER_C3D
	// The GPU present (docs/3ds/CITRO3D_DESIGN.md, 5.1): citro3d owns the top screen once it
	// is up and takes every surface it is given. False only while it is not in use.
	if (a1_c3d_present(pixels, pitch, width, height, sdl_format))
		return true;
#endif
	const bool done = present_screen_cpu(pixels, pitch, width, height, sdl_format);
#ifdef A1_RENDER_C3D
	if (!done)
		a1_c3d_note_false_return();
#endif
	return done;
}

#ifdef A1_RENDER_C3D
// The picture present_screen_cpu() would put in the framebuffer, written into `fb` (a
// 240x400 rotated RGBA8 buffer, bars black) by the same code. For the GPU present's
// self-check (debug-c3d "present_diff", render_c3d/back/present.cpp). False, `fb`
// untouched, for a surface the CPU present does not take (then SDL presents).
bool present_reference(u32* fb, const void* pixels, int pitch, int width, int height, uint32_t sdl_format)
{
	if (sdl_format != SDL_PIXELFORMAT_RGB888 && sdl_format != SDL_PIXELFORMAT_ARGB8888)
		return false;
	if (width == kScreenW && height == kScreenH)
		rotate_copy<u32, to_fb>(fb, 0, static_cast<const u8*>(pixels), pitch, kScreenW, kScreenH);
	else if (width == kSrcW && height == kSrcH)
	{
		memset(fb, 0, kScreenW * kScreenH * sizeof(u32));
		half_rotate(fb, static_cast<const u8*>(pixels), pitch);
	}
	else
		return false;
	return true;
}
#endif

bool present_bottom(const void* pixels, int pitch)
{
	constexpr int kBottomW = 320;
	if (gfxGetScreenFormat(GFX_BOTTOM) != GSP_RGB565_OES)
		return false;
	if (gspIsPresentPending(GSP_SCREEN_BOTTOM))
		gspWaitForVBlank();
	u16 fb_w = 0, fb_h = 0;
	u16* fb = reinterpret_cast<u16*>(gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, &fb_w, &fb_h));
	if (fb_w != kScreenH || fb_h != kBottomW)
		return false;
	rotate_copy<u16, to_rgb565>(fb, 0, static_cast<const u8*>(pixels), pitch, kBottomW, kScreenH);
	GSPGPU_FlushDataCache(fb, kBottomW * kScreenH * sizeof(u16));
	gfxScreenSwapBuffers(GFX_BOTTOM, false);
	return true;
}

void half_blit(const void* src, int src_pitch, int w, int h, void* dst, int dst_pitch)
{
	const u8* s = static_cast<const u8*>(src);
	u8* d = static_cast<u8*>(dst);
	for (int y = 0; y < h; ++y, s += 2 * src_pitch, d += dst_pitch)
	{
		const u32* r0 = reinterpret_cast<const u32*>(s);
		const u32* r1 = reinterpret_cast<const u32*>(s + src_pitch);
		u32* out = reinterpret_cast<u32*>(d);
		for (int x = 0; x < w; ++x)
			out[x] = box(r0, r1);
	}
}

} // namespace a1_3ds
