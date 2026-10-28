/*
	Aleph One for the Nintendo 3DS, citro3d back end: the C3D view frame
	(docs/3ds/CITRO3D_DESIGN.md, 2.1, 2.4 and section 7, M1 specification, "Back end" and "State
	save and restore around a view frame").

	One view frame draws the polygons the front end hands over (flat colours in M1) into the unlinked
	view target of back/view.cpp and reads it back keeping alpha. The order is the specification's:
	skips; target, staging, sentinel; frame_begin (checked); the clear, before the first draw; FrameDrawOn;
	SAVE the BufInfo, the AttrInfo and the recorded program; bind the world program through
	__real_C3D_BindProgram (so the record keeps the present's), attributes, buffers, GPU state, TEV 0,
	uniforms; polygons into the current ring half, merged into C3D_DrawElements by an equal key; at the end
	the pending draw, the restore, ranged flushes of exactly the bytes written, and the readback.

	The vertex layout is 2.1's, set up as /opt/devkitpro/examples/3ds/graphics/gpu/textured_cube
	main.c:130-146 does it: AttrInfo_AddLoader per attribute, BufInfo_Add(stride, count, permutation).

	Main thread only. Every failure is logged once; nothing here waits in a loop.

	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; either version 3 of the License, or
	(at your option) any later version.
*/
#include "draw.h"

#include "context.h"
#include "view_shared.h"
#include "../core/calibrate.h"
#include "../core/ring.h"

#include <3ds.h>
#include <citro3d.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "a1_c3d_world_shaders_shbin.h"

#ifdef A1_PROFILE
#include "a1_3ds_profile.h"
#endif

// back/context.cpp: -Wl,--wrap=C3D_BindProgram. Everything below binds through the real one.
extern "C" void __real_C3D_BindProgram(shaderProgram_s* program);

