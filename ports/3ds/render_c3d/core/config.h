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

	Key of plan B-a (design 5.7.4 and 5.7.11, the zero-copy present's pre-measurement):

	  two_frame 0|1     default 0 = one C3D frame per present, as in P1. 1 = the present runs as two
	                    C3D frames: U (staging fill, flush, upload transfer, no draw) and D (wait for
	                    U, then the quad); only to measure the cost structure of plan B-b. With
	                    suspend_gate 1 the gate is checked again between U and D

	Keys of M0 (design 7, "M0 specification"; the renderer harness):

	  renderer sw|c3d   default sw. c3d: the citro3d renderer (in M0 only the null mode runs)
	  mode null|readback|direct
	                    default readback. null: count what the rasteriser would draw, draw nothing
	                    (defines R); readback / direct: not before M1 / M6
	  diff N            0..100000, default 0 = off: every N-th world frame is a diff frame
	  diff_frames N     0..1000000, default 20: diff frames in all, then diffing stops and a summary
	                    is logged; 0 = no limit
	  classes LIST      default all: comma list, no spaces, of walls floors landscape static sprites
	                    weapons tinted other frame, or "all"; an unknown name or an empty item
	                    rejects the whole value
	  fullbright 0|1    every forwarded textured primitive is drawn with the brightest shading table
	  dump 0|1          diff frames and calibration frames are written as c3d-*.bmp
	  readback_cal 0|1  the readback calibration (the first 30 world frames; P1's "calibrate" is the
	                    present-target test, a different thing)
  readback_cal_resize 0|1
                    default 0; with readback_cal: every odd calibration frame uses a view 8 pixels
                    narrower and lower, so the target and buffers are deleted and created again every
                    frame (exercises the resize path out of frame)

	Keys of M1 (design 7, "M1 specification"; the first C3D draws, on diff frames):

	  cmdbuf_kib N      16..4096, default 512: the C3D_Init command-buffer size (below 16 KiB the
	                    present's own frames, which check no reserve, are not guaranteed)
	  gxq_budget N      1..24, default 24: the GX queue budget per C3D frame (GxQueue)
	  ring_vertices N   64..32768, default 1024: vertices per half of the vertex ring
	  ring_indices N    64..65536, default 2048: indices per half of the index ring
	  reserve_extra N   0..1000000 words, default 0: added to the command-buffer reserve test, to
	                    force a "cmdbuf" discard
	  film PATH         the rest of the line, trimmed; a '#' still starts a comment, so a path with a
	                    '#' is not supported. Relative to the user directory unless it starts with
	                    sdmc:/ or /. A film to replay from the command line (boot_begin)

	Keys of M2 (design 7, "M2 specification", "Keys"; the texture cache, pure part):

	  vram_cap KiB      0..4096, default 3584: accounting cap of the VRAM texture pool
	  linear_cap KiB    64..16384, default 4096: accounting cap of the linear texture pool
	  promote_max N     0..8, default 8: linear-to-VRAM promotions per view frame; 0 = never
	                    (linear sampling only)

	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; either version 3 of the License, or
	(at your option) any later version.
*/
#ifndef A1_C3D_CORE_CONFIG_H
#define A1_C3D_CORE_CONFIG_H

#include <cstdint>
#include <string>
#include <vector>

#include "classify.h"

namespace a1_c3d {

enum class Mode { Null, Readback, Direct };

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
	bool two_frame = false;
	// M0
	bool renderer_c3d = false;
	Mode mode = Mode::Readback;
	unsigned diff = 0;                      // every N-th world frame is a diff frame; 0 = off
	unsigned diff_frames = 20;              // 0 = no limit
	uint32_t classes = kAllClassesMask;
	bool fullbright = false;
	bool dump = false;
	bool readback_cal = false;
	bool readback_cal_resize = false;
	// M1
	unsigned cmdbuf_kib = 512;
	unsigned gxq_budget = 24;
	unsigned ring_vertices = 1024;
	unsigned ring_indices = 2048;
	unsigned reserve_extra = 0;
	// M2
	unsigned vram_cap = 3584;               // KiB
	unsigned linear_cap = 4096;             // KiB
	unsigned promote_max = 8;
	std::string film;
};

// Applies one setting. Returns false (leaving `cfg` alone) for an unknown key or a bad value.
bool config_apply(Config& cfg, const std::string& key, const std::string& value);

// Applies every line of `text`; one message per rejected line is appended to `problems`.
void config_parse(Config& cfg, const std::string& text, std::vector<std::string>& problems);

} // namespace a1_c3d

#endif
