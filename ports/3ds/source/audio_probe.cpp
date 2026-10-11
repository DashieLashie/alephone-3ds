/*
	Copyright (C) 2026 and beyond by the "Aleph One" developers.

	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; either version 3 of the License, or
	(at your option) any later version.

	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	Aleph One for the Nintendo 3DS: a probe on SDL's audio thread
	(docs/3ds/STAGE5-PREREQS.md items 3 and 5).

	The engine opens audio with SDL_OpenAudio() (Sound/AudioBackend.h); the
	callback runs OpenAL's mixer (alcRenderSamplesSOFT) on SDL's audio
	thread. The link wraps SDL_OpenAudio (-Wl,--wrap=SDL_OpenAudio) so the
	callback can be timed and the thread's stack measured, without an engine
	change:
	- stack: SDL's N3DS port creates the thread with an 80 KiB stack
	  (N3DS_THREAD_STACK_SIZE_DEFAULT; SDL_HINT_THREAD_STACK_SIZE does not
	  apply to SDL's internal threads). The first callback paints the unused
	  part; audio_probe_log() reports the deepest byte overwritten.
	- time: per callback, against the buffer's play time (samples / rate);
	  a callback that starts more than 1.5 buffers after the previous one
	  is counted as late (a likely audible gap).
*/

#include <3ds.h>
#include <SDL2/SDL.h>

#include <sys/reent.h>

#include <atomic>
#include <stdio.h>

extern "C" int __real_SDL_OpenAudio(SDL_AudioSpec* desired, SDL_AudioSpec* obtained);

