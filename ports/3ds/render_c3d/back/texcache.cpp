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

#include <stdio.h>

namespace a1_c3d {

namespace {

// part 6: citro3d allocator. Until the upload code exists (it waits for the short M1 console check)
// every allocation fails, so create() always reports "discard tex" and nothing is ever held.
class StubAllocator : public TexAllocator
{
public:
	void* alloc(TexPool, uint32_t) override { return nullptr; }
	void release(TexPool, void*) override {}
};

StubAllocator s_alloc;
TexCache* s_cache = nullptr;   // main thread only; lives for the process (release_all() empties it)

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

void texcache_collections_changed()
{
	TexCache& c = texcache();
	const unsigned retiring = c.bump_generation();
	if (config().renderer_c3d)
		printf("[c3d] tex: collections changed (gen %u, %u entries retire)\n",
		       static_cast<unsigned>(c.generation()), retiring);
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
