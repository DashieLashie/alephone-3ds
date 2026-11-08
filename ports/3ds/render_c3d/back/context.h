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
#include "../core/fade_fit.h"
#include "../core/gate.h"
#include "../core/gx_queue.h"

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
// M8: the Renderer preference (graphics_preferences->c3d_renderer), called by the front end before each frame's first
// decision. Without a debug-c3d file it switches the configuration (config_apply_preference) and logs the change;
// with the file it is ignored (logged once): the file decides, so every test and runbook keeps working.
void config_preference(bool gpu);

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
// view frame (back/view.cpp: the calibration frame, whose readback transfer is queued inside it and
// counted by gx_op; design 2.4: <= 27).
// M6: Composite = a direct frame's present (band uploads + end + 1 linked: <= 6).
enum class FrameKind { Present, View, Composite };
// C3D_FrameEnd(GX_CMDLIST_FLUSH) with the queue accounting of the frame closed.
// `gate_was_closed`: the gate condition held when the present began (diagnostic d2).
void frame_end(int drawn_linked_targets, bool gate_was_closed = false, FrameKind kind = FrameKind::Present);
// A view frame ran (back/view.cpp). The present's next sample_drawing_time(GpuTime::Draw) is skipped
// once: its "last finished queue" would be the view frame and its readback, not the previous present.
void note_view_frame();
// A release was requested from another thread and not served yet (serve_release()). Main thread
// callers that run between presents (back/view.cpp) draw nothing while it is pending.
bool release_pending();
// An operation that enters citro3d's GX queue is about to be issued: false = over the
// budget, do not issue it. Call right before each C3D_Sync* / C3D_RenderTargetClear.
bool gx_take();
// The frame's queue accounting itself, for the texture cache's promotion rule (core/texcache
// next_promotion takes its entry from it, with the same cost as gx_take()).
GxQueue& gx_queue();
// Wrapper form of gx_take(): runs `op` only if the budget allows.
template <class F>
inline bool gx_op(F&& op)
{
	if (!gx_take())
		return false;
	op();
	return true;
}

// The flushing split of a view frame (C3D_FrameSplit(GX_CMDLIST_FLUSH)): one queue entry whatever
// the command buffer holds (GxQueue::take_split, M1 specification "GX queue"). Runs `op` only if
// the budget allows.
bool gx_take_split();
template <class F>
inline bool gx_split(F&& op)
{
	if (!gx_take_split())
		return false;
	op();
	return true;
}

// --- M1: the program record, and the command-buffer measurements (M1 specification, "State save
// and restore around a view frame" and "Reserves and the command buffer, measured") ---

// The shaderProgram_s* last passed to C3D_BindProgram by anyone but back/draw.cpp (which binds
// through __real_C3D_BindProgram): the present's. Opaque here (shaderProgram_s is an anonymous
// typedef); nullptr before the first bind. `program_binds()` counts the recorded binds.
void* program_recorded();
unsigned program_binds();
// M2: the same for the texture on unit 0 (-Wl,--wrap=C3D_TexBind): the C3D_Tex* last bound to unit 0 by
// anyone but back/draw.cpp (which binds through __real_C3D_TexBind): the present's (present.cpp binds it
// once at init). `tex_binds()` counts the recorded binds of every unit.
void* tex_recorded();
unsigned tex_binds();
// M3: C3D_TexBind calls for units 1-2 by anyone but back/ (which binds unit 1, the shade LUT, through the real
// function): must stay 0 (design 7, M3 "Binding and the state the present relies on").
unsigned tex1_foreign_binds();
// The C3D_Init command-buffer size in words (cfg.cmdbuf_kib * 256).
uint32_t cmdbuf_size_words();
// Words of one C3D_DrawElements (state flush included) and of the readback sequence (flushing split +
// transfer), from back/draw.cpp and back/view.cpp; the per-frame maxima are kept here.
void cmdbuf_note_draw(uint32_t words);
void cmdbuf_note_readback(uint32_t words);
// "[c3d] cmdbuf ..." line, once a second (stats_tick, only after a view frame ran) and in the final stats.
void cmdbuf_log(const char* tag);

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
// frame (after the first or only frame's begin): in a two-frame present (two_frame 1 or zero-copy)
// frame D (draw + framebuffer transfer), in a one-frame present the whole present (upload too);
// `Upload`: frame U, the upload (after a two-frame present's frame D's begin).
// M6 `View`: after a composite frame's frame_begin, the direct view frame it waited for (G3-b's figure).
enum class GpuTime { Draw, Upload, View };
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

// --- M6b (design 7, M6b specification "The mechanism", "Lifetime"); defined in present.cpp ---
// This frame's fade, handed from hook 3a (front/frame.cpp) to the composite: copied; nullptr clears it. direct_policy
// clears it first every frame; the next composite consumes it, so a frame the gate skipped hands nothing on.
void fade_frame_set(const FadeFrame* frame);

} // namespace back
} // namespace a1_c3d

#endif
