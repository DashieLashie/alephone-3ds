/*
	Aleph One for the Nintendo 3DS, citro3d back end: the shade LUT's texture and (M5) the static noise's (see
	shade.h).

	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; either version 3 of the License, or
	(at your option) any later version.
*/
#include "shade.h"

#include "context.h"
#include "../core/noise.h"
#include "../core/shade_lut.h"
#include "../core/texcache.h"

#include <3ds.h>
#include <citro3d.h>

#include <stdint.h>
#include <stdio.h>

#ifdef A1_PROFILE
#include "a1_3ds_profile.h"
#endif

// back/context.cpp: -Wl,--wrap=C3D_TexBind. Unit 1 is bound only through the real one.
extern "C" void __real_C3D_TexBind(int unitId, C3D_Tex* tex);
// citro3d's context (base.o: a global object of 0x2d0 bytes in .bss; texture.o C3D_TexBind addresses it). Read
// and written through casts to other types: sound only because citro3d is a separate archive and the port
// builds without LTO (CLAUDE.md, "No LTO").
extern "C" uint8_t __C3D_Context[];

namespace a1_c3d {
namespace back {

namespace {

enum class State : uint8_t { None, Linear, Copying, Vram };

// M3's LUT lifecycle, generalised in M5 to the two textures that live outside the texture cache (design 7, M5
// specification "Static on the GPU"): built once into linear memory, promoted to VRAM in a later view frame by M2's
// in-frame TextureCopy, deleted by shade_free.
struct Resident
{
	const char* name;
	u16 side;
	GPU_TEXCOLOR fmt;
	GPU_TEXTURE_WRAP_PARAM wrap;
	size_t bytes;
	void (*build)(void* data);
	State state;
	// Static structs: citro3d keeps the pointer given to C3D_TexBind (texture.o C3D_TexBind+0x24-0x2c).
	C3D_Tex lin, vram;
	unsigned builds;
	bool logged_lin_fail, logged_vram_fail;
};

void build_lut(void* data) { shade_lut_build(static_cast<uint8_t*>(data)); }
void build_noise(void* data) { noise_build(data); }

// texture.h:94-98 (linear), :157-173: nearest; the LUT clamps (the texture clamp is SW's PIN), the noise repeats.
Resident s_lut = {"LUT", static_cast<u16>(kLutSide), GPU_L8, GPU_CLAMP_TO_EDGE, kLutBytes, build_lut, State::None, {}, {},
                  0, false, false};
Resident s_noise = {"noise", static_cast<u16>(kNoiseSide), GPU_RGBA8, GPU_REPEAT, kNoiseBytes, build_noise, State::None,
                    {}, {}, 0, false, false};
void* s_noise_frame = nullptr;   // what the noise is sampled from this frame (its frame start, or the build)

// texture.o C3D_TexBind (citro3d 1.7.1): flags at +0x20 (bit 0 = active), tex[unit] at (unit + 70) * 4, dirty
// bit 23 + unit.
constexpr size_t kCtxFlags = 0x20, kCtxTex0 = 0x118;
enum class Layout : uint8_t { Unknown, Ok, Bad };
Layout s_layout = Layout::Unknown;
uint32_t* ctx_flags() { return reinterpret_cast<uint32_t*>(__C3D_Context + kCtxFlags); }
void** ctx_tex(int unit) { return reinterpret_cast<void**>(__C3D_Context + kCtxTex0 + 4 * unit); }

// None -> Linear: the texture in linear memory, built and flushed. nullptr: no memory (logged once).
void* resident_build(Resident& r)
{
	if (!C3D_TexInit(&r.lin, r.side, r.side, r.fmt) || r.lin.size != r.bytes)
	{
		if (r.lin.data)
			C3D_TexDelete(&r.lin);
		if (!r.logged_lin_fail)
		{
			r.logged_lin_fail = true;
			printf("[c3d] shade: no linear memory for the %s: the frame is discarded (tex)\n", r.name);
		}
		return nullptr;
	}
	C3D_TexSetFilter(&r.lin, GPU_NEAREST, GPU_NEAREST);
	C3D_TexSetWrap(&r.lin, r.wrap, r.wrap);
	r.build(r.lin.data);
	// gspgpu.h: exactly the texture's bytes, before the GPU reads them (C3D_FrameEnd(GX_CMDLIST_FLUSH) skips the
	// whole-heap flush, design 0.3).
	GSPGPU_FlushDataCache(r.lin.data, r.lin.size);
	++r.builds;
	r.state = State::Linear;
	printf("[c3d] shade: %s built (linear, %lu B)\n", r.name, static_cast<unsigned long>(r.lin.size));
	return &r.lin;
}

// A view frame's start, after frame_begin() returned and before the cache's promotions and the clear (the promotion
// needs an empty command buffer). Returns what the texture is sampled from this frame.
void* resident_frame_start(Resident& r)
{
	switch (r.state)
	{
	case State::None:
		return nullptr;
	case State::Vram:
		return &r.vram;
	case State::Copying:
		// frame_begin() returned: the frame that queued the copy has retired (design 0.3). With the layout
		// confirmed unit 1 is already NULL between frames (shade_unbind_unit1), and binding here would leave it
		// bound into the present if view_begin discarded before its lit block (critic, post-review minor 1); in
		// the fallback it still holds the LUT's linear copy, so the LUT is re-bound before the delete (a frame never
		// ends with the noise bound: back/draw.cpp restore_state re-binds the LUT after a static draw).
		if (s_layout != Layout::Ok && &r == &s_lut)
			__real_C3D_TexBind(1, &r.vram);
		C3D_TexDelete(&r.lin);
		r.state = State::Vram;
		printf("[c3d] shade: %s promoted to VRAM\n", r.name);
		return &r.vram;
	case State::Linear:
	{
		// M2's promotion rule (core/texcache.cpp next_promotion): the entry only if the frame's own entries
		// (clear, flushing split, readback) still fit after it. Before the cache's promotions and the clear,
		// so the command buffer is empty and the copy costs one entry.
		GxQueue& gx = gx_queue();
		if (gx.used() + 1 + (gpuCmdBufOffset != 0 ? 1 : 0) + kViewReserveEntries > gx.budget())
			return &r.lin;
		if (!C3D_TexInitVRAM(&r.vram, r.side, r.side, r.fmt))
		{
			if (!r.logged_vram_fail)
			{
				r.logged_vram_fail = true;
				printf("[c3d] shade: no VRAM for the %s: sampled from linear memory\n", r.name);
			}
			return &r.lin;
		}
		C3D_TexSetFilter(&r.vram, GPU_NEAREST, GPU_NEAREST);
		C3D_TexSetWrap(&r.vram, r.wrap, r.wrap);
		// M2's verified promotion (back/texcache.cpp texcache_frame_start): citro3d's own VRAM upload, a raw copy
		// (texture.o C3D_TexLoadImage+0x60-0x7c), queued in frame; this frame's draws still sample the linear copy.
		Resident* rp = &r;
		if (!gx_op([rp] {
			    C3D_SyncTextureCopy(static_cast<u32*>(rp->lin.data), 0, static_cast<u32*>(rp->vram.data), 0,
			                        rp->lin.size, GX_TRANSFER_RAW_COPY(1));
		    }))
		{
			C3D_TexDelete(&r.vram);
			return &r.lin;
		}
		r.state = State::Copying;
		return &r.lin;
	}
	}
	return nullptr;
}

void resident_free(Resident& r)
{
	if (r.state == State::Linear || r.state == State::Copying)
		C3D_TexDelete(&r.lin);
	if (r.state == State::Copying || r.state == State::Vram)
		C3D_TexDelete(&r.vram);
	r.state = State::None;
}

} // namespace

void* shade_frame_start()
{
#ifdef A1_PROFILE
	::a1_3ds::prof::Scope prof_scope(::a1_3ds::prof::C3dTex);
#endif
	if (s_lut.state == State::None)
		return resident_build(s_lut);
	return resident_frame_start(s_lut);
}

void noise_frame_start()
{
	s_noise_frame = nullptr;
	if (s_noise.state == State::None)
		return;
#ifdef A1_PROFILE
	::a1_3ds::prof::Scope prof_scope(::a1_3ds::prof::C3dTex);
#endif
	s_noise_frame = resident_frame_start(s_noise);
}

void* noise_get()
{
	if (s_noise_frame)
		return s_noise_frame;
	if (s_noise.state != State::None)
		return nullptr;   // never: noise_frame_start set it for a built noise
#ifdef A1_PROFILE
	::a1_3ds::prof::Scope prof_scope(::a1_3ds::prof::C3dTex);
#endif
	s_noise_frame = resident_build(s_noise);
	return s_noise_frame;
}

void shade_check_bound(const void* lut)
{
	if (s_layout != Layout::Unknown)
		return;
	const bool ok = *ctx_tex(1) == lut && (*ctx_flags() & 1u) && (*ctx_flags() & (1u << 24));
	s_layout = ok ? Layout::Ok : Layout::Bad;
	printf("[c3d] shade: citro3d context layout %s (unit 1 slot at +0x%x); unit 1 is %s after each lit frame\n",
	       ok ? "confirmed" : "NOT as expected", static_cast<unsigned>(kCtxTex0 + 4),
	       ok ? "unbound" : "left bound to the LUT");
}

void* shade_lut_bound()
{
	switch (s_lut.state)
	{
	case State::None:
		return nullptr;
	case State::Vram:
		return &s_lut.vram;
	case State::Linear:
	case State::Copying:
		break;
	}
	return &s_lut.lin;
}

bool shade_unbind_unit1()
{
	if (s_layout != Layout::Ok || !(*ctx_flags() & 1u))
		return false;
	*ctx_tex(1) = nullptr;
	*ctx_flags() |= 1u << 24;
	return true;
}

void shade_free()
{
	resident_free(s_lut);
	resident_free(s_noise);
	s_noise_frame = nullptr;
}

const char* shade_state_name()
{
	static const char* const names[] = {"none", "linear", "copying", "vram"};
	return names[static_cast<int>(s_lut.state)];
}

const char* noise_state_name()
{
	static const char* const names[] = {"none", "linear", "copying", "vram"};
	return names[static_cast<int>(s_noise.state)];
}

unsigned shade_builds()
{
	return s_lut.builds;
}

} // namespace back
} // namespace a1_c3d
