/*
	Aleph One for the Nintendo 3DS: the debug-fatal tests of the fatal hand-off
	(docs/3ds/CITRO3D_DESIGN.md 5.6.3, "Tests"). Each mode raises one fatal error
	somewhere; the expected outcome of each is in the design's table.

	sdmc:/3ds/alephone/debug-fatal (one setting per line, '#' starts a comment):
	  mode N    1 vhalt() on the main thread
	            2 abort() on a second thread (each thread first logs what
	              signal(SIGABRT) held, and restores it at once)
	            3 vhalt() on a second thread
	            4 error_screen() on a second thread
	            5 error_screen() on a second thread, 10 s after the bottom screen
	              was first presented (a HUD + Map game)
	            6 vhalt() inside the SDL audio callback (the audio thread is parked)
	            7 a second thread takes a LightLock the main thread takes at every
	              present, then fails: main never serves, the fallback runs
	            8 two fatal errors 1 ms apart: the audio callback and a second thread
	  delay S   seconds after the first present (default 20); modes 1-4, 6-8
	  lock 1    mode 6: the main thread takes SDL's audio lock in a loop (the engine
	            does on volume changes and level transitions), so an audio-thread
	            failure finds main blocked on it: test mode 6b
	  first 0|1 mode 8: 1 (default) the audio callback fails first, 0 the thread

	The main thread's present_screen()/present_bottom() tick it.

	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; either version 3 of the License, or
	(at your option) any later version.
*/
#include "a1_3ds_platform.h"

#include <3ds.h>
#include <SDL2/SDL.h>

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>

#include <atomic>
#include <fstream>
#include <sstream>
#include <string>

// CSeries/csalerts_sdl.cpp
extern void vhalt(const char* message);

