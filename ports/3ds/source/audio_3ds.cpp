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

	Aleph One for the Nintendo 3DS: the audio output (docs/3ds/STAGE6.md).

	OpenALManager mixes into an OpenAL Soft loopback device and owns an
	AudioBackend that pulls the mix (alcRenderSamplesSOFT) from an SDL audio
	callback. On the 3DS, AudioBackend::CreateDefault() returns the backend
	below (Sound/AudioBackend.h):
	- output is always stereo signed 16-bit, at the rate from the sound
	  preferences (SoundManager::Parameters::Verify keeps it in range; the
	  3DS default is the DSP's own rate, 32728 Hz). At that rate the DSP's
	  channel is set to play SDL's buffers 1:1 without interpolation, so
	  OpenAL's resampler is the only one.
	- SDL's N3DS driver plays through ndsp. If ndspInit() fails (no
	  sdmc:/3ds/dspfirm.cdc), Open() fails, OpenALManager carries on without
	  a device (no sound, no crash), and dsp_problem() tells the bottom
	  screen to show a one-time notice (input_3ds.cpp).
	- instrumentation (log, and the debug-audio overlay): callback time
	  against the buffer's play time, late callbacks, the audio thread's
	  stack use, and DSP underruns. An underrun is counted when SDL queues a
	  wavebuf (ndspChnWaveBufAdd, wrapped with -Wl,--wrap) while none of the
	  previously queued ones is still waiting or playing: the DSP ran dry,
	  which is an audible gap. Suspensions (HOME menu, sleep, applets) are
	  excluded; an engine pause (SDL keeps queueing silence) never underruns.

	SDL's audio thread is not busy: it blocks on a CondVar in
	N3DSAUDIO_WaitDevice until the DSP frees a wavebuf.
*/

#include <3ds.h>
#include <SDL2/SDL.h>

#include "AudioBackend.h"
#include "a1_3ds_platform.h"

#include <sys/reent.h>

#include <atomic>
#include <memory>
#include <stdio.h>
#include <string.h>

