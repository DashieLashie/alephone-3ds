/*
	Aleph One for the Nintendo 3DS, citro3d back end: the context shared by
	back/context.cpp (init, GX accounting, release) and back/present.cpp.
	<3ds.h> / <citro3d.h> side only; no engine headers.

	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; either version 3 of the License, or
	(at your option) any later version.
*/
#ifndef A1_C3D_BACK_CONTEXT_H
#define A1_C3D_BACK_CONTEXT_H

#include "../core/config.h"

#include <cstdint>

namespace a1_c3d {
namespace back {

// Initialises citro3d on the first call (design 5.1: lazily, at the first present).
// True while citro3d is up and may be used.
bool ensure_up();
bool is_up();
const Config& config();

// --- the frame, and the GX queue budget (design 2.4) ---

// C3D_FrameBegin(0) (zone c3d_wait); false: already inside a frame, a bug.
bool frame_begin();
// C3D_FrameEnd(GX_CMDLIST_FLUSH) with the queue accounting of the frame closed.
void frame_end(int drawn_linked_targets);
// An operation that enters citro3d's GX queue is about to be issued: false = over the
// budget, do not issue it. Call right before each C3D_Sync* / C3D_RenderTargetClear.
bool gx_take();
// Wrapper form of gx_take(): runs `op` only if the budget allows.
template <class F>
inline bool gx_op(F&& op)
{
	if (!gx_take())
		return false;
	op();
	return true;
}

// Gives the top screen back and shuts citro3d down; idempotent, returns at once when down.
void release_and_fini();
// present_screen() is about to return false; counted while citro3d is up.
void note_false_return();
// Called by __wrap_gfxExit before the real gfxExit(): logs the call, then release_and_fini().
void gfx_exit_hook();

// --- counters (logged in "[c3d] stats" and at release) ---
void count_present();
void count_odd_surface();
void stats_tick();   // logs "[c3d] stats ..." once a second

// --- teardown, called by context.cpp; defined in present.cpp ---
bool present_init();       // after C3D_Init: target, texture, shader, buffers
void present_detach();     // before C3D_Fini: unlink the screen target
void present_free();       // after C3D_Fini: texture, shader, buffers
uint32_t present_counter();

} // namespace back
} // namespace a1_c3d

#endif
