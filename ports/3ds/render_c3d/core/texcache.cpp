/*
	Aleph One for the Nintendo 3DS, citro3d back end: the texture cache, pure part (see texcache.h).

	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; either version 3 of the License, or
	(at your option) any later version.
*/
#include "texcache.h"

#include <algorithm>
#include <cstdio>

namespace a1_c3d {

namespace {

inline int pi(TexPool p) { return static_cast<int>(p); }

} // namespace

TexCache::TexCache(TexAllocator& alloc, const TexCacheConfig& cfg) : alloc_(alloc), cfg_(cfg)
{
	cap_[pi(TexPool::Linear)] = cfg.linear_cap_kib * 1024u;
	cap_[pi(TexPool::Vram)] = cfg.vram_cap_kib * 1024u;
}

TexCache::~TexCache() {}

bool TexCache::protected_slot(const TexSlot& s, uint32_t frame)
{
	return s.copy_frame == frame || s.vram_handle != nullptr;
}

TexSlot* TexCache::lru_victim(bool any_pool, TexPool pool, uint32_t last_used_below)
{
	TexSlot* best = nullptr;
	for (TexSlot& s : slots_)
	{
		if (s.state != TexSlotState::Live || (!any_pool && s.pool != pool))
			continue;
		if (s.last_used_frame >= last_used_below || protected_slot(s, frame_))
			continue;
		if (!best || s.last_used_frame < best->last_used_frame ||
		    (s.last_used_frame == best->last_used_frame && s.seq < best->seq))
			best = &s;
	}
	return best;
}

void TexCache::free_slot_memory(TexSlot& s)
{
	if (s.handle && !s.handle_deferred)
	{
		alloc_.release(s.pool, s.handle);
		used_[pi(s.pool)] -= s.bytes;
	}
	if (s.vram_handle)
	{
		alloc_.release(TexPool::Vram, s.vram_handle);
		used_[pi(TexPool::Vram)] -= s.bytes;
	}
	s = TexSlot();
}

void TexCache::evict(TexSlot& s)
{
	free_slot_memory(s);   // also drops a waiting slot from the promotion order
	++stats_.evictions;
}

TexSlot* TexCache::take_free_slot()
{
	for (TexSlot& s : slots_)
		if (s.state == TexSlotState::Free)
			return &s;
	TexSlot* victim = lru_victim(true, TexPool::Linear, frame_);
	if (!victim)
		return nullptr;
	evict(*victim);
	return victim;
}

void* TexCache::alloc_with_eviction(TexPool pool, uint32_t bytes, uint32_t victims_used_below)
{
	if (bytes > cap_[pi(pool)])
		return nullptr;
	bool counted = false;
	for (;;)
	{
		if (used_[pi(pool)] + bytes <= cap_[pi(pool)])
		{
			if (void* h = alloc_.alloc(pool, bytes))
			{
				used_[pi(pool)] += bytes;
				return h;
			}
			if (!counted)
			{
				++stats_.alloc_failed_free;   // the cap had room: fragmentation or the real heap
				counted = true;
			}
		}
		TexSlot* victim = lru_victim(false, pool, victims_used_below);
		if (!victim)
			return nullptr;
		evict(*victim);
	}
}

unsigned TexCache::on_retired()
{
	unsigned freed = 0;
	for (const Block& b : deferred_)
	{
		alloc_.release(b.pool, b.handle);
		used_[pi(b.pool)] -= b.bytes;
		++freed;
	}
	deferred_.clear();
	for (TexSlot& s : slots_)
	{
		if (s.state == TexSlotState::Retiring)
		{
			free_slot_memory(s);
			++freed;
		}
		else if (s.state == TexSlotState::Live && s.vram_handle)
		{
			// The copy was issued in a frame that has retired: sample the VRAM texture from now on.
			// (Its linear source is on the deferred list, freed above.)
			s.handle = s.vram_handle;
			s.pool = TexPool::Vram;
			s.vram_handle = nullptr;
			s.handle_deferred = false;
			s.copy_frame = kNoFrame;
		}
	}
	stats_.deferred_frees += freed;
	return freed;
}

void TexCache::begin_frame(uint32_t frame)
{
	frame_ = frame;
	frame_bytes_ = 0;
	promoted_this_frame_ = 0;
	if (frames_since_gen_ < static_cast<int>(kTexGenWindow))
		++frames_since_gen_;
}

TexSlot* TexCache::lookup(const TexKey& key)
{
	for (TexSlot& s : slots_)
	{
		if (s.state == TexSlotState::Live && s.generation == stats_.generation && s.key.bitmap == key.bitmap &&
		    s.key.tables == key.tables && s.key.kind == key.kind)
		{
			s.last_used_frame = frame_;
			++stats_.hits;
			return &s;
		}
	}
	++stats_.misses;
	return nullptr;
}

TexSlot* TexCache::create(const TexKey& key, unsigned W)
{
	const uint32_t bytes = static_cast<uint32_t>(tex_bytes(W, key.kind));
	TexSlot* s = take_free_slot();
	void* h = s ? alloc_with_eviction(TexPool::Linear, bytes, frame_) : nullptr;
	if (!h)
	{
		++stats_.discards_tex;
		return nullptr;
	}
	*s = TexSlot();
	s->state = TexSlotState::Live;
	s->key = key;
	s->generation = stats_.generation;
	s->W = W;
	s->bytes = bytes;
	s->pool = TexPool::Linear;
	s->handle = h;
	s->last_used_frame = frame_;
	s->seq = next_seq_++;
	s->promote_pending = cfg_.promote_max > 0 && bytes <= cap_[pi(TexPool::Vram)];

	++stats_.conversions;
	stats_.bytes_converted += bytes;
	const bool was_over = frame_bytes_ > kTexSoftBudgetBytes;
	frame_bytes_ += bytes;
	stats_.frame_bytes_max = std::max(stats_.frame_bytes_max, frame_bytes_);
	if (!was_over && frame_bytes_ > kTexSoftBudgetBytes)
		++stats_.over_budget_frames;
	if (frames_since_gen_ >= 0 && frames_since_gen_ < static_cast<int>(kTexGenWindow))
		stats_.gen_window_bytes[frames_since_gen_] += bytes;
	return s;
}

bool TexCache::next_promotion(GxQueue& gx, bool command_buffer_not_empty, TexPromotion& out)
{
	if (promoted_this_frame_ >= cfg_.promote_max)
		return false;
	for (;;)
	{
		TexSlot* s = nullptr;
		for (TexSlot& c : slots_)
			if (c.state == TexSlotState::Live && c.promote_pending && c.tried_frame != frame_ &&
			    (!s || c.seq < s->seq))
				s = &c;
		if (!s)
			return false;
		if (gx.used() + 1 + kViewReserveEntries > gx.budget())
		{
			++stats_.promotions_deferred;
			return false;
		}
		// A VRAM slot is evicted for this one only if it was used less recently (no thrashing).
		void* dst = alloc_with_eviction(TexPool::Vram, s->bytes, std::min(frame_, s->last_used_frame));
		if (!dst)
		{
			s->tried_frame = frame_;   // stays linear and pending; the next waiting slot may fit
			continue;
		}
		if (!gx.take(command_buffer_not_empty))
		{
			alloc_.release(TexPool::Vram, dst);
			used_[pi(TexPool::Vram)] -= s->bytes;
			++stats_.promotions_deferred;
			return false;
		}
		s->vram_handle = dst;
		s->copy_frame = frame_;
		s->promote_pending = false;
		s->handle_deferred = true;
		deferred_.push_back(Block{TexPool::Linear, s->handle, s->bytes});
		++promoted_this_frame_;
		++stats_.promotions_done;
		out = TexPromotion{s, s->handle, dst, s->bytes};
		return true;
	}
}

unsigned TexCache::bump_generation()
{
	unsigned n = 0;
	for (TexSlot& s : slots_)
	{
		if (s.state == TexSlotState::Live)
		{
			s.state = TexSlotState::Retiring;
			s.promote_pending = false;
			++n;
		}
	}
	++stats_.generation;
	++stats_.collections_changed;
	frames_since_gen_ = -1;
	return n;
}

void TexCache::release_all()
{
	for (const Block& b : deferred_)
	{
		alloc_.release(b.pool, b.handle);
		used_[pi(b.pool)] -= b.bytes;
	}
	deferred_.clear();
	for (TexSlot& s : slots_)
		if (s.state != TexSlotState::Free)
			free_slot_memory(s);
}

unsigned TexCache::live_count(TexPool pool) const
{
	unsigned n = 0;
	for (const TexSlot& s : slots_)
		if (s.state == TexSlotState::Live && s.pool == pool)
			++n;
	return n;
}

unsigned TexCache::pending_count() const
{
	unsigned n = 0;
	for (const TexSlot& s : slots_)
		if (s.state == TexSlotState::Live && s.promote_pending)
			++n;
	return n;
}

unsigned TexCache::retiring_count() const
{
	unsigned n = 0;
	for (const TexSlot& s : slots_)
		if (s.state == TexSlotState::Retiring)
			++n;
	return n;
}

std::string TexCache::stats_line() const
{
	char buf[512];
	int n = std::snprintf(buf, sizeof buf, "[c3d] tex: entries %u/%u KiB %u/%u conv %u (%llu KiB, max/frame %u KiB, over %u) gen-window KiB",
	                      live_count(TexPool::Linear), live_count(TexPool::Vram), used_[0] / 1024, used_[1] / 1024,
	                      stats_.conversions, static_cast<unsigned long long>(stats_.bytes_converted / 1024),
	                      stats_.frame_bytes_max / 1024, stats_.over_budget_frames);
	for (unsigned i = 0; i < kTexGenWindow && n > 0 && n < static_cast<int>(sizeof buf); ++i)
		n += std::snprintf(buf + n, sizeof buf - n, " %u", stats_.gen_window_bytes[i] / 1024);
	if (n > 0 && n < static_cast<int>(sizeof buf))
		std::snprintf(buf + n, sizeof buf - n,
		              " promo %u/%u evict %u dfree %u gen %u alloc-fail %u discards %u hits %u misses %u",
		              stats_.promotions_done, stats_.promotions_deferred, stats_.evictions, stats_.deferred_frees,
		              stats_.generation, stats_.alloc_failed_free, stats_.discards_tex, stats_.hits, stats_.misses);
	return std::string(buf);
}

} // namespace a1_c3d
