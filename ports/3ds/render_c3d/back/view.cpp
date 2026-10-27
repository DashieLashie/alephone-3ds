/*
	Aleph One for the Nintendo 3DS, citro3d back end: the view target and the readback calibration
	(docs/3ds/CITRO3D_DESIGN.md, section 7, M0 specification, "Readback calibration").

	An UNLINKED render target of the view's size (never C3D_RenderTargetSetOutput on it: nothing
	of it ever reaches a framebuffer) and a linear staging buffer. A calibration frame k runs
	between two presents, inside render_frame (front/frame.cpp): the target and buffers are made
	(or, for another size, deleted and made again) OUT of frame, because C3D_RenderTargetDelete
	svcBreaks inside one; the staging is filled with a sentinel and flushed; then, through
	frame_begin() / frame_end(), the target is cleared to a colour that encodes k, four 16x16
	corner quads are drawn with the present's shader and attribute layout, and the display
	transfer (tiled -> linear) is queued INSIDE the frame, behind the draws. The frame ends; an
	empty frame_begin() / frame_end() pair then returns only when the queue has retired
	(C3D_FrameBegin(0) waits for every earlier entry, design 0.3); cache invalidate; conversion
	RGBA -> ARGB. core/calibrate.cpp decides what came out. The out-of-frame C3D_SyncDisplayTransfer
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
#include "../core/calibrate.h"

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
constexpr int kQuads = 4;

// The present's vertex layout (present.cpp): float3 position in pixels, float2 texcoord (unused here).
struct Vertex { float x, y, z, u, v; };

// The readback transfer (design 7, M0 "Starting values"; the same flags as present.cpp's
// kTargetTransferFlags, gx.h:51-ff): target tiled RGBA8 -> linear RGBA8, no flip, no scaling.
// UNVERIFIED until the calibration passes (design 5.2).
constexpr u32 kReadbackFlags =
	GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) |
	GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGBA8) |
	GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO);

C3D_RenderTarget* s_target = nullptr;   // owned by citro3d's context: C3D_Fini destroys it
int s_w = 0, s_h = 0;                   // the target's size
u32* s_staging = nullptr;               // linear, w*h*4 bytes
uint32_t* s_argb = nullptr;             // malloc, w*h: the converted readback handed to the caller
Vertex* s_vbo = nullptr;                // linear, kQuads*4 vertices
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

// The target and buffers for a w x h view, (re)created here OUT of frame (no frame_begin() open): an
// earlier target of another size is deleted first. C3D_RenderTargetDelete svcBreaks (USERBREAK_PANIC)
// when called inside a frame (libcitro3d.a renderqueue.o: it tests the in-frame flag first); outside
// one, for an unlinked target, it waits for the queue itself (gxCmdQueueWait) and then destroys.
// False: out of memory (logged once; the size is not retried).
bool ensure_resources(int w, int h)
{
	if (s_target && s_w == w && s_h == h)
		return true;
	if (s_target)
	{
		C3D_RenderTargetDelete(s_target);   // renderqueue.h:63; unlinked, so no output to detach
		s_target = nullptr;
		release_buffers();
		printf("[c3d] view: %dx%d target deleted (the next size is %dx%d)\n", s_w, s_h, w, h);
	}
	const size_t bytes = static_cast<size_t>(w) * h * sizeof(u32);
	// renderqueue.h:62; C3D_DEPTHTYPE(-1) = no depth buffer, as present.cpp. Never SetOutput.
	s_target = C3D_RenderTargetCreate(w, h, GPU_RB_RGBA8, C3D_DEPTHTYPE(-1));
	s_staging = static_cast<u32*>(linearAlloc(bytes));
	s_argb = static_cast<uint32_t*>(malloc(bytes));
	if (!s_vbo)
		s_vbo = static_cast<Vertex*>(linearAlloc(kQuads * 4 * sizeof(Vertex)));
	if (!s_target || !s_staging || !s_argb || !s_vbo)
	{
		if (s_target)
			C3D_RenderTargetDelete(s_target);   // out of frame here too
		s_target = nullptr;
		release_buffers();
		s_fail_w = w;
		s_fail_h = h;
		if (!s_logged_nomem)
		{
			s_logged_nomem = true;
			printf("[c3d] view: no memory for the %dx%d target and readback (%lu KiB linear, %lu KiB malloc): "
			       "calibration skipped\n", w, h, static_cast<unsigned long>(bytes / 1024),
			       static_cast<unsigned long>(bytes / 1024));
		}
		return false;
	}
	s_w = w;
	s_h = h;
	if (!s_uloc_known)
	{
		s_uloc_projection = find_projection_location();
		s_uloc_known = true;
	}
	printf("[c3d] view: %dx%d unlinked RGBA8 target, readback staging %lu KiB, projection uniform %d\n", w, h,
	       static_cast<unsigned long>(bytes / 1024), s_uloc_projection);
	fflush(stdout);
	return true;
}

// The four corner quads (pixels, y down) as triangle strips in the present's vertex order.
void fill_vbo(int w, int h)
{
	const float x[kQuads] = {0.0f, static_cast<float>(w - kCalQuad), 0.0f, static_cast<float>(w - kCalQuad)};
	const float y[kQuads] = {0.0f, 0.0f, static_cast<float>(h - kCalQuad), static_cast<float>(h - kCalQuad)};
	const float s = static_cast<float>(kCalQuad);
	for (int q = 0; q < kQuads; ++q)
	{
		Vertex* v = s_vbo + 4 * q;
		v[0] = Vertex{x[q], y[q], 0.5f, 0.0f, 0.0f};
		v[1] = Vertex{x[q], y[q] + s, 0.5f, 0.0f, 0.0f};
		v[2] = Vertex{x[q] + s, y[q], 0.5f, 0.0f, 0.0f};
		v[3] = Vertex{x[q] + s, y[q] + s, 0.5f, 0.0f, 0.0f};
	}
	GSPGPU_FlushDataCache(s_vbo, kQuads * 4 * sizeof(Vertex));   // gspgpu.h: the GPU reads the VBO from RAM
}

} // namespace

bool view_calibrate(unsigned k, int w, int h, CalResult& out, const uint32_t** pixels, int* out_w, int* out_h)
{
	if (pixels)
		*pixels = nullptr;
	// Design 5.6.1 a: no citro3d call without the GPU rights, while suspended or closing, or with a
	// release pending. The present serves a pending release; this does not.
	if (!is_up() || release_pending() || gate_closed())
		return false;
	auto supported = [](int ww, int hh) {
		return ww > 0 && hh > 0 && !(ww & 7) && !(hh & 7) && ww <= kMaxSide && hh <= kMaxSide &&
		       ww * hh <= kMaxPixels && ww >= 2 * kCalQuad && hh >= 2 * kCalQuad;
	};
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
	if (w == s_fail_w && h == s_fail_h)
		return false;

	// Out of frame, after the gate checks: a target of another size is deleted and the new one made
	// (C3D_RenderTargetDelete must not run inside a frame, see ensure_resources).
	if (!ensure_resources(w, h))
		return false;
	// Neither the clear of k nor a corner colour, flushed so that no dirty cache line is written back
	// over what the transfer writes; whatever of it is left after the readback was never written.
	{
		const u32 sentinel = cal_sentinel_rgba(k);
		const size_t n = static_cast<size_t>(w) * h;
		for (size_t i = 0; i < n; ++i)
			s_staging[i] = sentinel;
		GSPGPU_FlushDataCache(s_staging, static_cast<u32>(n * sizeof(u32)));   // gspgpu.h
	}

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
	// renderqueue.h:72; one GX queue entry, counted by gx_op.
	if (!gx_op([&] { C3D_RenderTargetClear(s_target, C3D_CLEAR_COLOR, cal_clear_rgba(k), 0); }) && !s_logged_gx)
	{
		s_logged_gx = true;
		printf("[c3d] GX budget: view clear refused\n");
	}
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
	Mtx_Ortho(&s_proj, 0.0f, static_cast<float>(w), static_cast<float>(h), 0.0f, 0.0f, 1.0f, true);
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
	BufInfo_Add(&s_view_buf, s_vbo, sizeof(Vertex), 2, 0x10);   // buffers.h:17, as present.cpp
	C3D_SetBufInfo(&s_view_buf);

	static const uint32_t corner[kQuads] = {kCalTopLeft, kCalTopRight, kCalBottomLeft, kCalBottomRight};
	for (int q = 0; q < kQuads; ++q)
	{
		// TEV stage 0 = the constant colour (texenv.h:49-88), fully reset per quad. C3D_GetTexEnv()
		// marks the stage dirty.
		C3D_TexEnv* env = C3D_GetTexEnv(0);
		C3D_TexEnvInit(env);
		C3D_TexEnvSrc(env, C3D_Both, GPU_CONSTANT, GPU_CONSTANT, GPU_CONSTANT);
		C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
		C3D_TexEnvColor(env, tev_constant(corner[q]));
		C3D_DrawArrays(GPU_TRIANGLE_STRIP, 4 * q, 4);   // the present's own primitive
	}
	C3D_SetBufInfo(&s_saved_buf);

	// The readback transfer, INSIDE the frame: C3D_SyncDisplayTransfer in a frame splits the command
	// list (C3Di_SplitFrame, GX_ProcessCommandList of the part so far) and queues GX_DisplayTransfer
	// behind the draws, without waiting (libcitro3d.a renderqueue.o; renderqueue.h:77). One or two
	// GX entries (two while the command buffer is not empty), counted by gx_op. Its completion is
	// confirmed below, by the next frame_begin().
	#ifdef A1_PROFILE
	A1_PROF_SCOPE(C3dReadback);   // the transfer, the retirement wait and the conversion
	#endif
	const bool queued = gx_op([&] {
		C3D_SyncDisplayTransfer(static_cast<u32*>(s_target->frameBuf.colorBuf), GX_BUFFER_DIM(w, h), s_staging,
		                        GX_BUFFER_DIM(w, h), kReadbackFlags);
	});
	frame_end(0, false, FrameKind::View);
	note_view_frame();
	if (!queued)
	{
		if (!s_logged_gx)
		{
			s_logged_gx = true;
			printf("[c3d] GX budget: view readback refused\n");
		}
		return false;
	}

	// Retirement: C3D_FrameBegin(0) returns only when every earlier queue entry, the draws and the
	// transfer, has finished (design 0.3); an empty frame closes it. The gate was open a moment ago.
	const FrameStart done = frame_begin(FrameZone::Wait);
	if (done != FrameStart::Ok)
	{
		if (!s_logged_wait)
		{
			s_logged_wait = true;
			printf("[c3d] view: the retirement wait after the readback returned %s: calibration frame dropped\n",
			       done == FrameStart::Abandoned ? "Abandoned" : "AlreadyInFrame");
		}
		return false;
	}
	frame_end(0, false, FrameKind::View);

	// The GPU wrote the staging: drop any stale cached copy before reading (gspgpu.h:246).
	const size_t n = static_cast<size_t>(w) * h;
	GSPGPU_InvalidateDataCache(s_staging, static_cast<u32>(n * sizeof(u32)));
	for (size_t i = 0; i < n; ++i)
		s_argb[i] = rgba_to_argb(s_staging[i]);

	out = cal_check(s_argb, w, h, w, k);
	if (pixels)
		*pixels = s_argb;
	if (out_w)
		*out_w = w;
	if (out_h)
		*out_h = h;
	return true;
}

void view_free()
{
	// C3D_Fini destroyed the target with the rest of the context (as present_free notes).
	s_target = nullptr;
	s_w = s_h = 0;
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
