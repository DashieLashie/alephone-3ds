/*
	Aleph One for the Nintendo 3DS, citro3d back end: the texture cache, pure part
	(docs/3ds/CITRO3D_DESIGN.md, 3.3 and section 7, M2 specification "Cache key and value",
	"Generations and the deferred flush", "Pools, caps, allocation", "Upload model", "Stats").
	Plain C++, no 3DS or engine headers; the GPU side is the small TexAllocator interface
	(a fake on the host, citro3d's C3D_TexInit / C3D_TexInitVRAM / C3D_TexDelete in part 6).

	  slots        a fixed array of kTexSlots (256) entries inside the object, so a slot's address
	               never changes (citro3d keeps the C3D_Tex* it was given until the next draw).
	  key          (bitmap pointer, shading-tables pointer, kind) + the generation; lookup() matches
	               only the current generation. bump_generation() (collections_changed()) turns every
	               live slot into a Retiring one: it cannot be found any more, but its memory and its
	               slot stay until on_retired() frees them (the caller calls on_retired() after a
	               frame_begin() returned, so no frame in flight can still sample them).
	  pools        Linear and Vram, KiB caps (accounting only; each texture is its own allocation).
	               A new texture is created in Linear. With promote_max > 0 and a VRAM cap that can
	               hold it, it is marked promote_pending; next_promotion() hands out, oldest first, the
	               copies to issue in the current view frame (≤ promote_max a frame, and only while
	               gx.used() + 1 + kViewReserveEntries <= gx.budget(); a refusal defers, never
	               discards). The slot keeps sampling its linear texture in the frame of the copy; the
	               linear block goes on the deferred-free list at once (freed by the next on_retired(),
	               which also switches the slot to its VRAM texture).
	  eviction     LRU by last_used_frame among the slots of the pool that must make room (any pool when
	               the slot array is full); never a slot used in the current view frame, never a slot
	               whose copy was issued in this frame (its linear source and VRAM destination are in
	               use). A slot only waiting for promotion may be evicted (that also drops it from the
	               promotion order). Nothing evictable: create() fails (the caller discards `tex`).

	Call order per view frame: frame_begin() returned -> on_retired() -> begin_frame(n) -> the
	promotion loop (before the clear) -> lookup()/create() per draw. Main thread only.

	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; either version 3 of the License, or
	(at your option) any later version.
*/
#ifndef A1_C3D_CORE_TEXCACHE_H
#define A1_C3D_CORE_TEXCACHE_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "gx_queue.h"
#include "tex_convert.h"

namespace a1_c3d {

enum class TexPool : uint8_t { Linear = 0, Vram = 1 };

// The GPU side. alloc returns an opaque handle (nullptr: failed) for `bytes` in the pool; release
// frees one. Handles are never interpreted by the cache.
class TexAllocator
{
public:
	virtual ~TexAllocator() {}
	virtual void* alloc(TexPool pool, uint32_t bytes) = 0;
	virtual void release(TexPool pool, void* handle) = 0;
};

constexpr unsigned kTexSlots = 256;
constexpr int kViewReserveEntries = 3;          // clear, flushing split, readback transfer (M1 "GX queue")
constexpr uint32_t kTexSoftBudgetBytes = 512u * 1024;   // converted per view frame: counted, not enforced
constexpr unsigned kTexGenWindow = 10;          // view frames after a generation change that are recorded
constexpr uint32_t kNoFrame = 0xFFFFFFFFu;

struct TexKey
{
	const void* bitmap = nullptr;
	const void* tables = nullptr;
	TexKind kind = TexKind::Normal;
};

enum class TexSlotState : uint8_t { Free, Live, Retiring };

struct TexSlot
{
	TexSlotState state = TexSlotState::Free;
	TexKey key;
	uint32_t generation = 0;
	unsigned W = 0;
	uint32_t bytes = 0;
	TexPool pool = TexPool::Linear;       // the pool of `handle`
	void* handle = nullptr;               // what to sample now (the back end's C3D_Tex)
	bool handle_deferred = false;         // `handle` (linear) is on the deferred-free list: sampled this frame only
	void* vram_handle = nullptr;          // the destination of a copy issued this frame (else null)
	uint32_t last_used_frame = 0;
	uint32_t seq = 0;                     // creation order (oldest first)
	bool promote_pending = false;
	uint32_t copy_frame = kNoFrame;       // the view frame the copy was issued in
	uint32_t tried_frame = kNoFrame;      // the last frame a promotion of this slot found no VRAM
};

struct TexCacheConfig
{
	unsigned linear_cap_kib = 4096;
	unsigned vram_cap_kib = 3584;
	unsigned promote_max = 8;             // 0 = never promote (linear sampling only)
};

struct TexStats
{
	uint32_t hits = 0, misses = 0;
	uint32_t conversions = 0;
	uint64_t bytes_converted = 0;
	uint32_t frame_bytes_max = 0;         // most bytes converted in one view frame
	uint32_t over_budget_frames = 0;      // frames past kTexSoftBudgetBytes
	uint32_t gen_window_bytes[kTexGenWindow] = {};   // bytes converted in the first view frames after a generation change
	uint32_t promotions_done = 0;
	uint32_t promotions_deferred = 0;     // next_promotion() stopped on the GX rule with a slot waiting
	uint32_t evictions = 0;
	uint32_t deferred_frees = 0;          // blocks and retired slots freed by on_retired()
	uint32_t generation = 0;
	uint32_t collections_changed = 0;     // bump_generation() calls
	uint32_t alloc_failed_free = 0;       // allocator failed while the cap had room ("alloc failed with free >= request")
	uint32_t discards_tex = 0;            // create() failed: the caller discards `tex`
};

struct TexPromotion
{
	TexSlot* slot;
	void* src;        // the linear texture (slot->handle)
	void* dst;        // the new VRAM texture (taken by the slot at the next on_retired())
	uint32_t bytes;
};

class TexCache
{
public:
	TexCache(TexAllocator& alloc, const TexCacheConfig& cfg);
	~TexCache();   // does not free: call release_all() first (the back end's teardown)
	TexCache(const TexCache&) = delete;
	TexCache& operator=(const TexCache&) = delete;

