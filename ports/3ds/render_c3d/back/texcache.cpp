/*
	Aleph One for the Nintendo 3DS, citro3d back end: the texture cache instance (see texcache.h).

	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; either version 3 of the License, or
	(at your option) any later version.
*/
#include "texcache.h"

#include "a1_c3d.h"
#include "context.h"
#include "../core/landscape.h"

#include <3ds.h>
#include <citro3d.h>

#include <algorithm>
#include <stdio.h>
#include <stdlib.h>

#ifdef A1_PROFILE
#include "a1_3ds_profile.h"
#endif

namespace a1_c3d {

namespace {

// One C3D_Tex per texture (M2 "Pools, caps, allocation"): the struct is malloc'd, so its address is stable
// for as long as the slot holds it (citro3d keeps the pointer given to C3D_TexBind). RGBA8, and A8 for the
// mask kind (M4, enums.h:73); S x T (M4: rectangles are not square).
class C3dAllocator : public TexAllocator
{
public:
	void* alloc(TexPool pool, unsigned S, unsigned T, TexKind kind, TexWrap wrap, uint32_t bytes) override
	{
		C3D_Tex* t = static_cast<C3D_Tex*>(malloc(sizeof(C3D_Tex)));
		if (!t)
			return nullptr;
		const GPU_TEXCOLOR fmt = kind == TexKind::Mask ? GPU_A8 : GPU_RGBA8;
		// texture.h:94-116: linear (linearMemAlign) or VRAM (vramMemAlign), no mipmaps; width S, height T.
		const bool ok = pool == TexPool::Vram ? C3D_TexInitVRAM(t, static_cast<u16>(S), static_cast<u16>(T), fmt)
		                                      : C3D_TexInit(t, static_cast<u16>(S), static_cast<u16>(T), fmt);
		if (!ok)
		{
			free(t);
			return nullptr;
		}
		if (t->size != bytes)
		{
			// Never expected: the caps would count another size than citro3d allocated.
			printf("[c3d] tex: C3D_TexInit size %lu != %lu for %u x %u: refused\n", static_cast<unsigned long>(t->size),
			       static_cast<unsigned long>(bytes), S, T);
			C3D_TexDelete(t);
			free(t);
			return nullptr;
		}
		// texture.h:157-173: nearest (SW samples whole texels); repeat for walls and floors (texcoords are shifted
		// by whole repeats), clamp-to-edge for rectangles (M4: transparent padding, SW never reads outside); M5
		// landscapes repeat along a row (s) and clamp across rows (t: the padded rows are SW's PIN). texture.h:169
		// takes (wrapS, wrapT).
		C3D_TexSetFilter(t, GPU_NEAREST, GPU_NEAREST);
		const GPU_TEXTURE_WRAP_PARAM ws = wrap == TexWrap::Clamp ? GPU_CLAMP_TO_EDGE : GPU_REPEAT;
		const GPU_TEXTURE_WRAP_PARAM wt = wrap == TexWrap::Repeat ? GPU_REPEAT : GPU_CLAMP_TO_EDGE;
		C3D_TexSetWrap(t, ws, wt);
		return t;
	}
	void release(TexPool, void* handle) override
	{
		C3D_Tex* t = static_cast<C3D_Tex*>(handle);
		C3D_TexDelete(t);
		free(t);
	}
};

C3dAllocator s_alloc;
TexCache* s_cache = nullptr;   // main thread only; lives for the process (release_all() empties it)
unsigned s_convert_failed = 0;

} // namespace

namespace back {

TexCache& texcache()
{
	if (!s_cache)
	{
		const Config& cfg = config();
		TexCacheConfig c;
		c.linear_cap_kib = cfg.linear_cap;
		// M6 g3a (test only): 9.1's M7 column, the pool capped at 2 048 KiB next to the 2 MiB view target.
		c.vram_cap_kib = cfg.g3a && cfg.mode == Mode::Direct ? std::min(cfg.vram_cap, 2048u) : cfg.vram_cap;
		c.promote_max = cfg.promote_max;
		c.fail_every = cfg.fail_every;
		s_cache = new TexCache(s_alloc, c);
	}
	return *s_cache;
}

void texcache_frame_start(uint32_t frame)
{
#ifdef A1_PROFILE
	::a1_3ds::prof::Scope prof_scope(::a1_3ds::prof::C3dTex);
#endif
	TexCache& c = texcache();
	c.on_retired();
	// M5 "Level-start counts": the window of the last generation is complete once the frame after it starts; a
	// mid-play burst (>= 512 KiB in one frame outside the window, e.g. an infravision toggle) is logged (first 8).
	static int s_last_gen_logged = -1;
	static uint32_t s_bursts_logged = 0;
	const TexStats& st = c.stats();
	if (c.frames_since_generation() == static_cast<int>(kTexGenWindow) - 1 &&
	    s_last_gen_logged != static_cast<int>(st.generation))
	{
		s_last_gen_logged = static_cast<int>(st.generation);
		char buf[512];
		int n = snprintf(buf, sizeof buf, "[c3d] tex: level start gen %u: KiB", static_cast<unsigned>(st.generation));
		for (unsigned i = 0; i < kTexGenWindow; ++i)
			n += snprintf(buf + n, sizeof buf - n, " %u", static_cast<unsigned>(st.ls_bytes[i] / 1024));
		n += snprintf(buf + n, sizeof buf - n, " textures");
		for (unsigned i = 0; i < kTexGenWindow; ++i)
			n += snprintf(buf + n, sizeof buf - n, " %u", static_cast<unsigned>(st.ls_textures[i]));
		n += snprintf(buf + n, sizeof buf - n, " pending");
		for (unsigned i = 0; i < kTexGenWindow; ++i)
			n += snprintf(buf + n, sizeof buf - n, " %u", static_cast<unsigned>(st.ls_pending[i]));
		snprintf(buf + n, sizeof buf - n, " gx_hw %d audit %s", gx_queue().high_water(), c.audit() ? "ok" : "MISMATCH");
		printf("%s\n", buf);
	}
	if (st.burst_frames != s_bursts_logged)
	{
		if (s_bursts_logged < 8)
			printf("[c3d] tex: burst frame %u: %u KiB converted outside the level start (%u bursts)\n",
			       static_cast<unsigned>(st.last_burst_frame), static_cast<unsigned>(st.last_burst_bytes / 1024),
			       static_cast<unsigned>(st.burst_frames));
		s_bursts_logged = st.burst_frames;
	}
	c.begin_frame(frame);
	// Oldest first, while promote_max and the GX rule allow (next_promotion took the entry). The source was
	// flushed when it was converted; the copy is citro3d's own VRAM upload: libcitro3d.a texture.o
	// C3D_TexLoadImage+0x60-0x7c = C3D_SyncTextureCopy(data, 0, tex->data, 0, size, 8), 8 =
	// GX_TRANSFER_RAW_COPY(1) (gx.h). In frame it is queued, after a split if the buffer is not empty
	// (renderqueue.o C3D_SyncTextureCopy+0x2c-0x64), and runs at C3D_FrameEnd before this frame's draws,
	// which still sample the linear texture; the slot samples VRAM once this frame has retired.
	TexPromotion p;
	while (c.next_promotion(gx_queue(), gpuCmdBufOffset != 0, p))
	{
		C3D_Tex* src = static_cast<C3D_Tex*>(p.src);
		C3D_Tex* dst = static_cast<C3D_Tex*>(p.dst);
		C3D_SyncTextureCopy(static_cast<u32*>(src->data), 0, static_cast<u32*>(dst->data), 0, p.bytes,
		                    GX_TRANSFER_RAW_COPY(1));
	}
}

TexGet texcache_get(const TexSource& src, void** out)
{
	TexCache& c = texcache();
	const TexKey key{src.id, src.tables, src.kind,
	                 src.rect ? TexWrap::Clamp : (src.landscape ? TexWrap::Landscape : TexWrap::Repeat),
	                 static_cast<uint8_t>(src.landscape ? src.land_part : 0)};
	if (TexSlot* s = c.lookup(key))
	{
		*out = s->handle;
		return TexGet::Ok;
	}
	// The converter's own refusals, checked before a slot is taken.
	unsigned S = 0, T = 0;
	if (src.rect)
	{
		const bool kind_ok = src.kind == TexKind::NormalKeyed || src.kind == TexKind::ShadelessKeyed || src.kind == TexKind::Mask;
		if (!kind_ok || !rect_tex_size(src.bitmap.width, src.bitmap.height, src.bitmap.rle, S, T) || !src.bitmap.rows ||
		    (src.kind != TexKind::Mask && !src.tables))
			return TexGet::Unsupported;
	}
	else if (src.landscape)
	{
		unsigned TT[2] = {0, 0};
		int parts = 0;
		if ((src.kind != TexKind::Normal && src.kind != TexKind::Shadeless) ||
		    !landscape_tex_size(src.bitmap.width, src.bitmap.height, src.bitmap.rle, S, TT, &parts) ||
		    src.land_part >= parts || !src.bitmap.rows || !src.tables)
			return TexGet::Unsupported;
		T = TT[src.land_part];
	}
	else
	{
		// M5: the Mask kind of a square wall bitmap (transparent static walls) needs no tables.
		if (!tex_supported(src.bitmap) || !src.bitmap.rows || (src.kind != TexKind::Mask && !src.tables))
			return TexGet::Unsupported;
		S = T = static_cast<unsigned>(src.bitmap.width);
	}
#ifdef A1_PROFILE
	::a1_3ds::prof::Scope prof_scope(::a1_3ds::prof::C3dTex);
#endif
	TexSlot* s = c.create(key, S, T);
	if (!s)
		return TexGet::Discard;
	C3D_Tex* t = static_cast<C3D_Tex*>(s->handle);
	const bool converted = src.rect        ? tex_convert_rle(src.bitmap, src.tables, src.kind, S, T, t->data)
	                       : src.landscape ? tex_convert_landscape(src.bitmap, src.tables, src.kind, src.land_part, S, T, t->data)
	                                       : tex_convert(src.bitmap, src.tables, src.kind, t->data);
	if (!converted)
	{
		if (s_convert_failed++ == 0)
			printf("[c3d] tex: conversion refused a %d x %d bitmap: view frame discarded (unsupported)\n",
			       src.bitmap.width, src.bitmap.height);
		c.forget(s);
		return TexGet::Unsupported;
	}
	// gspgpu.h: exactly the texture's bytes, before the GPU reads them (C3D_FrameEnd(GX_CMDLIST_FLUSH) skips
	// the whole-heap flush, design 0.3).
	GSPGPU_FlushDataCache(t->data, t->size);
	*out = t;
	return TexGet::Ok;
}

void texcache_collections_changed()
{
	TexCache& c = texcache();
	const unsigned retiring = c.bump_generation();
	if (config().renderer_c3d)
		printf("[c3d] tex: collections changed (gen %u, %u entries retire)\n",
		       static_cast<unsigned>(c.generation()), retiring);
}

void texcache_log(const char* tag)
{
	if (!s_cache)
		return;
	printf("%s [%s]\n", s_cache->stats_line().c_str(), tag);
}

void texcache_release_all()
{
	if (s_cache)
		s_cache->release_all();
}

} // namespace back

// Hooks 2a (end of load_collections) and 2b (end of unload_all_collections), PATCHES.md 46.
void collections_changed()
{
	back::texcache_collections_changed();
}

} // namespace a1_c3d
