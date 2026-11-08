/*
	Aleph One for the Nintendo 3DS, citro3d back end: the mode-direct view state (see direct_state.h).

	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; either version 3 of the License, or
	(at your option) any later version.
*/
#include "direct_state.h"

namespace a1_c3d {

DirectPlan direct_plan(const DirectState& s, ViewKind kind, C3dAvail avail)
{
	DirectPlan p;
	switch (kind)
	{
	case ViewKind::Direct:
	case ViewKind::Readback:
		p.seed = !s.tex_view;
		break;
	case ViewKind::Sw:
	case ViewKind::Clearing:
		if (s.dest_view)
			break;
		// The texture holds the newer view; read it back only when citro3d may be called and it is complete.
		if (avail == C3dAvail::Gate)
			p.skip = SyncSkip::Gate;
		else if (avail == C3dAvail::Off)
			p.skip = SyncSkip::Off;
		else if (!s.tex_view)
			p.skip = SyncSkip::Discard;
		else
			p.sync = true;
		break;
	}
	return p;
}

void direct_after(DirectState& s, ViewKind kind, bool discarded)
{
	if ((kind == ViewKind::Direct || kind == ViewKind::Readback) && !discarded)
	{
		s.tex_view = true;
		s.tex_partial = false;
		s.dest_view = kind == ViewKind::Readback;
		return;
	}
	// SW drew the frame into dest (a discard's redraw included); the texture holds an older or partial view, or (a
	// clearing frame) a cleared target.
	s.tex_partial = discarded;
	s.tex_view = false;
	s.dest_view = true;
}

void direct_forget_texture(DirectState& s)
{
	s.tex_view = false;
	s.tex_partial = false;
}

const char* direct_block_name(unsigned mask)
{
	static const char* const names[kDirectBlockCount] = {"not_native", "view_rect", "accel",   "map",     "terminal",
	                                                     "gamma",      "darken",    "network", "lua_hud", "movie"};
	const int i = direct_block_index(mask);
	return i < 0 ? "none" : names[i];
}

int direct_block_index(unsigned mask)
{
	for (int i = 0; i < kDirectBlockCount; ++i)
		if (mask >> i & 1)
			return i;
	return mask ? kDirectBlockCount - 1 : -1;   // an unknown high bit counts as the last reason
}

} // namespace a1_c3d
