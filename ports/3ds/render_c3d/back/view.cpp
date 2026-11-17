/*
	Aleph One for the Nintendo 3DS, citro3d back end: the view target and the readback calibration
	(docs/3ds/CITRO3D_DESIGN.md, section 7, M0 specification, "Readback calibration").

	An UNLINKED render target of the view's size (never C3D_RenderTargetSetOutput on it: nothing
	of it ever reaches a framebuffer) and a linear staging buffer. A calibration frame k runs
	between two presents, inside render_frame (front/frame.cpp): the target and buffers are made
	(or, for another size, deleted and made again) OUT of frame, because C3D_RenderTargetDelete
	svcBreaks inside one; the staging is filled with a sentinel and flushed; then, through
	frame_begin() / frame_end(), the target is cleared to a colour that encodes k, four 16x16
	corner quads and (M1) a fifth, half-transparent one at the top centre, which checks that the
	alpha byte survives, are drawn with the present's shader and attribute layout, and the display
	transfer (tiled -> linear) is queued INSIDE the frame, behind the draws (which are first split
	off with C3D_FrameSplit(GX_CMDLIST_FLUSH): see the transfer below). The frame ends; an
	empty frame_begin() / frame_end() pair then returns only when the queue has retired
	(C3D_FrameBegin(0) waits for every earlier entry, design 0.3); cache invalidate; conversion
	RGBA -> ARGB (M1: keeping alpha). core/calibrate.cpp decides what came out. M1: the target,
staging, readback and conversion are the shared functions of view_shared.h, used by back/draw.cpp too. The out-of-frame C3D_SyncDisplayTransfer
	is not used: it waits on the PPF event, which the present's in-frame transfers leave signalled
	(design 5.7.4 "Rejected"), so it can return before its own transfer is done.

	State hygiene with the present (back/present.cpp is not edited in M0; design 7, M0): the
	program is bound once at init by the present and is not touched here, nor are the AttrInfo,
	C3D_TexBind(0) or TEV stages 1-5. This file saves the buffer info and restores it after its
	draws, sets the projection uniform, TEV stage 0 and the cull/depth/stencil/alpha/blend state
	for its own draws; the present sets all of those again before each of its own draws.

	Main thread only (it runs between presents on the thread that brought citro3d up). Every
	failure is logged once; nothing here waits in a loop.

	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; either version 3 of the License, or
	(at your option) any later version.
*/
#include "view.h"

#include "context.h"
#include "stereo.h"
#include "view_shared.h"
#include "../core/calibrate.h"
#include "../core/config.h"

#include <3ds.h>
#include <citro3d.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "a1_c3d_shaders_shbin.h"

#ifdef A1_PROFILE
#include "a1_3ds_profile.h"
#endif

namespace a1_c3d {
namespace back {

namespace {

constexpr int kMaxSide = 1024;             // design 1.5: a view side beyond it is not handled
constexpr int kMaxPixels = 256 * 1024;     // bounds the staging and the converted copy to 1 MiB each
constexpr int kQuads = 5;   // four corners and the alpha quad (M1)

// The present's vertex layout (present.cpp): float3 position in pixels, float2 texcoord (unused here).
struct CalVertex { float x, y, z, u, v; };

// The readback transfer (design 7, M0 "Starting values"; the same flags as present.cpp's
// kTargetTransferFlags, gx.h:51-ff): target tiled RGBA8 -> linear RGBA8, no flip, no scaling.
// UNVERIFIED until the calibration passes (design 5.2).
constexpr u32 kReadbackFlags =
	GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) |
	GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGBA8) |
	GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO);

// g3a 1 (test only, M6 specification "G3-a"): the 1024x512 target is read back halved (2x2 box), 1024x512 -> 512x256.
constexpr u32 kReadbackHalfFlags =
	GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) |
	GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGBA8) |
	GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_XY);
// M6 seed: the readback's inverse (linear -> tiled into the colour buffer, no flip). UNVERIFIED until seed_check passes.
constexpr u32 kSeedFlags =
	GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(1) | GX_TRANSFER_RAW_COPY(0) |
	GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGBA8) |
	GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO);

C3D_RenderTarget* s_target = nullptr;   // owned by citro3d's context: C3D_Fini destroys it
int s_w = 0, s_h = 0;                   // the view's size
// M6 (design 7, M6 specification "The view target in mode direct"): in mode direct a view that fits 512x256 is drawn
// into a target on a VRAM texture (the composite samples it). The target is then tw x th (512x256; g3a: 1024x512)
// and the readback staging sw x sh (512x256: g3a's readback halves). Else tw = sw = w, th = sh = h, as since M0.
C3D_Tex s_vtex;
bool s_vtex_ready = false;
bool s_is_tex = false;
int s_tw = 0, s_th = 0, s_sw = 0, s_sh = 0;
// M9 (design 7, "M9 specification", "Right-eye objects and 3D mode", "The view textures stay"): the right eye's view texture
// and target, made with the left whenever it is made in mode direct and stereo is allocated. Never read back or seeded.
C3D_Tex s_vtex_r;
bool s_vtex_r_ready = false;
C3D_RenderTarget* s_target_r = nullptr;
bool s_right_fresh = false;             // just created, or the view's size changed: the next stereo frame clears it
bool s_logged_keep_refused = false;
bool s_tex_failed = false;              // a texture target could not be made: logged once, not retried
u32* s_staging = nullptr;               // linear, sw*sh*4 bytes
uint32_t* s_argb = nullptr;             // malloc, w*h: the converted readback handed to the caller
CalVertex* s_vbo = nullptr;             // linear, kQuads*4 vertices
int s_uloc_projection = -1;             // the present shader's "projection" uniform
bool s_uloc_known = false;