namespace a1_3ds {

namespace {

const char kDebugFatalFlag[] = "debug-fatal";

struct Config
{
	bool loaded = false;
	int mode = 0;
	double delay_s = 20;
	bool main_lock = false;
	bool audio_first = true;
};
Config s_cfg;

std::atomic<bool> s_audio_fire{false};   // the audio callback fails at its next call
LightLock s_frame_lock;                  // mode 7: taken by main at every tick, held by the thread
u64 s_first_tick = 0, s_first_bottom = 0;
bool s_fired = false;

const Config& config()
{
	if (s_cfg.loaded)
		return s_cfg;
	s_cfg.loaded = true;
	LightLock_Init(&s_frame_lock);
	std::ifstream in(user_dir() + "/" + kDebugFatalFlag);
	if (!in)
		return s_cfg;
	std::string line;
	while (std::getline(in, line))
	{
		line = line.substr(0, line.find('#'));
		std::istringstream ls(line);
		std::string key;
		double value = 0;
		if (!(ls >> key >> value))
			continue;
		if (key == "mode")
			s_cfg.mode = static_cast<int>(value);
		else if (key == "delay")
			s_cfg.delay_s = value;
		else if (key == "lock")
			s_cfg.main_lock = value != 0;
		else if (key == "first")
			s_cfg.audio_first = value != 0;
		else
			printf("[3ds] debug-fatal: unknown setting \"%s\"\n", key.c_str());
	}
	printf("[3ds] debug-fatal: mode %d, delay %.1f s, lock %d, first %s\n", s_cfg.mode, s_cfg.delay_s,
	       s_cfg.main_lock, s_cfg.audio_first ? "audio" : "thread");
	fflush(stdout);
	return s_cfg;
}

// What signal(SIGABRT) holds on this thread: SIG_DFL on a second thread (newlib keeps
// the handlers per _reent), on_abort on main. signal() swaps in place, so the probe
// puts the old value back at once.
void log_signal_probe(const char* who)
{
	void (*prev)(int) = signal(SIGABRT, SIG_DFL);
	signal(SIGABRT, prev);
	printf("[3ds] debug-fatal: %s: signal(SIGABRT) was %s\n", who,
	       prev == SIG_DFL ? "SIG_DFL" : is_abort_handler(prev) ? "on_abort" : "another handler");
	fflush(stdout);
}

void fatal_thread(void* arg)
{
	const int mode = static_cast<int>(reinterpret_cast<intptr_t>(arg));
	switch (mode)
	{
		case 2:
			log_signal_probe("second thread");
			abort();
		case 3:
			vhalt("debug-fatal mode 3: vhalt() on a second thread");
			break;
		case 4:
			error_screen("debug-fatal mode 4", "error_screen() was called on a second thread.");
			break;
		case 5:
			error_screen("debug-fatal mode 5", "error_screen() was called on a second thread, in a game.");
			break;
		case 7:
			LightLock_Lock(&s_frame_lock);   // never released: main blocks on it at its next present
			error_screen("debug-fatal mode 7", "A second thread failed holding a lock the main thread needs.");
			break;
		case 8:
			if (s_cfg.audio_first)
				svcSleepThread(1000000);   // 1 ms after the audio callback's request
			error_screen("debug-fatal mode 8", "error_screen() on a second thread, at the same time as the audio thread.");
			break;
		default:
			break;
	}
}

void start_thread(int mode)
{
	if (!threadCreate(fatal_thread, reinterpret_cast<void*>(static_cast<intptr_t>(mode)), 64 * 1024, 0x3f, -2, true))
	{
		printf("[3ds] debug-fatal: threadCreate failed\n");
		fflush(stdout);
	}
}

void fire(const Config& c)
{
	static const char* const what[] = {"", "vhalt() on the main thread", "abort() on a second thread",
	                                   "vhalt() on a second thread", "error_screen() on a second thread",
	                                   "error_screen() on a second thread, in a game",
	                                   "vhalt() inside the SDL audio callback",
	                                   "a second thread fails holding a lock main needs",
	                                   "two fatal errors at once (audio callback and a second thread)"};
	if (c.mode < 1 || c.mode > 8)
	{
		printf("[3ds] debug-fatal: unknown mode %d\n", c.mode);
		fflush(stdout);
		return;
	}
	printf("[3ds] debug-fatal mode %d: %s%s\n", c.mode, what[c.mode],
	       c.mode == 6 && c.main_lock ? " (6b: main takes SDL's audio lock in a loop)" : "");
	fflush(stdout);
	switch (c.mode)
	{
		case 1:
			vhalt("debug-fatal mode 1: vhalt() on the main thread");   // never returns
			break;
		case 2:
			log_signal_probe("main thread");
			start_thread(2);
			break;
		case 3:
		case 4:
		case 5:
		case 7:
			start_thread(c.mode);
			break;
		case 6:
			s_audio_fire = true;
			if (c.main_lock)
			{
				// The audio thread fails inside the callback, holding SDL's mixer lock, so
				// the next SDL_LockAudio() here blocks for good: no service point, and the
				// requester falls back after 5 s. Bounded in case the audio never fails.
				for (int i = 0; i < 20000; ++i)
				{
					SDL_LockAudio();
					SDL_UnlockAudio();
					svcSleepThread(1000000);
				}
			}
			break;
		case 8:
			if (c.audio_first)
			{
				s_audio_fire = true;
				start_thread(8);
			}
			else
			{
				start_thread(8);
				svcSleepThread(1000000);
				s_audio_fire = true;
			}
			break;
	}
}

} // namespace

// Main thread, at the top of present_screen()/present_bottom().
void fatal_test_tick(bool bottom_screen)
{
	const Config& c = config();
	if (!c.mode)
		return;
	const u64 now = svcGetSystemTick();
	if (!s_first_tick)
		s_first_tick = now;
	if (bottom_screen && !s_first_bottom)
		s_first_bottom = now;
	if (s_fired)
		return;
	const bool due = c.mode == 5 ? (s_first_bottom && now - s_first_bottom > 10 * SYSCLOCK_ARM11)
	                             : now - s_first_tick > static_cast<u64>(c.delay_s * SYSCLOCK_ARM11);
	if (!due)
		return;
	s_fired = true;
	fire(c);
}

// Main thread, at the start of every service point and tick: mode 7's lock, free until
// the second thread takes it for good. Main blocks here for ever, so it never serves.
void fatal_test_frame_lock()
{
	if (!s_cfg.loaded || s_cfg.mode != 7)
		return;
	LightLock_Lock(&s_frame_lock);
	LightLock_Unlock(&s_frame_lock);
}

// The SDL audio thread, at the start of each callback.
void fatal_test_audio_hook()
{
	if (!s_audio_fire.load(std::memory_order_relaxed) || !s_audio_fire.exchange(false))
		return;
	vhalt("debug-fatal: vhalt() inside the SDL audio callback");   // hands off and blocks
}

} // namespace a1_3ds
