/*
	Aleph One for the Nintendo 3DS, citro3d back end: the null mode's bounded bookkeeping
	(docs/3ds/CITRO3D_DESIGN.md, section 7, M0 specification, "Null mode"). Plain C++,
	host-testable.

	  Samples     per-frame values in a fixed array (beyond its capacity only the max and the
	              count go on); nearest-rank percentile, called only when logging
	  PairTable   the distinct (a, b) pairs of one frame in a fixed 1 024-slot open-addressing
	              table with a per-frame generation tag, so begin_frame() clears nothing

	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; either version 3 of the License, or
	(at your option) any later version.
*/
#ifndef A1_C3D_CORE_STATS_H
#define A1_C3D_CORE_STATS_H

#include <cstddef>
#include <cstdint>
#include <vector>

namespace a1_c3d {

class Samples
{
public:
	explicit Samples(size_t capacity = 16384) : capacity_(capacity) { values_.reserve(capacity); }

	// No allocation: the storage was reserved by the constructor.
	void add(uint32_t v)
	{
		if (values_.size() < capacity_)
			values_.push_back(v);
		if (v > max_)
			max_ = v;
		++count_;
	}

	uint32_t max() const { return max_; }
	size_t count() const { return count_; }       // all values added, stored or not
	size_t stored() const { return values_.size(); }

	// Nearest rank over the stored values: rank = ceil(pct/100 * n), 1-based, of the sorted
	// values. 0 if none. pct is clamped to 0..100 (rank at least 1).
	uint32_t percentile(int pct) const;

private:
	size_t capacity_;
	std::vector<uint32_t> values_;
	uint32_t max_ = 0;
	size_t count_ = 0;
};

class PairTable
{
public:
	static constexpr unsigned kSlots = 1024;

	PairTable() { clear_all(); }

	void begin_frame();

	// True if the pair is new this frame. False: seen already, or the table is full (full() is set).
	bool insert(uintptr_t a, uintptr_t b);

	unsigned count() const { return count_; }   // distinct pairs this frame; 1024 when full
	bool full() const { return full_; }

private:
	struct Slot
	{
		uintptr_t a, b;
		uint32_t gen;   // 0 = never used; live if equal to gen_
	};

	void clear_all();

	Slot slots_[kSlots];
	uint32_t gen_ = 1;
	unsigned count_ = 0;
	bool full_ = false;
};

} // namespace a1_c3d

#endif