// C3D_SetBufInfo() copies its argument into the context (VERIFIED by disassembling libcitro3d.a 1.7.1:
// a 152-byte memcpy when the context is active; buffers.h:21 does not say), so the structs need not
// outlive the call; they are kept static anyway.
C3D_BufInfo s_saved_buf;
C3D_BufInfo s_view_buf;

bool s_logged_skip_size = false, s_logged_nomem = false, s_logged_in_frame = false, s_logged_draw_on = false,
     s_logged_nouniform = false, s_logged_gx = false, s_logged_wait = false;
int s_skip_w = 0, s_skip_h = 0;     // the size s_logged_skip_size was logged for
int s_fail_w = 0, s_fail_h = 0;     // a size whose resources could not be created: not retried

C3D_Mtx s_proj;

// TEV stage 0's constant colour (texenv.h:85 stores the u32 as it is; C3D_TexEnvColor has no doc). The
// PICA register TEXENVi_COLOR is believed to be 0xAABBGGRR, R in the low byte: UNVERIFIED locally, no
// header or example states it. The clear colour's order (0xRRGGBBAA) was settled by P1's calibration,
// so a red/blue swap that shows only in the corner quads means this order is wrong.
inline u32 tev_constant(uint32_t argb)
{
	return ((argb >> 24) << 24) | ((argb & 0xFF) << 16) | (((argb >> 8) & 0xFF) << 8) | ((argb >> 16) & 0xFF);
}

// Deletes the target (out of frame: C3D_RenderTargetDelete panics inside one) and, for a texture target, its texture.
void delete_right()
{
	if (s_target_r)
		C3D_RenderTargetDelete(s_target_r);   // never linked
	s_target_r = nullptr;
	if (s_vtex_r_ready)
		C3D_TexDelete(&s_vtex_r);
	s_vtex_r_ready = false;
	s_right_fresh = false;
}

void delete_target()
{
	delete_right();
	if (s_target)
		C3D_RenderTargetDelete(s_target);   // renderqueue.h:63; never linked, so no output to detach
	s_target = nullptr;
	if (s_vtex_ready)
		C3D_TexDelete(&s_vtex);
	s_vtex_ready = false;
	s_is_tex = false;
}

void release_buffers()
{
	if (s_staging)
		linearFree(s_staging);
	s_staging = nullptr;
	free(s_argb);
	s_argb = nullptr;
}

// The location of the present shader's "projection" uniform, without present.cpp's private state:
// the same embedded shbin is parsed a second time (DVLB_ParseFile, shbin.h:110) and a throwaway
// shader instance (shaderProgram.h: shaderInstanceInit / GetUniformLocation / Free, which only
// read the DVLE and touch no citro3d context) answers shaderInstanceGetUniformLocation. The
// location depends on the shbin alone, so it is the one present.cpp's bound program uses.
// The instance must come from malloc: shaderInstanceFree() frees its float24 table AND the instance
// itself (libctru.a shaderProgram.o, shaderInstanceFree+0x28-0x2c: `mov r0, r3; bl free`), as
// shaderProgramSetVsh mallocs its instances. A stack instance here corrupted the malloc heap (M0
// Azahar calibration runs: _free_r faults right after the first calibration frame).
// -1: not found.
int find_projection_location()
{
	DVLB_s* dvlb = DVLB_ParseFile(const_cast<u32*>(reinterpret_cast<const u32*>(a1_c3d_shaders_shbin)),
	                              a1_c3d_shaders_shbin_size);
	if (!dvlb)
		return -1;
	int loc = -1;
	shaderInstance_s* si = static_cast<shaderInstance_s*>(malloc(sizeof(shaderInstance_s)));
	if (si && R_SUCCEEDED(shaderInstanceInit(si, &dvlb->DVLE[0])))
	{
		loc = shaderInstanceGetUniformLocation(si, "projection");
		shaderInstanceFree(si);   // frees si too
	}
	else
		free(si);
	DVLB_Free(dvlb);
	return loc;
}

