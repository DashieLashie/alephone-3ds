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

void* TexCache::alloc_with_eviction(TexPool pool, const TexSlot& shape, uint32_t victims_used_below)
{
	const uint32_t bytes = shape.bytes;
	if (bytes > cap_[pi(pool)])
		return nullptr;
	bool counted = false;
	unsigned evicted_after_fail = 0;
	for (;;)
	{
		if (used_[pi(pool)] + bytes <= cap_[pi(pool)])
		{
			if (void* h = alloc_.alloc(pool, shape.S, shape.T, shape.key.kind, shape.key.wrap, bytes))
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
		// After a failure with room under the cap, at most kEvictAfterAllocFail more victims: fragmentation must
		// not empty the pool for one texture (M2 commit review, minor 3).
		if (counted && evicted_after_fail++ >= kEvictAfterAllocFail)
			return nullptr;
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
	if (frames_since_gen_ >= 0 && frames_since_gen_ < static_cast<int>(kTexGenWindow))
		stats_.ls_pending[frames_since_gen_] = pending_count();
}

TexSlot* TexCache::lookup(const TexKey& key)
{
	for (TexSlot& s : slots_)
	{
		if (s.state == TexSlotState::Live && s.generation == stats_.generation && s.key.bitmap == key.bitmap &&
		    s.key.tables == key.tables && s.key.kind == key.kind && s.key.wrap == key.wrap && s.key.part == key.part)
		{
			s.last_used_frame = frame_;
			++stats_.hits;
			return &s;
		}
	}
	++stats_.misses;
	return nullptr;
}

TexSlot* TexCache::create(const TexKey& key, unsigned S, unsigned T)
{
	// M5 "fail_every": a hard failure before any allocation, as if both pools were full after eviction.
	++stats_.creates;
	if (cfg_.fail_every && stats_.creates % cfg_.fail_every == 0)
	{
		++stats_.forced_fails;
		++stats_.discards_tex;
		return nullptr;
	}
	TexSlot shape;
	shape.key = key;
	shape.S = S;
	shape.T = T;
	shape.bytes = static_cast<uint32_t>(tex_bytes(S, T, key.kind));
	TexSlot* s = take_free_slot();
	void* h = s ? alloc_with_eviction(TexPool::Linear, shape, frame_) : nullptr;
	if (!h)
	{
		++stats_.discards_tex;
		return nullptr;
	}
	*s = TexSlot();
	s->state = TexSlotState::Live;
	s->key = key;
	s->generation = stats_.generation;
	s->S = S;
	s->T = T;
	s->bytes = shape.bytes;
	s->pool = TexPool::Linear;
	s->handle = h;
	s->last_used_frame = frame_;
	s->seq = next_seq_++;
	const uint32_t bytes = shape.bytes;
	s->promote_pending = cfg_.promote_max > 0 && bytes <= cap_[pi(TexPool::Vram)];

	++stats_.conversions;
	stats_.bytes_converted += bytes;
	const bool was_over = frame_bytes_ > kTexSoftBudgetBytes;
	frame_bytes_ += bytes;
	stats_.frame_bytes_max = std::max(stats_.frame_bytes_max, frame_bytes_);
	const bool in_window = frames_since_gen_ >= 0 && frames_since_gen_ < static_cast<int>(kTexGenWindow);
	if (!was_over && frame_bytes_ > kTexSoftBudgetBytes)
	{
		++stats_.over_budget_frames;
		if (!in_window)
			++stats_.burst_frames;
	}
	if (!in_window && frame_bytes_ > kTexSoftBudgetBytes)
	{
		stats_.last_burst_frame = frame_;
		stats_.last_burst_bytes = frame_bytes_;
	}
	if (in_window)
	{
		stats_.gen_window_bytes[frames_since_gen_] += bytes;
		stats_.ls_bytes[frames_since_gen_] += bytes;
		++stats_.ls_textures[frames_since_gen_];
	}
	return s;
}

void TexCache::forget(TexSlot* s)
{
	if (s && s->state == TexSlotState::Live)
		free_slot_memory(*s);
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
		// The op itself (2 entries with a non-empty command buffer: the split, 2.4) plus the frame's own entries.
		if (gx.used() + 1 + (command_buffer_not_empty ? 1 : 0) + kViewReserveEntries > gx.budget())
		{
			++stats_.promotions_deferred;
			return false;
		}
		// A VRAM slot is evicted for this one only if it was used less recently (no thrashing).
		void* dst = alloc_with_eviction(TexPool::Vram, *s,
		                                std::min(frame_, s->last_used_frame));
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
	for (unsigned i = 0; i < kTexGenWindow; ++i)
		stats_.ls_bytes[i] = stats_.ls_textures[i] = stats_.ls_pending[i] = 0;
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

bool TexCache::audit(uint32_t* expected_linear, uint32_t* expected_vram) const
{
	uint64_t e[2] = {0, 0};
	for (const TexSlot& s : slots_)
	{
		if (s.state == TexSlotState::Free)
			continue;
		if (s.handle && !s.handle_deferred)
			e[pi(s.pool)] += s.bytes;
		if (s.vram_handle)
			e[pi(TexPool::Vram)] += s.bytes;
	}
	for (const Block& b : deferred_)
		e[pi(b.pool)] += b.bytes;
	if (expected_linear)
		*expected_linear = static_cast<uint32_t>(e[0]);
	if (expected_vram)
		*expected_vram = static_cast<uint32_t>(e[1]);
	return e[0] == used_[0] && e[1] == used_[1];
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
	char buf[768];
	int n = std::snprintf(buf, sizeof buf, "[c3d] tex: entries %u/%u KiB %u/%u conv %u (%llu KiB, max/frame %u KiB, over %u) gen-window KiB",
	                      live_count(TexPool::Linear), live_count(TexPool::Vram), used_[0] / 1024, used_[1] / 1024,
	                      stats_.conversions, static_cast<unsigned long long>(stats_.bytes_converted / 1024),
	                      stats_.frame_bytes_max / 1024, stats_.over_budget_frames);
	for (unsigned i = 0; i < kTexGenWindow && n > 0 && n < static_cast<int>(sizeof buf); ++i)
		n += std::snprintf(buf + n, sizeof buf - n, " %u", stats_.gen_window_bytes[i] / 1024);
	if (n > 0 && n < static_cast<int>(sizeof buf))
		std::snprintf(buf + n, sizeof buf - n,
		              " promo %u/%u evict %u dfree %u gen %u alloc-fail %u discards %u hits %u misses %u creates %u "
		              "forced %u bursts %u audit %s",
		              stats_.promotions_done, stats_.promotions_deferred, stats_.evictions, stats_.deferred_frees,
		              stats_.generation, stats_.alloc_failed_free, stats_.discards_tex, stats_.hits, stats_.misses,
		              stats_.creates, stats_.forced_fails, stats_.burst_frames, audit() ? "ok" : "MISMATCH");
	return std::string(buf);
}

} // namespace a1_c3d
