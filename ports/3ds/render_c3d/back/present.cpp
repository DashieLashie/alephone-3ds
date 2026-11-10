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

	debug-c3d "two_frame 1" (plan B-a, design 5.7.4): the same steps as two C3D frames. Frame U:
	steps 1-3 (fill, flush, upload transfer), C3D_FrameEnd(0): nothing drawn, no framebuffer
	transfer. Frame D: C3D_FrameBegin(0) (it waits for U: zone c3d_upload), steps 4-5.

	The zero-copy present (plan B-b, design 5.7, debug-c3d "zero_copy 1", the default): a 400x240
	game surface (HUD + Map) lives in a 512 KiB linear buffer made by main_surface_pixels() (the
	engine's main_surface, screen.cpp, PATCHES.md 55). Always two frames: U has no step 2 (no
	fill): the surface itself is flushed (rows 0-239) and is the source of the upload transfer;
	D draws the texture with three one-channel masked draws that put the surface's 0xXXRRGGBB
	words (texture: R = X, G = surface R, B = surface G, A = surface B) back in order (5.7.3).
	Everything else (640x480, odd surfaces, any other surface) takes the staging path above.

	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; either version 3 of the License, or
	(at your option) any later version.
*/
#include "context.h"
#include "overlay.h"
#include "shade.h"
#include "stereo.h"
#include "view_shared.h"

#include "../core/noise.h"
#include "../core/stereo.h"

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

// back/context.cpp's wrap of C3D_TexBind: the composite binds the view and overlay textures through the real one, so the
// record keeps the present's texture (M6 specification, critic round 1 minor 10).
extern "C" void __real_C3D_TexBind(int unitId, C3D_Tex* tex);

#ifdef A1_PROFILE
#include "a1_3ds_profile.h"
#endif

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
C3D_RenderTarget* s_target_r = nullptr;   // M9: the right eye, linked to GFX_TOP / GFX_RIGHT; drawn on stereo frames only
C3D_Tex s_tex;
bool s_tex_ready = false;
DVLB_s* s_dvlb = nullptr;
shaderProgram_s s_program;
bool s_program_ready = false;
int s_uloc_projection = -1;
int s_uloc_t1map = -1;        // M6b: texcoord1's map (present.v.pica)
C3D_Mtx s_projection;
Vertex* s_vbo = nullptr;
u32* s_staging = nullptr;
// M6: the composite frame's quads (the view, then one per overlay band), rewritten each composite after its frame_begin.
// M6b: then the fade passes' row ranges (the bands and the gaps between them, view texcoords) and the static's noise quad.
constexpr int kRangeQuad0 = 1 + kMaxBands;
constexpr int kMaxRanges = 2 * kMaxBands + 1;
constexpr int kNoiseQuad = kRangeQuad0 + kMaxRanges;
constexpr int kCompositeQuads = kNoiseQuad + 1;
Vertex* s_cvbo = nullptr;
C3D_BufInfo s_cbuf, s_saved_cbuf;

// M6 counters ("[c3d] composite" in the stats and at exit).
struct CompositeCounters
{
	unsigned composites = 0, band_quads = 0, not_checkable = 0, audits = 0, leaks = 0;
	unsigned checks = 0, check_bad = 0, check_big = 0, check_skipped = 0;
	unsigned check_max[3] = {0, 0, 0};
	double check_min_within = 100.0;
	// M6b: part 1 quads, part 2 MIN/MAX passes, part 3 noise quads, the state restores after parts 2-3; direct_checks of
	// fade frames, bad ones (within_2 < 99.9 % or max > 4), their worst figures, static frames not checked, and the
	// checked pixels where SW's own curve differs from the reference (dodge's wrap, dodge/burn's int32 overflow)
	unsigned fade_quads = 0, fade_passes = 0, fade_noise = 0, fade_restores = 0;
	unsigned fade_checks = 0, fade_bad = 0, fade_max = 0, fade_unchecked = 0;
	unsigned long long fade_quirk_px = 0;
	double fade_min_within2 = 100.0;
	// M9: composites with two eyes; stereo_check: right framebuffer against the left, bad ones (RGB differences, 2D mode or
	// one buffer for both eyes), RGB-different pixels over all checks, alpha-only differences, checks not made
	unsigned stereo_composites = 0, stereo_checks = 0, stereo_check_bad = 0, stereo_check_skipped = 0;
	unsigned long long stereo_diff_px = 0, stereo_alpha_px = 0;
} s_cn;
// M9: this frame's view was drawn for two eyes (stereo_frame_set), consumed by the composite.
bool s_stereo_set = false;
// M6b: this frame's fade (fade_frame_set), consumed by the composite.
FadeFrame s_fade;
bool s_fade_set = false;
uint32_t* s_check_ref = nullptr;
u32 s_vram_free_at_exit = 0;           // vramSpaceFree() noted by present_detach (G3-a)   // malloc'd 400x240 XRGB: direct_check's reference before present_reference

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
unsigned s_diff_checks = 0, s_diff_bad = 0, s_diff_logged = 0;
unsigned s_diff_no_cpu = 0;           // not checkable: no CPU present for the surface
unsigned s_diff_fb_size = 0;          // not checkable: unexpected framebuffer size
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

// test_vhalt 2 / 3 / 4: the fatal-error paths on a thread other than main.
void test_vhalt_thread(void* arg)
{
	const intptr_t mode = reinterpret_cast<intptr_t>(arg);
	if (mode == 2)
		abort();   // on_abort -> error_screen -> a1_c3d_release_top
	if (mode == 4)
	{
		// error_screen() itself, without die_with_message's stdio first (critic P1 review
		// round 1, finding 6): a1_c3d_release_top() off the main thread and top_free.
		a1_3ds::error_screen("debug-c3d test_vhalt 4", "error_screen() called on a second thread.");
		_Exit(1);
	}
	vhalt("debug-c3d test_vhalt 3 (second thread)");   // SDL_Quit's gfxExit, then the error screen
}

// Which picture the present texture holds, i.e. how TEV stage 0 reads it. Plain: the texel as it is
// (the staging is filled with xrgb << 8). ToRed / ToGreen / ToBlue: the three draws that reorder the
// zero-copy surface's words (design 5.7.3), each writing one colour channel.
enum class Swizzle { Plain, ToRed, ToGreen, ToBlue };

// What the present texture holds now: false = the staging's words (draw Plain), true = the zero-copy
// surface's words (three reorder draws). Set when an upload transfer was actually issued; frame D and
// present_close_frame() draw accordingly (design 5.7.5).
bool s_tex_reordered = false;

// The zero-copy surface (design 5.7.2), made once by a1_3ds::main_surface_pixels(), never freed.
u32* s_zc_surface = nullptr;
bool s_zc_tried = false;
constexpr int kZcPitch = kStagingPitchPx * sizeof(u32);   // 2048 bytes