namespace a1_c3d {
namespace back {

namespace {

// BufInfo_Init's base (buffers.o: base_paddr = 0x18000000, the start of VRAM). Offsets from it are
// stored in 28 bits (the attribute-buffer location) and written by C3D_DrawElements unchecked for the
// index buffer (drawElements.o +0x18-0x30: below the base it silently draws nothing).
constexpr uint32_t kBufBase = 0x18000000u;
constexpr uint32_t kOffsetLimit = 0x10000000u;   // 28 bits

constexpr int kMaxPolygonVertices = 16;   // design 2.1

// --- state ---

shaderProgram_s s_prog;
DVLB_s* s_dvlb = nullptr;
bool s_prog_ready = false;
int s_uloc_proj = -1, s_uloc_stereo = -1;
C3D_AttrInfo s_attr;           // the world layout, built once

Vertex* s_vbo = nullptr;       // linear, 2 halves x ring_vertices
uint16_t* s_ibo = nullptr;     // linear, 2 halves x ring_indices
unsigned s_ring_v = 0, s_ring_i = 0;
RingState s_ring;
bool s_rings_ready = false;

bool s_off = false;            // the C3D side is off for the run (logged)
bool s_logged_noprog = false;

// The frame in progress.
struct Frame
{
	bool open = false;
	bool discarded = false;
	DiscardReason why = DiscardReason::Unsupported;
	bool drew = false;                 // at least one C3D_DrawElements was issued
	bool pending = false;
	DrawKey pkey{};
	unsigned pfirst = 0, pcount = 0;   // index offset in the half, index count
	shaderProgram_s* saved_prog = nullptr;
	uint32_t sentinel_argb = 0;
	int w = 0, h = 0;
	int half = 0;
} s_f;

C3D_BufInfo s_saved_buf, s_buf;
C3D_AttrInfo s_saved_attr;
C3D_Mtx s_proj;
unsigned s_k = 0;              // view frame counter: the sentinel's tag

// Counters (the "[c3d] cmdbuf" line).
struct Counters
{
	unsigned frames = 0, draws = 0, polygons = 0, vertices = 0, indices = 0;
	unsigned discards[static_cast<int>(DiscardReason::kCount)] = {};
	unsigned skips[static_cast<int>(SkipReason::kCount)] = {};
	unsigned prog_restore_mismatch = 0;
} s_n;

// --- helpers ---

const uint32_t* cmd_pos()
{
	return gpuCmdBuf + gpuCmdBufOffset;
}

bool phys_range_ok(const void* p, size_t bytes)
{
	const uint32_t pa = osConvertVirtToPhys(p);
	return pa >= kBufBase && static_cast<uint64_t>(pa - kBufBase) + bytes <= kOffsetLimit;
}

void go_off(const char* why)
{
	s_off = true;
	printf("[c3d] draw: the C3D side is off for the run: %s\n", why);
	fflush(stdout);
}

// The world program, at the first use. The instance comes from shaderProgramSetVsh (malloc), as
// shaderProgramFree expects (M0: shaderInstanceFree frees the instance itself).
bool world_init()
{
	if (s_prog_ready)
		return true;
	s_dvlb = DVLB_ParseFile(const_cast<u32*>(reinterpret_cast<const u32*>(a1_c3d_world_shaders_shbin)),
	                        a1_c3d_world_shaders_shbin_size);
	if (!s_dvlb)
		return false;
	shaderProgramInit(&s_prog);
	if (R_FAILED(shaderProgramSetVsh(&s_prog, &s_dvlb->DVLE[0])))
	{
		shaderProgramFree(&s_prog);
		DVLB_Free(s_dvlb);
		s_dvlb = nullptr;
		return false;
	}
	s_uloc_proj = shaderInstanceGetUniformLocation(s_prog.vertexShader, "proj");
	s_uloc_stereo = shaderInstanceGetUniformLocation(s_prog.vertexShader, "stereo");
	// 2.1's layout: v0 float3 (sx, sy, w), v1 float2, v2 float2, v3 ubyte4 (attribs.h:11-13).
	AttrInfo_Init(&s_attr);
	AttrInfo_AddLoader(&s_attr, 0, GPU_FLOAT, 3);
	AttrInfo_AddLoader(&s_attr, 1, GPU_FLOAT, 2);
	AttrInfo_AddLoader(&s_attr, 2, GPU_FLOAT, 2);
	AttrInfo_AddLoader(&s_attr, 3, GPU_UNSIGNED_BYTE, 4);
	printf("[c3d] draw: world program %s (uniforms proj %d, stereo %d)\n", s_uloc_proj >= 0 ? "ready" : "unusable",
	       s_uloc_proj, s_uloc_stereo);
	if (s_uloc_proj < 0)
	{
		// Without the projection uniform nothing can be placed: freed now, so a later call starts afresh
		// and draw_free() has nothing to free.
		shaderProgramFree(&s_prog);
		DVLB_Free(s_dvlb);
		s_dvlb = nullptr;
		return false;
	}
	s_prog_ready = true;
	return true;
}

// Both rings, at the first view frame: 2 halves of ring_vertices vertices (32 B) and ring_indices u16
// indices, linear memory, with the physical-range checks of the specification for both halves of both.
bool rings_init()
{
	if (s_rings_ready)
		return true;
	const Config& cfg = config();
	s_ring_v = cfg.ring_vertices;
	s_ring_i = cfg.ring_indices;
	const size_t vbytes = static_cast<size_t>(s_ring_v) * sizeof(Vertex);
	const size_t ibytes = static_cast<size_t>(s_ring_i) * sizeof(uint16_t);
	s_vbo = static_cast<Vertex*>(linearAlloc(2 * vbytes));
	s_ibo = static_cast<uint16_t*>(linearAlloc(2 * ibytes));
	if (!s_vbo || !s_ibo)
	{
		go_off("no linear memory for the rings");
		return false;
	}
	bool ok = true;
	for (int h = 0; h < 2; ++h)
	{
		const uint8_t* vp = reinterpret_cast<const uint8_t*>(s_vbo) + h * vbytes;
		const uint8_t* ip = reinterpret_cast<const uint8_t*>(s_ibo) + h * ibytes;
		ok = ok && phys_range_ok(vp, vbytes) && phys_range_ok(ip, ibytes);
	}
	printf("[c3d] draw: rings: vertices 2 x %u x %u B at phys 0x%08lx, indices 2 x %u x 2 B at phys 0x%08lx; "
	       "physical range check (base 0x%08lx, 28-bit offsets) %s\n",
	       s_ring_v, static_cast<unsigned>(sizeof(Vertex)), static_cast<unsigned long>(osConvertVirtToPhys(s_vbo)),
	       s_ring_i, static_cast<unsigned long>(osConvertVirtToPhys(s_ibo)), static_cast<unsigned long>(kBufBase),
	       ok ? "ok" : "FAILED");
	fflush(stdout);
	if (!ok)
	{
		go_off("a ring half lies outside the 28-bit range of the buffer base");
		return false;
	}
	s_ring.configure(s_ring_v, s_ring_i);
	s_rings_ready = true;
	return true;
}

Vertex* vbo_half(int h) { return s_vbo + static_cast<size_t>(h) * s_ring_v; }
uint16_t* ibo_half(int h) { return s_ibo + static_cast<size_t>(h) * s_ring_i; }

ViewBegin skipped(SkipReason r)
{
	++s_n.skips[static_cast<int>(r)];
	return ViewBegin{ViewBegin::Kind::Skipped, r, DiscardReason::Unsupported};
}

ViewBegin discarded_at_begin(DiscardReason r)
{
	++s_n.discards[static_cast<int>(r)];
	return ViewBegin{ViewBegin::Kind::Discarded, SkipReason::Down, r};
}

// Gives the frame up; the first reason stays.
void discard(DiscardReason r)
{
	if (s_f.discarded)
		return;
	s_f.discarded = true;
	s_f.why = r;
	s_f.pending = false;
}

// 2.4: the command-buffer reserve before every draw and before the readback. Reserve test discard.
bool reserve_left()
{
	return reserve_ok(gpuCmdBufSize, gpuCmdBufOffset, kDrawReserveWords, kEndReserveWords,
	                  config().reserve_extra);
}

// The pending merged draw, if any: the reserve test, then one C3D_DrawElements; the words it took
// (citro3d's lazy state flush included) are measured around it.
void issue_pending()
{
	if (!s_f.pending)
		return;
	s_f.pending = false;
	if (s_f.discarded)
		return;
	if (!reserve_left())
	{
		discard(DiscardReason::Cmdbuf);
		return;
	}
	const uint32_t* const a = cmd_pos();
	// base.h:24; the indices are relative to the half's vertex start (BufInfo_Add of the half).
	C3D_DrawElements(GPU_TRIANGLES, static_cast<int>(s_f.pcount), C3D_UNSIGNED_SHORT,
	                 ibo_half(s_f.half) + s_f.pfirst);
	const uint32_t* const b = cmd_pos();
	cmdbuf_note_draw(b > a ? static_cast<uint32_t>(b - a) : 0);
	++s_n.draws;
	s_f.drew = true;
}

// Program, AttrInfo and BufInfo back to what the present set (the file head). The program through the
// real bind, so the record is untouched; then the check that the record is still the one re-bound.
void restore_state()
{
	__real_C3D_BindProgram(s_f.saved_prog);
	C3D_SetAttrInfo(&s_saved_attr);   // copy semantics: attribs.o C3D_SetAttrInfo (24 bytes into the context)
	C3D_SetBufInfo(&s_saved_buf);     // copy semantics: M0, buffers.h:21
	if (program_recorded() != static_cast<void*>(s_f.saved_prog))
		++s_n.prog_restore_mismatch;
}

// The end of a frame that is given up (or never drew): ranged flush of what was written, normal end.
void flush_rings()
{
	const unsigned nv = s_ring.vertices_used(), ni = s_ring.index_used();
	// gspgpu.h:239: exactly the bytes written this frame (the GPU runs nothing before C3D_FrameEnd).
	if (nv)
		GSPGPU_FlushDataCache(vbo_half(s_f.half), nv * static_cast<u32>(sizeof(Vertex)));
	if (ni)
		GSPGPU_FlushDataCache(ibo_half(s_f.half), ni * static_cast<u32>(sizeof(uint16_t)));
}

ViewEnd finish_discarded()
{
	frame_end(0, false, FrameKind::View);
	note_view_frame();
	s_f.open = false;
	++s_n.discards[static_cast<int>(s_f.why)];
	return ViewEnd{false, s_f.why};
}

} // namespace

const char* skip_name(SkipReason r)
{
	static const char* const names[] = {"down", "gate", "size"};
	return names[static_cast<int>(r)];
}

const char* discard_name(DiscardReason r)
{
	static const char* const names[] = {"unsupported", "ring", "cmdbuf", "gx", "begin"};
	return names[static_cast<int>(r)];
}

ViewBegin view_begin(int w, int h)
{
	s_f = Frame();
	// Design 5.6.1 a: no citro3d call without the GPU rights, while suspended or closing, or with a
	// release pending; nothing is drawn, so this is a skip.
	if (s_off || !is_up() || release_pending())
		return skipped(SkipReason::Down);
	if (gate_closed())
		return skipped(SkipReason::Gate);
	if (!view_size_supported(w, h, 8))
		return skipped(SkipReason::Size);
	if (!program_recorded())
	{
		// Impossible after present_init(); without the present's program there is nothing to restore.
		if (!s_logged_noprog)
		{
			s_logged_noprog = true;
			printf("[c3d] draw: no program recorded (the present has not bound one): view frames skipped\n");
		}
		return skipped(SkipReason::Down);
	}
	if (!world_init())
	{
		go_off("the world program could not be created");
		return skipped(SkipReason::Down);
	}
	if (!rings_init())
		return skipped(SkipReason::Down);
	// Out of frame: a target of another size is deleted and made (C3D_RenderTargetDelete must not run
	// inside a frame).
	if (!view_ensure(w, h))
		return skipped(SkipReason::Size);

	// The ring half of this frame and its BufInfo (a local struct: nothing of the context is touched
	// yet). BufInfo_Add's return is checked every frame (buffers.o +0x18-0x38: below the base it returns
	// -2 with a half-built slot).
	s_ring.begin_frame();
	s_f.half = s_ring.half();
	BufInfo_Init(&s_buf);
	if (BufInfo_Add(&s_buf, vbo_half(s_f.half), sizeof(Vertex), 4, 0x3210) < 0)   // buffers.h:17; v0..v3 in order
	{
		go_off("BufInfo_Add refused the vertex ring half");
		return skipped(SkipReason::Down);
	}

	// The staging is filled with a per-frame sentinel and flushed before the frame: a pixel the transfer
	// never wrote is recognisable. The colour hash never produces the sentinel's RGB (shape_colour).
	++s_k;
	s_f.w = w;
	s_f.h = h;
	s_f.sentinel_argb = rgba_to_argb_keep(cal_sentinel_rgba(s_k));
	view_fill_staging(cal_sentinel_rgba(s_k));
	++s_n.frames;

	const FrameStart fs = frame_begin(FrameZone::Wait);
	if (fs != FrameStart::Ok)
		return discarded_at_begin(DiscardReason::Begin);
	s_f.open = true;

	// The clear, only before the first draw (C3D_FrameBufClear is a GX_MemoryFill without a split, so
	// after draws it would run before them), to 0x00000000: alpha 0 is "not covered".
	if (!view_clear_target(0x00000000u))
	{
		frame_end(0, false, FrameKind::View);
		note_view_frame();
		s_f.open = false;
		return discarded_at_begin(DiscardReason::Gx);
	}
	if (!C3D_FrameDrawOn(view_target()))   // renderqueue.h:32
	{
		frame_end(0, false, FrameKind::View);
		note_view_frame();
		s_f.open = false;
		return discarded_at_begin(DiscardReason::Begin);
	}

	// SAVE before any bind of this frame.
	s_saved_buf = *C3D_GetBufInfo();
	s_saved_attr = *C3D_GetAttrInfo();
	s_f.saved_prog = static_cast<shaderProgram_s*>(program_recorded());

	__real_C3D_BindProgram(&s_prog);   // never through the wrapper: the record must keep the present's
	C3D_SetAttrInfo(&s_attr);
	C3D_SetBufInfo(&s_buf);

	// 2.4, "World draws": cull none, depth off, stencil off, alpha test GEQUAL 128, blend ONE, ZERO.
	C3D_CullFace(GPU_CULL_NONE);
	C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
	C3D_StencilTest(false, GPU_ALWAYS, 0, 0xFF, 0);
	C3D_AlphaTest(true, GPU_GEQUAL, 128);
	C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);

