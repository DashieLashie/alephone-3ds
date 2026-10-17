/*
	Aleph One for the Nintendo 3DS, citro3d back end: GX command queue accounting
	(docs/3ds/CITRO3D_DESIGN.md, 2.4). citro3d's queue holds 32 entries and
	svcBreaks on the 33rd, so every GX operation is counted per C3D frame against a
	hard budget. Plain C++, host-testable.

	Cost of one operation: 1 entry, plus 1 more if the frame's command buffer is not
	empty at the call (citro3d then splits the command list first).

	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; either version 3 of the License, or
	(at your option) any later version.
*/
#ifndef A1_C3D_CORE_GX_QUEUE_H
#define A1_C3D_CORE_GX_QUEUE_H

#include <algorithm>

namespace a1_c3d {

class GxQueue
{
public:
	static constexpr int kBudget = 24;         // operations; leaves room for C3D_FrameEnd + linked targets
	static constexpr int kFrameEndEntries = 1; // C3D_FrameEnd itself

	void begin_frame() { used_ = 0; }

	// An operation about to be issued. False: refused (budget); do not issue it.
	bool take(bool command_buffer_not_empty)
	{
		const int cost = 1 + (command_buffer_not_empty ? 1 : 0);
		if (used_ + cost > kBudget)
		{
			++refused_;
			return false;
		}
		used_ += cost;
		return true;
	}

	// The frame is being ended with `linked_targets` drawn linked targets; returns the
	// frame's total queue entries and keeps the high-water mark.
	int end_frame(int linked_targets)
	{
		const int total = used_ + kFrameEndEntries + linked_targets;
		high_water_ = std::max(high_water_, total);
		return total;
	}

	int used() const { return used_; }
	int high_water() const { return high_water_; }
	unsigned refused() const { return refused_; }

private:
	int used_ = 0;
	int high_water_ = 0;
	unsigned refused_ = 0;
};

} // namespace a1_c3d

#endif
