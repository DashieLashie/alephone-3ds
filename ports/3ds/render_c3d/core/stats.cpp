/*
	Aleph One for the Nintendo 3DS, citro3d back end: null-mode bookkeeping (see stats.h).

	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; either version 3 of the License, or
	(at your option) any later version.
*/
#include "stats.h"

#include <algorithm>

namespace a1_c3d {

uint32_t Samples::percentile(int pct) const
{
	if (values_.empty())
		return 0;
	pct = std::max(0, std::min(100, pct));
	std::vector<uint32_t> sorted(values_);
	std::sort(sorted.begin(), sorted.end());
	const size_t n = sorted.size();
	size_t rank = (static_cast<size_t>(pct) * n + 99) / 100;   // ceil(pct * n / 100)
	if (rank < 1)
		rank = 1;
	return sorted[rank - 1];
}

void PairTable::clear_all()
{
	for (Slot& s : slots_)
		s.gen = 0;
}

void PairTable::begin_frame()
{
	count_ = 0;
	full_ = false;
	if (++gen_ == 0)   // the 32-bit tag wrapped: stale slots could look live again
	{
		clear_all();
		gen_ = 1;
	}
}

bool PairTable::insert(uintptr_t a, uintptr_t b)
{
	uint64_t h = (static_cast<uint64_t>(a) * 0x9E3779B97F4A7C15ull) ^ (static_cast<uint64_t>(b) + 0x7F4A7C15u);
	h *= 0xBF58476D1CE4E5B9ull;
	h ^= h >> 29;
	unsigned i = static_cast<unsigned>(h) % kSlots;
	for (unsigned probe = 0; probe < kSlots; ++probe, i = (i + 1) % kSlots)
	{
		Slot& s = slots_[i];
		if (s.gen != gen_)   // free for this frame
		{
			s.a = a;
			s.b = b;
			s.gen = gen_;
			++count_;
			return true;
		}
		if (s.a == a && s.b == b)
			return false;
	}
	full_ = true;   // a new pair and no free slot
	return false;
}

} // namespace a1_c3d