	// TEV stage 0 = the vertex colour, RGB and alpha (texenv.h:49-88); stages 1-5 and C3D_TexBind(0) are
	// not touched.
	C3D_TexEnv* env = C3D_GetTexEnv(0);
	C3D_TexEnvInit(env);
	C3D_TexEnvSrc(env, C3D_Both, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
	C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);

	// Pixels, x right, y down (M0's calibrated value); after the bind, which reloads the constants.
	Mtx_Ortho(&s_proj, 0.0f, static_cast<float>(w), static_cast<float>(h), 0.0f, 0.0f, 1.0f, true);
	C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, s_uloc_proj, &s_proj);
	if (s_uloc_stereo >= 0)
		C3D_FVUnifSet(GPU_VERTEX_SHADER, s_uloc_stereo, 0.0f, 0.0f, 0.0f, 0.0f);
	return ViewBegin{ViewBegin::Kind::Ok, SkipReason::Down, DiscardReason::Unsupported};
}

bool view_polygon(const Vertex* v, int n, const DrawKey& key)
{
	if (!s_f.open || s_f.discarded)
		return false;
	if (n < 3 || n > kMaxPolygonVertices)
	{
		discard(DiscardReason::Unsupported);
		return false;
	}
	const unsigned ni = 3u * static_cast<unsigned>(n - 2);
	if (!s_ring.fits(static_cast<unsigned>(n), ni))
	{
		discard(DiscardReason::Ring);
		return false;
	}
	if (!s_f.pending || key != s_f.pkey)
	{
		issue_pending();
		if (s_f.discarded)
			return false;
		s_f.pending = true;
		s_f.pkey = key;
		s_f.pfirst = s_ring.index_used();
		s_f.pcount = 0;
	}
	const unsigned first_index = s_ring.index_used();
	const unsigned base = s_ring.take(static_cast<unsigned>(n), ni);   // relative to the half's vertex start
	memcpy(vbo_half(s_f.half) + base, v, static_cast<size_t>(n) * sizeof(Vertex));
	fan_indices(ibo_half(s_f.half) + first_index, n, base);
	s_f.pcount += ni;
	++s_n.polygons;
	s_n.vertices += static_cast<unsigned>(n);
	s_n.indices += ni;
	return true;
}

