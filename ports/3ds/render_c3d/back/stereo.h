/*
	Aleph One for the Nintendo 3DS, citro3d back end: stereo 3D, the back-end state
	(docs/3ds/CITRO3D_DESIGN.md, section 7, "M9 specification", "Right-eye objects and 3D mode", "The preference and
	stereo"). Plain header, no <3ds.h>: front/ includes it.

	Two things are kept apart. ALLOCATED, fixed for the run at present_init: the right screen target, the second view
	texture and the smaller texture-pool cap (core/stereo.h stereo_allocated). ENABLED (cfg.stereo, may change between
	frames): 3D mode (gfxSet3D) follows it, and a stereo frame is decided only if it is on and the objects exist.
	Once stereo is off for the run (vram, uniform, mode, g3a) it stays off and 3D mode stays 2D.

	Main thread only.

	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; either version 3 of the License, or
	(at your option) any later version.
*/
#ifndef A1_C3D_BACK_STEREO_H
#define A1_C3D_BACK_STEREO_H

#include "../core/config.h"

#include <cstdint>

namespace a1_c3d {
namespace back {

// Why stereo is off for the run. None: not off. Uniform: world.v.pica has no "stereo" uniform (the right-eye objects
// stay allocated and are never drawn); Mode: a file said stereo 1 without mode direct; G3a: the 1024x512 probe.
enum class StereoOff : uint8_t { None, Vram, Uniform, Mode, G3a };
const char* stereo_off_name(StereoOff why);

// present_init, before the screen targets: decides the allocation (core/stereo.h stereo_allocated, plus the mode rule)
// and calls gfxSet3D(enabled). Returns whether the right-eye objects are wanted.
bool stereo_begin(const Config& cfg, bool has_file);
// The right-eye objects are wanted for the run (stereo_begin) and not given up for want of VRAM or mode: the right screen
// target and the second view texture are made (or being made). A Uniform failure leaves this true: allocation never
// depends on the uniform.
bool stereo_objects_wanted();
// Stereo frames may be decided: the objects are wanted and nothing turned stereo off (no off reason at all).
bool stereo_usable();
StereoOff stereo_off_reason();
// Gives stereo up for the run: logs "[c3d] stereo: off for the run (why)" once, gfxSet3D(false), 3D mode stays off.
void stereo_turn_off(StereoOff why);
// 3D mode follows enabled (cfg.stereo, usable): called after the preference changed cfg.stereo.
void stereo_follow(const Config& cfg);
// present_detach: back to 2D mode for SDL's present, the fatal screen and whatever follows citro3d.
void stereo_end();
// gfxIs3D(): a stereo frame is decided only in 3D mode.
bool stereo_is_3d();
// The slider in 0..1: stereo_slider N / 100 when N >= 0, else osGet3DSliderState().
float stereo_slider(const Config& cfg);

} // namespace back
} // namespace a1_c3d

#endif