namespace a1_3ds {

namespace {

// SDL 2.32.10, src/thread/n3ds/SDL_systhread.c
const size_t kSdlAudioStack = 80 * 1024;
const u8 kPaint = 0x5A;

// libctru 2.7.0 source/internal.h, struct Thread_tag (not installed): only
// stacktop is read. threadCreate() lays a thread out as [Thread_tag][stack]
// with stacktop = start + allocsize >= start + stackoffset + stack size.
struct ThreadLayout
{
	Handle handle;
	ThreadFunc ep;
	void* arg;
	int rc;
	bool detached, finished;
	struct _reent reent;
	void* stacktop;
};

SDL_AudioCallback s_callback = nullptr;
void* s_userdata = nullptr;
u64 s_budget_ticks = 0;
float s_budget_ms = 0;

u8* s_stack_bottom = nullptr;
u8* s_stack_top = nullptr;
std::atomic<bool> s_started{false};
std::atomic<int> s_core{-1};
std::atomic<s32> s_priority{0};
const char* s_stack_problem = nullptr;

// Written by the audio thread only; read (racily, for the log) elsewhere.
std::atomic<u32> s_calls{0};
std::atomic<u32> s_late{0};
std::atomic<u64> s_busy_ticks{0};
std::atomic<u64> s_max_ticks{0};
std::atomic<u64> s_max_gap_ticks{0};
u64 s_last_start = 0;

__attribute__((noinline)) void paint_audio_stack()
{
	Thread t = threadGetCurrent();
	if (!t)
	{
		s_stack_problem = "not a libctru thread";
		return;
	}
	u8* top = static_cast<u8*>(reinterpret_cast<ThreadLayout*>(t)->stacktop);
	u8* bottom = top - kSdlAudioStack;
	volatile u8 marker = 0;
	u8* sp = const_cast<u8*>(&marker);
	if (bottom < reinterpret_cast<u8*>(t) + sizeof(ThreadLayout) || sp <= bottom + 1024 || sp > top)
	{
		s_stack_problem = "unexpected stack layout";
		return;
	}
	for (u8* p = bottom; p < sp - 512; ++p)
		*p = kPaint;
	s_stack_bottom = bottom;
	s_stack_top = top;
}

size_t audio_stack_used()
{
	const u8* p = s_stack_bottom;
	while (p < s_stack_top && *p == kPaint)
		++p;
	return s_stack_top - p;
}

void SDLCALL probe_callback(void* userdata, Uint8* stream, int len)
{
	(void) userdata;
	if (!s_started.load(std::memory_order_relaxed))
	{
		s_core = svcGetProcessorID();
		s32 prio = 0;
		svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
		s_priority = prio;
		paint_audio_stack();
		s_started = true;
	}
	const u64 t0 = svcGetSystemTick();
	if (s_last_start)
	{
		const u64 gap = t0 - s_last_start;
		if (gap > s_max_gap_ticks.load(std::memory_order_relaxed))
			s_max_gap_ticks = gap;
		if (gap > s_budget_ticks * 3 / 2)
			s_late.fetch_add(1, std::memory_order_relaxed);
	}
	s_last_start = t0;
	s_callback(s_userdata, stream, len);
	const u64 dt = svcGetSystemTick() - t0;
	s_busy_ticks.fetch_add(dt, std::memory_order_relaxed);
	if (dt > s_max_ticks.load(std::memory_order_relaxed))
		s_max_ticks = dt;
	s_calls.fetch_add(1, std::memory_order_relaxed);
}

float ticks_ms(u64 ticks)
{
	return static_cast<float>(ticks) * 1000.0f / SYSCLOCK_ARM11;
}

} // namespace

// Logs the audio thread's stack use and the callback timing since the
// previous call (one interval per report).
void audio_probe_log()
{
	if (!s_callback)
		return;
	if (!s_started)
	{
		printf("[3ds]   audio thread: no callback yet\n");
		return;
	}
	static u32 prev_calls = 0, prev_late = 0;
	static u64 prev_busy = 0;
	const u32 calls = s_calls, late = s_late;
	const u64 busy = s_busy_ticks;
	const u32 n = calls - prev_calls;
	const float avg = n ? ticks_ms(busy - prev_busy) / n : 0.0f;
	const float max = ticks_ms(s_max_ticks.exchange(0));
	const float max_gap = ticks_ms(s_max_gap_ticks.exchange(0));
	if (s_stack_bottom)
		printf("[3ds]   audio thread (core %d, priority 0x%lX): stack %lu KiB, deepest use so far %lu KiB\n",
		       s_core.load(), static_cast<long>(s_priority.load()),
		       static_cast<unsigned long>(kSdlAudioStack / 1024),
		       static_cast<unsigned long>((audio_stack_used() + 1023) / 1024));
	else
		printf("[3ds]   audio thread (core %d, priority 0x%lX): stack not measured (%s)\n",
		       s_core.load(), static_cast<long>(s_priority.load()), s_stack_problem);
	printf("[3ds]   audio callback, last interval: %lu calls, avg %.2f ms, max %.2f ms per %.2f ms buffer "
	       "(avg load %.0f%%); %lu late (> 1.5 buffers apart), longest gap %.2f ms\n",
	       static_cast<unsigned long>(n), avg, max, s_budget_ms,
	       s_budget_ms > 0 ? 100.0f * avg / s_budget_ms : 0.0f,
	       static_cast<unsigned long>(late - prev_late), max_gap);
	prev_calls = calls;
	prev_late = late;
	prev_busy = busy;
}

} // namespace a1_3ds

extern "C" int __wrap_SDL_OpenAudio(SDL_AudioSpec* desired, SDL_AudioSpec* obtained)
{
	using namespace a1_3ds;
	if (!desired || !desired->callback)
		return __real_SDL_OpenAudio(desired, obtained);
	// A re-open (sound preferences changed) starts a new audio thread: measure
	// that one. SDL_CloseAudio() has joined the old thread by now.
	s_started = false;
	s_stack_bottom = s_stack_top = nullptr;
	s_stack_problem = nullptr;
	s_last_start = 0;
	s_callback = desired->callback;
	s_userdata = desired->userdata;
	desired->callback = probe_callback;
	desired->userdata = nullptr;
	const int rc = __real_SDL_OpenAudio(desired, obtained);
	desired->callback = s_callback;
	desired->userdata = s_userdata;
	const SDL_AudioSpec& got = obtained ? *obtained : *desired;
	if (rc >= 0 && got.freq > 0)
	{
		s_budget_ticks = static_cast<u64>(got.samples) * SYSCLOCK_ARM11 / got.freq;
		s_budget_ms = 1000.0f * got.samples / got.freq;
		printf("[3ds] audio: SDL_OpenAudio %d Hz, %d channels, format 0x%04X, %d samples (%.2f ms per buffer)\n",
		       got.freq, got.channels, got.format, got.samples, s_budget_ms);
	}
	else
	{
		s_callback = nullptr;
		printf("[3ds] audio: SDL_OpenAudio failed: %s\n", SDL_GetError());
	}
	return rc;
}