// The four corner quads and the alpha quad (pixels, y down) as triangle strips in the present's vertex
// order. The alpha quad is at the top centre, x in [w/2 - 8, w/2 + 8), y in [0, 16) (calibrate.h).
void fill_vbo(int w, int h)
{
	const float x[kQuads] = {0.0f, static_cast<float>(w - kCalQuad), 0.0f, static_cast<float>(w - kCalQuad),
	                         static_cast<float>(w / 2 - kCalQuad / 2)};
	const float y[kQuads] = {0.0f, 0.0f, static_cast<float>(h - kCalQuad), static_cast<float>(h - kCalQuad), 0.0f};
	const float s = static_cast<float>(kCalQuad);
	for (int q = 0; q < kQuads; ++q)
	{
		CalVertex* v = s_vbo + 4 * q;
		v[0] = CalVertex{x[q], y[q], 0.5f, 0.0f, 0.0f};
		v[1] = CalVertex{x[q], y[q] + s, 0.5f, 0.0f, 0.0f};
		v[2] = CalVertex{x[q] + s, y[q], 0.5f, 0.0f, 0.0f};
		v[3] = CalVertex{x[q] + s, y[q] + s, 0.5f, 0.0f, 0.0f};
	}
	GSPGPU_FlushDataCache(s_vbo, kQuads * 4 * sizeof(CalVertex));   // gspgpu.h: the GPU reads the VBO from RAM
}

} // namespace

// --- the shared target, staging and readback (view_shared.h) ---

bool view_size_supported(int w, int h, int min_side)
{
	return w > 0 && h > 0 && !(w & 7) && !(h & 7) && w <= kMaxSide && h <= kMaxSide && w * h <= kMaxPixels &&
	       w >= min_side && h >= min_side;
}

// True when view_ensure failed for this size (it is not retried).
bool view_size_failed(int w, int h)
{
	return w == s_fail_w && h == s_fail_h;
}