namespace a1_3ds {

namespace {

// SDL 2.32.10, src/thread/n3ds/SDL_systhread.c
const size_t kSdlAudioStack = 80 * 1024;
const u8 kPaint = 0x5A;
// SDL 2.32.10, src/audio/n3ds/SDL_n3dsaudio.h: NUM_BUFFERS
const int kSdlWaveBufs = 3;
// ndsp's output rate: SYSCLOCK_ARM11 / 8192 = 32728.498 Hz.
const int kDspRate = 32728;
const float kDspRateExact = SYSCLOCK_ARM11 / 8192.0f;

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

AudioBackend::PullCallback s_callback = nullptr;
void* s_userdata = nullptr;
std::atomic<bool> s_open{false};
AudioBackend::Spec s_spec;
u64 s_budget_ticks = 0;
float s_budget_ms = 0;

u8* s_stack_bottom = nullptr;
u8* s_stack_top = nullptr;
std::atomic<bool> s_started{false};
std::atomic<int> s_core{-1};
std::atomic<s32> s_priority{0};
const char* s_stack_problem = nullptr;

// Callback timing. Written by the audio thread only; read elsewhere.
std::atomic<u32> s_calls{0};
std::atomic<u32> s_late{0};
std::atomic<u32> s_over_budget{0};
std::atomic<u64> s_busy_ticks{0};
std::atomic<u64> s_max_ticks_log{0};
std::atomic<u64> s_max_ticks_overlay{0};
std::atomic<u64> s_max_gap_ticks{0};
u64 s_last_start = 0;
// Bumped whenever the callback stops being called on purpose (engine pause,
// suspension); the next gap is then not "late".
std::atomic<u32> s_pause_epoch{0};
u32 s_seen_epoch = 0;

// Underruns (ndspChnWaveBufAdd wrapper, SDL's audio thread).
std::atomic<u32> s_underruns{0};
std::atomic<u32> s_wavebufs{0};   // queued while counting (= time played)
std::atomic<bool> s_suspended{false};
std::atomic<bool> s_forget_recent{true};
ndspWaveBuf* s_recent[kSdlWaveBufs] = {};

// DSP failure (ndspInit() inside SDL_OpenAudio).
std::atomic<int> s_dsp_problem{static_cast<int>(DspProblem::None)};
char s_dsp_error[128] = "";

void note_max(std::atomic<u64>& slot, u64 v)
{
	if (v > slot.load(std::memory_order_relaxed))
		slot.store(v, std::memory_order_relaxed);
}

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

void SDLCALL timed_callback(void*, Uint8* stream, int len)
{
	if (!s_started.load(std::memory_order_relaxed))
	{
		fatal_note_audio_thread();   // the fatal hand-off needs to know this thread (5.6.3)
		s_core = svcGetProcessorID();
		s32 prio = 0;
		svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
		s_priority = prio;
		paint_audio_stack();
		s_started = true;
	}
	fatal_test_audio_hook();   // debug-fatal modes 6, 8
	const u64 t0 = svcGetSystemTick();
	const u32 epoch = s_pause_epoch.load(std::memory_order_relaxed);
	if (s_last_start && epoch == s_seen_epoch)
	{
		const u64 gap = t0 - s_last_start;
		note_max(s_max_gap_ticks, gap);
		if (gap > s_budget_ticks * 3 / 2)
			s_late.fetch_add(1, std::memory_order_relaxed);
	}
	s_seen_epoch = epoch;
	s_last_start = t0;
	s_callback(s_userdata, stream, len);
	const u64 dt = svcGetSystemTick() - t0;
	s_busy_ticks.fetch_add(dt, std::memory_order_relaxed);
	note_max(s_max_ticks_log, dt);
	note_max(s_max_ticks_overlay, dt);
	if (dt > s_budget_ticks)
		s_over_budget.fetch_add(1, std::memory_order_relaxed);
	s_calls.fetch_add(1, std::memory_order_relaxed);
}

float ticks_ms(u64 ticks)
{
	return static_cast<float>(ticks) * 1000.0f / SYSCLOCK_ARM11;
}

void reset_counters()
{
	s_started = false;
	s_stack_bottom = s_stack_top = nullptr;
	s_stack_problem = nullptr;
	s_last_start = 0;
	s_forget_recent = true;
}

void record_open_failure()
{
	const char* err = SDL_GetError();
	snprintf(s_dsp_error, sizeof s_dsp_error, "%s", err ? err : "");
	printf("[3ds] audio: SDL_OpenAudio failed: %s\n", s_dsp_error);
	// SDL_n3dsaudio.c: "DSP init failed: dspfirm.cdc missing!" or
	// "DSP init failed. Error code: 0x...".
	DspProblem problem = DspProblem::Other;
	if (strstr(s_dsp_error, "dspfirm.cdc"))
		problem = DspProblem::NoFirmware;
	int expected = static_cast<int>(DspProblem::None);
	if (s_dsp_problem.compare_exchange_strong(expected, static_cast<int>(problem)))
		printf("[3ds] audio: continuing without sound%s\n",
		       problem == DspProblem::NoFirmware
		           ? " (no sdmc:/3ds/dspfirm.cdc; dump it with Luma3DS: Rosalina menu > "
		             "Miscellaneous options > Dump DSP firmware)"
		           : "");
}

// The engine's AudioBackend on the 3DS: SDL's legacy device 1, which the
// HOME/sleep hook (input_3ds.cpp) pauses with SDL_PauseAudio().
class N3DSAudioBackend final : public AudioBackend
{
public:
	bool Open(const Spec& desired, Spec& obtained, PullCallback callback, void* userdata) override
	{
		if (s_open)
			Close();
		reset_counters();
		s_callback = callback;
		s_userdata = userdata;

		SDL_AudioSpec want = {};
		want.freq = desired.rate > 0 ? desired.rate : kDspRate;
		want.format = AUDIO_S16SYS;   // ndsp plays PCM8/PCM16 only
		want.channels = 2;
		want.samples = desired.samples ? desired.samples : 1024;
		want.callback = timed_callback;
		want.userdata = nullptr;

		// obtained = NULL: SDL converts if the device differs, so what the
		// engine is told is exactly what it renders.
		if (SDL_OpenAudio(&want, nullptr) < 0)
		{
			s_callback = nullptr;
			record_open_failure();
			return false;
		}
		s_spec.rate = want.freq;
		s_spec.format = want.format;
		s_spec.channels = want.channels;
		s_spec.samples = want.samples;
		obtained = s_spec;

		s_budget_ticks = static_cast<u64>(want.samples) * SYSCLOCK_ARM11 / want.freq;
		s_budget_ms = 1000.0f * want.samples / want.freq;
		const bool native = want.freq == kDspRate;
		if (native)
		{
			// SDL set linear interpolation at want.freq. At the DSP's own
			// rate play the samples as they are (the 0.0015% speed
			// difference is inaudible); OpenAL already resampled the sounds.
			SDL_LockAudio();
			ndspChnSetInterp(0, NDSP_INTERP_NONE);
			ndspChnSetRate(0, kDspRateExact);
			SDL_UnlockAudio();
		}
		s_open = true;
		printf("[3ds] audio: OpenAL loopback -> SDL %d Hz, stereo S16, %d samples (%.2f ms per buffer, "
		       "%d buffers); DSP %s\n",
		       want.freq, want.samples, s_budget_ms, kSdlWaveBufs,
		       native ? "plays 1:1 (native rate)" : "resamples (linear)");
		return true;
	}

	void Close() override
	{
		SDL_CloseAudio();
		s_open = false;
		s_callback = nullptr;
	}

	void Pause(bool paused) override
	{
		s_pause_epoch.fetch_add(1, std::memory_order_relaxed);
		SDL_PauseAudio(paused);
	}

