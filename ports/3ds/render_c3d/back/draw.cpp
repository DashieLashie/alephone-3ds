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
#include "shade.h"
#include "stereo.h"
#include "view_shared.h"
#include "../core/calibrate.h"
#include "../core/classify.h"
#include "../core/noise.h"
#include "../core/ring.h"
#include "../core/stereo.h"

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

// back/context.cpp: -Wl,--wrap=C3D_BindProgram and C3D_TexBind. Everything below binds through the real ones.
extern "C" void __real_C3D_BindProgram(shaderProgram_s* program);
extern "C" void __real_C3D_TexBind(int unitId, C3D_Tex* tex);

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
	void* bound_tex = nullptr;         // what this frame bound to unit 0 (null: nothing, the present's stays)
	unsigned tex_binds0 = 0;           // tex_binds() at the start: a change means someone else bound mid-frame
	bool lit = false;                  // M3: the shade LUT on unit 1 and TEV stage 1 (saved, restored)
	void* lut = nullptr;               // the LUT's C3D_Tex this frame (shade_frame_start)
	unsigned blend = 0;                // M4: 0 = 2.4's world blend; 0x100 | c = the tinted blend with constant c
	bool tinted_used = false;          // M4: the tinted blend was set this frame (restore_state puts the world's back)
	unsigned mode = 0;                 // M5: 0 = Lit (M3's TEV, the LUT on unit 1); 0x10000 | masked << 8 | ref = Static
	bool stereo = false;               // M9: a frame for two eyes
	float eye_w0 = 1024.0f, eye_near = 1.0f, eye_p = 0.0f;
	bool fill = true;
	uint32_t sentinel_argb = 0;
	int w = 0, h = 0;
	int half = 0;
} s_f;

C3D_BufInfo s_saved_buf, s_buf;
C3D_AttrInfo s_saved_attr;
C3D_TexEnv s_saved_env1;       // M3: TEV stage 1 as it was before a lit frame (restore_state puts it back)
C3D_Mtx s_proj;
unsigned s_k = 0;              // view frame counter: the sentinel's tag

// Counters (the "[c3d] cmdbuf" line).
struct Counters
{
	unsigned frames = 0, draws = 0, polygons = 0, vertices = 0, indices = 0;
	unsigned discards[static_cast<int>(DiscardReason::kCount)] = {};
	unsigned skips[static_cast<int>(SkipReason::kCount)] = {};
	unsigned prog_restore_mismatch = 0;
	unsigned tex_bind_calls = 0, tex_restore_mismatch = 0;
	unsigned tex1_restore_mismatch = 0, tev1_not_init = 0, unit1_left_bound = 0;   // M3
	unsigned blend_sets = 0, blend_restores = 0;   // M4: tinted/world blend switches between draws; restores
	unsigned static_sets = 0, static_restores = 0, static_frames_ended = 0;   // M5: Lit/Static switches
	unsigned direct_frames = 0, seeds = 0, syncs = 0, seed_checks = 0;   // M6
	// M9: view frames begun for two eyes, passes drawn (all, the streamed one included), replayed draws, the draw list's
	// high-water mark, replay discards by reason, right-texture clears
	unsigned stereo_frames = 0, passes = 0, replay_draws = 0, recs_max = 0, recs_cap = 0, r_clears = 0;
	unsigned replay_discards[static_cast<int>(DiscardReason::kCount)] = {};
} s_n;

// M9 "The draw list and the passes": one record per issued draw, appended by issue_pending on every frame; the other
// passes of a stereo frame replay it. uint32_t: ring_indices may be 65 536. The record is sufficient: the LUT, the noise
// texture, TEV, unit 1 and the alpha test are per frame or rebuilt from `mode`, and the ring half is not rewritten before
// C3D_FrameEnd.
struct DrawRec
{
	uint32_t first, count;
	void* tex;           // the texture bound on unit 0 for the draw, or null (none bound yet: unmasked static, textures 0)
	unsigned blend, mode;
};
DrawRec* s_recs = nullptr;   // malloc, capacity ring_indices / 3 (a draw has at least 3 indices: the list cannot fill first)
unsigned s_recs_cap = 0, s_nrecs = 0;

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
	// M9: no stereo uniform, no stereo frames (the right-eye objects stay allocated and are never drawn).
	if (s_uloc_stereo < 0)
		stereo_turn_off(StereoOff::Uniform);
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
	s_recs_cap = s_n.recs_cap = s_ring_i / 3;
	s_recs = static_cast<DrawRec*>(malloc(static_cast<size_t>(s_recs_cap) * sizeof(DrawRec)));
	if (!s_vbo || !s_ibo || !s_recs)
	{
		go_off("no memory for the rings and the draw list");
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
	// M9: the record of this draw (on every frame: one code path; cheap). Full: given up like a full ring.
	if (s_nrecs >= s_recs_cap)
	{
		discard(DiscardReason::Ring);
		return;
	}
	s_recs[s_nrecs++] = DrawRec{s_f.pfirst, s_f.pcount, s_f.bound_tex, s_f.blend, s_f.mode};
	if (s_nrecs > s_n.recs_max)
		s_n.recs_max = s_nrecs;
	const uint32_t* const a = cmd_pos();
	// base.h:24; the indices are relative to the half's vertex start (BufInfo_Add of the half).
	C3D_DrawElements(GPU_TRIANGLES, static_cast<int>(s_f.pcount), C3D_UNSIGNED_SHORT,
	                 ibo_half(s_f.half) + s_f.pfirst);
	const uint32_t* const b = cmd_pos();
	cmdbuf_note_draw(b > a ? static_cast<uint32_t>(b - a) : 0);
	++s_n.draws;
	s_f.drew = true;
}

