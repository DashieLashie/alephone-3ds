/*
	Aleph One for the Nintendo 3DS, citro3d back end: the GPU present
	(docs/3ds/CITRO3D_DESIGN.md, 5.1).

	citro3d owns the top screen: one 240x400 RGBA8 target linked to GFX_TOP.
	Per present:
	  1. C3D_FrameBegin(0): every earlier C3D frame has retired (zone c3d_wait);
	  2. the CPU writes the picture into a linear staging image (pitch 512 pixels, RGBA8,
	     400x240 used): 400x240 copied with << 8, 640x480 halved with the same UHADD8
	     2x2 box and the same << 8 as video_3ds.cpp's CPU present (so Classic and menus are
	     byte-identical), centred with black bars; any other size or format is converted
	     (SDL_ConvertPixels) and centre-cropped / letterboxed;
	  3. one display transfer (linear -> tiled) into a 512x256 RGBA8 texture in VRAM;
	  4. one textured quad into the screen target;
	  5. ranged flush of the staging's written rows, wait for the displayed buffer to be known (VBlank, if
	     the previous swap is pending), C3D_FrameEnd(GX_CMDLIST_FLUSH). citro3d transfers
	     the target to the framebuffer and swaps, asynchronously.

	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; either version 3 of the License, or
	(at your option) any later version.
*/
#include "context.h"

#include "../include/a1_c3d_port.h"
#include "a1_3ds_platform.h"

#include <3ds.h>
#include <SDL2/SDL.h>
#include <arm_acle.h>   // __uhadd8
#include <citro3d.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vector>

#include "a1_c3d_shaders_shbin.h"

// CSeries/csalerts_sdl.cpp (test_vhalt only)
extern void vhalt(const char* message);

namespace a1_3ds {
// source/a1_3ds_platform.cpp: both screens to sdmc:/3ds/alephone/screen-<tag>-*.bmp
void save_screens(const std::string& tag);
// source/video_3ds.cpp: the picture the CPU present would put in the framebuffer, by its own
// code, into a 240x400 rotated RGBA8 buffer; false for a surface the CPU present does not take
bool present_reference(u32* fb, const void* pixels, int pitch, int width, int height, uint32_t sdl_format);
}

namespace a1_c3d {
namespace back {

namespace {

constexpr int kScreenW = 400, kScreenH = 240;
constexpr int kSrcW = 640, kSrcH = 480;               // halved to 320x240, centred
constexpr int kOutW = kSrcW / 2, kOutH = kSrcH / 2;
constexpr int kBarW = (kScreenW - kOutW) / 2;
constexpr int kTexW = 512, kTexH = 256;               // present texture (design 9.1)
constexpr int kStagingPitchPx = kTexW;
constexpr size_t kStagingBytes = kStagingPitchPx * kTexH * sizeof(u32);   // 512x256: the whole texture
// What a present writes: rows 0-239, all 512 columns of them (picture, bars, and the unused
// columns 400-511 when a layout change clears them). Rows 240-255 are cleared and flushed once
// at init and never written again; the transfer still reads the whole 512x256 texture (a
// 512x240 transfer shifted the picture 16 rows in Azahar, PERF.md "Stage 10 P1"). 480 KiB.
constexpr size_t kStagingWrittenBytes = kStagingPitchPx * kScreenH * sizeof(u32);

// The screen target's framebuffer is RGBA8 (SDL_n3dsvideo.c: gfxInit(GSP_RGBA8_OES, ...));
// the target is 240x400, the LCD's native rotated shape (simple_tri/source/main.c).
constexpr u32 kTargetTransferFlags =
	GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) |
	GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGBA8) |
	GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO);
// Staging (linear, row-major, row 0 = image top) -> present texture (tiled), no format
// change, flipped (as textures are loaded from top-down images: tex3ds, sf2d); the quad then
// maps pixel y to v = y / 256 with y down.
constexpr u32 kUploadTransferFlags =
	GX_TRANSFER_FLIP_VERT(1) | GX_TRANSFER_OUT_TILED(1) | GX_TRANSFER_RAW_COPY(0) |
	GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGBA8) |
	GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO);

struct Vertex { float x, y, z, u, v; };

C3D_RenderTarget* s_target = nullptr;
C3D_Tex s_tex;
bool s_tex_ready = false;
DVLB_s* s_dvlb = nullptr;
shaderProgram_s s_program;
bool s_program_ready = false;
int s_uloc_projection = -1;
C3D_Mtx s_projection;
Vertex* s_vbo = nullptr;
u32* s_staging = nullptr;