// TEV stage 0 and the colour write mask, set in full before every draw of the present texture
// (design 5.7.3, critic round 1 finding 1): the texture's reading may differ from the previous
// draw's, so nothing is left to what an earlier draw or present_init set. C3D_GetTexEnv() marks
// the stage dirty (texenv.o C3D_GetTexEnv+0x8-0x24: flag bit 26 + id; NULL if the context is not
// active, which cannot happen here: every caller runs between C3D_Init and C3D_Fini).
void set_present_draw(Swizzle swizzle)
{
	C3D_TexEnv* env = C3D_GetTexEnv(0);
	C3D_TexEnvInit(env);
	switch (swizzle)
	{
		case Swizzle::Plain:
			// RGB and alpha: texture 0 as it is (REPLACE of source 1; sources 2 and 3 unused).
			C3D_TexEnvSrc(env, C3D_Both, GPU_TEXTURE0, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
			C3D_TexEnvOpRgb(env, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_COLOR);
			C3D_TexEnvOpAlpha(env, GPU_TEVOP_A_SRC_ALPHA, GPU_TEVOP_A_SRC_ALPHA, GPU_TEVOP_A_SRC_ALPHA);
			C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
			C3D_TexEnvColor(env, 0xFFFFFFFF);   // C3D_TexEnvInit's value; no source reads it
			C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
			break;
		// The reorder draws (design 5.7.3). The texel's R is the surface's X, G its R, B its G, A its B.
		// Each draw copies one texture channel into the three RGB inputs (REPLACE of source 1 with a
		// channel operand: enums.h:331-346, GPU_TEVOP_RGB_SRC_G = 0x08, _SRC_B = 0x0C, _SRC_ALPHA = 0x02)
		// and the colour write mask (enums.h:224-230, effect.h:10 C3D_DepthTest) lets one channel through.
		// UNVERIFIED on hardware: that the operands broadcast the channel and that the PICA honours a
		// one-channel mask as a read-modify-write (design 5.7.10 items 2 and 4); present_diff settles it.
		case Swizzle::ToRed:
			// red = texel G (surface R); alpha written as 0 from the constant colour, as to_fb() does.
			C3D_TexEnvSrc(env, C3D_RGB, GPU_TEXTURE0, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
			C3D_TexEnvOpRgb(env, GPU_TEVOP_RGB_SRC_G, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_COLOR);
			C3D_TexEnvSrc(env, C3D_Alpha, GPU_CONSTANT, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
			C3D_TexEnvOpAlpha(env, GPU_TEVOP_A_SRC_ALPHA, GPU_TEVOP_A_SRC_ALPHA, GPU_TEVOP_A_SRC_ALPHA);
			C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
			C3D_TexEnvColor(env, 0);
			C3D_DepthTest(false, GPU_ALWAYS, static_cast<GPU_WRITEMASK>(GPU_WRITE_RED | GPU_WRITE_ALPHA));
			break;
		case Swizzle::ToGreen:
			// green = texel B (surface G); alpha masked off (its source is still set in full).
			C3D_TexEnvSrc(env, C3D_RGB, GPU_TEXTURE0, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
			C3D_TexEnvOpRgb(env, GPU_TEVOP_RGB_SRC_B, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_COLOR);
			C3D_TexEnvSrc(env, C3D_Alpha, GPU_CONSTANT, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
			C3D_TexEnvOpAlpha(env, GPU_TEVOP_A_SRC_ALPHA, GPU_TEVOP_A_SRC_ALPHA, GPU_TEVOP_A_SRC_ALPHA);
			C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
			C3D_TexEnvColor(env, 0);
			C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_GREEN);
			break;
		case Swizzle::ToBlue:
			// blue = texel A (surface B); alpha masked off.
			C3D_TexEnvSrc(env, C3D_RGB, GPU_TEXTURE0, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
			C3D_TexEnvOpRgb(env, GPU_TEVOP_RGB_SRC_ALPHA, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_COLOR);
			C3D_TexEnvSrc(env, C3D_Alpha, GPU_CONSTANT, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
			C3D_TexEnvOpAlpha(env, GPU_TEVOP_A_SRC_ALPHA, GPU_TEVOP_A_SRC_ALPHA, GPU_TEVOP_A_SRC_ALPHA);
			C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
			C3D_TexEnvColor(env, 0);
			C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_BLUE);
			break;
	}
}

// The GPU state of a present and its quad into the screen target (step 4 below; also the
// close_frame forced frame): one Plain draw, or, for a texture holding the zero-copy surface's words
// (`reordered`), the three channel draws of design 5.7.3, each with its stage 0 and mask set in full.
void draw_present_quad(bool reordered)
{
	C3D_CullFace(GPU_CULL_NONE);
	C3D_StencilTest(false, GPU_ALWAYS, 0, 0xFF, 0);
	C3D_AlphaTest(false, GPU_ALWAYS, 0);
	C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
	C3D_FrameDrawOn(s_target);
	C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, s_uloc_projection, &s_projection);
	if (!reordered)
	{
		set_present_draw(Swizzle::Plain);
		C3D_DrawArrays(GPU_TRIANGLE_STRIP, 0, 4);
		return;
	}
	for (Swizzle channel : {Swizzle::ToRed, Swizzle::ToGreen, Swizzle::ToBlue})
	{
		set_present_draw(channel);
		C3D_DrawArrays(GPU_TRIANGLE_STRIP, 0, 4);
	}
}

// frame_begin() for a present, and what a failed one means for the caller (the same handling for
// the one frame of a P1 present and for each of the two of two_frame).
enum class Begin { Ok, ReturnTrue, ReturnFalse };

Begin begin_present_frame(FrameZone zone)
{
	const FrameStart fs = frame_begin(zone);
	if (fs == FrameStart::Abandoned)
		return Begin::ReturnTrue;   // citro3d abandoned (top surrendered): draw nothing, and no CPU present (5.6.1 b)
	if (fs != FrameStart::Ok)
	{
		printf("[c3d] C3D_FrameBegin(0) returned false (already inside a frame): giving the top screen back\n");
		release_and_fini();
		return Begin::ReturnFalse;
	}
	return Begin::Ok;
}

// M6b: a colour for TEV stage 0's constant (texenv.h:85) and for C3D_BlendingColor (effect.h:8): 0xAABBGGRR, R in the
// low byte, the order back/view.cpp's tev_constant settled for the TEV constant in M1. For the blend colour the same
// order is UNVERIFIED (C3D_BlendingColor has no doc; M4 used greys only): the Fade Test's red flash settles it (a swap
// shows blue and fails direct_check).
inline u32 abgr(unsigned r, unsigned g, unsigned b)
{
	return 0xFF000000u | (b << 16) | (g << 8) | r;
}

// M6b part 1 (design 7, M6b "The mechanism"): after the overlay bands, one quad over the composited picture,
// dst' = B + A * dst per channel: TEV stage 0 = REPLACE of the constant B, the blend ONE, CONSTANT_COLOR with A as the
// blend colour (enums.h:237-261), dst alpha kept (ZERO, ONE). It redraws the view quad's vertices 0-3 (the texcoords
// are not read). The restore: the blend colour to 0, stage 0 back to the present's (the caller restores the blend).
void draw_fade_affine(const FadeFit& fit)
{
	C3D_TexEnv* env = C3D_GetTexEnv(0);
	C3D_TexEnvInit(env);
	C3D_TexEnvSrc(env, C3D_Both, GPU_CONSTANT, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
	C3D_TexEnvOpRgb(env, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_COLOR);
	C3D_TexEnvOpAlpha(env, GPU_TEVOP_A_SRC_ALPHA, GPU_TEVOP_A_SRC_ALPHA, GPU_TEVOP_A_SRC_ALPHA);
	C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
	C3D_TexEnvColor(env, abgr(fit.ch[0].b8, fit.ch[1].b8, fit.ch[2].b8));
	C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_CONSTANT_COLOR, GPU_ZERO, GPU_ONE);
	C3D_BlendingColor(abgr(fit.ch[0].a8, fit.ch[1].a8, fit.ch[2].a8));
	C3D_DrawArrays(GPU_TRIANGLE_STRIP, 0, 4);
	C3D_BlendingColor(0);
	set_present_draw(Swizzle::Plain);
	++s_cn.fade_quads;
}

// Part 1's quad changes nothing for the identity (A = 255, B = 0 on every channel): not drawn.
bool fade_affine_is_identity(const FadeFit& fit)
{
	for (const FadeChannel& c : fit.ch)
		if (c.a8 != 255 || c.b8 != 0)
			return false;
	return true;
}

// M6b part 2: a row range of the fade passes, inside an overlay band or not.
struct FadeRange
{
	int y0, y1;
	bool band;
};

// The screen's rows as the bands (normalised: sorted, disjoint) and the gaps between them.
int fade_ranges(const Band* bands, int nb, FadeRange* out)
{
	int n = 0, y = 0;
	for (int i = 0; i < nb; ++i)
	{
		if (bands[i].y0 > y)
			out[n++] = FadeRange{y, bands[i].y0, false};
		out[n++] = FadeRange{bands[i].y0, bands[i].y1, true};
		y = bands[i].y1;
	}
	if (y < kScreenH)
		out[n++] = FadeRange{y, kScreenH, false};
	return n;
}

// Part 2's stage 0: the composited pixel rebuilt from the textures, as SW composites before apply_gamma. In a band
// MULTIPLY_ADD(view, 1 - overlay alpha, overlay) (enums.h:373; the overlay is premultiplied, M6's CPU composite);
// outside the bands the view alone, and no stage reads unit 1 (its texture holds stale rows there: round 3, minor 4).
void set_fade_stage0(bool band)
{
	C3D_TexEnv* env = C3D_GetTexEnv(0);
	C3D_TexEnvInit(env);
	if (band)
	{
		C3D_TexEnvSrc(env, C3D_RGB, GPU_TEXTURE0, GPU_TEXTURE1, GPU_TEXTURE1);
		C3D_TexEnvOpRgb(env, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_ONE_MINUS_SRC_ALPHA, GPU_TEVOP_RGB_SRC_COLOR);
		C3D_TexEnvFunc(env, C3D_RGB, GPU_MULTIPLY_ADD);
	}
	else
	{
		C3D_TexEnvSrc(env, C3D_RGB, GPU_TEXTURE0, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
		C3D_TexEnvOpRgb(env, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_COLOR);
		C3D_TexEnvFunc(env, C3D_RGB, GPU_REPLACE);
	}
	C3D_TexEnvSrc(env, C3D_Alpha, GPU_TEXTURE0, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
	C3D_TexEnvOpAlpha(env, GPU_TEVOP_A_SRC_ALPHA, GPU_TEVOP_A_SRC_ALPHA, GPU_TEVOP_A_SRC_ALPHA);
	C3D_TexEnvFunc(env, C3D_Alpha, GPU_REPLACE);
}

// One TEV stage i >= 1: RGB = func(sources), alpha passed on, constant `colour`.
void set_fade_stage(int i, GPU_TEVSRC s0, GPU_TEVSRC s1, GPU_TEVOP_RGB o0, GPU_COMBINEFUNC func, u32 colour)
{
	C3D_TexEnv* env = C3D_GetTexEnv(i);
	C3D_TexEnvInit(env);   // alpha: REPLACE of PREVIOUS
	C3D_TexEnvSrc(env, C3D_RGB, s0, s1, GPU_PRIMARY_COLOR);
	C3D_TexEnvOpRgb(env, o0, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_COLOR);
	C3D_TexEnvFunc(env, C3D_RGB, func);
	C3D_TexEnvColor(env, colour);
}

// Part 2's stages 1-3 of the MIN (`min_pass`) or MAX pass: g = sigma * (A * C + B) in two stages (each stage has one
// constant, texenv.h:85), then K - g as SUBTRACT(K, g), or as ADD(1 - g, K - 1) when a channel's K is above 1
// (pass_ok made the form valid for all three; enums.h:365-374).
void set_fade_terms(const FadeFit& fit, bool min_pass)
{
	unsigned sa[3], sb[3], k[3];
	bool add = false;
	for (int ch = 0; ch < 3; ++ch)
	{
		const FadeChannel& c = fit.ch[ch];
		const bool s = min_pass ? c.s1 : c.s2;
		sa[ch] = s ? c.a8 : 0;
		sb[ch] = s ? c.b8 : 0;
		k[ch] = min_pass ? c.k1 : c.k2;
		add = add || k[ch] > 255;
	}
	set_fade_stage(1, GPU_PREVIOUS, GPU_CONSTANT, GPU_TEVOP_RGB_SRC_COLOR, GPU_MODULATE, abgr(sa[0], sa[1], sa[2]));
	set_fade_stage(2, GPU_PREVIOUS, GPU_CONSTANT, GPU_TEVOP_RGB_SRC_COLOR, GPU_ADD, abgr(sb[0], sb[1], sb[2]));
	if (add)
		set_fade_stage(3, GPU_PREVIOUS, GPU_CONSTANT, GPU_TEVOP_RGB_ONE_MINUS_SRC_COLOR, GPU_ADD,
		               abgr(k[0] - 255, k[1] - 255, k[2] - 255));
	else
		set_fade_stage(3, GPU_CONSTANT, GPU_PREVIOUS, GPU_TEVOP_RGB_SRC_COLOR, GPU_SUBTRACT, abgr(k[0], k[1], k[2]));
}

// After the last fade pass (design 7, M6b "State after parts 2 and 3"): stages 1-3 back to the passthrough the present
// and the world draws assume, unit 1 unbound as shade does it (or the LUT back, the M3 fallback), the alpha test off,
// the plain blend, the blend colour 0, stage 0 the present's. The program and the flip uniform stay.
void fade_restore(bool unit1)
{
	for (int i = 1; i <= 3; ++i)
		C3D_TexEnvInit(C3D_GetTexEnv(i));   // C3D_GetTexEnv marks the stage dirty
	if (unit1 && !shade_unbind_unit1())
	{
		if (void* lut = shade_lut_bound())
			__real_C3D_TexBind(1, static_cast<C3D_Tex*>(lut));
	}
	C3D_AlphaTest(false, GPU_ALWAYS, 0);
	C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
	C3D_BlendingColor(0);
	set_present_draw(Swizzle::Plain);
	++s_cn.fade_restores;
}

// M6b part 2 (design 7, M6b "The passes"): part 1's quad for g (when not the identity), then the MIN pass
// (dst = min(dst, K1 - s1 g)) and the MAX pass (dst = max(dst, K2 - s2 g)) where a channel needs them, each over the
// row ranges (quads kRangeQuad0..), blend MIN / MAX with factors ONE, ONE (whether PICA applies factors there cannot
// matter), alpha kept (ZERO, ONE).
void draw_fade_fold(const FadeFit& fit, C3D_Tex* vtex, C3D_Tex* otex, const FadeRange* ranges, int nr, int nb,
                    bool flip)
{
	if (!fade_affine_is_identity(fit))
		draw_fade_affine(fit);
	bool need[2] = {false, false};
	for (const FadeChannel& c : fit.ch)
	{
		need[0] = need[0] || c.s1 || c.k1 != 255;
		need[1] = need[1] || c.s2 || c.k2 != 0;
	}
	__real_C3D_TexBind(0, vtex);
	const bool unit1 = nb > 0;
	if (unit1)
	{
		__real_C3D_TexBind(1, otex);
		shade_check_bound(otex);   // the first bind of unit 1 of the run decides the context layout, as for the LUT
	}
	C3D_FVUnifSet(GPU_VERTEX_SHADER, s_uloc_t1map, flip ? 1.0f : 0.0f, flip ? -1.0f : 1.0f, 0.0f, 0.0f);
	for (int pass = 0; pass < 2; ++pass)
	{
		if (!need[pass])
			continue;
		set_fade_terms(fit, pass == 0);
		C3D_AlphaBlend(pass == 0 ? GPU_BLEND_MIN : GPU_BLEND_MAX, GPU_BLEND_ADD, GPU_ONE, GPU_ONE, GPU_ZERO, GPU_ONE);
		for (int i = 0; i < nr; ++i)
		{
			set_fade_stage0(ranges[i].band);
			C3D_DrawArrays(GPU_TRIANGLE_STRIP, 4 * (kRangeQuad0 + i), 4);
		}
		++s_cn.fade_passes;
	}
	fade_restore(unit1);
}

// M6b part 3 (design 7, M6b "Part 3"): part 1's quad for the base line (when not the identity), then M5's noise
// texture over the screen at a new offset each frame (quad kNoiseQuad, GPU_REPEAT), REPLACE of the texel, the alpha
// test GEQUAL noise_ref(65535 - mask) (M5's density rule: about mask/65536 of the pixels), blend ONE, ZERO.
void draw_fade_static(const FadeFit& fit, C3D_Tex* noise)
{
	if (!fade_affine_is_identity(fit))
		draw_fade_affine(fit);
	__real_C3D_TexBind(0, noise);
	set_present_draw(Swizzle::Plain);   // REPLACE of texture 0, RGB and alpha
	// ONE, ZERO set here: the blend before is part 1's or the bands' (critic, M6b code review minor 1).
	C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ZERO, GPU_ONE);
	C3D_AlphaTest(true, GPU_GEQUAL, static_cast<int>(noise_ref(static_cast<uint16_t>(65535u - fit.mask))));
	C3D_DrawArrays(GPU_TRIANGLE_STRIP, 4 * kNoiseQuad, 4);
	++s_cn.fade_noise;
	fade_restore(false);
}

// M6 direct_check: the composite's framebuffer against the read-back view with the overlay's bands composited on the CPU
// (premultiplied over: c = ov + view * (255 - A) / 255, rounded to nearest), through the CPU present's own conversion
// (present_reference). `fb` is the framebuffer the composite's transfer wrote (retired).
// M6b: on a fade frame (`fade`) the reference then goes through the frame's curves, ref[c][x] (= curve >> 8, exactly
// SW's apply_gamma), and the frame is judged by the accepted gate: within_2 >= 99.9 % and max <= 4.
void direct_check_compare(const u32* fb, const uint32_t* view, const Band* bands, int nb, const FadeFrame* fade)
{
	if (!s_check_ref)
		s_check_ref = static_cast<uint32_t*>(malloc(kFbPixels * sizeof(uint32_t)));
	if (!s_diff_ref)
		s_diff_ref = static_cast<u32*>(malloc(kFbPixels * sizeof(u32)));
	const uint32_t* ov = overlay_pixels();
	if (!s_check_ref || !s_diff_ref || !ov)
	{
		++s_cn.check_skipped;
		return;
	}
	memcpy(s_check_ref, view, kFbPixels * sizeof(uint32_t));
	for (int i = 0; i < nb; ++i)
		for (int y = bands[i].y0; y < bands[i].y1; ++y)
			for (int x = 0; x < kScreenW; ++x)
			{
				const uint32_t o = ov[static_cast<size_t>(y) * kTexW + x];   // 0xRRGGBBAA, premultiplied
				const unsigned a = o & 0xFF;
				if (!a && !(o >> 8))
					continue;
				uint32_t& v = s_check_ref[static_cast<size_t>(y) * kScreenW + x];   // 0xAARRGGBB
				uint32_t out = 0xFF000000u;
				for (int ch = 0; ch < 3; ++ch)
				{
					const unsigned oc = (o >> (24 - 8 * ch)) & 0xFF, vc = (v >> (16 - 8 * ch)) & 0xFF;
					const unsigned c = oc + (vc * (255 - a) + 127) / 255;
					out |= (c > 255 ? 255 : c) << (16 - 8 * ch);
				}
				v = out;
			}
	unsigned quirk = 0;
	if (fade)
		for (size_t i = 0; i < kFbPixels; ++i)
		{
			const uint32_t v = s_check_ref[i];
			const unsigned c[3] = {(v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF};
			bool q = false;
			for (int ch = 0; ch < 3; ++ch)
			{
				const int d = static_cast<int>(fade->sw[ch][c[ch]]) - fade->ref[ch][c[ch]];
				q = q || d > 2 || d < -2;
			}
			quirk += q;
			s_check_ref[i] = 0xFF000000u | static_cast<uint32_t>(fade->ref[0][c[0]]) << 16 |
			                 static_cast<uint32_t>(fade->ref[1][c[1]]) << 8 | fade->ref[2][c[2]];
		}
	if (!a1_3ds::present_reference(s_diff_ref, s_check_ref, kScreenW * 4, kScreenW, kScreenH, SDL_PIXELFORMAT_ARGB8888))
	{
		++s_cn.check_skipped;
		return;
	}
	GSPGPU_InvalidateDataCache(fb, kFbPixels * sizeof(u32));
	unsigned over1 = 0, over2 = 0, over32 = 0, mx[3] = {0, 0, 0};
	for (size_t i = 0; i < kFbPixels; ++i)
	{
		const u32 g = fb[i], c = s_diff_ref[i];
		if (!((g ^ c) & 0xFFFFFF00u))
			continue;
		unsigned m = 0;
		for (int ch = 0; ch < 3; ++ch)
		{
			const int shift = 24 - 8 * ch;
			const int x = (g >> shift) & 0xFF, y = (c >> shift) & 0xFF;
			const unsigned d = static_cast<unsigned>(x > y ? x - y : y - x);
			if (d > mx[ch])
				mx[ch] = d;
			if (d > m)
				m = d;
		}
		over1 += m > 1;
		over2 += m > 2;
		over32 += m > 32;
	}
	const double within = 100.0 * (kFbPixels - over1) / kFbPixels;
	const double within2 = 100.0 * (kFbPixels - over2) / kFbPixels;
	const unsigned max_all = mx[0] > mx[1] ? (mx[0] > mx[2] ? mx[0] : mx[2]) : (mx[1] > mx[2] ? mx[1] : mx[2]);
	++s_cn.checks;
	if (fade)
	{
		const bool bad = within2 < 99.9 || max_all > 4;
		++s_cn.fade_checks;
		s_cn.fade_quirk_px += quirk;
		s_cn.fade_bad += bad;
		s_cn.check_bad += bad;
		if (max_all > s_cn.fade_max)
			s_cn.fade_max = max_all;
		if (within2 < s_cn.fade_min_within2)
			s_cn.fade_min_within2 = within2;
	}
	else
		s_cn.check_bad += within < 99.9 || over32 > 0;
	s_cn.check_big += over32;
	for (int ch = 0; ch < 3; ++ch)
		if (mx[ch] > s_cn.check_max[ch])
			s_cn.check_max[ch] = mx[ch];
	if (within < s_cn.check_min_within)
		s_cn.check_min_within = within;
	printf("[c3d] direct_check: present %u: within_1=%.3f%% within_2=%.3f%% over_1=%u over_32=%u max R %u G %u B %u "
	       "max=%u bands=%d fade=%d%s%s quirk_px=%u\n",
	       s_present, within, within2, over1, over32, mx[0], mx[1], mx[2], max_all, nb, fade ? 1 : 0,
	       fade ? " " : "", fade ? fade_kind_name(fade->fit.kind) : "", quirk);
}

// What one eye's composite draws (the quads were written once by present_composite).
struct CompositeJob
{
	int nb;                     // overlay bands
	FadeKind fkind;             // the frame's fade (M6b)
	const FadeRange* ranges;    // part 2's row ranges
	int nr;
	C3D_Tex* otex;              // the overlay texture
	C3D_Tex* noise;             // part 3's noise texture
	bool flip;                  // view_v_flip
};

// M9 (design 7, "M9 specification", "The composite"): one eye of a direct frame's composite, from the full state block to
// the end of the fade passes: the present's draw state, the target, the projection, the view texture, the view quad, the
// bands, the fade. It starts from every piece of state the previous eye may have left (the band blend, a fade's blend,
// TEV stages and alpha test, the fold's unit 1 and the t1map uniform), because for the second eye it is called with the
// first eye's leftovers: set_present_draw sets stage 0 and the colour mask in full, the block below the rest, and
// draw_fade_*() restore stages 1-3, unit 1, the alpha test, the blend and the blend colour (0) at their ends.
void composite_eye(C3D_RenderTarget* target, C3D_Tex* vtex, const CompositeJob& job)
{
	set_present_draw(Swizzle::Plain);
	C3D_CullFace(GPU_CULL_NONE);
	C3D_StencilTest(false, GPU_ALWAYS, 0, 0xFF, 0);
	C3D_AlphaTest(false, GPU_ALWAYS, 0);
	C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
	C3D_FrameDrawOn(target);
	C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, s_uloc_projection, &s_projection);
	__real_C3D_TexBind(0, vtex);
	C3D_DrawArrays(GPU_TRIANGLE_STRIP, 0, 4);
	if (job.nb)
	{
		__real_C3D_TexBind(0, job.otex);
		C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ONE_MINUS_SRC_ALPHA, GPU_ONE,
		               GPU_ONE_MINUS_SRC_ALPHA);
		for (int i = 0; i < job.nb; ++i)
			C3D_DrawArrays(GPU_TRIANGLE_STRIP, 4 * (1 + i), 4);
		s_cn.band_quads += job.nb;
	}
	// M6b: the fade after the overlays (SW applies the curves to the crosshair and the text too).
	if (job.fkind == FadeKind::Affine && !fade_affine_is_identity(s_fade.fit))
		draw_fade_affine(s_fade.fit);
	else if (job.fkind == FadeKind::Fold)
		draw_fade_fold(s_fade.fit, vtex, job.otex, job.ranges, job.nr, job.nb, job.flip);
	else if (job.fkind == FadeKind::Static && job.noise)
		draw_fade_static(s_fade.fit, job.noise);
}

// stereo_check (test only): the right framebuffer against the left, pixel for pixel in RGB (the LCD ignores A; alpha-only
// differences are counted apart). Also the two things that make the comparison mean something: 3D mode is on, and the two
// eyes' buffers are not the same buffer (in 2D mode libctru returns the left buffer for both: gfxGetFramebuffer+0x28-0x34).
void stereo_check_compare(const u32* fl, const u32* fr, bool is3d)
{
	GSPGPU_InvalidateDataCache(fl, kFbPixels * sizeof(u32));
	GSPGPU_InvalidateDataCache(fr, kFbPixels * sizeof(u32));
	unsigned differ = 0, alpha_only = 0;
	const bool distinct = fl != fr;
	for (size_t i = 0; i < kFbPixels; ++i)
	{
		const u32 a = fl[i], b = fr[i];
		if (a == b)
			continue;
		if ((a ^ b) & 0xFFFFFF00u)
			++differ;
		else
			++alpha_only;
	}
	++s_cn.stereo_checks;
	s_cn.stereo_diff_px += differ;
	s_cn.stereo_alpha_px += alpha_only;
	const bool bad = differ || !is3d || !distinct;
	s_cn.stereo_check_bad += bad;
	printf("[c3d] stereo_check: present %u: differ=%u alpha_only=%u is3d=%d distinct=%d%s\n", s_present, differ, alpha_only,
	       is3d ? 1 : 0, distinct ? 1 : 0, bad ? " BAD" : "");
}

// M6 (design 7, M6 specification "The composite frame"): a direct frame's present. One C3D frame: the band uploads, the
// view quad (view texture, ONE/ZERO), one quad per band (overlay texture, ONE/ONE_MINUS_SRC_ALPHA), the restore, the
// VBlank guard, C3D_FrameEnd. No staging fill, no upload of main_surface.
bool present_composite(const Config& cfg, unsigned n, bool trace)
{
	C3D_Tex* const vtex = view_texture();
	C3D_Tex* const vtex_r = view_texture_right();
	C3D_Tex* const otex = overlay_texture();
	// M6b: this frame's fade, consumed whatever happens below.
	const bool fade = s_fade_set;
	s_fade_set = false;
	// M9: this frame's view was drawn for two eyes (and the right-eye objects exist): two eyes, else today's frame.
	const bool stereo = s_stereo_set && s_target_r && vtex_r;
	s_stereo_set = false;
	switch (begin_present_frame(FrameZone::Wait))   // the view frame's GPU time not hidden by the CPU: c3d_wait
	{
	case Begin::Ok:
		break;
	case Begin::ReturnTrue:
		overlay_drop();
		return true;
	case Begin::ReturnFalse:
		overlay_drop();
		return false;
	}
	sample_drawing_time(GpuTime::View);
	if (s_diff_fb)
		diff_compare();   // a check of the last non-direct present
	if (cfg.present_diff && n % cfg.present_diff == 0)
		++s_cn.not_checkable;
	bool check = cfg.direct_check && (s_cn.composites + 1) % cfg.direct_check == 0;
	if (check && fade && s_fade.fit.kind == FadeKind::Static)
	{
		check = false;   // the static is GPU-styled (user decision): no SW reference
		++s_cn.fade_unchecked;
	}
	Band bands[kMaxBands];
	int nb = 0;
	{
#ifdef A1_PROFILE
		A1_PROF_SCOPE(C3dComposite);
#endif
		if (cfg.overlay_audit && (s_cn.composites + 1) % cfg.overlay_audit == 0)
		{
			++s_cn.audits;
			const unsigned leaks = overlay_leaks_now();
			s_cn.leaks += leaks;
			if (leaks)
				printf("[c3d] overlay_audit: present %u: %u pixels outside the bands\n", n, leaks);
		}
		nb = overlay_upload(bands);   // before any draw: each transfer one GX entry (empty command buffer)

		// The quads: positions in screen pixels (Mtx_OrthoTilt, as the present), texcoords in the 512x256 textures.
		const float u1 = static_cast<float>(kScreenW) / kTexW;
		const float vt = cfg.view_v_flip ? 1.0f : 0.0f;
		const float vb = cfg.view_v_flip ? 1.0f - static_cast<float>(kScreenH) / kTexH : static_cast<float>(kScreenH) / kTexH;
		Vertex* q = s_cvbo;
		q[0] = Vertex{0.0f, 0.0f, 0.5f, 0.0f, vt};
		q[1] = Vertex{0.0f, static_cast<float>(kScreenH), 0.5f, 0.0f, vb};
		q[2] = Vertex{static_cast<float>(kScreenW), 0.0f, 0.5f, u1, vt};
		q[3] = Vertex{static_cast<float>(kScreenW), static_cast<float>(kScreenH), 0.5f, u1, vb};
		for (int i = 0; i < nb; ++i)
		{
			const float y0 = static_cast<float>(bands[i].y0), y1 = static_cast<float>(bands[i].y1);
			Vertex* b = s_cvbo + 4 * (1 + i);
			b[0] = Vertex{0.0f, y0, 0.5f, 0.0f, y0 / kTexH};
			b[1] = Vertex{0.0f, y1, 0.5f, 0.0f, y1 / kTexH};
			b[2] = Vertex{static_cast<float>(kScreenW), y0, 0.5f, u1, y0 / kTexH};
			b[3] = Vertex{static_cast<float>(kScreenW), y1, 0.5f, u1, y1 / kTexH};
		}
		// M6b: the fade passes' row ranges (view texcoords; texcoord1 from the t1map uniform) or the static's noise quad.
		const FadeKind fkind = fade ? s_fade.fit.kind : FadeKind::None;
		FadeRange ranges[kMaxRanges];
		int nr = 0;
		C3D_Tex* noise = nullptr;
		int used_quads = 1 + nb;
		if (fkind == FadeKind::Fold)
		{
			nr = fade_ranges(bands, nb, ranges);
			for (int i = 0; i < nr; ++i)
			{
				const float y0 = static_cast<float>(ranges[i].y0), y1 = static_cast<float>(ranges[i].y1);
				const float v0 = cfg.view_v_flip ? 1.0f - y0 / kTexH : y0 / kTexH;
				const float v1 = cfg.view_v_flip ? 1.0f - y1 / kTexH : y1 / kTexH;
				Vertex* r = s_cvbo + 4 * (kRangeQuad0 + i);
				r[0] = Vertex{0.0f, y0, 0.5f, 0.0f, v0};
				r[1] = Vertex{0.0f, y1, 0.5f, 0.0f, v1};
				r[2] = Vertex{static_cast<float>(kScreenW), y0, 0.5f, u1, v0};
				r[3] = Vertex{static_cast<float>(kScreenW), y1, 0.5f, u1, v1};
			}
			used_quads = kRangeQuad0 + nr;
		}
		else if (fkind == FadeKind::Static && (noise = static_cast<C3D_Tex*>(noise_get())) != nullptr)
		{
			// A new offset each frame (an LCG on the composite counter), the noise repeating (GPU_REPEAT).
			const uint32_t h = (s_cn.composites + 1) * 2654435761u;
			const float ox = static_cast<float>((h >> 8) & 0xFF) / kNoiseSide, oy = static_cast<float>((h >> 16) & 0xFF) / kNoiseSide;
			const float nu = static_cast<float>(kScreenW) / kNoiseSide, nv = static_cast<float>(kScreenH) / kNoiseSide;
			Vertex* r = s_cvbo + 4 * kNoiseQuad;
			r[0] = Vertex{0.0f, 0.0f, 0.5f, ox, oy};
			r[1] = Vertex{0.0f, static_cast<float>(kScreenH), 0.5f, ox, oy + nv};
			r[2] = Vertex{static_cast<float>(kScreenW), 0.0f, 0.5f, ox + nu, oy};
			r[3] = Vertex{static_cast<float>(kScreenW), static_cast<float>(kScreenH), 0.5f, ox + nu, oy + nv};
			used_quads = kCompositeQuads;
		}
		GSPGPU_FlushDataCache(s_cvbo, static_cast<u32>(used_quads * 4 * sizeof(Vertex)));
		s_saved_cbuf = *C3D_GetBufInfo();
		BufInfo_Init(&s_cbuf);
		BufInfo_Add(&s_cbuf, s_cvbo, sizeof(Vertex), 2, 0x10);
		C3D_SetBufInfo(&s_cbuf);

		// M9: each eye is the same composite from its own view texture into its own linked target ("overlays at zero
		// parallax": the same quads, the same bands, the same fade).
		CompositeJob job{nb, fkind, ranges, nr, otex, noise, cfg.view_v_flip};
		composite_eye(s_target, vtex, job);
		if (stereo)
			composite_eye(s_target_r, vtex_r, job);
		// The restore: the present's texture (the record), its buffer info, the plain blend.
		__real_C3D_TexBind(0, static_cast<C3D_Tex*>(tex_recorded()));
		C3D_SetBufInfo(&s_saved_cbuf);
		C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
	}

	if (trace)
		crumb("composite: before the VBlank wait");
	if (gspIsPresentPending(GSP_SCREEN_TOP))
	{
#ifdef A1_PROFILE
		A1_PROF_SCOPE(C3dVblank);
#endif
		gspWaitForVBlank();
	}
	const u32* check_fb = nullptr;
	if (check)
	{
		u16 fb_w = 0, fb_h = 0;
		check_fb = reinterpret_cast<const u32*>(gfxGetFramebuffer(GFX_TOP, GFX_LEFT, &fb_w, &fb_h));
		if (fb_w != kScreenH || fb_h != kScreenW)
			check_fb = nullptr;
	}
	// stereo_check (test only): both eyes' framebuffers, taken like the left one's above, before C3D_FrameEnd (the
	// transfers' destinations; the swap comes after).
	const bool scheck = stereo && cfg.stereo_check && (s_cn.stereo_composites + 1) % cfg.stereo_check == 0;
	const u32* scheck_l = nullptr;
	const u32* scheck_r = nullptr;
	bool scheck_3d = false;
	if (scheck)
	{
		u16 lw = 0, lh = 0, rw = 0, rh = 0;
		scheck_3d = stereo_is_3d();
		scheck_l = reinterpret_cast<const u32*>(gfxGetFramebuffer(GFX_TOP, GFX_LEFT, &lw, &lh));
		scheck_r = reinterpret_cast<const u32*>(gfxGetFramebuffer(GFX_TOP, GFX_RIGHT, &rw, &rh));
		if (lw != kScreenH || lh != kScreenW || rw != kScreenH || rh != kScreenW)
			scheck_l = scheck_r = nullptr;
	}
	frame_end(stereo ? 2 : 1, trace, FrameKind::Composite);
	++s_cn.composites;
	s_cn.stereo_composites += stereo;
	bool retired = false;   // a test frame has begun after the composite: its queue entry has retired
	if (check)
	{
		// Test only, after the composite and in frames of its own (so the composite is the same with or without the check,
		// and the software backend's gxq_budget 3 holds): the composite retires (its framebuffer transfer done), the view
		// texture (unchanged until the next view frame) is read back, retires, and the two are compared.
		bool done = false;
		// The gate can close between frames (as between a two-frame present's U and D, 5.7.6): no test frame then.
		if (!(cfg.suspend_gate && gate_closed()) && frame_begin(FrameZone::Wait) == FrameStart::Ok)
		{
			const bool queued = view_queue_readback();
			frame_end(0, false, FrameKind::View);
			note_view_frame();
			if (queued && frame_begin(FrameZone::Wait) == FrameStart::Ok)
			{
				frame_end(0, false, FrameKind::View);
				if (check_fb)
				{
					direct_check_compare(check_fb, view_convert_readback(), bands, nb, fade ? &s_fade : nullptr);
					done = true;
				}
			}
		}
		if (!done)
			++s_cn.check_skipped;
	}
	if (scheck)
	{
		// Test only: an empty frame retires the composite (unless direct_check's frames above did), then the compare.
		if (!retired && !(cfg.suspend_gate && gate_closed()) && frame_begin(FrameZone::Wait) == FrameStart::Ok)
		{
			frame_end(0, false, FrameKind::View);
			note_view_frame();
			retired = true;
		}
		if (retired && scheck_l && scheck_r)
			stereo_check_compare(scheck_l, scheck_r, scheck_3d);
		else
			++s_cn.stereo_check_skipped;
	}
	bool burst = false;
	for (unsigned first : {120u, 600u, 1200u, 1800u})
		burst = burst || (n >= first && n < first + 6);
	if (cfg.calibrate && burst)
	{
		printf("[c3d] calibrate: capture after present %u (colour R=%u G=%u, band row %u; composite)\n", n, n & 0xFF,
		       (n >> 8) & 0xFF, 8 * (n % 30));
		a1_3ds::save_screens("c3d-cal-" + std::to_string(n));
	}
	stats_tick();
	return true;
}

} // namespace

void fade_frame_set(const FadeFrame* frame)
{
	s_fade_set = frame != nullptr;
	if (frame)
		s_fade = *frame;
}

void stereo_frame_set(bool stereo)
{
	s_stereo_set = stereo;
}

bool present_right_ready()
{
	return s_target_r != nullptr;
}

void present_stereo_drop()
{
	// Out of frame (view_ensure): the right screen target goes, so no VRAM is left to a stereo that is off for the run.
	if (!s_target_r)
		return;
	C3D_RenderTargetDetachOutput(s_target_r);
	C3D_RenderTargetDelete(s_target_r);   // renderqueue.h:64; unlinked now, so it waits for the queue itself
	s_target_r = nullptr;
}

bool present_close_frame()
{
	if (!s_tex_ready || !s_target)
		return true;
	// Console run 1 (design 5.6.1): the GPU work of a present (a draw from the present texture
	// into the linked target plus the display transfer to the framebuffer), forced while the gate
	// condition holds, with breadcrumbs. No upload and no staging write: the texture holds the
	// last picture.
	crumb("close_frame: before frame_begin");
	const FrameStart fs = frame_begin();
	if (fs == FrameStart::Abandoned)
		return false;
	if (fs != FrameStart::Ok)
	{
		crumb("close_frame: frame_begin refused (already in a frame); no forced frame");
		return true;
	}
	crumb("close_frame: after frame_begin");
	draw_present_quad(s_tex_reordered);
	char line[96];
	snprintf(line, sizeof line, "close_frame: before frame_end (gpu right %d, in frame 1)", gspHasGpuRight() ? 1 : 0);
	crumb(line);
	frame_end(1, true);
	crumb("close_frame: after frame_end");
	return true;
}

bool present_init()
{
	s_staging = static_cast<u32*>(linearAlloc(kStagingBytes));
	s_vbo = static_cast<Vertex*>(linearAlloc(4 * sizeof(Vertex)));
	s_cvbo = static_cast<Vertex*>(linearAlloc(kCompositeQuads * 4 * sizeof(Vertex)));
	if (!s_staging || !s_vbo || !s_cvbo)
	{
		printf("[c3d] linearAlloc failed (staging %lu KiB)\n", static_cast<unsigned long>(kStagingBytes / 1024));
		return false;
	}
	memset(s_staging, 0, kStagingBytes);
	GSPGPU_FlushDataCache(s_staging, kStagingBytes);

	// M9 (design 7, "M9 specification", "Right-eye objects and 3D mode"): 3D mode before the targets (the example's order,
	// stereoscopic_2d/source/main.cpp:27-37), then the right screen target exactly as the left, whatever the mode byte is
	// then. It is drawn only on stereo frames.
	const bool stereo_wanted = stereo_begin(config(), config_has_file());

	s_target = C3D_RenderTargetCreate(kScreenH, kScreenW, GPU_RB_RGBA8, C3D_DEPTHTYPE(-1));
	if (!s_target)
	{
		printf("[c3d] C3D_RenderTargetCreate failed\n");
		return false;
	}
	C3D_RenderTargetSetOutput(s_target, GFX_TOP, GFX_LEFT, kTargetTransferFlags);

	if (stereo_wanted)
	{
		s_target_r = C3D_RenderTargetCreate(kScreenH, kScreenW, GPU_RB_RGBA8, C3D_DEPTHTYPE(-1));
		if (s_target_r)
			C3D_RenderTargetSetOutput(s_target_r, GFX_TOP, GFX_RIGHT, kTargetTransferFlags);
		else
			stereo_turn_off(StereoOff::Vram);   // "[c3d] stereo: off for the run (vram)"; the left target is untouched
	}

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
	s_uloc_t1map = shaderInstanceGetUniformLocation(s_program.vertexShader, "t1map");

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
	set_present_draw(Swizzle::Plain);   // and again before every draw
	return true;
}

void present_detach()
{
	s_vram_free_at_exit = vramSpaceFree();   // the targets and textures still allocated (G3-a)
	if (s_target_r)
		C3D_RenderTargetDetachOutput(s_target_r);
	if (s_target)
		C3D_RenderTargetDetachOutput(s_target);
	stereo_end();   // 2D mode again: SDL's present, the fatal screen and whatever follows citro3d
}

void present_free()
{
	// The targets are destroyed by C3D_Fini (every target of the context is).
	s_target = nullptr;
	s_target_r = nullptr;
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
	if (s_cvbo)
		linearFree(s_cvbo);
	s_cvbo = nullptr;
	if (s_cn.composites)
		printf("[c3d] composite final: composites=%u band_quads=%u present_diff_not_checkable=%u audits=%u leaks=%u "
		       "direct_checks=%u bad=%u big=%u skipped=%u min_within=%.3f%% max R %u G %u B %u bottom_pending=%u "
		       "vram_free_kib=%lu fade_quads=%u fade_passes=%u fade_noise=%u fade_restores=%u fade_checks=%u fade_bad=%u "
		       "fade_min_within_2=%.3f%% fade_max=%u fade_unchecked=%u fade_quirk_px=%llu stereo_composites=%u "
		       "stereo_checks=%u stereo_check_bad=%u stereo_check_skipped=%u stereo_diff_px=%llu stereo_alpha_px=%llu "
		       "(at exit, before the release)\n",
		       s_cn.composites, s_cn.band_quads, s_cn.not_checkable, s_cn.audits, s_cn.leaks, s_cn.checks, s_cn.check_bad,
		       s_cn.check_big, s_cn.check_skipped, s_cn.check_min_within, s_cn.check_max[0], s_cn.check_max[1],
		       s_cn.check_max[2], overlay_bottom_pending(), static_cast<unsigned long>(s_vram_free_at_exit / 1024),
		       s_cn.fade_quads, s_cn.fade_passes, s_cn.fade_noise, s_cn.fade_restores, s_cn.fade_checks, s_cn.fade_bad,
		       s_cn.fade_min_within2, s_cn.fade_max, s_cn.fade_unchecked, s_cn.fade_quirk_px, s_cn.stereo_composites,
		       s_cn.stereo_checks, s_cn.stereo_check_bad, s_cn.stereo_check_skipped, s_cn.stereo_diff_px,
		       s_cn.stereo_alpha_px);
	free(s_check_ref);
	s_check_ref = nullptr;
	if (s_staging)
		linearFree(s_staging);
	s_staging = nullptr;
	// s_zc_surface is not freed: the engine's main_surface lives in it for the rest of the run (design 5.7.2).
	if (s_diff_checks || s_diff_no_cpu || s_diff_fb_size)
		printf("[c3d] present_diff final: %u checks, %u with RGB differences; not checkable: %u (no CPU present "
		       "for the surface), %u (framebuffer size); max |diff| R %u G %u B %u A %u\n",
		       s_diff_checks, s_diff_bad, s_diff_no_cpu, s_diff_fb_size, s_diff_max[0], s_diff_max[1],
		       s_diff_max[2], s_diff_max[3]);
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

// The engine's main_surface pixels for a 400x240 game screen (design 5.7.2; declared in
// a1_3ds_platform.h). No citro3d call: linearAlloc, memset, GSPGPU_FlushDataCache, printf only.
void* a1_3ds::main_surface_pixels(int width, int height, int& pitch)
{
	using namespace a1_c3d;
	using namespace a1_c3d::back;
	if (width != kScreenW || height != kScreenH)
		return nullptr;
	const Config& cfg = config();   // loads debug-c3d on first use
	if (!cfg.present_gpu || !cfg.zero_copy)
		return nullptr;
	if (!s_zc_tried)
	{
		s_zc_tried = true;   // a failed allocation is logged once and not retried
		const unsigned before = linearSpaceFree() / 1024;
		s_zc_surface = static_cast<u32*>(linearAlloc(kStagingBytes));
		if (s_zc_surface)
		{
			memset(s_zc_surface, 0, kStagingBytes);
			GSPGPU_FlushDataCache(s_zc_surface, kStagingBytes);
			printf("[c3d] zero_copy: surface buffer %p, %lu KiB, pitch %d; linear free %u -> %u KiB\n",
			       static_cast<void*>(s_zc_surface), static_cast<unsigned long>(kStagingBytes / 1024), kZcPitch,
			       before, linearSpaceFree() / 1024);
		}
		else
			printf("[c3d] zero_copy: linearAlloc of %lu KiB failed (linear free %u KiB): the engine allocates the surface\n",
			       static_cast<unsigned long>(kStagingBytes / 1024), before);
		fflush(stdout);
	}
	if (!s_zc_surface)
		return nullptr;
	pitch = kZcPitch;
	return s_zc_surface;
}

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
			static const char* const what[] = {"", "vhalt()", "abort() on a second thread",
			                                   "vhalt() on a second thread", "error_screen() on a second thread"};
			printf("[c3d] test_vhalt %d: calling %s (citro3d %s)\n", cfg.test_vhalt, what[cfg.test_vhalt],
			       is_up() ? "up" : "not in use");
			fflush(stdout);
			if (cfg.test_vhalt == 1)
				vhalt("debug-c3d test_vhalt");   // never returns
			// 2, 3, 4: the error screen from a thread other than main (critic P1 minor 4). Lower
			// priority than the main thread, same core: it runs while the main thread sleeps
			// between frames, i.e. possibly between two C3D frames, and the main thread keeps
			// presenting until it serves the release request.
			thread_started = true;
			if (!threadCreate(test_vhalt_thread, reinterpret_cast<void*>(cfg.test_vhalt), 64 * 1024, 0x3f, -2, true))
				printf("[c3d] test_vhalt: threadCreate failed\n");
		}
	}
	// Design 5.6.1 a, in this order: 1. a pending release is served; 2. the gate: no GPU work without
	// the GPU rights, while suspended or closing (the HOME menu owns the screens; after a Close
	// the GPU never retires the work); 3. ensure_up().
	if (serve_release())
		return top_surrendered();
	switch (gate_present())
	{
	case GatePresent::Skip:
		return true;    // counted by the gate; no citro3d call
	case GatePresent::Cpu:
		return false;   // citro3d not up: the CPU present, as without citro3d
	case GatePresent::Open:
		break;
	}
	if (!ensure_up())
		return top_surrendered();   // true: the frame is "presented" by leaving the top screen alone
	// d3 breadcrumbs only while the gate condition holds (so only with suspend_gate 0).
	const bool trace = gate_closed();
	const unsigned n = ++s_present;
	const uint64_t now = svcGetSystemTick();
	count_present();

	if (cfg.alt_load && (n & 1))
		busy_wait_ms(20);

	// M6: a direct frame (hook 3b handed out the overlay surface this frame): the composite, not main_surface.
	if (overlay_surface_now())
	{
		if (width == kScreenW && height == kScreenH && view_texture() && overlay_texture())
			return present_composite(cfg, n, trace);
		overlay_drop();   // not expected: the surface changed between hook 3b and the present
	}

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

	// Design 5.7.5: the zero-copy path iff the picture is the engine's own surface in the 5.7.2 buffer
	// (it exists only with zero_copy 1), 400x240, pitch 2048, a format with the XRGB word layout.
	// The format is checked explicitly, not left to the conversion above (critic B-b minor 3).
	const bool zero = !odd_now && s_zc_surface && src == s_zc_surface && src_w == kScreenW &&
	                  src_h == kScreenH && src_pitch == kZcPitch &&
	                  (src_fmt == SDL_PIXELFORMAT_RGB888 || src_fmt == SDL_PIXELFORMAT_ARGB8888);
	// Design 5.7.4: a zero-copy present is always two frames; with two_frame the staging upload is a
	// C3D frame of its own (U), and the draw another (D), which begins only when U has retired.
	const bool two = cfg.two_frame || zero;
	static bool logged_two = false, logged_zero = false;
	if (zero && !logged_zero)
	{
		logged_zero = true;
		printf("[c3d] present: 400x240 zero-copy: the GPU reads main_surface (frames U and D, three channel draws)\n");
	}
	else if (two && !zero && !logged_two)
	{
		logged_two = true;
		printf("[c3d] present: two_frame 1: frame U (fill, flush, upload) and frame D (draw) per present\n");
	}

	// 1. The previous C3D frame has retired when this returns.
	switch (begin_present_frame(FrameZone::Wait))
	{
		case Begin::Ok:
			break;
		case Begin::ReturnTrue:
			return true;
		case Begin::ReturnFalse:
			return false;
	}
	sample_drawing_time(GpuTime::Draw);   // the previous present's draw (and framebuffer transfer)

	// present_diff: the previous checked present has reached its framebuffer now.
	if (s_diff_fb)
		diff_compare();
	const bool diff_now = cfg.present_diff && !cfg.calibrate && n % cfg.present_diff == 0;
	bool diff_ready = false;
	if (diff_now)
	{
		diff_ready = diff_prepare(src, src_pitch, src_w, src_h, src_fmt);
		if (!diff_ready)
			++s_diff_no_cpu;
	}

	// 2. Staging, written only now that the previous transfer has read the old one. Not for a
	// zero-copy present: the engine's surface is the source of the upload.
	if (!zero)
	{
#ifdef A1_PROFILE
		A1_PROF_SCOPE(C3dFill);
#endif
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
	}
	// two_frame: the staging is flushed before frame U ends (the transfer starts at its C3D_FrameEnd);
	// otherwise at step 5, as in P1. Zero-copy: the surface's rows 0-239 (the engine wrote them through the cache).
	if (two)
		GSPGPU_FlushDataCache(zero ? static_cast<void*>(s_zc_surface) : static_cast<void*>(s_staging),
		                      kStagingWrittenBytes);   // critic P1 minor 5: rows 0-239 only

	// 3. Linear -> tiled into the present texture: the first GX operation of the frame.
	const u32* const upload_src = zero ? s_zc_surface : s_staging;
	if (gx_op([&] {
		    C3D_SyncDisplayTransfer(const_cast<u32*>(upload_src), GX_BUFFER_DIM(kTexW, kTexH),
		                            static_cast<u32*>(s_tex.data), GX_BUFFER_DIM(kTexW, kTexH),
		                            kUploadTransferFlags);
	    }))
		s_tex_reordered = zero;   // the texture now holds this picture's words (5.7.5)
	else
		printf("[c3d] GX budget: present upload refused\n");   // the texture keeps the previous picture

	if (two)
	{
		// End of frame U: the upload only, no linked target drawn (no framebuffer transfer, no swap).
		frame_end(0, trace);
		// 5.7.6: aptShouldClose() (and the GPU right) can change between the frames. Do not draw while
		// the gate condition holds; retire U (bounded: frame_begin() sees the gate closed) and end an
		// empty frame. Only with suspend_gate 1 (0 is the deliberate reproduction: D draws as today).
		if (cfg.suspend_gate && gate_closed())
		{
			printf("[c3d] two-frame present (%s): the gate closed between frames U and D: frame D not drawn\n",
			       zero ? "zero-copy" : "two_frame 1");
			fflush(stdout);
			switch (begin_present_frame(FrameZone::Wait))
			{
				case Begin::Ok:
					break;
				case Begin::ReturnTrue:
					return true;
				case Begin::ReturnFalse:
					return false;
			}
			frame_end(0, true);
			return true;
		}
		// Frame D: begins when U has retired, i.e. the upload has read the staging.
		switch (begin_present_frame(FrameZone::Upload))
		{
			case Begin::Ok:
				break;
			case Begin::ReturnTrue:
				return true;
			case Begin::ReturnFalse:
				return false;
		}
		sample_drawing_time(GpuTime::Upload);   // frame U's upload
	}

	// 4. One quad into the screen target (or, calibrating, a flat frame-indexed colour).
	if (cfg.calibrate)
	{
		C3D_CullFace(GPU_CULL_NONE);
		C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
		C3D_StencilTest(false, GPU_ALWAYS, 0, 0xFF, 0);
		C3D_AlphaTest(false, GPU_ALWAYS, 0);
		C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
		const u32 colour = ((n & 0xFF) << 24) | (((n >> 8) & 0xFF) << 16) | (0xA5u << 8) | 0xFF;
		gx_op([&] { C3D_RenderTargetClear(s_target, C3D_CLEAR_COLOR, colour, 0); });
		C3D_FrameDrawOn(s_target);
	}
	else
		draw_present_quad(s_tex_reordered);

	// 5. The transfer's destination is taken at C3D_FrameEnd, and the previous queue (with
	// its swap request) finished in step 1: if that swap has not taken effect yet (next
	// VBlank), the buffer gfxGetFramebuffer() would return is still on screen. Wait.
	if (!two)
		GSPGPU_FlushDataCache(s_staging, kStagingWrittenBytes);   // critic P1 minor 5: rows 0-239 only
	if (trace)
		crumb("before the VBlank wait");
	if (gspIsPresentPending(GSP_SCREEN_TOP))
	{
#ifdef A1_PROFILE
		A1_PROF_SCOPE(C3dVblank);
#endif
		gspWaitForVBlank();
	}
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
			++s_diff_fb_size;
	}
	if (trace)
	{
		char line[96];
		snprintf(line, sizeof line, "before frame_end (gpu right %d, in frame 1)", gspHasGpuRight() ? 1 : 0);
		crumb(line);
	}
	frame_end(1, trace);
	if (trace)
		crumb("after frame_end");

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
