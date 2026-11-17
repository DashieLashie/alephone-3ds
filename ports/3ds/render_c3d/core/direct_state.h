/*
	Aleph One for the Nintendo 3DS, citro3d back end: what the view texture and the engine's world view hold in mode
	direct (docs/3ds/CITRO3D_DESIGN.md, section 7, M6 specification "No clear in direct mode, and what the texture
	holds"), and the eligibility mask's reasons ("Eligibility"). Plain C++, host-tested.

	Mode direct never clears the view texture, so a pixel no primitive covers keeps the last view, as SW's world_pixels
	does. tex_view: the texture holds the last view; dest_view: dest (world_pixels) holds it. Before a frame
	direct_plan() says whether the texture must be seeded from dest or dest synced from the texture; after it
	direct_after() records what the frame left.

	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; either version 3 of the License, or
	(at your option) any later version.
*/
#ifndef A1_C3D_CORE_DIRECT_STATE_H
#define A1_C3D_CORE_DIRECT_STATE_H

#include <cstdint>

namespace a1_c3d {

// How a world frame is drawn.
enum class ViewKind : uint8_t
{
	Direct,     // into the texture without a clear; no readback
	Readback,   // into the texture without a clear; every pixel copied into dest
	Sw,         // by SW into dest
	Clearing    // a diff or calibration frame: the target is cleared, the real frame goes to dest by SW
};

// Whether citro3d may be called before the frame (5.6.1 a).
enum class C3dAvail : uint8_t { Ok, Gate, Off };

enum class SyncSkip : uint8_t { None, Gate, Off, Discard, kCount };

struct DirectState
{
	bool tex_view = false;
	bool dest_view = true;
	bool tex_partial = false;   // the texture holds a discarded (partial) frame
};

struct DirectPlan
{
	bool seed = false;      // copy dest into the texture first (Direct / Readback with !tex_view)
	bool sync = false;      // read the texture back into dest first (Sw / Clearing with !dest_view)
	SyncSkip skip = SyncSkip::None;   // a sync was needed and is not made
};

DirectPlan direct_plan(const DirectState& s, ViewKind kind, C3dAvail avail);
// After the frame. `discarded`: a Direct or Readback frame was given up and redrawn by SW into dest.
void direct_after(DirectState& s, ViewKind kind, bool discarded);
// A new dest, a re-created target, a release: the texture no longer holds the view.
void direct_forget_texture(DirectState& s);

// The eligibility mask (include/a1_c3d.h's DirectBlock bits, in this order): the first set bit's name, or "none".
const char* direct_block_name(unsigned mask);
// The index of the first set bit (0..kDirectBlockCount-1), or -1 for 0.
int direct_block_index(unsigned mask);
constexpr int kDirectBlockCount = 10;

} // namespace a1_c3d

#endif