unsigned s_present = 0;       // present counter (0 = none yet), the frame index of calibrate
int s_layout = -1;            // what the staging holds around the picture: 0 = 640x480 path, 1 = other
bool s_logged_native = false, s_logged_half = false, s_logged_other = false;

// present_diff self-check (critic P1 minor 1). Every N-th present the CPU present's picture
// is built into s_diff_ref and the framebuffer citro3d will transfer to is noted at
// C3D_FrameEnd (design 0.3: the destination is gfxGetFramebuffer() at C3D_FrameEnd); after the
// next C3D_FrameBegin(0) that frame, transfer included, has retired and the two are compared.
constexpr size_t kFbPixels = static_cast<size_t>(kScreenW) * kScreenH;
u32* s_diff_ref = nullptr;            // malloc'd 240x400 RGBA8, the CPU present's picture
const u32* s_diff_fb = nullptr;       // the checked present's framebuffer, compared at the next present
unsigned s_diff_present = 0;          // the checked present's number
unsigned s_diff_checks = 0, s_diff_bad = 0, s_diff_skipped = 0, s_diff_logged = 0;
unsigned s_diff_max[4] = {0, 0, 0, 0};   // R, G, B, A over every check

// Odd-surface test state.
uint64_t s_odd_last = 0;
bool s_odd_capture_pending = false;
unsigned s_odd_captures = 0;

constexpr uint64_t kTicksPerSecond = SYSCLOCK_ARM11;

inline u32 to_fb(u32 xrgb)
{
	return xrgb << 8;   // 0x00RRGGBB -> 0xRRGGBB00 (RGBA8; the LCD ignores A)
}

// The 2x2 block at the front of two source rows, averaged per channel (UHADD8); the
// rows then move on by two pixels. Same as video_3ds.cpp.
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

// 640x480 -> 320x240 centred in the 400x240 staging, black bars.
void fill_half(const u8* src, int pitch)
{
	if (s_layout != 0)
	{
		memset(s_staging, 0, kStagingWrittenBytes);
		s_layout = 0;
	}
	for (int y = 0; y < kOutH; ++y)
	{
		const u8* row = src + 2 * y * pitch;
		const u32* r0 = reinterpret_cast<const u32*>(row);
		const u32* r1 = reinterpret_cast<const u32*>(row + pitch);
		u32* out = s_staging + y * kStagingPitchPx + kBarW;
		for (int x = 0; x < kOutW; ++x)
			out[x] = to_fb(box(r0, r1));
	}
}

// Any other ARGB8888/XRGB8888 picture: 1:1, centre-cropped or letterboxed into 400x240.
void fill_other(const u8* src, int pitch, int w, int h)
{
	if (s_layout != 1 || w < kScreenW || h < kScreenH)
	{
		memset(s_staging, 0, kStagingWrittenBytes);
		s_layout = 1;
	}
	const int cw = w < kScreenW ? w : kScreenW, ch = h < kScreenH ? h : kScreenH;
	const int dx = (kScreenW - cw) / 2, dy = (kScreenH - ch) / 2;
	const int sx = (w - cw) / 2, sy = (h - ch) / 2;
	for (int y = 0; y < ch; ++y)
	{
		const u32* in = reinterpret_cast<const u32*>(src + (sy + y) * pitch) + sx;
		u32* out = s_staging + (dy + y) * kStagingPitchPx + dx;
		int x = 0;
		for (; x + 8 <= cw; x += 8)   // eight at a time: LDM/STM, one shift each
		{
			const u32 a0 = in[x], a1 = in[x + 1], a2 = in[x + 2], a3 = in[x + 3];
			const u32 a4 = in[x + 4], a5 = in[x + 5], a6 = in[x + 6], a7 = in[x + 7];
			out[x] = to_fb(a0); out[x + 1] = to_fb(a1); out[x + 2] = to_fb(a2); out[x + 3] = to_fb(a3);
			out[x + 4] = to_fb(a4); out[x + 5] = to_fb(a5); out[x + 6] = to_fb(a6); out[x + 7] = to_fb(a7);
		}
		for (; x < cw; ++x)
			out[x] = to_fb(in[x]);
	}
}

