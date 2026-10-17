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
// (release_from_any_thread). The main thread presents at least every 33 ms in menus and
// in game; if it has not released after this long it is blocked (e.g. on a lock the
// dying thread holds) and will not present again.
constexpr int kHandoffWaitMs = 500;

Config s_cfg;
bool s_cfg_loaded = false;
// citro3d and its GX queue are used only by the main thread (citro3d is not thread-safe).
// s_up and s_release_requested are also read or set from other threads (the hand-off
// below); everything else here is main-thread state.
std::atomic<bool> s_up{false};                  // set after a successful init, cleared by release_and_fini()
std::atomic<bool> s_release_requested{false};   // another thread asked the main thread to release
u32 s_main_thread_id = 0;                       // kernel thread id of the thread that brought citro3d up
bool s_off = false;         // not in use for the rest of the run (cpu present, init failed, released)
bool s_in_frame = false;    // a C3D frame of ours is open
GxQueue s_gx;

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
	printf("[c3d] debug-c3d: present %s, calibrate %d, alt_load %d, odd_surface %d, test_vhalt %d\n",
	       s_cfg.present_gpu ? "gpu" : "cpu", s_cfg.calibrate, s_cfg.alt_load, s_cfg.odd_surface,
	       s_cfg.test_vhalt);
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
	       "present_screen_false_returns=%u\n",
	       s_presents, s_odd, s_gx.high_water(), s_gx.refused(), s_false_returns);
}

u32 current_thread_id()
{
	u32 id = 0;
	if (R_FAILED(svcGetThreadId(&id, CUR_THREAD_HANDLE)))   // svc.h:913, CUR_THREAD_HANDLE svc.h:133
		return 0xFFFFFFFF;                                  // matches no thread: treated as "not main"
	return id;
}

} // namespace

bool on_main_thread()
{
	return current_thread_id() == s_main_thread_id;
}

bool release_from_any_thread(const char* who)
{
	if (!s_up)
		return true;
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
	if (!s_up)
		return;
	++s_releases;
	printf("[c3d] release #%u: ending the frame, retiring the queue, detaching the top screen\n", s_releases);
	fflush(stdout);
	// Design 1.3 / 5.1: end any open frame; an empty frame (C3D_FrameBegin(0) returning
	// means every earlier C3D frame has retired); detach; C3D_Fini (which waits for the
	// queue and destroys the targets); then the top screen is the console's again.
	if (s_in_frame)
	{
		C3D_FrameEnd(0);
		s_in_frame = false;
	}
	if (C3D_FrameBegin(0))
		C3D_FrameEnd(0);
	present_detach();
	C3D_Fini();
	present_free();
	s_up = false;
	s_off = true;
	printf("[c3d] release done: C3D_Fini returned; linear free %lu KiB, VRAM free %lu KiB\n",
	       kib(linearSpaceFree()), kib(vramSpaceFree()));
	log_final_stats();
	fflush(stdout);
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
	{
		if (!s_release_requested)
			return true;
		// The other half of release_from_any_thread(): this is the main thread.
		printf("[c3d] release requested by another thread: releasing on the main thread\n");
		release_and_fini();
		return false;
	}
	if (s_off)
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

bool frame_begin()
{
	if (!s_up)
		return false;
	bool ok;
	{
#ifdef A1_PROFILE
		A1_PROF_SCOPE(C3dWait);
#endif
		ok = C3D_FrameBegin(0);   // never C3D_FRAME_SYNCDRAW (design 2.4)
	}
	if (!ok)
		return false;
	s_in_frame = true;
	s_gx.begin_frame();
	return true;
}

void frame_end(int drawn_linked_targets)
{
	const int total = s_gx.end_frame(drawn_linked_targets);
	if (total > 4)
		printf("[c3d] warning: %d GX queue entries in one present frame (design: <= 4)\n", total);
	C3D_FrameEnd(GX_CMDLIST_FLUSH);
	s_in_frame = false;
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
	       "cmdbuf_usage=%.4f present_screen_false_returns=%u\n",
	       s_presents, s_presents - s_stats_presents, s_odd, s_gx.high_water(), s_gx.refused(),
	       static_cast<double>(C3D_GetCmdBufUsage()), s_false_returns);
	s_stats_presents = s_presents;
}

} // namespace back
} // namespace a1_c3d

// --- the port-internal API (a1_c3d_port.h) -------------------------------------

bool a1_c3d_release_top()
{
	return a1_c3d::back::release_from_any_thread("release_top");
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
		__real_gfxExit();
	else
	{
		printf("[c3d] gfxExit skipped: citro3d is still up on the main thread\n");
		fflush(stdout);
	}
}
