/*
	Aleph One for the Nintendo 3DS, citro3d back end: the shade LUT's texture (see shade.h).

	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; either version 3 of the License, or
	(at your option) any later version.
*/
#include "shade.h"

#include "context.h"
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
State s_state = State::None;
// Static structs: citro3d keeps the pointer given to C3D_TexBind (texture.o C3D_TexBind+0x24-0x2c).
C3D_Tex s_lin, s_vram;
unsigned s_builds = 0;
bool s_logged_lin_fail = false, s_logged_vram_fail = false;

// texture.o C3D_TexBind (citro3d 1.7.1): flags at +0x20 (bit 0 = active), tex[unit] at (unit + 70) * 4, dirty
// bit 23 + unit.
constexpr size_t kCtxFlags = 0x20, kCtxTex0 = 0x118;
enum class Layout : uint8_t { Unknown, Ok, Bad };
Layout s_layout = Layout::Unknown;

uint32_t* ctx_flags() { return reinterpret_cast<uint32_t*>(__C3D_Context + kCtxFlags); }
void** ctx_tex(int unit) { return reinterpret_cast<void**>(__C3D_Context + kCtxTex0 + 4 * unit); }

} // namespace

void* shade_frame_start()
{
#ifdef A1_PROFILE
	::a1_3ds::prof::Scope prof_scope(::a1_3ds::prof::C3dTex);
#endif
	switch (s_state)
	{
	case State::Vram:
		return &s_vram;
	case State::Copying:
		// frame_begin() returned: the frame that queued the copy has retired (design 0.3). With the layout
		// confirmed unit 1 is already NULL between frames (shade_unbind_unit1), and binding here would leave it
		// bound into the present if view_begin discarded before its lit block (critic, post-review minor 1); in
		// the fallback it still holds the linear copy, so it is re-bound before the delete.
		if (s_layout != Layout::Ok)
			__real_C3D_TexBind(1, &s_vram);
		C3D_TexDelete(&s_lin);
		s_state = State::Vram;
		printf("[c3d] shade: LUT promoted to VRAM\n");
		return &s_vram;
	case State::None:
		// texture.h:94-98 (linear), :157-173: nearest (one level per texel), clamp (the texture clamp is SW's PIN).
		if (!C3D_TexInit(&s_lin, kLutSide, kLutSide, GPU_L8) || s_lin.size != kLutBytes)
		{
			if (s_lin.data)
				C3D_TexDelete(&s_lin);
			if (!s_logged_lin_fail)
			{
				s_logged_lin_fail = true;
				printf("[c3d] shade: no linear memory for the LUT: the check is discarded (tex)\n");
			}
			return nullptr;
		}
		C3D_TexSetFilter(&s_lin, GPU_NEAREST, GPU_NEAREST);
		C3D_TexSetWrap(&s_lin, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
		shade_lut_build(static_cast<uint8_t*>(s_lin.data));
		// gspgpu.h: exactly the LUT's bytes, before the GPU reads them (C3D_FrameEnd(GX_CMDLIST_FLUSH) skips the
		// whole-heap flush, design 0.3).
		GSPGPU_FlushDataCache(s_lin.data, s_lin.size);
		++s_builds;
		s_state = State::Linear;
		printf("[c3d] shade: LUT built (linear, %lu B)\n", static_cast<unsigned long>(s_lin.size));
		return &s_lin;
	case State::Linear:
	{
		// M2's promotion rule (core/texcache.cpp next_promotion): the entry only if the frame's own entries
		// (clear, flushing split, readback) still fit after it. Before the cache's promotions and the clear,
		// so the command buffer is empty and the copy costs one entry.
		GxQueue& gx = gx_queue();
		if (gx.used() + 1 + (gpuCmdBufOffset != 0 ? 1 : 0) + kViewReserveEntries > gx.budget())
			return &s_lin;
		if (!C3D_TexInitVRAM(&s_vram, kLutSide, kLutSide, GPU_L8))
		{
			if (!s_logged_vram_fail)
			{
				s_logged_vram_fail = true;
				printf("[c3d] shade: no VRAM for the LUT: sampled from linear memory\n");
			}
			return &s_lin;
		}
		C3D_TexSetFilter(&s_vram, GPU_NEAREST, GPU_NEAREST);
		C3D_TexSetWrap(&s_vram, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
		// M2's verified promotion (back/texcache.cpp texcache_frame_start): citro3d's own VRAM upload, a raw copy
		// (texture.o C3D_TexLoadImage+0x60-0x7c), queued in frame; this frame's draws still sample the linear copy.
		if (!gx_op([] {
			    C3D_SyncTextureCopy(static_cast<u32*>(s_lin.data), 0, static_cast<u32*>(s_vram.data), 0, s_lin.size,
			                        GX_TRANSFER_RAW_COPY(1));
		    }))
		{
			C3D_TexDelete(&s_vram);
			return &s_lin;
		}
		s_state = State::Copying;
		return &s_lin;
	}
	}
	return nullptr;
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
	if (s_state == State::Linear || s_state == State::Copying)
		C3D_TexDelete(&s_lin);
	if (s_state == State::Copying || s_state == State::Vram)
		C3D_TexDelete(&s_vram);
	s_state = State::None;
}

const char* shade_state_name()
{
	static const char* const names[] = {"none", "linear", "copying", "vram"};
	return names[static_cast<int>(s_state)];
}

unsigned shade_builds()
{
	return s_builds;
}

} // namespace back
} // namespace a1_c3d