void view_discard(DiscardReason reason)
{
	if (s_f.open)
		discard(reason);
}

ViewEnd view_end(uint32_t* out_argb, int pitch, unsigned* sentinel_count)
{
	if (sentinel_count)
		*sentinel_count = 0;
	if (!s_f.open)
		return ViewEnd{false, DiscardReason::Begin};
	{
		issue_pending();
		restore_state();
		// On every path that wrote anything, the discard path included: the draws issued before a discard
		// still run at C3D_FrameEnd(GX_CMDLIST_FLUSH), which skips the whole-heap flush (0.3).
		flush_rings();
		if (!s_f.discarded && !reserve_left())
			discard(DiscardReason::Cmdbuf);
	}
	if (s_f.discarded)
		return finish_discarded();

	switch (view_readback(true))
	{
	case Readback::GxRefused:
		s_f.open = false;
		discard(DiscardReason::Gx);
		++s_n.discards[static_cast<int>(DiscardReason::Gx)];
		return ViewEnd{false, DiscardReason::Gx};
	case Readback::WaitFailed:
		s_f.open = false;
		discard(DiscardReason::Begin);
		++s_n.discards[static_cast<int>(DiscardReason::Begin)];
		return ViewEnd{false, DiscardReason::Begin};
	case Readback::Ok:
		break;
	}
	s_f.open = false;
	const uint32_t* src = view_argb();
	unsigned sentinel = 0;
	for (int y = 0; y < s_f.h; ++y)
	{
		const uint32_t* row = src + static_cast<size_t>(y) * s_f.w;
		uint32_t* dst = out_argb + static_cast<size_t>(y) * pitch;
		for (int x = 0; x < s_f.w; ++x)
		{
			dst[x] = row[x];
			sentinel += row[x] == s_f.sentinel_argb;
		}
	}
	if (sentinel_count)
		*sentinel_count = sentinel;
	return ViewEnd{true, DiscardReason::Unsupported};
}