	bool IsStopped() const override { return SDL_GetAudioStatus() == SDL_AUDIO_STOPPED; }
	void Lock() override { SDL_LockAudio(); }
	void Unlock() override { SDL_UnlockAudio(); }
};

} // namespace

std::unique_ptr<AudioBackend> create_audio_backend()
{
	return std::make_unique<N3DSAudioBackend>();
}

DspProblem dsp_problem(std::string* detail)
{
	const auto p = static_cast<DspProblem>(s_dsp_problem.load());
	if (detail && p != DspProblem::None)
		*detail = s_dsp_error;
	return p;
}

void audio_set_suspended(bool suspended)
{
	s_pause_epoch.fetch_add(1, std::memory_order_relaxed);
	if (!suspended)
		s_forget_recent = true;   // the DSP was stopped; start over
	s_suspended = suspended;
}

AudioStats audio_stats()
{
	AudioStats st;
	st.open = s_open;
	st.rate = s_spec.rate;
	st.samples = s_spec.samples;
	st.budget_ms = s_budget_ms;
	st.calls = s_calls;
	st.busy_ms = ticks_ms(s_busy_ticks);
	st.max_ms = ticks_ms(s_max_ticks_overlay.exchange(0));
	st.late = s_late;
	st.over_budget = s_over_budget;
	st.underruns = s_underruns;
	st.played_s = s_spec.rate > 0 ? static_cast<double>(s_wavebufs) * s_spec.samples / s_spec.rate : 0.0;
	return st;
}

#ifdef A1_PROFILE
// profile_3ds.cpp: total time spent in the audio callback, in system ticks.
uint64_t audio_busy_ticks()
{
	return s_busy_ticks.load(std::memory_order_relaxed);
}
#endif

// Logs the audio thread's stack use and the callback timing since the
// previous call (one interval per report), plus the running underrun total.
void audio_probe_log()
{
	if (!s_open)
	{
		if (s_dsp_problem != static_cast<int>(DspProblem::None))
			printf("[3ds]   audio: no device (%s)\n", s_dsp_error);
		return;
	}
	if (!s_started)
	{
		printf("[3ds]   audio thread: no callback yet\n");
		return;
	}
	static u32 prev_calls = 0, prev_late = 0, prev_over = 0;
	static u64 prev_busy = 0;
	const u32 calls = s_calls, late = s_late, over = s_over_budget;
	const u64 busy = s_busy_ticks;
	const u32 n = calls - prev_calls;
	const float avg = n ? ticks_ms(busy - prev_busy) / n : 0.0f;
	const float max = ticks_ms(s_max_ticks_log.exchange(0));
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
	       "(avg load %.0f%%); %lu over budget, %lu late (> 1.5 buffers apart), longest gap %.2f ms\n",
	       static_cast<unsigned long>(n), avg, max, s_budget_ms,
	       s_budget_ms > 0 ? 100.0f * avg / s_budget_ms : 0.0f,
	       static_cast<unsigned long>(over - prev_over),
	       static_cast<unsigned long>(late - prev_late), max_gap);
	const AudioStats st = audio_stats();
	printf("[3ds]   audio underruns: %lu in %.1f min played (%.2f/min)\n",
	       static_cast<unsigned long>(st.underruns), st.played_s / 60.0,
	       st.played_s > 0 ? st.underruns * 60.0 / st.played_s : 0.0);
	const DecodeStats dec = decode_stats(false);
	if (dec.audio_s > 0)
		printf("[3ds]   music decoding so far: %.1f s of audio in %.0f ms (%.1f%% of real time), "
		       "longest request %.1f ms; last file %s\n",
		       dec.audio_s, dec.decode_ms, dec.decode_ms / (10.0 * dec.audio_s), dec.max_call_ms,
		       dec.last_file.c_str());
	prev_calls = calls;
	prev_late = late;
	prev_over = over;
	prev_busy = busy;
}

} // namespace a1_3ds

// SDL's N3DS driver queues every mixed buffer with this (channel 0).
extern "C" void __real_ndspChnWaveBufAdd(int id, ndspWaveBuf* buf);
extern "C" void __wrap_ndspChnWaveBufAdd(int id, ndspWaveBuf* buf)
{
	using namespace a1_3ds;
	if (id == 0 && s_open)
	{
		if (s_forget_recent.exchange(false))
			memset(s_recent, 0, sizeof s_recent);
		int previous = 0;
		bool pending = false;
		for (ndspWaveBuf* p : s_recent)
		{
			if (!p || p == buf)
				continue;
			++previous;
			const u8 status = *reinterpret_cast<volatile u8*>(&p->status);
			if (status == NDSP_WBUF_QUEUED || status == NDSP_WBUF_PLAYING)
				pending = true;
		}
		if (!s_suspended)
		{
			if (previous && !pending)
				s_underruns.fetch_add(1, std::memory_order_relaxed);
			s_wavebufs.fetch_add(1, std::memory_order_relaxed);
		}
		memmove(s_recent + 1, s_recent, sizeof s_recent - sizeof s_recent[0]);
		s_recent[0] = buf;
	}
	__real_ndspChnWaveBufAdd(id, buf);
}
