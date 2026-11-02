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

#include <3ds.h>
#include <citro3d.h>

#include <stdio.h>
#include <stdlib.h>

#ifdef A1_PROFILE
#include "a1_3ds_profile.h"
#endif

namespace a1_c3d {

namespace {

// One C3D_Tex per texture (M2 "Pools, caps, allocation"): the struct is malloc'd, so its address is stable
// for as long as the slot holds it (citro3d keeps the pointer given to C3D_TexBind). RGBA8 kinds only in M2
// (the A8 mask is M4's).
class C3dAllocator : public TexAllocator
{
public:
	void* alloc(TexPool pool, unsigned W, TexKind kind, uint32_t bytes) override
	{
		if (tex_texel_bytes(kind) != 4)
			return nullptr;
		C3D_Tex* t = static_cast<C3D_Tex*>(malloc(sizeof(C3D_Tex)));
		if (!t)
			return nullptr;
		// texture.h:94-116: linear (linearMemAlign) or VRAM (vramMemAlign), no mipmaps.
		const bool ok = pool == TexPool::Vram ? C3D_TexInitVRAM(t, static_cast<u16>(W), static_cast<u16>(W), GPU_RGBA8)
		                                      : C3D_TexInit(t, static_cast<u16>(W), static_cast<u16>(W), GPU_RGBA8);
		if (!ok)
		{
			free(t);
			return nullptr;
		}
		if (t->size != bytes)
		{
			// Never expected: the caps would count another size than citro3d allocated.
			printf("[c3d] tex: C3D_TexInit size %lu != %lu for %u x %u: refused\n", static_cast<unsigned long>(t->size),
			       static_cast<unsigned long>(bytes), W, W);
			C3D_TexDelete(t);
			free(t);
			return nullptr;
		}
		// texture.h:157-173: nearest (SW samples whole texels), repeat (texcoords are shifted by whole repeats).
		C3D_TexSetFilter(t, GPU_NEAREST, GPU_NEAREST);
		C3D_TexSetWrap(t, GPU_REPEAT, GPU_REPEAT);
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
		c.vram_cap_kib = cfg.vram_cap;
		c.promote_max = cfg.promote_max;
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
	const TexKey key{src.id, src.tables, src.kind};
	if (TexSlot* s = c.lookup(key))
	{
		*out = s->handle;
		return TexGet::Ok;
	}
	// The converter's own refusals, checked before a slot is taken.
	if (!tex_supported(src.bitmap) || !src.bitmap.rows || !src.tables || tex_texel_bytes(src.kind) != 4)
		return TexGet::Unsupported;
#ifdef A1_PROFILE
	::a1_3ds::prof::Scope prof_scope(::a1_3ds::prof::C3dTex);
#endif
	TexSlot* s = c.create(key, static_cast<unsigned>(src.bitmap.width));
	if (!s)
		return TexGet::Discard;
	C3D_Tex* t = static_cast<C3D_Tex*>(s->handle);
	if (!tex_convert(src.bitmap, src.tables, src.kind, t->data))
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