// The synthetic 300x200 RGB565 picture of debug-c3d "odd_surface": a gradient with a
// white frame and a diagonal, so cropping, centring and the format conversion show.
void make_odd_picture(std::vector<uint16_t>& px, int w, int h)
{
	px.assign(static_cast<size_t>(w) * h, 0);
	for (int y = 0; y < h; ++y)
		for (int x = 0; x < w; ++x)
		{
			unsigned r = x * 31 / (w - 1), g = y * 63 / (h - 1), b = 31 - r;
			if (x == 0 || y == 0 || x == w - 1 || y == h - 1 || x * h / w == y)
				r = 31, g = 63, b = 31;
			px[static_cast<size_t>(y) * w + x] = static_cast<uint16_t>((r << 11) | (g << 5) | b);
		}
}

// 20 ms of CPU, the "heavy frame" of alt_load.
void busy_wait_ms(int ms)
{
	const uint64_t end = svcGetSystemTick() + static_cast<uint64_t>(ms) * (kTicksPerSecond / 1000);
	while (svcGetSystemTick() < end) {}
}

// present_diff: the CPU present's picture for this present into s_diff_ref. False (the
// present is not checked) for a surface the CPU present does not take or without memory.
bool diff_prepare(const void* src, int pitch, int w, int h, uint32_t fmt)
{
	if (!s_diff_ref)
	{
		s_diff_ref = static_cast<u32*>(malloc(kFbPixels * sizeof(u32)));
		if (!s_diff_ref)
		{
			printf("[c3d] present_diff: no memory for the %lu KiB reference\n",
			       static_cast<unsigned long>(kFbPixels * sizeof(u32) / 1024));
			return false;
		}
	}
	return a1_3ds::present_reference(s_diff_ref, src, pitch, w, h, fmt);
}

// present_diff: s_diff_fb (what the GPU present put on screen) against s_diff_ref.
void diff_compare()
{
	// The GPU wrote it: drop any stale cached copy before reading (gspgpu.h:246).
	GSPGPU_InvalidateDataCache(s_diff_fb, kFbPixels * sizeof(u32));
	unsigned differ = 0, alpha_only = 0, mx[4] = {0, 0, 0, 0};
	size_t first = kFbPixels;
	for (size_t i = 0; i < kFbPixels; ++i)
	{
		const u32 g = s_diff_fb[i], c = s_diff_ref[i];
		if (g == c)
			continue;
		for (int ch = 0; ch < 4; ++ch)   // 0xRRGGBBAA
		{
			const int shift = 24 - 8 * ch;
			const int a = (g >> shift) & 0xFF, b = (c >> shift) & 0xFF;
			const unsigned d = static_cast<unsigned>(a > b ? a - b : b - a);
			if (d > mx[ch])
				mx[ch] = d;
		}
		if ((g ^ c) & 0xFFFFFF00u)
		{
			++differ;
			if (first == kFbPixels)
				first = i;
		}
		else
			++alpha_only;
	}
	++s_diff_checks;
	for (int ch = 0; ch < 4; ++ch)
		if (mx[ch] > s_diff_max[ch])
			s_diff_max[ch] = mx[ch];
	if (differ)
		++s_diff_bad;
	// Every mismatch is logged (up to 30 lines in all), a match only for the first 3 checks.
	if ((differ || s_diff_checks <= 3) && s_diff_logged < 30)
	{
		++s_diff_logged;
		printf("[c3d] present_diff: present %u: %u of %lu pixels differ in RGB (%u in A only), max |diff| "
		       "R %u G %u B %u A %u",
		       s_diff_present, differ, static_cast<unsigned long>(kFbPixels), alpha_only, mx[0], mx[1], mx[2],
		       mx[3]);
		if (differ)
			// framebuffer word i is screen pixel (i / 240, 239 - i % 240) (video_3ds.cpp)
			printf("; first at screen (%lu, %lu): gpu 0x%08lx cpu 0x%08lx",
			       static_cast<unsigned long>(first / kScreenH),
			       static_cast<unsigned long>(kScreenH - 1 - first % kScreenH),
			       static_cast<unsigned long>(s_diff_fb[first]), static_cast<unsigned long>(s_diff_ref[first]));
		printf("\n");
	}
	s_diff_fb = nullptr;
}

// test_vhalt 2 / 3: the fatal-error paths on a thread other than main.
void test_vhalt_thread(void* arg)
{
	if (reinterpret_cast<intptr_t>(arg) == 2)
		abort();   // on_abort -> error_screen -> a1_c3d_release_top
	vhalt("debug-c3d test_vhalt 3 (second thread)");   // SDL_Quit's gfxExit, then the error screen
}

} // namespace

