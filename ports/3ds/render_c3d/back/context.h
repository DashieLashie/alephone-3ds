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
#include "../core/gate.h"

#include <cstdint>

namespace a1_c3d {
namespace back {

// Initialises citro3d on the first call (design 5.1: lazily, at the first present).
// True while citro3d is up and may be used.
bool ensure_up();
bool is_up();
// True once citro3d was released for another thread (release_from_any_thread): the present
// then draws nothing, CPU or GPU, for the rest of the run.
bool top_surrendered();
const Config& config();

// --- the frame, and the GX queue budget (design 2.4) ---

// Design 5.6.1 a: the GPU rights are gone (gspHasGpuRight()), the app is closing
// (aptShouldClose()) or suspended (a1_3ds::app_suspended()). Main thread; a few flag reads.
bool gate_closed();
// The gate step of a1_c3d_present(), in the design's order: 1. a pending release is served
// (true: citro3d is down now, the caller returns top_surrendered()); the caller then asks
// gate_present() for 2.: GateSkip = count a skipped present and return true with no citro3d
// call; GateCpu = closed and citro3d not up: return false (CPU present); GateOpen = go on.
bool serve_release();
enum class GatePresent { Open, Skip, Cpu };
GatePresent gate_present();
// Diagnostic d3: one "[c3d] d3 ..." line (the caller decides whether the gate condition holds).
void crumb(const char* what);

// Which profiler zone (A1_PROFILE builds; no effect otherwise) a frame_begin() is timed in:
// Wait = c3d_wait (the default: the previous frame's retirement), Upload = c3d_upload (two_frame's
// frame D: the wait for frame U's upload, design 5.7.4). The zone is opened inside frame_begin()
// itself: a wrapper around the call would hand the time to the nested c3d_wait.
enum class FrameZone { Wait, Upload };
// C3D_FrameBegin(0) (zone c3d_wait, or `zone`). With the gate condition true (only reachable with
// suspend_gate 0, or between frames U and D) the wait is bounded: retire_wait(), and on timeout
// citro3d is abandoned.
FrameStart frame_begin(FrameZone zone = FrameZone::Wait);
// What a frame is, for the queue warning of frame_end(): a present (design: <= 4 entries) or an M0
// view frame (back/view.cpp: the calibration frame, whose out-of-frame readback adds one more
// entry; design 2.4: <= 27).
enum class FrameKind { Present, View };
// C3D_FrameEnd(GX_CMDLIST_FLUSH) with the queue accounting of the frame closed.
// `gate_was_closed`: the gate condition held when the present began (diagnostic d2).
void frame_end(int drawn_linked_targets, bool gate_was_closed = false, FrameKind kind = FrameKind::Present);
// The out-of-frame C3D_SyncDisplayTransfer of the view readback was issued right after a frame_end():
// one more entry in that frame's queue (design 2.4); warns when the frame's total exceeds 27.
void gx_note_out_of_frame();
// A view frame ran (back/view.cpp). The present's next sample_drawing_time(GpuTime::Draw) is skipped
// once: its "last finished queue" would be the view frame and its readback, not the previous present.
void note_view_frame();
// A release was requested from another thread and not served yet (serve_release()). Main thread
// callers that run between presents (back/view.cpp) draw nothing while it is pending.
bool release_pending();
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
// Main thread only.
void release_and_fini();
// True on the thread that brought citro3d up (the main thread: presents come only from it).
bool on_main_thread();
// release_and_fini() from any thread. On the main thread: release_and_fini(), true. On
// another thread: no citro3d call; asks the main thread to release at its next present and
// sleeps until it has (true) or until a bounded wait runs out (false: citro3d still owns the
// top screen). True at once when citro3d is down; the request is then still recorded, so
// citro3d does not start later (ensure_up). `who` names the caller in the log.
bool release_from_any_thread(const char* who);
// present_screen() is about to return false; counted while citro3d is up.
void note_false_return();
// Called by __wrap_gfxExit before the real gfxExit(): logs the call, then
// release_from_any_thread(). False: citro3d is still up; do not call the real gfxExit().
bool gfx_exit_hook();

// --- counters (logged in "[c3d] stats" and at release) ---
void count_present();
void count_odd_surface();
void stats_tick();   // logs "[c3d] stats ..." once a second
// C3D_GetDrawingTime() (the last *finished* queue) right after a successful frame_begin() of a
// present, averaged into the stats line (design 5.7.3). `Draw`: the previous present's last
// frame (after the first or only frame's begin): with two_frame 1 frame D (draw + framebuffer
// transfer), with two_frame 0 the whole present (upload too); `Upload`: frame U, the staging
// upload (after two_frame's frame D's begin).
enum class GpuTime { Draw, Upload };
void sample_drawing_time(GpuTime which);

// --- defined in front/frame.cpp (M0 piece 2b), called by release_and_fini() just before the final
// stats line: logs the null-mode and diff summaries.
void frame_final_stats();

// --- teardown, called by context.cpp; defined in present.cpp ---
// close_frame (console run 1): one frame that draws the present quad from the present texture
// into the linked target, with d3 breadcrumbs. False: citro3d was abandoned meanwhile.
bool present_close_frame();
bool present_init();       // after C3D_Init: target, texture, shader, buffers
void present_detach();     // before C3D_Fini: unlink the screen target
void present_free();       // after C3D_Fini: texture, shader, buffers
uint32_t present_counter();

} // namespace back
} // namespace a1_c3d

#endif
