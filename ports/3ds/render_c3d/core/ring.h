/*
	Aleph One for the Nintendo 3DS, citro3d back end: the bookkeeping of the vertex/index ring
	(docs/3ds/CITRO3D_DESIGN.md, 2.1 and section 7, M1 specification, "Back end"): two halves,
	each with `vertices` vertex slots and `indices` index slots; a view frame fills one half
	from 0, the next frame the other. Plain C++, host-tested; the memory itself is back/draw.cpp's.

	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; either version 3 of the License, or
	(at your option) any later version.
*/
#ifndef A1_C3D_CORE_RING_H
#define A1_C3D_CORE_RING_H

#include <algorithm>

namespace a1_c3d {

class RingState
{
public:
	void configure(unsigned vertices, unsigned indices)
	{
		vertices_ = vertices;
		indices_ = indices;
	}

	// A new frame: the other half (the first frame uses half 0), empty.
	void begin_frame()
	{
		half_ = started_ ? 1 - half_ : 0;
		started_ = true;
		nv_ = ni_ = 0;
	}

	// Can a polygon of nv vertices and ni indices still be appended to the current half?
	bool fits(unsigned nv, unsigned ni) const { return nv_ + nv <= vertices_ && ni_ + ni <= indices_; }

	// Appends; returns the vertex index (relative to the half) of the first new vertex. The caller
	// checked fits(). The index offset of the new indices is index_used() before the call.
	unsigned take(unsigned nv, unsigned ni)
	{
		const unsigned base = nv_;
		nv_ += nv;
		ni_ += ni;
		hw_v_[half_] = std::max(hw_v_[half_], nv_);
		hw_i_[half_] = std::max(hw_i_[half_], ni_);
		return base;
	}

	int half() const { return half_; }
	unsigned vertices_used() const { return nv_; }
	unsigned index_used() const { return ni_; }
	unsigned high_water_vertices(int half) const { return hw_v_[half]; }
	unsigned high_water_indices(int half) const { return hw_i_[half]; }

private:
	unsigned vertices_ = 0, indices_ = 0;
	int half_ = 1;
	bool started_ = false;
	unsigned nv_ = 0, ni_ = 0;
	unsigned hw_v_[2] = {0, 0}, hw_i_[2] = {0, 0};
};

} // namespace a1_c3d

#endif
