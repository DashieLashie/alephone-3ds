/*
	Aleph One for the Nintendo 3DS, citro3d back end: lazy init, the frame, the GX
	queue budget, release/fini, and the gfxExit wrapper
	(docs/3ds/CITRO3D_DESIGN.md, 1.3, 2.4 and 5.1).

	citro3d is brought up at the first present, not at boot: the scenario chooser
	writes the top screen through SDL before that. From then on citro3d is the only
	writer of the top framebuffer until release_and_fini() (HOME > Close, vhalt(),
	the error screen), which is idempotent and also runs from __wrap_gfxExit.

	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; either version 3 of the License, or
	(at your option) any later version.
*/
#include "context.h"

#include "../core/gate.h"
#include "../core/gx_queue.h"
#include "../include/a1_c3d_port.h"
#include "a1_3ds_platform.h"
#include "a1_3ds_profile.h"

#include <3ds.h>
#include <citro3d.h>

#include <stdio.h>

#include <atomic>
#include <fstream>
#include <sstream>
#include <string>

extern "C" void __real_gfxExit(void);

namespace a1_c3d {
namespace back {

namespace {

// Design 2.4: C3D_Init(512 KiB); P1's present needs a few hundred words.
constexpr size_t kCmdBufBytes = 512 * 1024;

// How long a thread other than main waits for the main thread to release citro3d
// (release_from_any_thread). The main thread presents every 33 ms in menus and in game;
// whether it presents during a level load (3-4 s on the console) is not established, so the
// wait covers a few seconds of not presenting: assumed, not measured. Past it the main thread
// is taken to be blocked (e.g. on a lock the dying thread holds); the process is exiting.
constexpr int kHandoffWaitMs = 2000;

Config s_cfg;
bool s_cfg_loaded = false;
// citro3d and its GX queue are used only by the main thread (citro3d is not thread-safe).
// s_up and s_release_requested are also read or set from other threads (the hand-off
// below); everything else here is main-thread state.
std::atomic<bool> s_up{false};                  // set after a successful init, cleared by release_and_fini()
std::atomic<bool> s_release_requested{false};   // another thread asked the main thread to release
u32 s_main_thread_id = 0;                       // kernel thread id of the thread that brought citro3d up
bool s_off = false;         // not in use for the rest of the run (cpu present, init failed, released)
bool s_surrendered = false; // released for another thread: the top screen is left alone from then on
bool s_in_frame = false;    // a C3D frame of ours is open
bool s_abandoned = false;   // release() gave up on a queue that did not retire (design 5.6.1 b)
bool s_releasing = false;   // release_and_fini() is running (a fatal error inside it must not re-enter)
GxQueue s_gx;
GateTracker s_gate;
unsigned s_frames_gate_closed = 0;   // frames ended while the gate condition held (only with suspend_gate 0)
uint64_t s_last_frame_end = 0;       // system tick of the last frame_end()

unsigned s_presents = 0, s_odd = 0, s_false_returns = 0, s_releases = 0, s_gfx_exits = 0;
unsigned s_stats_presents = 0;   // presents at the previous stats line
uint64_t s_stats_tick = 0;

constexpr uint64_t kTicksPerSecond = SYSCLOCK_ARM11;

void load_config()
{
	if (s_cfg_loaded)
		return;
	s_cfg_loaded = true;
	std::ifstream in(a1_3ds::user_dir() + "/debug-c3d");
	if (!in)
		return;
	std::stringstream text;
	text << in.rdbuf();
	std::vector<std::string> problems;
	config_parse(s_cfg, text.str(), problems);
	printf("[c3d] debug-c3d: present %s, calibrate %d, alt_load %d, odd_surface %d, test_vhalt %d, "
	       "present_diff %u, suspend_gate %d, retire_wait_ms %u, force_abandon %d, close_frame %d\n",
	       s_cfg.present_gpu ? "gpu" : "cpu", s_cfg.calibrate, s_cfg.alt_load, s_cfg.odd_surface,
	       s_cfg.test_vhalt, s_cfg.present_diff, s_cfg.suspend_gate, s_cfg.retire_wait_ms,
	       s_cfg.force_abandon, s_cfg.close_frame);
	if (s_cfg.present_diff && s_cfg.calibrate)
		printf("[c3d] debug-c3d: present_diff ignored with calibrate (the target holds a flat colour)\n");
	for (const std::string& p : problems)
		printf("[c3d] debug-c3d: %s\n", p.c_str());
}

unsigned long kib(u32 bytes)
{
	return static_cast<unsigned long>(bytes / 1024);
}

void log_final_stats()
{
	printf("[c3d] final: presents=%u odd_surface=%u gx_high_water=%d gx_refused=%u "
	       "present_screen_false_returns=%u gate_skipped=%u gate_episodes=%u frames_gate_closed=%u%s\n",
	       s_presents, s_odd, s_gx.high_water(), s_gx.refused(), s_false_returns, s_gate.total_skipped(),
	       s_gate.episodes(), s_frames_gate_closed, s_abandoned ? " abandoned=1" : "");
}

u32 current_thread_id()
{
	u32 id = 0;
	if (R_FAILED(svcGetThreadId(&id, CUR_THREAD_HANDLE)))   // svc.h:913, CUR_THREAD_HANDLE svc.h:133
		return 0xFFFFFFFF;                                  // matches no thread: treated as "not main"
	return id;
}

GateInputs gate_inputs()
{
	GateInputs g;
	g.gpu_right = gspHasGpuRight();            // gspgpu.h:122: a cached flag, cleared by GSPGPU_ReleaseRight
	g.should_close = aptShouldClose();         // apt.h:155
	g.suspended = a1_3ds::app_suspended();     // input_3ds.cpp: s_in_home || s_asleep
	return g;
}

uint64_t now_ms()
{
	return svcGetSystemTick() / (SYSCLOCK_ARM11 / 1000);
}

// Design 5.6.1 b: poll C3D_FrameBegin(C3D_FRAME_NONBLOCK) (renderqueue.h:24, "Return false instead of
// waiting if the GPU is busy"), sleeping 1 ms between the tries, up to `limit_ms` (> 0). True =
// retired and now inside a frame. Never gxCmdQueueWait(q, N > 0): gx.h:125, any positive timeout
// blocks in gspWaitForAnyEvent, which after a Close may never be signalled.
bool wait_retired(uint32_t limit_ms, uint32_t& waited_ms)
{
	return retire_wait([] { return C3D_FrameBegin(C3D_FRAME_NONBLOCK); },
	                   [](int ms) { svcSleepThread(1000000LL * ms); }, now_ms, limit_ms,
	                   waited_ms) == RetireResult::Retired;
}

// Design 5.6.1 b: give up on a queue that does not retire. C3D_Fini (unbounded wait) and
// present_free (the GPU might still read the staging image and texture if it ever ran the work)
// are skipped; the top screen is surrendered for good, so no CPU present writes into the
// framebuffer the abandoned transfer targets. s_up goes false last (a thread waiting in
// release_from_any_thread goes on only when this one is done). The real gfxExit() still runs
// (__wrap_gfxExit): without gspExit the GSP event thread outlives the heap (STAGE5.md).
void abandon(const char* where, uint32_t waited_ms, const char* why)
{
	printf("[c3d] release abandoned (%s): %s after %u ms (retire_wait_ms %u); C3D_Fini and the buffers are "
	       "left alone, the top screen is surrendered\n",
	       where, why, waited_ms, s_cfg.retire_wait_ms);
	s_in_frame = false;
	s_off = true;
	s_surrendered = true;
	s_abandoned = true;
	log_final_stats();
	fflush(stdout);
	s_up = false;
	s_releasing = false;
}

} // namespace

bool on_main_thread()
{
	return current_thread_id() == s_main_thread_id;
}

bool release_from_any_thread(const char* who)
{
	if (!s_up)
	{
		// Not up (yet): make sure it never comes up after a fatal error elsewhere (ensure_up).
		s_release_requested = true;
		return true;
	}
	if (on_main_thread())
	{
		release_and_fini();
		return true;
	}
	// Critic P1 minor 4: error_screen() (on_abort/on_terminate, alert_user) and SDL_Quit's
	// gfxExit() can run on another thread, e.g. an assertion on the audio thread, while the
	// main thread is inside a C3D frame. citro3d is not thread-safe, so this thread makes no
	// citro3d call: it asks the main thread, whose next present (ensure_up) releases, and
	// sleeps until then (never spins: Horizon threads are cooperative). The wait is bounded:
	// a main thread that does not present again (blocked) leaves citro3d up, and the caller
	// then keeps off the top screen. The process exits afterwards in every case.
	printf("[c3d] %s on a thread other than main (thread %lu, main %lu): asking the main thread to release\n",
	       who, static_cast<unsigned long>(current_thread_id()), static_cast<unsigned long>(s_main_thread_id));
	fflush(stdout);
	s_release_requested = true;
	for (int ms = 0; ms < kHandoffWaitMs && s_up; ++ms)
		svcSleepThread(1000000);
	if (!s_up)
	{
		printf("[c3d] %s: released by the main thread\n", who);
		fflush(stdout);
		return true;
	}
	printf("[c3d] %s: the main thread did not release within %d ms; citro3d left alone (still owns the top "
	       "screen)\n", who, kHandoffWaitMs);
	fflush(stdout);
	return false;
}

void release_and_fini()
{
	// s_up is cleared only at the end (after the logs); a fatal error raised on this thread
	// while releasing (e.g. by a failing printf) must not release a second time.
	if (!s_up || s_releasing)
		return;
	s_releasing = true;
	++s_releases;
	const Config& cfg = config();
	const GateInputs g = gate_inputs();
	const uint64_t since_ms =
		s_last_frame_end ? (svcGetSystemTick() - s_last_frame_end) / (SYSCLOCK_ARM11 / 1000) : 0;
	// d2: the state first, then one line after each step.
	printf("[c3d] release #%u: in_frame %d, gate %s (gpu_right %d, should_close %d, suspended %d), "
	       "suspend_gate %d, frames ended with the gate closed %u, skipped presents %u, %llu ms since the "
	       "last frame end\n",
	       s_releases, s_in_frame, g.closed() ? "closed" : "open", g.gpu_right, g.should_close, g.suspended,
	       cfg.suspend_gate, s_frames_gate_closed, s_gate.total_skipped(),
	       static_cast<unsigned long long>(since_ms));
	fflush(stdout);
	// Design 1.3 / 5.1: end any open frame; an empty frame (the queue has retired); detach;
	// C3D_Fini (which waits for the queue and destroys the targets); then the top screen is the
	// console's again. 5.6.1 b: the wait for the queue is bounded, and gives up (abandon).
	if (s_in_frame)
	{
		C3D_FrameEnd(0);
		s_in_frame = false;
		printf("[c3d] release: open frame ended\n");
		fflush(stdout);
	}
	if (cfg.force_abandon)
	{
		abandon("release", 0, "force_abandon 1, not polling");
		return;
	}
	if (cfg.close_frame && g.closed())
	{
		printf("[c3d] release: close_frame 1: one forced present frame (gate condition holds)\n");
		fflush(stdout);
		if (!present_close_frame())
			return;   // abandoned inside (frame_begin)
	}
	uint32_t waited = 0;
	bool retired;
	if (cfg.retire_wait_ms == 0)
	{
		printf("[c3d] release: waiting for the queue (unbounded: retire_wait_ms 0)\n");
		fflush(stdout);
		const uint64_t t0 = now_ms();
		retired = C3D_FrameBegin(0);
		waited = static_cast<uint32_t>(now_ms() - t0);
	}
	else
	{
		printf("[c3d] release: waiting for the queue (up to %u ms)\n", cfg.retire_wait_ms);
		fflush(stdout);
		retired = wait_retired(cfg.retire_wait_ms, waited);
	}
	if (!retired)
	{
		abandon("release", waited, "the GPU queue did not retire");
		return;
	}
	printf("[c3d] release: queue retired after %u ms\n", waited);
	C3D_FrameEnd(0);   // the empty frame: no GPU work, one whole-heap GSPGPU_FlushDataCache (design 5.6.1 b)
	printf("[c3d] release: empty frame ended\n");
	present_detach();
	printf("[c3d] release: detached; calling C3D_Fini\n");
	fflush(stdout);
	C3D_Fini();
	present_free();
	s_off = true;
	printf("[c3d] release done: C3D_Fini returned; linear free %lu KiB, VRAM free %lu KiB\n",
	       kib(linearSpaceFree()), kib(vramSpaceFree()));
	log_final_stats();
	fflush(stdout);
	// Last: a thread waiting in release_from_any_thread() goes on (and may take the console
	// over) only when this thread is done, logging included.
	s_up = false;
	s_releasing = false;
}

bool top_surrendered()
{
	return s_surrendered;
}

bool is_up()
{
	return s_up;
}

const Config& config()
{
	load_config();
	return s_cfg;
}

bool ensure_up()
{
	if (s_up)
		return !serve_release();
	if (s_off || s_release_requested)
		return false;
	load_config();
	if (!s_cfg.present_gpu)
	{
		s_off = true;
		printf("[c3d] present cpu: citro3d not started\n");
		return false;
	}
	if (gfxGetScreenFormat(GFX_TOP) != GSP_RGBA8_OES)
	{
		s_off = true;
		printf("[c3d] top screen format is not RGBA8: citro3d not started\n");
		return false;
	}
	if (gate_closed())   // design 5.6.1 a: init only ever happens with the gate open
		return false;
	const u32 linear0 = linearSpaceFree(), vram0 = vramSpaceFree();
	if (!C3D_Init(kCmdBufBytes))
	{
		s_off = true;
		printf("[c3d] C3D_Init failed: CPU present\n");
		return false;
	}
	if (!present_init())
	{
		present_detach();
		C3D_Fini();
		present_free();
		s_off = true;
		printf("[c3d] present setup failed: CPU present\n");
		return false;
	}
	// Presents come only from the main thread (present_screen()), so this is its id.
	s_main_thread_id = current_thread_id();
	s_up = true;
	s_gx = GxQueue();
	s_stats_tick = svcGetSystemTick();
	printf("[c3d] init ok: command buffer %lu KiB; linear free %lu -> %lu KiB, VRAM free %lu -> %lu KiB\n",
	       kib(kCmdBufBytes), kib(linear0), kib(linearSpaceFree()), kib(vram0), kib(vramSpaceFree()));
	fflush(stdout);
	return true;
}

bool gate_closed()
{
	return gate_inputs().closed();
}

bool serve_release()
{
	if (!s_up || !s_release_requested)
		return false;
	// The other half of release_from_any_thread(): this is the main thread. From now on
	// the top screen is left alone (top_surrendered()): the requester goes on to the real
	// gfxExit(), and a CPU present would write into framebuffers being freed.
	printf("[c3d] release requested by another thread: releasing on the main thread; the top screen "
	       "is left alone from now on\n");
	s_surrendered = true;
	release_and_fini();
	return true;
}

GatePresent gate_present()
{
	const Config& cfg = config();
	const GateInputs g = gate_inputs();
	const bool closed = g.closed();
	const bool have_citro = s_up || s_surrendered;
	const bool skip = closed && cfg.suspend_gate && have_citro;
	if (have_citro)
	{
		// d5: transitions, once each; the pure logic is core/gate.h (host-tested).
		const unsigned n = s_presents + s_gate.total_skipped() + (skip ? 0 : 1);
		switch (s_gate.on_present(closed, skip))
		{
		case GateTracker::Event::Closed:
			printf("[c3d] gate closed at present %u (gpu_right %d, should_close %d, suspended %d)%s\n", n,
			       g.gpu_right, g.should_close, g.suspended,
			       cfg.suspend_gate ? "" : "; suspend_gate 0: presenting anyway");
			fflush(stdout);
			break;
		case GateTracker::Event::Reopened:
			printf("[c3d] gate open again after %u skipped presents\n", s_gate.episode_skipped());
			fflush(stdout);
			break;
		case GateTracker::Event::None:
			break;
		}
	}
	if (!closed || !cfg.suspend_gate)
		return GatePresent::Open;
	return have_citro ? GatePresent::Skip : GatePresent::Cpu;
}

void crumb(const char* what)
{
	printf("[c3d] d3 %s\n", what);
	fflush(stdout);
}

FrameStart frame_begin()
{
	if (!s_up || s_in_frame)
		return FrameStart::AlreadyInFrame;
	const Config& cfg = config();
	bool ok;
	{
#ifdef A1_PROFILE
		A1_PROF_SCOPE(C3dWait);
#endif
		// With the gate open this is the measured path, unchanged (never C3D_FRAME_SYNCDRAW, design
		// 2.4). With the gate condition true (only reachable with suspend_gate 0, or from
		// release's close_frame) the wait is bounded (design 5.6.1 b).
		if (cfg.retire_wait_ms != 0 && gate_closed())
		{
			char line[96];
			snprintf(line, sizeof line, "frame_begin: before (bounded, up to %u ms)", cfg.retire_wait_ms);
			crumb(line);
			uint32_t waited = 0;
			ok = wait_retired(cfg.retire_wait_ms, waited);
			if (!ok)
			{
				abandon("frame_begin", waited, "the GPU queue did not retire");
				return FrameStart::Abandoned;
			}
			snprintf(line, sizeof line, "frame_begin: after, retired in %u ms", waited);
			crumb(line);
		}
		else
			ok = C3D_FrameBegin(0);
	}
	if (!ok)
		return FrameStart::AlreadyInFrame;
	s_in_frame = true;
	s_gx.begin_frame();
	return FrameStart::Ok;
}

void frame_end(int drawn_linked_targets, bool gate_was_closed)
{
	const int total = s_gx.end_frame(drawn_linked_targets);
	if (total > 4)
		printf("[c3d] warning: %d GX queue entries in one present frame (design: <= 4)\n", total);
	if (gate_was_closed)
		++s_frames_gate_closed;
	C3D_FrameEnd(GX_CMDLIST_FLUSH);
	s_in_frame = false;
	s_last_frame_end = svcGetSystemTick();
}

bool gx_take()
{
	return s_gx.take(gpuCmdBufOffset != 0);
}

void note_false_return()
{
	if (!s_up)
		return;
	if (++s_false_returns == 1)
		printf("[c3d] warning: present_screen() returned false while citro3d is up\n");
}

bool gfx_exit_hook()
{
	++s_gfx_exits;
	printf("[c3d] gfxExit call #%u (citro3d %s)\n", s_gfx_exits, s_up ? "up: release first" : "down");
	fflush(stdout);
	return release_from_any_thread("gfxExit");
}

void count_present()
{
	++s_presents;
}

void count_odd_surface()
{
	++s_odd;
}

void stats_tick()
{
	const uint64_t now = svcGetSystemTick();
	if (now - s_stats_tick < kTicksPerSecond)
		return;
	s_stats_tick = now;
	printf("[c3d] stats presents=%u (+%u) odd_surface=%u gx_high_water=%d gx_refused=%u "
	       "cmdbuf_usage=%.4f present_screen_false_returns=%u gate_skipped=%u\n",
	       s_presents, s_presents - s_stats_presents, s_odd, s_gx.high_water(), s_gx.refused(),
	       static_cast<double>(C3D_GetCmdBufUsage()), s_false_returns, s_gate.total_skipped());
	s_stats_presents = s_presents;
}

} // namespace back
} // namespace a1_c3d

// --- the port-internal API (a1_c3d_port.h) -------------------------------------

bool a1_c3d_release_top()
{
	return a1_c3d::back::release_from_any_thread("release_top");
}

bool a1_c3d_owns_top()
{
	return a1_c3d::back::s_up.load();
}

void a1_c3d_note_false_return()
{
	a1_c3d::back::note_false_return();
}

// -Wl,--wrap=gfxExit: every teardown path (HOME > Close, vhalt(), the error screen)
// reaches libctru's gfxExit() through here, so citro3d is shut down before the
// framebuffers go away. Idempotent: with citro3d down it only calls the real one
// (design 1.3). Called on a thread other than main while citro3d stays up (the hand-off
// timed out), the real gfxExit() is skipped: freeing the framebuffers and closing GSP
// under the main thread's GPU frames is worse than leaving them to the process exit
// that follows every caller.
extern "C" void __wrap_gfxExit(void)
{
	if (a1_c3d::back::gfx_exit_hook())
	{
		__real_gfxExit();
		printf("[c3d] real gfxExit returned\n");   // d4
		fflush(stdout);
	}
	else
	{
		printf("[c3d] gfxExit skipped: citro3d is still up on the main thread\n");
		fflush(stdout);
	}
}
