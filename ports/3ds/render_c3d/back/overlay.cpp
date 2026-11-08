/*
	Aleph One for the Nintendo 3DS, citro3d back end: the overlay surface and texture of direct mode (see overlay.h).

	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; either version 3 of the License, or
	(at your option) any later version.
*/
#include "overlay.h"

#include "context.h"

#include <3ds.h>
#include <SDL2/SDL.h>
#include <citro3d.h>

#include <stdio.h>
#include <string.h>

namespace a1_c3d {
namespace back {

namespace {

constexpr int kW = 400, kH = 240;              // the surface the engine draws into
constexpr int kTexW = 512, kTexH = 256;        // the buffer and the texture
constexpr int kPitch = kTexW * 4;              // bytes
constexpr size_t kBytes = static_cast<size_t>(kPitch) * kTexH;

// Linear (row 0 = the top) -> tiled, flipped, as the present's upload (present.cpp kUploadTransferFlags).
constexpr u32 kBandFlags =
	GX_TRANSFER_FLIP_VERT(1) | GX_TRANSFER_OUT_TILED(1) | GX_TRANSFER_RAW_COPY(0) |
	GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGBA8) |
	GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO);

u32* s_buf = nullptr;
SDL_Surface* s_surf = nullptr;
C3D_Tex s_tex;
bool s_tex_ready = false, s_tex_linear = false;
bool s_failed = false;                     // creation failed once: not retried
uint32_t s_dirty = 0;                      // tiles written since they were last cleared
Band s_bands[kMaxBands];
int s_nb = 0;
bool s_handed = false;
unsigned s_bottom_pending = 0;

struct Counters
{
	unsigned handouts = 0, uploads = 0, bands = 0, tile_clears = 0, refused = 0, dropped = 0, leaks = 0, audits = 0;
} s_n;

bool ensure()
{
	if (s_surf && s_tex_ready)
		return true;
	if (s_failed)
		return false;
	s_buf = static_cast<u32*>(linearAlloc(kBytes));
	if (s_buf)
	{
		memset(s_buf, 0, kBytes);
		GSPGPU_FlushDataCache(s_buf, kBytes);
		// SDL_PIXELFORMAT_RGBA8888: the 32-bit word 0xRRGGBBAA, the GPU's RGBA8 texel. Blend mode NONE: the surface is a
		// destination only.
		s_surf = SDL_CreateRGBSurfaceWithFormatFrom(s_buf, kW, kH, 32, kPitch, SDL_PIXELFORMAT_RGBA8888);
		if (s_surf)
			SDL_SetSurfaceBlendMode(s_surf, SDL_BLENDMODE_NONE);
	}
	if (s_surf)
	{
		s_tex_ready = C3D_TexInitVRAM(&s_tex, kTexW, kTexH, GPU_RGBA8);
		if (!s_tex_ready)
		{
			s_tex_ready = s_tex_linear = C3D_TexInit(&s_tex, kTexW, kTexH, GPU_RGBA8);
			if (s_tex_linear)
				printf("[c3d] overlay: no VRAM for the 512 KiB overlay texture: linear texture\n");
		}
		if (s_tex_ready)
		{
			C3D_TexSetFilter(&s_tex, GPU_NEAREST, GPU_NEAREST);
			C3D_TexSetWrap(&s_tex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
		}
	}
	if (!s_surf || !s_tex_ready)
	{
		printf("[c3d] overlay: creation failed (buffer %p, surface %p, texture %d): direct frames off\n",
		       static_cast<void*>(s_buf), static_cast<void*>(s_surf), s_tex_ready ? 1 : 0);
		overlay_free();
		s_failed = true;
		return false;
	}
	printf("[c3d] overlay: 400x240 RGBA8888 surface on a 512x256 linear buffer (%u KiB), texture in %s\n",
	       static_cast<unsigned>(kBytes / 1024), s_tex_linear ? "linear memory" : "VRAM");
	fflush(stdout);
	return true;
}

void clear_tiles(uint32_t tiles)
{
	for (int t = 0; t < kTexH / kBandTile; ++t)
		if (tiles >> t & 1)
		{
			for (int y = t * kBandTile; y < (t + 1) * kBandTile; ++y)   // columns 0-399: SDL never writes 400-511
				memset(s_buf + static_cast<size_t>(y) * kTexW, 0, static_cast<size_t>(kW) * 4);
			++s_n.tile_clears;
		}
}

} // namespace

SDL_Surface* overlay_begin(const Band* bands, int n, int cal_row)
{
	if (!ensure())
		return nullptr;
	if (cal_row >= 0)
	{
		const Band b{cal_row, cal_row + kBandTile};
		s_nb = bands_normalise(&b, 1, kH, s_bands);
	}
	else
		s_nb = bands_normalise(bands, n, kH, s_bands);
	const uint32_t cur = bands_tile_mask(s_bands, s_nb);
		clear_tiles(s_dirty);   // every dirty tile: the surface is transparent outside the bands (M6 spec, critic round 1)
	s_dirty = cur;
	if (cal_row >= 0)
		for (int y = s_bands[0].y0; y < s_bands[0].y1; ++y)
			for (int x = 0; x < kW; ++x)
				s_buf[static_cast<size_t>(y) * kTexW + x] = 0xFFFFFFFFu;
	s_handed = true;
	++s_n.handouts;
	if (gspIsPresentPending(GSP_SCREEN_BOTTOM))
		++s_bottom_pending;
	return s_surf;
}

bool overlay_ensure()
{
	return ensure();
}

const uint32_t* overlay_pixels()
{
	return s_buf;
}

unsigned overlay_bottom_pending()
{
	return s_bottom_pending;
}

const SDL_Surface* overlay_surface_now()
{
	return s_handed ? s_surf : nullptr;
}

void overlay_drop()
{
	if (s_handed)
		++s_n.dropped;
	s_handed = false;
	s_nb = 0;
}

int overlay_upload(Band* out)
{
	if (!s_handed)
		return 0;
	s_handed = false;
	int k = 0;
	for (int i = 0; i < s_nb; ++i)
	{
		const Band& b = s_bands[i];
		const int rows = b.y1 - b.y0;
		u32* src = s_buf + static_cast<size_t>(b.y0) * kTexW;
		GSPGPU_FlushDataCache(src, static_cast<u32>(rows) * kPitch);   // the CPU wrote these rows
		const int d = config().band_from_end ? kTexH - b.y1 : b.y0;
		u32* dst = static_cast<u32*>(s_tex.data) + static_cast<size_t>(d) * kTexW;
		if (gx_op([&] {
			    C3D_SyncDisplayTransfer(src, GX_BUFFER_DIM(kTexW, rows), dst, GX_BUFFER_DIM(kTexW, rows), kBandFlags);
		    }))
		{
			out[k++] = b;
			++s_n.bands;
		}
		else
			++s_n.refused;   // not composited: the frame shows the view without this band
	}
	s_nb = 0;
	++s_n.uploads;
	return k;
}

C3D_Tex* overlay_texture()
{
	return s_tex_ready ? &s_tex : nullptr;
}

unsigned overlay_leaks_now()
{
	if (!s_handed || !s_buf)
		return 0;
	++s_n.audits;
	const uint32_t cur = bands_tile_mask(s_bands, s_nb);
	unsigned leaks = 0;
	for (int y = 0; y < kH; ++y)
	{
		if (cur >> (y / kBandTile) & 1)
			continue;
		const u32* row = s_buf + static_cast<size_t>(y) * kTexW;
		for (int x = 0; x < kW; ++x)
			leaks += row[x] != 0;
	}
	s_n.leaks += leaks;
	return leaks;
}

void overlay_free()
{
	if (s_tex_ready)
		C3D_TexDelete(&s_tex);
	s_tex_ready = s_tex_linear = false;
	if (s_surf)
		SDL_FreeSurface(s_surf);   // SDL_PREALLOC: the pixels stay ours
	s_surf = nullptr;
	if (s_buf)
		linearFree(s_buf);
	s_buf = nullptr;
	s_dirty = 0;
	s_nb = 0;
	s_handed = false;
}

int overlay_stats_text(char* buf, unsigned size)
{
	return snprintf(buf, size, "overlay handouts=%u uploads=%u bands=%u tile_clears=%u refused=%u dropped=%u audits=%u "
	                "leaks=%u linear_tex=%d",
	                s_n.handouts, s_n.uploads, s_n.bands, s_n.tile_clears, s_n.refused, s_n.dropped, s_n.audits,
	                s_n.leaks, s_tex_linear ? 1 : 0);
}

} // namespace back
} // namespace a1_c3d