// M4 (design 7, M4 specification "Tinted", "State and its restore"): a draw's blend follows its class.
// 2.4's world blend (ONE, ZERO), or for kTinted dst x c/255 on RGB, dst alpha kept: C3D_AlphaBlend(ADD, ADD, ZERO,
// CONSTANT_COLOR, ZERO, ONE) with C3D_BlendingColor (effect.h:8, :12; enums.h:247-257; the same factor on the three
// channels, so the register's byte order does not matter). c = 8k, k = transfer_data & 0xff (SW's tint table
// k << 3 of 256, low_level_textures.h:504, shapes.cpp:2006-2021); the front end refuses k >= 32 (rect_support), the
// clamp to 255 is only a guard.
unsigned blend_of(const DrawKey& key)
{
	if (key.cls != kTinted)
		return 0;
	const unsigned k = static_cast<unsigned>(key.transfer_data) & 0xFFu;
	const unsigned c = 8 * k > 255 ? 255 : 8 * k;
	return 0x100u | c;
}

void set_blend(unsigned blend)
{
	if (blend == s_f.blend)
		return;
	if (blend)
	{
		const u32 c = blend & 0xFFu;
		C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ZERO, GPU_CONSTANT_COLOR, GPU_ZERO, GPU_ONE);
		C3D_BlendingColor(0xFF000000u | (c << 16) | (c << 8) | c);
		s_f.tinted_used = true;
	}
	else
		C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
	s_f.blend = blend;
	++s_n.blend_sets;
}

