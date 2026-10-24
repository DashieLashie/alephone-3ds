/*
	Aleph One for the Nintendo 3DS, citro3d back end: the debug-c3d settings file
	(docs/3ds/CITRO3D_DESIGN.md, section 7 "Runtime config"). Plain C++, no 3DS or
	engine headers, so the parser can be tested on the host.

	sdmc:/3ds/alephone/debug-c3d, one "key value" per line, '#' starts a comment.
	Keys of P1:

	  present cpu|gpu   top screen on the CPU (as without citro3d) or on the GPU (default)
	  calibrate 0|1     the screen target is cleared to a colour that encodes the present
	                    counter instead of drawing the picture; the screens are saved
	                    (screen-c3d-cal-<n>-*.bmp) after presents 120-125, 600-605, 1200-1205, 1800-1805
	  alt_load 0|1      every second present starts with a 20 ms busy wait (a heavy frame)
	  odd_surface 0|1   once a second the present is handed a 300x200 RGB565 picture
	  test_vhalt 0-4    20 s after the first present: 1 = vhalt() (the error-screen path);
	                    2 = abort() on a second thread, 3 = vhalt() on a second thread, 4 =
	                    error_screen() on a second thread (the hand-off of a release requested
	                    off the main thread, critic P1 minor 4)
	  present_diff N    0 = off (default); N > 0: every N-th present, the picture the CPU present
	                    would show (video_3ds.cpp's own code, into a 240x400 buffer) is compared
	                    with the framebuffer the GPU present wrote, at the next present; per-check
	                    and final "[c3d] present_diff" log lines (critic P1 minor 1)

	Keys of plan A piece 5 (design 5.6.1, the HOME > Close hang):

	  suspend_gate 0|1  default 1: no GPU present without GPU rights or while suspended/closing.
	                    0 restores the pre-gate behaviour (console confirmation run only)
	  retire_wait_ms N  default 500, 0..600000: how long a wait for the GPU queue may take where it
	                    might never retire (release; a frame begun with the gate condition true)
	                    before citro3d is abandoned. 0 = unbounded (C3D_FrameBegin(0), the old
	                    behaviour, for a deliberate reproduction)
	  force_abandon 0|1 the release takes the abandon path without polling (Azahar cannot hang a queue)
	  close_frame 0|1   the release, when the gate condition holds, first ends one frame that draws
	                    the present quad (the GPU work of a present), console run 1

	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; either version 3 of the License, or
	(at your option) any later version.
*/
#ifndef A1_C3D_CORE_CONFIG_H
#define A1_C3D_CORE_CONFIG_H

#include <string>
#include <vector>

namespace a1_c3d {

struct Config
{
	bool present_gpu = true;
	bool calibrate = false;
	bool alt_load = false;
	bool odd_surface = false;
	int test_vhalt = 0;
	unsigned present_diff = 0;
	bool suspend_gate = true;
	unsigned retire_wait_ms = 500;   // 0 = unbounded
	bool force_abandon = false;
	bool close_frame = false;
};

// Applies one setting. Returns false (leaving `cfg` alone) for an unknown key or a bad value.
bool config_apply(Config& cfg, const std::string& key, const std::string& value);

// Applies every line of `text`; one message per rejected line is appended to `problems`.
void config_parse(Config& cfg, const std::string& text, std::vector<std::string>& problems);

} // namespace a1_c3d

#endif