bool present_init()
{
	s_staging = static_cast<u32*>(linearAlloc(kStagingBytes));
	s_vbo = static_cast<Vertex*>(linearAlloc(4 * sizeof(Vertex)));
	if (!s_staging || !s_vbo)
	{
		printf("[c3d] linearAlloc failed (staging %lu KiB)\n", static_cast<unsigned long>(kStagingBytes / 1024));
		return false;
	}
	memset(s_staging, 0, kStagingBytes);
	GSPGPU_FlushDataCache(s_staging, kStagingBytes);

	s_target = C3D_RenderTargetCreate(kScreenH, kScreenW, GPU_RB_RGBA8, C3D_DEPTHTYPE(-1));
	if (!s_target)
	{
		printf("[c3d] C3D_RenderTargetCreate failed\n");
		return false;
	}
	C3D_RenderTargetSetOutput(s_target, GFX_TOP, GFX_LEFT, kTargetTransferFlags);

	if (!C3D_TexInitVRAM(&s_tex, kTexW, kTexH, GPU_RGBA8))
	{
		printf("[c3d] C3D_TexInitVRAM %dx%d failed\n", kTexW, kTexH);
		return false;
	}
	s_tex_ready = true;
	C3D_TexSetFilter(&s_tex, GPU_NEAREST, GPU_NEAREST);
	C3D_TexSetWrap(&s_tex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);

	s_dvlb = DVLB_ParseFile(const_cast<u32*>(reinterpret_cast<const u32*>(a1_c3d_shaders_shbin)),
	                        a1_c3d_shaders_shbin_size);
	if (!s_dvlb)
	{
		printf("[c3d] DVLB_ParseFile failed\n");
		return false;
	}
	shaderProgramInit(&s_program);
	s_program_ready = true;
	shaderProgramSetVsh(&s_program, &s_dvlb->DVLE[0]);
	C3D_BindProgram(&s_program);
	s_uloc_projection = shaderInstanceGetUniformLocation(s_program.vertexShader, "projection");

	C3D_AttrInfo* attr = C3D_GetAttrInfo();
	AttrInfo_Init(attr);
	AttrInfo_AddLoader(attr, 0, GPU_FLOAT, 3);   // v0 = position (pixels)
	AttrInfo_AddLoader(attr, 1, GPU_FLOAT, 2);   // v1 = texcoord

	// Pixels, x right, y down (proctex/source/main.c:43); the quad covers the target
	// exactly and maps texel (x, y) to pixel (x, y).
	Mtx_OrthoTilt(&s_projection, 0.0f, kScreenW, kScreenH, 0.0f, 0.0f, 1.0f, true);
	const float u1 = static_cast<float>(kScreenW) / kTexW, v1 = static_cast<float>(kScreenH) / kTexH;
	s_vbo[0] = Vertex{0.0f, 0.0f, 0.5f, 0.0f, 0.0f};
	s_vbo[1] = Vertex{0.0f, static_cast<float>(kScreenH), 0.5f, 0.0f, v1};
	s_vbo[2] = Vertex{static_cast<float>(kScreenW), 0.0f, 0.5f, u1, 0.0f};
	s_vbo[3] = Vertex{static_cast<float>(kScreenW), static_cast<float>(kScreenH), 0.5f, u1, v1};
	GSPGPU_FlushDataCache(s_vbo, 4 * sizeof(Vertex));
	C3D_BufInfo* buf = C3D_GetBufInfo();
	BufInfo_Init(buf);
	BufInfo_Add(buf, s_vbo, sizeof(Vertex), 2, 0x10);

	C3D_TexBind(0, &s_tex);
	C3D_TexEnv* env = C3D_GetTexEnv(0);
	C3D_TexEnvInit(env);
	C3D_TexEnvSrc(env, C3D_Both, GPU_TEXTURE0, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
	C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
	return true;
}

void present_detach()
{
	if (s_target)
		C3D_RenderTargetDetachOutput(s_target);
}

void present_free()
{
	// The target is destroyed by C3D_Fini (every target of the context is).
	s_target = nullptr;
	if (s_program_ready)
		shaderProgramFree(&s_program);
	s_program_ready = false;
	if (s_dvlb)
		DVLB_Free(s_dvlb);
	s_dvlb = nullptr;
	if (s_tex_ready)
		C3D_TexDelete(&s_tex);
	s_tex_ready = false;
	if (s_vbo)
		linearFree(s_vbo);
	s_vbo = nullptr;
	if (s_staging)
		linearFree(s_staging);
	s_staging = nullptr;
	if (s_diff_checks || s_diff_skipped)
		printf("[c3d] present_diff final: %u checks, %u with RGB differences, %u presents not checkable (no CPU "
		       "path for the surface); max |diff| R %u G %u B %u A %u\n",
		       s_diff_checks, s_diff_bad, s_diff_skipped, s_diff_max[0], s_diff_max[1], s_diff_max[2],
		       s_diff_max[3]);
	free(s_diff_ref);
	s_diff_ref = nullptr;
	s_diff_fb = nullptr;
}

uint32_t present_counter()
{
	return s_present;
}

} // namespace back
} // namespace a1_c3d