// M3 "TEV" (design 4.2; enums.h:331-383), stages 0 and 1 of a lit frame. Stage 0: x = A0 * (1 - L) (the texel's
// alpha, 128 for self-luminous, times one minus the LUT's level), alpha = the texel's (x2 only with alpha_x2: the
// UNVERIFIED clamp). Stage 1: c * (1 - x): c * L for A 255, c * (1/2 + L/2) for A 128. view_begin, and (M5) the
// switch back from a static draw.
void lit_tev()
{
	C3D_TexEnv* env = C3D_GetTexEnv(0);
	C3D_TexEnvInit(env);
	C3D_TexEnvSrc(env, C3D_RGB, GPU_TEXTURE0, GPU_TEXTURE1, GPU_PRIMARY_COLOR);
	C3D_TexEnvOpRgb(env, GPU_TEVOP_RGB_SRC_ALPHA, GPU_TEVOP_RGB_ONE_MINUS_SRC_R, GPU_TEVOP_RGB_SRC_COLOR);
	C3D_TexEnvFunc(env, C3D_RGB, GPU_MODULATE);
	C3D_TexEnvSrc(env, C3D_Alpha, GPU_TEXTURE0, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
	C3D_TexEnvOpAlpha(env, GPU_TEVOP_A_SRC_ALPHA, GPU_TEVOP_A_SRC_ALPHA, GPU_TEVOP_A_SRC_ALPHA);
	C3D_TexEnvFunc(env, C3D_Alpha, GPU_REPLACE);
	if (config().alpha_x2)
		C3D_TexEnvScale(env, C3D_Alpha, GPU_TEVSCALE_2);
	C3D_TexEnv* env1 = C3D_GetTexEnv(1);
	C3D_TexEnvInit(env1);
	C3D_TexEnvSrc(env1, C3D_RGB, GPU_TEXTURE0, GPU_PREVIOUS, GPU_PRIMARY_COLOR);
	C3D_TexEnvOpRgb(env1, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_ONE_MINUS_SRC_COLOR, GPU_TEVOP_RGB_SRC_COLOR);
	C3D_TexEnvFunc(env1, C3D_RGB, GPU_MODULATE);
	C3D_TexEnvSrc(env1, C3D_Alpha, GPU_PREVIOUS, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
	C3D_TexEnvOpAlpha(env1, GPU_TEVOP_A_SRC_ALPHA, GPU_TEVOP_A_SRC_ALPHA, GPU_TEVOP_A_SRC_ALPHA);
	C3D_TexEnvFunc(env1, C3D_Alpha, GPU_REPLACE);
}

// M5 (design 7, M5 specification "Static on the GPU", "Unit 1 and TEV, the state machine"): a draw's mode follows
// its key. kStatic: the noise on unit 1, its colour, the alpha test GEQUAL ref; the alpha is the noise's, times the
// mask's (A8 0/255 on unit 0) when the draw is masked (a transparent bitmap: `tex` given).
unsigned mode_of(const DrawKey& key, bool masked)
{
	if (key.cls != kStatic)
		return 0;
	return 0x10000u | (masked ? 0x100u : 0u) | noise_ref(static_cast<uint16_t>(key.transfer_data));
}

// Between draws (after issue_pending), as set_blend. False: the noise has no memory (the caller discards tex).
bool set_mode(unsigned mode)
{
	if (mode == s_f.mode)
		return true;
	if (mode)
	{
		if (!s_f.mode)
		{
			void* noise = noise_get();
			if (!noise)
				return false;
			__real_C3D_TexBind(1, static_cast<C3D_Tex*>(noise));
			// Stage 1 passes stage 0 on.
			C3D_TexEnv* env1 = C3D_GetTexEnv(1);
			C3D_TexEnvInit(env1);
			C3D_TexEnvSrc(env1, C3D_Both, GPU_PREVIOUS, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
			C3D_TexEnvFunc(env1, C3D_Both, GPU_REPLACE);
			++s_n.static_sets;
		}
		// Stage 0: RGB = the noise texel's (SW's seed ^ seed << 8); alpha = the noise's (A = hi + 1), masked or not.
		C3D_TexEnv* env = C3D_GetTexEnv(0);
		C3D_TexEnvInit(env);
		C3D_TexEnvSrc(env, C3D_RGB, GPU_TEXTURE1, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
		C3D_TexEnvFunc(env, C3D_RGB, GPU_REPLACE);
		if (mode & 0x100u)
		{
			C3D_TexEnvSrc(env, C3D_Alpha, GPU_TEXTURE0, GPU_TEXTURE1, GPU_PRIMARY_COLOR);
			C3D_TexEnvFunc(env, C3D_Alpha, GPU_MODULATE);
		}
		else
		{
			C3D_TexEnvSrc(env, C3D_Alpha, GPU_TEXTURE1, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
			C3D_TexEnvFunc(env, C3D_Alpha, GPU_REPLACE);
		}
		C3D_AlphaTest(true, GPU_GEQUAL, static_cast<int>(mode & 0xFFu));
	}
	else
	{
		__real_C3D_TexBind(1, static_cast<C3D_Tex*>(s_f.lut));
		lit_tev();
		C3D_AlphaTest(true, GPU_GEQUAL, 128);
		++s_n.static_restores;
	}
	s_f.mode = mode;
	return true;
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
	// M2 "Texture binding and its restore": unit 0 back to the present's texture, if this frame bound one.
	// A mismatch: no texture recorded (nothing to restore), or a recorded bind happened during the frame.
	if (s_f.bound_tex)
	{
		C3D_Tex* const present_tex = static_cast<C3D_Tex*>(tex_recorded());
		__real_C3D_TexBind(0, present_tex);
		s_f.bound_tex = nullptr;
		if (!present_tex || tex_binds() != s_f.tex_binds0)
			++s_n.tex_restore_mismatch;
	}
	// M3 "Binding and the state the present relies on": TEV stage 1 back (copy semantics: texenv.o
	// C3D_SetTexEnv+0x14-0x58), checked against C3D_Init's pass-through (base.o C3D_Init+0x15c-0x178); unit 1
	// back to no texture (shade_unbind_unit1: C3D_TexBind(1, NULL) would dereference NULL + 12); no one but
	// back/ may bind it.
	if (s_f.lit)
	{
		// M5: a frame ending on a static draw holds the noise on unit 1: the LUT goes back first, so whatever follows
		// (the unbind, or the layout fallback that leaves unit 1 bound) sees what an M3 frame leaves.
		if (s_f.mode)
		{
			__real_C3D_TexBind(1, static_cast<C3D_Tex*>(s_f.lut));
			s_f.mode = 0;
			++s_n.static_frames_ended;
		}
		C3D_SetTexEnv(1, &s_saved_env1);
		C3D_TexEnv init;
		C3D_TexEnvInit(&init);
		if (memcmp(&init, &s_saved_env1, sizeof init) != 0)
			++s_n.tev1_not_init;
		if (tex1_foreign_binds() != 0)
			++s_n.tex1_restore_mismatch;
		if (!shade_unbind_unit1())
			++s_n.unit1_left_bound;
		s_f.lit = false;
	}
	// M4: the world blend and blend colour 0 (C3D_Init's: base.o C3D_Init never stores the context's +0x100) after
	// a frame that drew tinted rectangles; the present sets its own blend before every draw (present.cpp:339-340).
	if (s_f.tinted_used)
	{
		C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
		C3D_BlendingColor(0);
		s_f.blend = 0;
		s_f.tinted_used = false;
		++s_n.blend_restores;
	}
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
	static const char* const names[] = {"down", "gate", "size", "pref"};
	return names[static_cast<int>(r)];
}

const char* discard_name(DiscardReason r)
{
	static const char* const names[] = {"unsupported", "ring", "cmdbuf", "gx", "begin", "tex"};
	return names[static_cast<int>(r)];
}

ViewBegin view_skip(SkipReason reason)
{
	return skipped(reason);
}

bool view_play_ready(int w, int h, SkipReason* why)
{
	SkipReason r = SkipReason::Down;
	bool ok = false;
	if (s_off || !is_up() || release_pending() || !program_recorded())
		r = SkipReason::Down;
	else if (gate_closed())
		r = SkipReason::Gate;
	else if (!view_size_supported(w, h, 8) || view_size_failed(w, h))
		r = SkipReason::Size;
	else
		ok = true;
	if (!ok && why)
		*why = r;
	return ok;
}

ViewBegin view_begin(int w, int h, uint32_t clear_rgba, const ViewOptions& opt)
{
	s_f = Frame();
	s_nrecs = 0;
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

	// M9: a stereo frame needs the right-eye objects, which view_ensure has just made (or failed to: stereo is then off for
	// the run and the frame is flat). The uniform's values for the streamed pass are set below.
	s_f.stereo = opt.stereo && stereo_usable() && view_texture_right() != nullptr;
	s_f.eye_p = opt.P;
	s_f.eye_w0 = opt.w0;
	s_f.eye_near = opt.near;
	s_f.fill = opt.fill;
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
	// never wrote is recognisable (the sentinel's RGB with alpha 0x01, which no M2/M3 draw writes: textured
	// pixels can have any RGB). The colour hash never produces the sentinel's RGB (shape_colour).
	++s_k;
	s_f.w = w;
	s_f.h = h;
	s_f.sentinel_argb = rgba_to_argb_keep(view_sentinel_rgba(s_k));
	// M6: a seeded frame's staging holds the seed; a direct frame reads nothing back.
	if (opt.seed_rows)
		view_seed_fill(opt.seed_rows);
	else if (opt.sentinel)
		view_fill_staging(view_sentinel_rgba(s_k));
	++s_n.frames;

	const FrameStart fs = frame_begin(FrameZone::Wait);
	if (fs != FrameStart::Ok)
		return discarded_at_begin(DiscardReason::Begin);
	s_f.open = true;
	s_f.tex_binds0 = tex_binds();

	// M6 seed: the view the texture must start from, as the frame's first GX operation (empty command buffer: 1 entry).
	if (opt.seed_rows)
	{
		if (!view_seed_transfer())
		{
			frame_end(0, false, FrameKind::View);
			note_view_frame();
			s_f.open = false;
			return discarded_at_begin(DiscardReason::Gx);
		}
		++s_n.seeds;
	}

	// M9 "What the two textures hold": the right texture is cleared to opaque black on a stereo frame when the previous
	// world frame was not one, or it was just created / the view changed size; else it keeps its last picture. One GX
	// entry, with the other grouped operations right after frame_begin (the command buffer is still empty). It comes
	// BEFORE the shade LUT and the texture promotions (code review of commit 1): under a tight budget (the software
	// backend's gxq_budget 3) a refused promotion is retried next frame, a refused clear would discard the first stereo
	// frame after a map or terminal. stereo_gaps (test only): cleared to magenta on every stereo frame instead.
	if (s_f.stereo)
	{
		const bool fresh = view_right_take_fresh();
		if (opt.gaps || opt.clear_right || fresh)
		{
			if (!view_clear_target_right(opt.gaps ? 0xFF00FFFFu : 0x000000FFu))
			{
				frame_end(0, false, FrameKind::View);
				note_view_frame();
				s_f.open = false;
				return discarded_at_begin(DiscardReason::Gx);
			}
			++s_n.r_clears;
		}
	}

	// M3: the shade LUT first (built once; its one promotion, before the cache's, with the command buffer
	// still empty), then the texture cache's frame: what retired frames left is freed now (frame_begin
	// returned), then this frame's VRAM promotions, before the clear (both cost 1 GX entry with an empty
	// command buffer, 2.4).
	const bool lit = config().textures && config().lit && !opt.flat;
	if (lit)
	{
		s_f.lut = shade_frame_start();
		if (!s_f.lut)
		{
			frame_end(0, false, FrameKind::View);
			note_view_frame();
			s_f.open = false;
			return discarded_at_begin(DiscardReason::Tex);
		}
	}
	if (lit)
		noise_frame_start();
	texcache_frame_start(s_k);

	// The clear, only before the first draw (C3D_FrameBufClear is a GX_MemoryFill without a split, so
	// after draws it would run before them), to 0x00000000: alpha 0 is "not covered" (M4: the tinted check's grey,
	// 8.1).
	const bool gaps_left = s_f.stereo && opt.gaps;
	if ((opt.clear || gaps_left) && !view_clear_target(gaps_left ? 0xFF00FFFFu : clear_rgba))
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

	// TEV stage 0, RGB and alpha (texenv.h:49-88): texture 0 with `textures 1` (M2 "View frame with
	// textures"; the bind comes with the first draw), else the vertex colour (M1; C3D_TexBind(0) is then not
	// touched). The present sets its own stage 0 in full before each draw.
	C3D_TexEnv* env = C3D_GetTexEnv(0);
	C3D_TexEnvInit(env);
	if (lit)
	{
		// M3 "TEV" (design 4.2; enums.h:331-383). Stage 0: x = A0 * (1 - L) (the texel's alpha, 128 for
		// self-luminous, times one minus the LUT's level), alpha = the texel's (x2 only with alpha_x2: the
		// UNVERIFIED clamp). Stage 1: c * (1 - x): c * L for A 255, c * (1/2 + L/2) for A 128.
		// Stage 1 is saved first, after the SAVE above (so the early returns leave nothing to restore): by copy, put
		// back by restore_state; both stages are then built by lit_tev (from C3D_TexEnvInit: scale 1).
		s_saved_env1 = *C3D_GetTexEnv(1);
		lit_tev();
		// Unit 1 = the LUT (texture unit 1 samples texcoord1: normal_mapping example, design 0.3); unbound again
		// by restore_state.
		__real_C3D_TexBind(1, static_cast<C3D_Tex*>(s_f.lut));
		shade_check_bound(s_f.lut);
		s_f.lit = true;
	}
	else
	{
		const GPU_TEVSRC src = config().textures && !opt.flat ? GPU_TEXTURE0 : GPU_PRIMARY_COLOR;
		C3D_TexEnvInit(env);
		C3D_TexEnvSrc(env, C3D_Both, src, src, src);
		C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
	}

	// Pixels, x right, y down (M0's calibrated value); after the bind, which reloads the constants.
	// M6: on a texture target the projection covers the whole target (view_proj_*), the view in its top-left corner.
	Mtx_Ortho(&s_proj, 0.0f, static_cast<float>(view_proj_width()), static_cast<float>(view_proj_height()), 0.0f, 0.0f,
	          1.0f, true);
	C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, s_uloc_proj, &s_proj);
	// M9: the streamed pass is the left eye: unshifted with fill (the centre pass), else shifted by -P. A flat frame sets
	// (0, 1, 0, 0), not all zeros: every term of the shader's sum stays finite.
	if (s_uloc_stereo >= 0)
	{
		if (s_f.stereo && !s_f.fill)
			C3D_FVUnifSet(GPU_VERTEX_SHADER, s_uloc_stereo, -s_f.eye_p, s_f.eye_w0, -s_f.eye_near, 0.0f);
		else
			C3D_FVUnifSet(GPU_VERTEX_SHADER, s_uloc_stereo, 0.0f, 1.0f, 0.0f, 0.0f);
	}
	s_n.stereo_frames += s_f.stereo;
	ViewBegin ok{ViewBegin::Kind::Ok, SkipReason::Down, DiscardReason::Unsupported};
	ok.stereo = s_f.stereo;
	return ok;
}

bool view_polygon(const Vertex* v, int n, const DrawKey& key, const TexSource* tex)
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
		set_blend(blend_of(key));
		const unsigned mode = mode_of(key, tex != nullptr);
		if (mode && !s_f.lit)
		{
			discard(DiscardReason::Unsupported);   // the front end refuses static without lit (M5 specification)
			return false;
		}
		if (!set_mode(mode))
		{
			discard(DiscardReason::Tex);
			return false;
		}
		if (tex)
		{
			void* t = nullptr;
			switch (texcache_get(*tex, &t))
			{
			case TexGet::Ok:
				break;
			case TexGet::Discard:
				discard(DiscardReason::Tex);
				return false;
			case TexGet::Unsupported:
				discard(DiscardReason::Unsupported);
				return false;
			}
			// After the previous draw was issued, before this one: citro3d reads the C3D_Tex at the next
			// draw's state flush (texture.o C3D_TexBind: pointer + dirty bit only).
			if (t != s_f.bound_tex)
			{
				__real_C3D_TexBind(0, static_cast<C3D_Tex*>(t));
				s_f.bound_tex = t;
				++s_n.tex_bind_calls;
			}
		}
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

bool view_ring_room(unsigned vertices, unsigned indices)
{
	return s_f.open && !s_f.discarded && s_ring.fits(vertices, indices);
}

void view_discard(DiscardReason reason)
{
	if (s_f.open)
		discard(reason);
}

namespace {

// view_end's common part: the pending draw, the restore, the flushes, the reserve test, then the readback (or the
// discarded end). ok: view_argb() holds the frame.
ViewEnd end_and_read_back()
{
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
	return ViewEnd{true, DiscardReason::Unsupported};
}

} // namespace

ViewEnd view_end(uint32_t* out_argb, int pitch, unsigned* sentinel_count)
{
	if (sentinel_count)
		*sentinel_count = 0;
	const ViewEnd e = end_and_read_back();
	if (!e.ok)
		return e;
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
	return e;
}

namespace {

ViewEnd play_copy(uint8_t* const* rows, unsigned* sentinel_count, unsigned* copied, bool full)
{
	if (sentinel_count)
		*sentinel_count = 0;
	if (copied)
		*copied = 0;
	const ViewEnd e = end_and_read_back();
	if (!e.ok)
		return e;
	if (full)
	{
		// M6: the texture target holds the whole view (never cleared in mode direct): every pixel, 5.2's form.
		const uint32_t* src = view_argb();
		for (int y = 0; y < s_f.h; ++y)
		{
			const uint32_t* row = src + static_cast<size_t>(y) * s_f.w;
			uint32_t* dst = reinterpret_cast<uint32_t*>(rows[y]);
			for (int x = 0; x < s_f.w; ++x)
				dst[x] = row[x] | 0xFF000000u;
		}
		if (copied)
			*copied = static_cast<unsigned>(s_f.w * s_f.h);
		return e;
	}
	const uint32_t* src = view_argb();
	unsigned sentinel = 0, n = 0;
	for (int y = 0; y < s_f.h; ++y)
	{
		const uint32_t* row = src + static_cast<size_t>(y) * s_f.w;
		uint32_t* dst = reinterpret_cast<uint32_t*>(rows[y]);
		for (int x = 0; x < s_f.w; ++x)
		{
			const uint32_t p = row[x];
			if (p == s_f.sentinel_argb)
				++sentinel;
			else if (p >> 24)
			{
				dst[x] = p | 0xFF000000u;   // 5.2: the form of SW's pixels
				++n;
			}
		}
	}
	if (sentinel_count)
		*sentinel_count = sentinel;
	if (copied)
		*copied = n;
	return e;
}

} // namespace

ViewEnd view_end_play(uint8_t* const* rows, unsigned* sentinel_count, unsigned* copied)
{
	return play_copy(rows, sentinel_count, copied, false);
}

ViewEnd view_end_play_full(uint8_t* const* rows, unsigned* sentinel_count, unsigned* copied)
{
	return play_copy(rows, sentinel_count, copied, true);
}

namespace {

// A replay pass gives the frame up: counted by reason (the first reason of the frame stays, as discard()).
void replay_discard(DiscardReason r)
{
	++s_n.replay_discards[static_cast<int>(r)];
	discard(r);
}

// One further pass of a stereo frame (M9 "The draw list and the passes"): switch to the eye's target, set the uniform
// (x = e * P pixels, w0, -near), and issue every record again with the state it was issued with. Draws only; the state
// the streamed pass left (program, attributes, buffers, TEV, the LUT on unit 1) is still in force. citro3d's state flush
// on the first draw writes the new target's frame buffer registers (the stereo examples draw both eyes in one frame:
// stereoscopic_2d/source/main.cpp:53-80).
void replay_pass(C3D_RenderTarget* target, float x)
{
	if (s_f.discarded)
		return;
	if (!C3D_FrameDrawOn(target))   // renderqueue.h:32
	{
		replay_discard(DiscardReason::Begin);
		return;
	}
	C3D_FVUnifSet(GPU_VERTEX_SHADER, s_uloc_stereo, x, s_f.eye_w0, -s_f.eye_near, 0.0f);
	for (unsigned i = 0; i < s_nrecs; ++i)
	{
		const DrawRec& r = s_recs[i];
		set_blend(r.blend);
		if (!set_mode(r.mode))
		{
			replay_discard(DiscardReason::Tex);
			return;
		}
		// null = keep the current bind: C3D_TexBind(n, NULL) dereferences it (an unmasked static draw and `textures 0`
		// record null)
		if (r.tex && r.tex != s_f.bound_tex)
		{
			__real_C3D_TexBind(0, static_cast<C3D_Tex*>(r.tex));
			s_f.bound_tex = r.tex;
			++s_n.tex_bind_calls;
		}
		if (!reserve_left())
		{
			replay_discard(DiscardReason::Cmdbuf);
			return;
		}
		const uint32_t* const a = cmd_pos();
		C3D_DrawElements(GPU_TRIANGLES, static_cast<int>(r.count), C3D_UNSIGNED_SHORT, ibo_half(s_f.half) + r.first);
		const uint32_t* const b = cmd_pos();
		cmdbuf_note_draw(b > a ? static_cast<uint32_t>(b - a) : 0);
		++s_n.draws;
		++s_n.replay_draws;
	}
	++s_n.passes;
}

// The passes after the streamed one: with fill: L shifted, R centre, R shifted; without: R shifted. (The streamed pass
// was L centre, or L shifted without fill.)
void replay_stereo()
{
#ifdef A1_PROFILE
	A1_PROF_SCOPE(C3dStereo);
#endif
	C3D_RenderTarget* const left = view_target();
	C3D_RenderTarget* const right = view_target_right();
	const float P = s_f.eye_p;
	if (s_f.fill)
	{
		replay_pass(left, -P);
		replay_pass(right, 0.0f);
	}
	replay_pass(right, P);
}

} // namespace

ViewEnd view_end_direct()
{
	if (!s_f.open)
		return ViewEnd{false, DiscardReason::Begin};
	issue_pending();
	if (s_f.stereo && !s_f.discarded)
	{
		++s_n.passes;   // the streamed pass
		replay_stereo();
	}
	restore_state();
	flush_rings();   // the draws run at C3D_FrameEnd(GX_CMDLIST_FLUSH), which skips the whole-heap flush (0.3)
	if (s_f.discarded)
		return finish_discarded();
	frame_end(0, false, FrameKind::View);
	note_view_frame();
	s_f.open = false;
	++s_n.direct_frames;
	return ViewEnd{true, DiscardReason::Unsupported};
}

// M9 part 2, stereo_cal (design 7, "M9 specification", "Tests", T1): the calibration quads through the whole stereo path (the
// streamed pass, the replayed passes, the composite). The left texture is cleared to opaque black by view_begin (opt.clear
// with the clear colour), the right by opt.clear_right.
ViewBegin view_stereo_calibrate(int w, int h, const ViewOptions& opt, ViewEnd* ve)
{
	ViewBegin vb = view_begin(w, h, 0x000000FFu, opt);
	if (vb.kind != ViewBegin::Kind::Ok)
		return vb;
	const DrawKey key{classify(Entry::Vertical, Transfer::Textured, false), nullptr, nullptr, 0, 0, 0};
	for (int q = 0; q < kStereoCalQuads && vb.stereo; ++q)
	{
		const float x0 = static_cast<float>(stereo_cal_col(q)), x1 = x0 + static_cast<float>(kStereoCalSize);
		const float y0 = static_cast<float>(kStereoCalRow), y1 = y0 + static_cast<float>(kStereoCalSize);
		const float qw = stereo_cal_w(q, opt.w0);
		const uint32_t rgb = stereo_cal_rgb(q);
		const float xs[4] = {x0, x1, x1, x0}, ys[4] = {y0, y0, y1, y1};
		Vertex v[4];
		for (int i = 0; i < 4; ++i)
			v[i] = Vertex{xs[i], ys[i], qw, 0.0f, 0.0f, 0.0f, 0.0f, static_cast<uint8_t>(rgb >> 16),
			              static_cast<uint8_t>(rgb >> 8), static_cast<uint8_t>(rgb), 0xFF};
		if (!view_polygon(v, 4, key, nullptr))
			break;
	}
	*ve = view_end_direct();
	return vb;
}

ViewEnd view_sync(uint8_t* const* rows)
{
	if (s_off || !is_up() || release_pending() || gate_closed() || !view_is_texture())
		return ViewEnd{false, DiscardReason::Begin};
	if (frame_begin(FrameZone::Wait) != FrameStart::Ok)
		return ViewEnd{false, DiscardReason::Begin};
	switch (view_readback(true))   // the split, the transfer, the frame's end, the retirement pair, the conversion
	{
	case Readback::GxRefused:
		return ViewEnd{false, DiscardReason::Gx};
	case Readback::WaitFailed:
		return ViewEnd{false, DiscardReason::Begin};
	case Readback::Ok:
		break;
	}
	const uint32_t* src = view_argb();
	const int w = view_width(), h = view_height();
	for (int y = 0; y < h; ++y)
	{
		const uint32_t* row = src + static_cast<size_t>(y) * w;
		uint32_t* dst = reinterpret_cast<uint32_t*>(rows[y]);
		for (int x = 0; x < w; ++x)
			dst[x] = row[x] | 0xFF000000u;
	}
	++s_n.syncs;
	return ViewEnd{true, DiscardReason::Unsupported};
}

bool view_seed_check(const uint8_t* const* rows, unsigned* differ)
{
	if (s_off || !is_up() || release_pending() || gate_closed() || !view_is_texture())
		return false;
	view_seed_fill(rows);
	if (frame_begin(FrameZone::Wait) != FrameStart::Ok)
		return false;
	if (!view_seed_transfer())
	{
		frame_end(0, false, FrameKind::View);
		note_view_frame();
		return false;
	}
	if (view_readback(true) != Readback::Ok)
		return false;
	const uint32_t* src = view_argb();
	const int w = view_width(), h = view_height();
	unsigned n = 0;
	for (int y = 0; y < h; ++y)
	{
		const uint32_t* a = src + static_cast<size_t>(y) * w;
		const uint32_t* b = reinterpret_cast<const uint32_t*>(rows[y]);
		for (int x = 0; x < w; ++x)
			n += ((a[x] ^ b[x]) & 0x00FFFFFFu) != 0;
	}
	*differ = n;
	++s_n.seed_checks;
	return true;
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
	free(s_recs);
	s_recs = nullptr;
	s_recs_cap = s_nrecs = 0;
	s_rings_ready = false;
	s_f = Frame();
}

int draw_stereo_text(char* buf, size_t size)
{
	const Counters& n = s_n;
	return snprintf(buf, size, "views=%u passes=%u replay_draws=%u recs_max=%u/%u replay_discards=%u/%u/%u/%u/%u/%u r_clears=%u",
	                n.stereo_frames, n.passes, n.replay_draws, n.recs_max, n.recs_cap, n.replay_discards[0],
	                n.replay_discards[1], n.replay_discards[2], n.replay_discards[3], n.replay_discards[4],
	                n.replay_discards[5], n.r_clears);
}

int draw_stats_text(char* buf, size_t size)
{
	const Counters& n = s_n;
	return snprintf(buf, size,
	                " views=%u draws=%u polys=%u verts=%u idx=%u ring_hw_v=%u,%u ring_hw_i=%u,%u "
	                "discards=%u/%u/%u/%u/%u/%u(unsup/ring/cmdbuf/gx/begin/tex) skips=%u/%u/%u/%u(down/gate/size/pref) "
	                "prog_restore_mismatch=%u tex_bind_calls=%u tex_restore_mismatch=%u lut=%s lut_builds=%u "
	                "tex1_restore_mismatch=%u tev1_not_init=%u unit1_left_bound=%u blend_sets=%u blend_restores=%u "
	                "static_sets=%u static_restores=%u static_frames_ended=%u noise=%s direct_views=%u seeds=%u syncs=%u "
	                "seed_checks=%u",
	                n.frames, n.draws, n.polygons, n.vertices, n.indices, s_ring.high_water_vertices(0),
	                s_ring.high_water_vertices(1), s_ring.high_water_indices(0), s_ring.high_water_indices(1),
	                n.discards[0], n.discards[1], n.discards[2], n.discards[3], n.discards[4], n.discards[5],
	                n.skips[0], n.skips[1], n.skips[2], n.skips[3], n.prog_restore_mismatch, n.tex_bind_calls,
	                n.tex_restore_mismatch, shade_state_name(), shade_builds(), n.tex1_restore_mismatch, n.tev1_not_init, n.unit1_left_bound,
	                n.blend_sets, n.blend_restores, n.static_sets, n.static_restores, n.static_frames_ended,
	                noise_state_name(), n.direct_frames, n.seeds, n.syncs, n.seed_checks);
}

} // namespace back
} // namespace a1_c3d