	// After a frame_begin() returned and before begin_frame(): frees the Retiring slots and the
	// deferred blocks, and switches every slot whose copy was issued to its VRAM texture. Returns
	// the number of blocks/slots freed.
	unsigned on_retired();
	// A view frame starts (`frame` increases by at least 1 each call): per-frame counters reset.
	void begin_frame(uint32_t frame);

	// A hit (current generation only) sets last_used_frame to the current frame. nullptr: miss.
	TexSlot* lookup(const TexKey& key);
	// A miss: a slot with a new Linear texture of tex_bytes(W, key.kind) (evicting as above), or
	// nullptr (discard `tex`). The caller converts into the texture and flushes it. W is a
	// supported size (tex_supported); the cache only needs W * W.
	TexSlot* create(const TexKey& key, unsigned W);

	// The next copy to issue this view frame (VRAM texture already allocated, one GX entry taken
	// from `gx` with the cost of `command_buffer_not_empty`), or false: promote_max reached,
	// nothing waiting, or the GX rule refused (counted in promotions_deferred).
	bool next_promotion(GxQueue& gx, bool command_buffer_not_empty, TexPromotion& out);

	// collections_changed(): every live slot retires, lookups miss from now on. Returns the number
	// of slots retiring (the "[c3d] tex: collections changed" log line).
	unsigned bump_generation();

	// Teardown (after C3D_Fini in the back end): frees everything, whatever its state.
	void release_all();

	uint32_t generation() const { return stats_.generation; }
	uint32_t frame() const { return frame_; }
	const TexStats& stats() const { return stats_; }
	const TexSlot& slot(unsigned i) const { return slots_[i]; }
	unsigned live_count(TexPool pool) const;
	unsigned pending_count() const;
	unsigned retiring_count() const;
	unsigned deferred_block_count() const { return static_cast<unsigned>(deferred_.size()); }
	uint32_t used_bytes(TexPool pool) const { return used_[static_cast<int>(pool)]; }
	uint32_t cap_bytes(TexPool pool) const { return cap_[static_cast<int>(pool)]; }

	// "[c3d] tex entries L/V KiB L/V conv N KiB max/frame gen0..9 promo done/deferred evict
	// deferred-free gen alloc-fail discards" (one line, no newline).
	std::string stats_line() const;

private:
	struct Block { TexPool pool; void* handle; uint32_t bytes; };

	static bool protected_slot(const TexSlot& s, uint32_t frame);
	TexSlot* lru_victim(bool any_pool, TexPool pool, uint32_t last_used_below);
	void evict(TexSlot& s);
	void free_slot_memory(TexSlot& s);
	TexSlot* take_free_slot();
	void* alloc_with_eviction(TexPool pool, uint32_t bytes, uint32_t victims_used_below);

	TexAllocator& alloc_;
	TexCacheConfig cfg_;
	uint32_t cap_[2];
	uint32_t used_[2] = {0, 0};
	TexSlot slots_[kTexSlots];
	std::vector<Block> deferred_;
	TexStats stats_;
	uint32_t frame_ = 0;
	uint32_t next_seq_ = 1;
	uint32_t frame_bytes_ = 0;
	unsigned promoted_this_frame_ = 0;
	int frames_since_gen_ = -1;
};

} // namespace a1_c3d

#endif