bool a1_c3d_present(const void* pixels, int pitch, int width, int height, uint32_t sdl_format)
{
	using namespace a1_c3d;
	using namespace a1_c3d::back;
	const Config& cfg = config();
	if (cfg.test_vhalt)
	{
		// Also with "present cpu": the same forced vhalt() without citro3d is the baseline of the
		// error screen.
		static const uint64_t first = svcGetSystemTick();
		static bool thread_started = false;
		if (!thread_started && svcGetSystemTick() - first > 20 * kTicksPerSecond)
		{
			printf("[c3d] test_vhalt %d: calling %s (citro3d %s)\n", cfg.test_vhalt,
			       cfg.test_vhalt == 1 ? "vhalt()" : cfg.test_vhalt == 2 ? "abort() on a second thread"
			                                                             : "vhalt() on a second thread",
			       is_up() ? "up" : "not in use");
			fflush(stdout);
			if (cfg.test_vhalt == 1)
				vhalt("debug-c3d test_vhalt");   // never returns
			// 2, 3: the error screen from a thread other than main (critic P1 minor 4). Lower
			// priority than the main thread, same core: it runs while the main thread sleeps
			// between frames, i.e. possibly between two C3D frames, and the main thread keeps
			// presenting until it serves the release request.
			thread_started = true;
			if (!threadCreate(test_vhalt_thread, reinterpret_cast<void*>(cfg.test_vhalt), 32 * 1024, 0x3f, -2, true))
				printf("[c3d] test_vhalt: threadCreate failed\n");
		}
	}
	if (!ensure_up())
		return false;
	const unsigned n = ++s_present;
	const uint64_t now = svcGetSystemTick();
	count_present();

	if (cfg.alt_load && (n & 1))
		busy_wait_ms(20);

	// The picture to show: the engine's, or (odd_surface, once a second) a 300x200 RGB565 one.
	std::vector<uint16_t> odd;
	std::vector<u32> converted;
	const void* src = pixels;
	int src_pitch = pitch, src_w = width, src_h = height;
	uint32_t src_fmt = sdl_format;
	bool odd_now = false;
	if (cfg.odd_surface && now - s_odd_last >= kTicksPerSecond)
	{
		s_odd_last = now;
		make_odd_picture(odd, 300, 200);
		src = odd.data();
		src_pitch = 300 * 2;
		src_w = 300;
		src_h = 200;
		src_fmt = SDL_PIXELFORMAT_RGB565;
		odd_now = true;
		count_odd_surface();
	}
	if (src_fmt != SDL_PIXELFORMAT_RGB888 && src_fmt != SDL_PIXELFORMAT_ARGB8888)
	{
		converted.resize(static_cast<size_t>(src_w) * src_h);
		if (SDL_ConvertPixels(src_w, src_h, src_fmt, src, src_pitch, SDL_PIXELFORMAT_ARGB8888, converted.data(),
		                      src_w * 4) != 0)
		{
			printf("[c3d] SDL_ConvertPixels failed (%s): %s\n", SDL_GetPixelFormatName(src_fmt), SDL_GetError());
			converted.assign(converted.size(), 0);
		}
		if (!s_logged_other || odd_now)
		{
			if (!s_logged_other)
				printf("[c3d] present: %dx%d %s converted and letterboxed into 400x240\n", src_w, src_h,
				       SDL_GetPixelFormatName(src_fmt));
			s_logged_other = true;
		}
		src = converted.data();
		src_pitch = src_w * 4;
	}

	// 1. The previous C3D frame has retired when this returns.
	if (!frame_begin())
	{
		printf("[c3d] C3D_FrameBegin(0) returned false (already inside a frame): giving the top screen back\n");
		release_and_fini();
		return false;
	}

	// present_diff: the previous checked present has reached its framebuffer now.
	if (s_diff_fb)
		diff_compare();
	const bool diff_now = cfg.present_diff && !cfg.calibrate && n % cfg.present_diff == 0;
	bool diff_ready = false;
	if (diff_now)
	{
		diff_ready = diff_prepare(src, src_pitch, src_w, src_h, src_fmt);
		if (!diff_ready)
			++s_diff_skipped;
	}

	// 2. Staging, written only now that the previous transfer has read the old one.
	if (src_w == kSrcW && src_h == kSrcH)
	{
		if (!s_logged_half)
		{
			s_logged_half = true;
			printf("[c3d] present: 640x480 -> 320x240 (UHADD8 box, << 8) centred, GPU upload\n");
		}
		fill_half(static_cast<const u8*>(src), src_pitch);
	}
	else
	{
		if (src_w == kScreenW && src_h == kScreenH && !s_logged_native)
		{
			s_logged_native = true;
			printf("[c3d] present: 400x240 -> staging 1:1, GPU upload\n");
		}
		fill_other(static_cast<const u8*>(src), src_pitch, src_w, src_h);
	}

	// 3. Linear -> tiled into the present texture: the first GX operation of the frame.
	if (!gx_op([&] {
		    C3D_SyncDisplayTransfer(s_staging, GX_BUFFER_DIM(kTexW, kTexH), static_cast<u32*>(s_tex.data),
		                            GX_BUFFER_DIM(kTexW, kTexH), kUploadTransferFlags);
	    }))
		printf("[c3d] GX budget: present upload refused\n");

	// 4. One quad into the screen target (or, calibrating, a flat frame-indexed colour).
	C3D_CullFace(GPU_CULL_NONE);
	C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
	C3D_StencilTest(false, GPU_ALWAYS, 0, 0xFF, 0);
	C3D_AlphaTest(false, GPU_ALWAYS, 0);
	C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
	if (cfg.calibrate)
	{
		const u32 colour = ((n & 0xFF) << 24) | (((n >> 8) & 0xFF) << 16) | (0xA5u << 8) | 0xFF;
		gx_op([&] { C3D_RenderTargetClear(s_target, C3D_CLEAR_COLOR, colour, 0); });
		C3D_FrameDrawOn(s_target);
	}
	else
	{
		C3D_FrameDrawOn(s_target);
		C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, s_uloc_projection, &s_projection);
		C3D_DrawArrays(GPU_TRIANGLE_STRIP, 0, 4);
	}

	// 5. The transfer's destination is taken at C3D_FrameEnd, and the previous queue (with
	// its swap request) finished in step 1: if that swap has not taken effect yet (next
	// VBlank), the buffer gfxGetFramebuffer() would return is still on screen. Wait.
	GSPGPU_FlushDataCache(s_staging, kStagingWrittenBytes);   // critic P1 minor 5: rows 0-239 only
	if (gspIsPresentPending(GSP_SCREEN_TOP))
		gspWaitForVBlank();
	if (s_odd_capture_pending)
	{
		// The previous present was the odd picture; its swap has taken effect (above):
		// it is what the display shows now.
		s_odd_capture_pending = false;
		printf("[c3d] odd_surface: capturing the displayed odd picture (present %u)\n", n - 1);
		a1_3ds::save_screens("c3d-odd" + std::to_string(++s_odd_captures));
	}
	if (diff_ready)
	{
		// The transfer's destination: taken by citro3d at the C3D_FrameEnd below.
		u16 fb_w = 0, fb_h = 0;
		const u32* fb = reinterpret_cast<const u32*>(gfxGetFramebuffer(GFX_TOP, GFX_LEFT, &fb_w, &fb_h));
		if (fb_w == kScreenH && fb_h == kScreenW)
		{
			s_diff_fb = fb;
			s_diff_present = n;
		}
		else
			++s_diff_skipped;
	}
	frame_end(1);

	if (odd_now && s_odd_captures < 2)
		s_odd_capture_pending = true;
	bool burst = false;
	for (unsigned first : {120u, 600u, 1200u, 1800u})
		burst = burst || (n >= first && n < first + 6);
	if (cfg.calibrate && burst)
	{
		// frame N or N-1 must be on the display (design 5.1 test); the log line says N.
		printf("[c3d] calibrate: capture after present %u (colour R=%u G=%u)\n", n, n & 0xFF, (n >> 8) & 0xFF);
		a1_3ds::save_screens("c3d-cal-" + std::to_string(n));
	}
	stats_tick();
	return true;
}