void draw_free()
{
	// After C3D_Fini: the program was never given to a live context.
	if (s_prog_ready)
	{
		shaderProgramFree(&s_prog);
		if (s_dvlb)
			DVLB_Free(s_dvlb);
	}
	s_dvlb = nullptr;
	s_prog_ready = false;
	s_uloc_proj = s_uloc_stereo = -1;
	if (s_vbo)
		linearFree(s_vbo);
	if (s_ibo)
		linearFree(s_ibo);
	s_vbo = nullptr;
	s_ibo = nullptr;
	s_rings_ready = false;
	s_f = Frame();
}

int draw_stats_text(char* buf, size_t size)
{
	const Counters& n = s_n;
	return snprintf(buf, size,
	                " views=%u draws=%u polys=%u verts=%u idx=%u ring_hw_v=%u,%u ring_hw_i=%u,%u "
	                "discards=%u/%u/%u/%u/%u(unsup/ring/cmdbuf/gx/begin) skips=%u/%u/%u(down/gate/size) "
	                "prog_restore_mismatch=%u",
	                n.frames, n.draws, n.polygons, n.vertices, n.indices, s_ring.high_water_vertices(0),
	                s_ring.high_water_vertices(1), s_ring.high_water_indices(0), s_ring.high_water_indices(1),
	                n.discards[0], n.discards[1], n.discards[2], n.discards[3], n.discards[4], n.skips[0],
	                n.skips[1], n.skips[2], n.prog_restore_mismatch);
}

} // namespace back
} // namespace a1_c3d
