/*
	Aleph One for the Nintendo 3DS: the frame profiler (docs/3ds/PERF.md;
	implementation: ports/3ds/source/profile_3ds.cpp).

	Compiled in only when the port is configured with -DA1_PROFILE=ON, which
	defines A1_PROFILE. Engine call sites are fenced with #ifdef A1_PROFILE,
	so a normal build contains none of it.

	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; either version 3 of the License, or
	(at your option) any later version.

	Deliberately free of <3ds.h>, like a1_3ds_platform.h.
*/
#ifndef A1_3DS_PROFILE_H
#define A1_3DS_PROFILE_H

#ifdef A1_PROFILE

#include <cstdint>

namespace a1_3ds {
namespace prof {

// Main-thread zones. Time is exclusive: a zone nested in another (the HUD
// update inside the world update) is subtracted from the outer one.
enum Zone
{
	World,      // update_world(): game ticks, minus the HUD update
	Render,     // render_view(): the software rasteriser
	Hud,        // HUD buffer update (update_interface) + its blit to the screen surface
	Blit,       // world view -> screen surface (update_screen)
	Present,    // screen surface -> display (MainScreenUpdateRects)
	Bottom,     // the bottom screen in game: HUD/map, buttons, present (bottom_3ds.cpp)
	Idle,       // in-game frame pacing sleep (shell.cpp)
	Overlay,    // this profiler's overlay, once a second; not in the frame times
#ifdef A1_RENDER_C3D
	C3dWait,    // C3D_FrameBegin(0): waiting for the previous citro3d frame (inside Present)
#endif
	kZoneCount
};

// svcGetSystemTick(): the ARM11 clock, 268 111 856 Hz.
uint64_t ticks();

// Main thread only. push() starts timing a zone, pop() stops the innermost
// one; Scope does both for a block.
void push(Zone zone);
void pop();

class Scope
{
public:
	explicit Scope(Zone zone) { push(zone); }
	~Scope() { pop(); }
	Scope(const Scope&) = delete;
	Scope& operator=(const Scope&) = delete;
};

// End of render_screen(): one displayed game frame.
void frame_done();

// The level the debug-perf file asks for (begin_game starts it instead of
// level 0), or -1.
int bench_level();

// True once bench_level() has started a benchmark (begin_game then uses a
// fixed random seed).
bool bench_armed();

} // namespace prof
} // namespace a1_3ds

#define A1_PROF_CAT2(a, b) a##b
#define A1_PROF_CAT(a, b) A1_PROF_CAT2(a, b)
#define A1_PROF_SCOPE(zone) \
	::a1_3ds::prof::Scope A1_PROF_CAT(a1_prof_scope_, __LINE__)(::a1_3ds::prof::zone)

#endif // A1_PROFILE

#endif