// The target and buffers for a w x h view, (re)created here OUT of frame (no frame_begin() open): an
// earlier target of another size is deleted first. C3D_RenderTargetDelete svcBreaks (USERBREAK_PANIC)
// when called inside a frame (libcitro3d.a renderqueue.o: it tests the in-frame flag first); outside
// one, for an unlinked target, it waits for the queue itself (gxCmdQueueWait) and then destroys.
// False: out of memory (logged once; the size is not retried).
bool view_ensure(int w, int h)
{
	if (w == s_fail_w && h == s_fail_h)
		return false;
	// M6: in mode direct every view that fits 512x256 (HUD + Map's 400x240) goes to the texture target; any other size,
	// and mode readback, to M0's unlinked target. g3a: a 1024x512 texture (G3-a's budget probe).
	const bool want_tex = config().mode == Mode::Direct && !s_tex_failed && w <= 512 && h <= 256;
	const bool g3a = want_tex && config().g3a;
	if (s_target && s_w == w && s_h == h && s_is_tex == want_tex)
		return true;
	// M9 "The view textures stay": a texture target again, of the same texture size: both textures and targets are kept (a
	// late 512 KiB allocation, twice with stereo, may not be contiguous once the pool has filled); only the viewport
	// changes, the right texture is cleared as just created, and s_argb was made for 512x256 once.
	if (s_target && s_is_tex && want_tex && s_tw == (g3a ? 1024 : 512) && s_th == (g3a ? 512 : 256))
	{
		printf("[c3d] view: %dx%d -> %dx%d, the texture targets are kept\n", s_w, s_h, w, h);
		fflush(stdout);
		s_w = w;
		s_h = h;
		s_right_fresh = s_target_r != nullptr;
		return true;
	}
	// With the right-eye objects allocated a view that does not fit is drawn by SW (the front end's stereo_size) and never
	// deletes the textures; this refusal is the guard for a caller that did not ask (readback calibration in mode direct).
	if (s_target && s_is_tex && !want_tex && stereo_objects_wanted())
	{
		if (!s_logged_keep_refused)
		{
			s_logged_keep_refused = true;
			printf("[c3d] view: %dx%d does not fit the texture targets, which stay (stereo): refused\n", w, h);
		}
		return false;
	}
	if (s_target)
	{
		delete_target();
		release_buffers();
		printf("[c3d] view: %dx%d target deleted (the next size is %dx%d)\n", s_w, s_h, w, h);
	}
	const int tw = want_tex ? (g3a ? 1024 : 512) : w, th = want_tex ? (g3a ? 512 : 256) : h;
	const int sw = want_tex ? 512 : w, sh = want_tex ? 256 : h;
	if (want_tex)
	{
		s_vtex_ready = C3D_TexInitVRAM(&s_vtex, tw, th, GPU_RGBA8);   // texture.h; render targets live in VRAM
		if (s_vtex_ready)
		{
			C3D_TexSetFilter(&s_vtex, GPU_NEAREST, GPU_NEAREST);
			C3D_TexSetWrap(&s_vtex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
			s_target = C3D_RenderTargetCreateFromTex(&s_vtex, GPU_TEXFACE_2D, 0, C3D_DEPTHTYPE(-1));   // renderqueue.h:63
		}
		if (!s_target)
		{
			// Direct mode needs it; readback frames work on M0's target (logged once; the front end sees view_direct_ok()).
			printf("[c3d] view: no %dx%d texture target (texture %d): direct frames off, readback on the unlinked target\n",
			       tw, th, s_vtex_ready ? 1 : 0);
			delete_target();
			s_tex_failed = true;
			return view_ensure(w, h);
		}
		s_is_tex = true;
		// M9: the right eye's texture and target, exactly as the first. A failure gives stereo up for the run and frees
		// whichever of the right-eye objects was made; direct mode itself is untouched.
		if (!g3a && stereo_objects_wanted())
		{
			if (!config().stereo_fail)
				s_vtex_r_ready = C3D_TexInitVRAM(&s_vtex_r, tw, th, GPU_RGBA8);
			if (s_vtex_r_ready)
			{
				C3D_TexSetFilter(&s_vtex_r, GPU_NEAREST, GPU_NEAREST);
				C3D_TexSetWrap(&s_vtex_r, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
				s_target_r = C3D_RenderTargetCreateFromTex(&s_vtex_r, GPU_TEXFACE_2D, 0, C3D_DEPTHTYPE(-1));
			}
			if (!s_target_r)
			{
				delete_right();
				present_stereo_drop();
				stereo_turn_off(StereoOff::Vram);
			}
			else
				s_right_fresh = true;
		}
		else if (g3a && stereo_objects_wanted())
			stereo_turn_off(StereoOff::G3a);
	}
	else
		// renderqueue.h:62; C3D_DEPTHTYPE(-1) = no depth buffer, as present.cpp. Never SetOutput.
		s_target = C3D_RenderTargetCreate(w, h, GPU_RB_RGBA8, C3D_DEPTHTYPE(-1));
	const size_t staging_bytes = static_cast<size_t>(sw) * sh * sizeof(u32);
	// M9: the converted readback of a texture target is made once for 512x256, so a larger view cannot overflow it.
	const size_t bytes = want_tex ? static_cast<size_t>(512) * 256 * sizeof(u32) : static_cast<size_t>(w) * h * sizeof(u32);
	s_staging = static_cast<u32*>(linearAlloc(staging_bytes));
	s_argb = static_cast<uint32_t*>(malloc(bytes));
	if (!s_vbo)
		s_vbo = static_cast<CalVertex*>(linearAlloc(kQuads * 4 * sizeof(CalVertex)));
	if (!s_target || !s_staging || !s_argb || !s_vbo)
	{
		delete_target();   // out of frame here too
		release_buffers();
		s_fail_w = w;
		s_fail_h = h;
		if (!s_logged_nomem)
		{
			s_logged_nomem = true;
			printf("[c3d] view: no memory for the %dx%d target and readback (%lu KiB linear, %lu KiB malloc): "
			       "view frames skipped\n", w, h, static_cast<unsigned long>(staging_bytes / 1024),
			       static_cast<unsigned long>(bytes / 1024));
		}
		return false;
	}
	s_w = w;
	s_h = h;
	s_tw = tw;
	s_th = th;
	s_sw = sw;
	s_sh = sh;
	if (!s_uloc_known)
	{
		s_uloc_projection = find_projection_location();
		s_uloc_known = true;
	}
	if (s_is_tex)
		printf("[c3d] view: %dx%d view on a %dx%d VRAM texture target%s, readback staging %dx%d (%lu KiB), projection "
		       "uniform %d\n", w, h, tw, th, s_target_r ? " (+ the right eye's)" : "", sw, sh,
		       static_cast<unsigned long>(staging_bytes / 1024), s_uloc_projection);
	else
		printf("[c3d] view: %dx%d unlinked RGBA8 target, readback staging %lu KiB, projection uniform %d\n", w, h,
		       static_cast<unsigned long>(staging_bytes / 1024), s_uloc_projection);
	fflush(stdout);
	return true;
}

bool view_is_texture()
{
	return s_target && s_is_tex;
}

C3D_Tex* view_texture()
{
	return s_target && s_is_tex ? &s_vtex : nullptr;
}

C3D_RenderTarget* view_target_right()
{
	return s_target && s_is_tex ? s_target_r : nullptr;
}

C3D_Tex* view_texture_right()
{
	return s_target && s_is_tex && s_target_r ? &s_vtex_r : nullptr;
}

bool view_right_take_fresh()
{
	const bool fresh = s_right_fresh;
	s_right_fresh = false;
	return fresh;
}

bool view_direct_ok()
{
	return !s_tex_failed;
}

int view_proj_width()
{
	return s_is_tex ? 512 : s_w;
}

int view_proj_height()
{
	return s_is_tex ? 256 : s_h;
}

void view_seed_fill(const uint8_t* const* rows)
{
	// dest's ARGB (SW's form, alpha 0xFF) -> the RGBA8 word, into the staging's top-left w x h, flushed (out of frame).
	for (int y = 0; y < s_h; ++y)
	{
		const uint32_t* in = reinterpret_cast<const uint32_t*>(rows[y]);
		u32* out = s_staging + static_cast<size_t>(y) * s_sw;
		for (int x = 0; x < s_w; ++x)
			out[x] = (in[x] << 8) | 0xFFu;
	}
	GSPGPU_FlushDataCache(s_staging, static_cast<u32>(static_cast<size_t>(s_sw) * s_sh * sizeof(u32)));
}

bool view_seed_transfer()
{
	// The frame's first GX operation (an empty command buffer: 1 entry, 2.4). g3a's 2x target cannot be seeded by a
	// transfer (they only shrink): it is cleared instead (test-only deviation, M6 specification "G3-a").
	if (s_tw != s_sw || s_th != s_sh)
		return gx_op([&] { C3D_RenderTargetClear(s_target, C3D_CLEAR_COLOR, 0x000000FFu, 0); });
	return gx_op([&] {
		C3D_SyncDisplayTransfer(s_staging, GX_BUFFER_DIM(s_sw, s_sh), static_cast<u32*>(s_target->frameBuf.colorBuf),
		                        GX_BUFFER_DIM(s_tw, s_th), kSeedFlags);
	});
}

C3D_RenderTarget* view_target()
{
	return s_target;
}

int view_width()
{
	return s_w;
}

int view_height()
{
	return s_h;
}

const uint32_t* view_argb()
{
	return s_argb;
}

void view_fill_staging(uint32_t rgba)
{
	const size_t n = static_cast<size_t>(s_sw) * s_sh;
	for (size_t i = 0; i < n; ++i)
		s_staging[i] = rgba;
	GSPGPU_FlushDataCache(s_staging, static_cast<u32>(n * sizeof(u32)));   // gspgpu.h
}

bool view_clear_target(uint32_t rgba)
{
	// renderqueue.h:72; one GX queue entry, counted by gx_op.
	if (gx_op([&] { C3D_RenderTargetClear(s_target, C3D_CLEAR_COLOR, rgba, 0); }))
		return true;
	if (!s_logged_gx)
	{
		s_logged_gx = true;
		printf("[c3d] GX budget: view clear refused\n");
	}
	return false;
}

bool view_clear_target_right(uint32_t rgba)
{
	if (!s_target_r)
		return false;
	if (gx_op([&] { C3D_RenderTargetClear(s_target_r, C3D_CLEAR_COLOR, rgba, 0); }))
		return true;
	if (!s_logged_gx)
	{
		s_logged_gx = true;
		printf("[c3d] GX budget: view clear refused\n");
	}
	return false;
}

// M9 (design 7, "What the two textures hold", changed after the code review of commit 2): the right eye's texture starts a
// stereo run from the left's picture, so both eyes hold the same pixel wherever no draw covers one. The form is the port's
// own texture copy (back/texcache.cpp texcache_frame_start promotions, back/shade.cpp): citro3d's C3D_SyncTextureCopy
// (renderqueue.h:78) with GX_TRANSFER_RAW_COPY(1) (gx.h:55), size in bytes, in and out dims 0 (a plain byte copy, gx.h:9-10
// "linesize / gap for GX_TextureCopy"). Inside a frame it is queued behind a split when the command buffer is not empty and
// does not wait (renderqueue.o C3D_SyncTextureCopy+0x2c-0x64, as noted at the promotions), so it runs in queue order: called
// right after the seed, it copies the seeded picture. One GX entry (gx_op), exactly like the clear it replaces. Both
// textures are 512x256 RGBA8 render-target textures in VRAM (the target's colour buffer is the texture's data), so one raw
// copy of the level-0 data is the whole picture; the left texture has no draw in flight (its last frame retired in frame_begin).
bool view_copy_left_to_right()
{
	if (!s_target_r || !s_vtex_ready || !s_vtex_r_ready)
		return false;
	if (gx_op([&] {
		    C3D_SyncTextureCopy(static_cast<u32*>(s_vtex.data), 0, static_cast<u32*>(s_vtex_r.data), 0, s_vtex.size,
		                        GX_TRANSFER_RAW_COPY(1));
	    }))
		return true;
	if (!s_logged_gx)
	{
		s_logged_gx = true;
		printf("[c3d] GX budget: right-eye copy refused\n");
	}
	return false;
}

Readback view_readback(bool keep_alpha)
{
	// The readback transfer, INSIDE the frame: C3D_SyncDisplayTransfer in a frame splits the command
	// list (C3Di_SplitFrame, GX_ProcessCommandList of the part so far) and queues GX_DisplayTransfer
	// behind the draws, without waiting (libcitro3d.a renderqueue.o; renderqueue.h:77). Its completion
	// is confirmed below, by the next frame_begin().
	// Its own split submits the draws with flags 0, i.e. without GSP's cache flush (renderqueue.o
	// C3D_SyncDisplayTransfer+0xcc-0xdc; libctru gx.o GX_ProcessCommandList+0x1c-0x44: flush =
	// (flags >> 1) & 1), and frame_end()'s C3D_FrameEnd(GX_CMDLIST_FLUSH) skips the whole-heap flush
	// (C3D_FrameEnd+0x7c-0x90): the GPU could run the draw segment from RAM with its bytes still in
	// the dcache. So the draws are split off first WITH the flush (C3D_FrameSplit, renderqueue.h:33,
	// passes its flags to GX_ProcessCommandList); the transfer's split then finds an empty buffer.
	// The flushing split is ONE queue entry (gx_split, M1 "GX queue"); the transfer then costs 1.
	// Critic M0 round 2, finding 1.
	#ifdef A1_PROFILE
	A1_PROF_SCOPE(C3dReadback);   // the split, the transfer and the conversion; the retirement wait
	                              // inside frame_begin() is timed in c3d_wait (zones are exclusive)
	#endif
	const int w = s_w, h = s_h;
	const uint32_t* const pos0 = gpuCmdBuf + gpuCmdBufOffset;   // M1: the readback's own command words
	const bool queued = gx_split([] { C3D_FrameSplit(GX_CMDLIST_FLUSH); }) && gx_op([&] {
		C3D_SyncDisplayTransfer(static_cast<u32*>(s_target->frameBuf.colorBuf), GX_BUFFER_DIM(s_tw, s_th), s_staging,
		                        GX_BUFFER_DIM(s_sw, s_sh), s_tw != s_sw ? kReadbackHalfFlags : kReadbackFlags);
	});
	const uint32_t* const pos1 = gpuCmdBuf + gpuCmdBufOffset;
	if (queued)
		cmdbuf_note_readback(pos1 > pos0 ? static_cast<uint32_t>(pos1 - pos0) : 0);
	frame_end(0, false, FrameKind::View);
	note_view_frame();
	if (!queued)
	{
		if (!s_logged_gx)
		{
			s_logged_gx = true;
			printf("[c3d] GX budget: view readback refused\n");
		}
		return Readback::GxRefused;
	}

	// Retirement: C3D_FrameBegin(0) returns only when every earlier queue entry, the draws and the
	// transfer, has finished (design 0.3); an empty frame closes it. The gate was open a moment ago.
	const FrameStart done = frame_begin(FrameZone::Wait);
	if (done != FrameStart::Ok)
	{
		if (!s_logged_wait)
		{
			s_logged_wait = true;
			printf("[c3d] view: the retirement wait after the readback returned %s: view frame dropped\n",
			       done == FrameStart::Abandoned ? "Abandoned" : "AlreadyInFrame");
		}
		return Readback::WaitFailed;
	}
	frame_end(0, false, FrameKind::View);

	// The GPU wrote the staging: drop any stale cached copy before reading (gspgpu.h:246).
	GSPGPU_InvalidateDataCache(s_staging, static_cast<u32>(static_cast<size_t>(s_sw) * s_sh * sizeof(u32)));
	for (int y = 0; y < h; ++y)
	{
		const u32* in = s_staging + static_cast<size_t>(y) * s_sw;   // M6: a texture target's staging is 512 wide
		uint32_t* out = s_argb + static_cast<size_t>(y) * w;
		if (keep_alpha)
			for (int x = 0; x < w; ++x)
				out[x] = rgba_to_argb_keep(in[x]);
		else
			for (int x = 0; x < w; ++x)
				out[x] = rgba_to_argb(in[x]);
	}
	return Readback::Ok;
}

bool view_queue_readback(bool right)
{
	// M6 direct_check: the readback transfer of the texture target queued behind the composite's draws (the flushing
	// split first, 0.3); confirmed by the next frame_begin. M9: `right` reads the right eye's texture target (stereo_cal,
	// stereo_gaps; the same size and format as the left's).
	C3D_RenderTarget* const target = right ? s_target_r : s_target;
	if (!target)
		return false;
	return gx_split([] { C3D_FrameSplit(GX_CMDLIST_FLUSH); }) && gx_op([&] {
		C3D_SyncDisplayTransfer(static_cast<u32*>(target->frameBuf.colorBuf), GX_BUFFER_DIM(s_tw, s_th), s_staging,
		                        GX_BUFFER_DIM(s_sw, s_sh), s_tw != s_sw ? kReadbackHalfFlags : kReadbackFlags);
	});
}

const uint32_t* view_convert_readback(bool full)
{
	GSPGPU_InvalidateDataCache(s_staging, static_cast<u32>(static_cast<size_t>(s_sw) * s_sh * sizeof(u32)));
	// full (M9 stereo_cal): the whole staging (512x256 for a texture target), pitch s_sw: the columns beyond the view too
	const int cw = full ? s_sw : s_w, ch = full ? s_sh : s_h;
	for (int y = 0; y < ch; ++y)
	{
		const u32* in = s_staging + static_cast<size_t>(y) * s_sw;
		uint32_t* out = s_argb + static_cast<size_t>(y) * cw;
		for (int x = 0; x < cw; ++x)
			out[x] = rgba_to_argb(in[x]);
	}
	return s_argb;
}

// --- M0's calibration ---

bool view_calibrate(unsigned k, int w, int h, CalResult& out, const uint32_t** pixels, int* out_w, int* out_h)
{
	if (pixels)
		*pixels = nullptr;
	// Design 5.6.1 a: no citro3d call without the GPU rights, while suspended or closing, or with a
	// release pending. The present serves a pending release; this does not.
	if (!is_up() || release_pending() || gate_closed())
		return false;
	auto supported = [](int ww, int hh) { return view_size_supported(ww, hh, 2 * kCalQuad); };
	if (!supported(w, h))
	{
		if (!s_logged_skip_size || s_skip_w != w || s_skip_h != h)
		{
			s_logged_skip_size = true;
			s_skip_w = w;
			s_skip_h = h;
			printf("[c3d] view: %dx%d is not supported (multiples of 8, at most %d per side and %d pixels): "
			       "calibration skipped\n", w, h, kMaxSide, kMaxPixels);
		}
		return false;
	}
	// readback_cal_resize: odd frames use a view 8 pixels smaller each way (still a multiple of 8 and
	// at least two quads), so the target is deleted and created again on every frame.
	if (config().readback_cal_resize && (k & 1) && supported(w - 8, h - 8))
	{
		w -= 8;
		h -= 8;
	}

	// Out of frame, after the gate checks: a target of another size is deleted and the new one made
	// (C3D_RenderTargetDelete must not run inside a frame, see view_ensure).
	if (!view_ensure(w, h))
		return false;
	// Neither the clear of k nor a corner colour, flushed so that no dirty cache line is written back
	// over what the transfer writes; whatever of it is left after the readback was never written.
	view_fill_staging(cal_sentinel_rgba(k));

	// frame_begin(): every earlier C3D frame has retired when it returns Ok (zone c3d_wait).
	const FrameStart fs = frame_begin(FrameZone::Wait);
	if (fs != FrameStart::Ok)
	{
		if (fs == FrameStart::AlreadyInFrame && !s_logged_in_frame)
		{
			s_logged_in_frame = true;
			printf("[c3d] view: C3D_FrameBegin(0) refused (already inside a frame): calibration skipped\n");
		}
		return false;   // Abandoned: citro3d is surrendered; nothing to do
	}

	// Clear to the colour that encodes k (0xRRGGBBAA, as present.cpp's calibrate; verified by P1).
	view_clear_target(cal_clear_rgba(k));
	if (!C3D_FrameDrawOn(s_target))   // renderqueue.h:32
	{
		if (!s_logged_draw_on)
		{
			s_logged_draw_on = true;
			printf("[c3d] view: C3D_FrameDrawOn failed: calibration skipped\n");
		}
		frame_end(0, false, FrameKind::View);
		return false;
	}

	// The same GPU state draw_present_quad() sets (present.cpp), for this frame's draws.
	C3D_CullFace(GPU_CULL_NONE);
	C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
	C3D_StencilTest(false, GPU_ALWAYS, 0, 0xFF, 0);
	C3D_AlphaTest(false, GPU_ALWAYS, 0);
	C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);

	// Pixels, x right, y down: left 0, right w, bottom h, top 0 (maths.h:492, the design's starting
	// value for an untilted unlinked target). If the calibration shows flipped rows or columns, this
	// is the one place that changes (design 7, M0).
	// M6: a texture target's projection covers the whole target (the view in its top-left corner).
	Mtx_Ortho(&s_proj, 0.0f, static_cast<float>(view_proj_width()), static_cast<float>(view_proj_height()), 0.0f, 0.0f,
	          1.0f, true);
	if (s_uloc_projection >= 0)
		C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, s_uloc_projection, &s_proj);   // uniforms.h:38
	else if (!s_logged_nouniform)
	{
		s_logged_nouniform = true;
		printf("[c3d] view: the projection uniform was not found: the quads will not be placed\n");
	}

	fill_vbo(w, h);
	// Our own buffer info for these draws, the present's restored afterwards (see the file head).
	s_saved_buf = *C3D_GetBufInfo();
	BufInfo_Init(&s_view_buf);
	BufInfo_Add(&s_view_buf, s_vbo, sizeof(CalVertex), 2, 0x10);   // buffers.h:17, as present.cpp
	C3D_SetBufInfo(&s_view_buf);

	static const uint32_t colour[kQuads] = {kCalTopLeft, kCalTopRight, kCalBottomLeft, kCalBottomRight,
	                                        kCalAlphaQuad};
	for (int q = 0; q < kQuads; ++q)
	{
		// TEV stage 0 = the constant colour (texenv.h:49-88), fully reset per quad. C3D_GetTexEnv()
		// marks the stage dirty. For the alpha quad the constant carries alpha 0x80 (tev_constant keeps
		// the top byte) and the blend is ONE, ZERO: the alpha byte reaches the target as drawn.
		C3D_TexEnv* env = C3D_GetTexEnv(0);
		C3D_TexEnvInit(env);
		C3D_TexEnvSrc(env, C3D_Both, GPU_CONSTANT, GPU_CONSTANT, GPU_CONSTANT);
		C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
		C3D_TexEnvColor(env, tev_constant(colour[q]));
		C3D_DrawArrays(GPU_TRIANGLE_STRIP, 4 * q, 4);   // the present's own primitive
	}
	C3D_SetBufInfo(&s_saved_buf);

	// The readback: the split, the transfer, the frame's end, the retirement pair, the conversion,
	// keeping the alpha byte (M1: the alpha check).
	if (view_readback(true) != Readback::Ok)
		return false;

	out = cal_check(s_argb, w, h, w, k);
	if (pixels)
		*pixels = s_argb;
	if (out_w)
		*out_w = w;
	if (out_h)
		*out_h = h;
	return true;
}

