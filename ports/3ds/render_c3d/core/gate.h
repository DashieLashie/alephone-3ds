/*
	Aleph One for the Nintendo 3DS, citro3d back end: the pure state of the present gate and
	the bounded queue wait (docs/3ds/CITRO3D_DESIGN.md, 5.6.1 a and b). Plain C++, no 3DS
	headers, tested on the host (scripts/3ds/tests/c3d-host).

	  GateInputs     the four-way "no GPU work now" condition
	  GateTracker    transitions and skip counts of the gate (diagnostic d5)
	  retire_wait    poll a non-blocking "has the queue retired" call with a sleep between
	                 the tries, up to a bound; never a spin, never a positive-timeout wait
	  FrameStart     frame_begin()'s tri-state

	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; either version 3 of the License, or
	(at your option) any later version.
*/
#ifndef A1_C3D_CORE_GATE_H
#define A1_C3D_CORE_GATE_H

#include <cstdint>

namespace a1_c3d {

// Design 5.6.1 a: no GPU work without the GPU rights, while the app is suspended or closing.
struct GateInputs
{
	bool gpu_right = true;    // gspHasGpuRight(): the hardware-authoritative input
	bool should_close = false; // aptShouldClose()
	bool suspended = false;   // a1_3ds::app_suspended(): the input Azahar tests can flip

	bool closed() const { return !gpu_right || should_close || suspended; }
};

// Counts the presents the gate refused and reports its transitions, once each.
class GateTracker
{
public:
	enum class Event { None, Closed, Reopened };

	// One call per present attempt that reaches the gate. `closed`: the gate condition holds;
	// `skipped`: the present is actually refused for it (false with suspend_gate 0, where the
	// condition is only watched). Returns Closed on the first closed observation of an episode
	// and Reopened on the first open one after it; episode_skipped() then holds the episode's
	// count (also after Reopened, until the next closing).
	Event on_present(bool closed, bool skipped)
	{
		Event ev = Event::None;
		if (closed)
		{
			if (!closed_)
			{
				closed_ = true;
				episode_skipped_ = 0;
				++episodes_;
				ev = Event::Closed;
			}
			if (skipped)
			{
				++episode_skipped_;
				++total_skipped_;
			}
		}
		else if (closed_)
		{
			closed_ = false;
			ev = Event::Reopened;
		}
		return ev;
	}

	bool is_closed() const { return closed_; }
	unsigned episode_skipped() const { return episode_skipped_; }
	unsigned total_skipped() const { return total_skipped_; }
	unsigned episodes() const { return episodes_; }

private:
	bool closed_ = false;
	unsigned episode_skipped_ = 0, total_skipped_ = 0, episodes_ = 0;
};

enum class RetireResult { Retired, TimedOut };

// Design 5.6.1 b. `try_once()` is C3D_FrameBegin(C3D_FRAME_NONBLOCK): true = retired (and now
// inside a frame), false = still busy, returns at once. `sleep_ms(1)` sleeps (svcSleepThread),
// `now_ms()` is a monotonic clock. Tries once, then once per sleep, until `limit_ms` has
// passed. `limit_ms` must be > 0 (0 means "unbounded" and is the caller's C3D_FrameBegin(0)).
// `waited_ms` receives the time spent. At least one try is always made, also at limit 1.
template <class TryOnce, class SleepMs, class NowMs>
RetireResult retire_wait(TryOnce&& try_once, SleepMs&& sleep_ms, NowMs&& now_ms, uint32_t limit_ms,
                         uint32_t& waited_ms)
{
	const uint64_t start = now_ms();
	for (;;)
	{
		if (try_once())
		{
			waited_ms = static_cast<uint32_t>(now_ms() - start);
			return RetireResult::Retired;
		}
		const uint64_t elapsed = now_ms() - start;
		if (elapsed >= limit_ms)
		{
			waited_ms = static_cast<uint32_t>(elapsed);
			return RetireResult::TimedOut;
		}
		sleep_ms(1);
	}
}

// frame_begin()'s result (design 5.6.1 b).
enum class FrameStart
{
	Ok,              // a C3D frame is open
	AlreadyInFrame,  // C3D_FrameBegin(0) refused: we are already inside a frame (a bug)
	Abandoned        // the queue did not retire in time: citro3d is abandoned, draw nothing
};

} // namespace a1_c3d

#endif