// M6 calibrate 1 in mode direct (design 7, M6 specification "Calibration"): a direct view frame without the world: the
// texture target cleared to the frame-indexed colour of present `n` (present.cpp's calibrate formula), M0's four corner
// quads at the view's corners and rows h..255 in a fifth colour, so a flipped or shifted composite shows. No readback.
bool view_direct_calibrate(unsigned n, int w, int h)
{
	if (!is_up() || release_pending() || gate_closed() || !view_ensure(w, h) || !view_is_texture())
		return false;
	if (frame_begin(FrameZone::Wait) != FrameStart::Ok)
		return false;
	const u32 colour = ((n & 0xFF) << 24) | (((n >> 8) & 0xFF) << 16) | (0xA5u << 8) | 0xFF;
	if (!view_clear_target(colour) || !C3D_FrameDrawOn(s_target))
	{
		frame_end(0, false, FrameKind::View);
		note_view_frame();
		return false;
	}
	C3D_CullFace(GPU_CULL_NONE);
	C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
	C3D_StencilTest(false, GPU_ALWAYS, 0, 0xFF, 0);
	C3D_AlphaTest(false, GPU_ALWAYS, 0);
	C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
	Mtx_Ortho(&s_proj, 0.0f, static_cast<float>(view_proj_width()), static_cast<float>(view_proj_height()), 0.0f, 0.0f,
	          1.0f, true);
	if (s_uloc_projection >= 0)
		C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, s_uloc_projection, &s_proj);
	// The corners as fill_vbo; the fifth quad is the band below the view (rows h..255, all 512 columns).
	fill_vbo(w, h);
	CalVertex* v = s_vbo + 4 * (kQuads - 1);
	const float x1 = static_cast<float>(view_proj_width()), y0 = static_cast<float>(h),
	            y1 = static_cast<float>(view_proj_height());
	v[0] = CalVertex{0.0f, y0, 0.5f, 0.0f, 0.0f};
	v[1] = CalVertex{0.0f, y1, 0.5f, 0.0f, 0.0f};
	v[2] = CalVertex{x1, y0, 0.5f, 0.0f, 0.0f};
	v[3] = CalVertex{x1, y1, 0.5f, 0.0f, 0.0f};
	GSPGPU_FlushDataCache(s_vbo, kQuads * 4 * sizeof(CalVertex));
	s_saved_buf = *C3D_GetBufInfo();
	BufInfo_Init(&s_view_buf);
	BufInfo_Add(&s_view_buf, s_vbo, sizeof(CalVertex), 2, 0x10);
	C3D_SetBufInfo(&s_view_buf);
	static const uint32_t quad_colour[kQuads] = {kCalTopLeft, kCalTopRight, kCalBottomLeft, kCalBottomRight, 0xFFFF00FFu};
	for (int q = 0; q < kQuads; ++q)
	{
		C3D_TexEnv* env = C3D_GetTexEnv(0);
		C3D_TexEnvInit(env);
		C3D_TexEnvSrc(env, C3D_Both, GPU_CONSTANT, GPU_CONSTANT, GPU_CONSTANT);
		C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
		C3D_TexEnvColor(env, tev_constant(quad_colour[q]));
		C3D_DrawArrays(GPU_TRIANGLE_STRIP, 4 * q, 4);
	}
	C3D_SetBufInfo(&s_saved_buf);
	frame_end(0, false, FrameKind::View);
	note_view_frame();
	return true;
}

void view_free()
{
	// C3D_Fini destroyed the target with the rest of the context (as present_free notes), but not the texture under a
	// texture target (C3D_TexDelete: vramFree, no citro3d context needed).
	s_target = nullptr;
	s_target_r = nullptr;
	if (s_vtex_ready)
		C3D_TexDelete(&s_vtex);
	s_vtex_ready = false;
	if (s_vtex_r_ready)
		C3D_TexDelete(&s_vtex_r);
	s_vtex_r_ready = false;
	s_right_fresh = false;
	s_is_tex = false;
	s_tex_failed = false;
	s_w = s_h = s_tw = s_th = s_sw = s_sh = 0;
	release_buffers();
	if (s_vbo)
		linearFree(s_vbo);
	s_vbo = nullptr;
	s_fail_w = s_fail_h = 0;
	s_uloc_known = false;
	s_uloc_projection = -1;
}

} // namespace back
} // namespace a1_c3d
