/*
	Aleph One for the Nintendo 3DS, citro3d back end: stereo 3D, the back-end state (see stereo.h).

	gfxSet3D only stores the mode byte; it takes effect at the next top-screen swap (libctru gfx.h:79, disassembly
	gfxSet3D+0x4 per the M9 specification (e)). The example order is gfxSet3D(true), C3D_Init, the two screen targets
	(examples/3ds/graphics/gpu/stereoscopic_2d/source/main.cpp:27-37).

	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; either version 3 of the License, or
	(at your option) any later version.
*/
#include "stereo.h"

#include "../core/stereo.h"

#include <3ds.h>

#include <stdio.h>

namespace a1_c3d {
namespace back {

namespace {

bool s_wanted = false;              // present_init decided: the right-eye objects exist for the run
StereoOff s_off = StereoOff::None;
bool s_mode3d = false;              // what gfxSet3D was last given
unsigned s_mode_lines = 0, s_mode_changes = 0;

void set_mode(bool on, const char* why)
{
	if (on == s_mode3d)
		return;
	s_mode3d = on;
	gfxSet3D(on);
	++s_mode_changes;
	if (s_mode_lines < 20)
	{
		++s_mode_lines;
		printf("[c3d] stereo: 3D mode %s (%s)\n", on ? "on" : "off", why);
		fflush(stdout);
	}
}

} // namespace

const char* stereo_off_name(StereoOff why)
{
	static const char* const names[] = {"none", "vram", "uniform", "mode", "g3a"};
	return names[static_cast<int>(why)];
}

bool stereo_begin(const Config& cfg, bool has_file)
{
	s_off = StereoOff::None;
	s_wanted = stereo_allocated(has_file, cfg.stereo);
	if (s_wanted && has_file && cfg.mode != Mode::Direct)
	{
		// mode readback with stereo 1 in a file: no texture target exists for the view (M9 "The view textures stay")
		s_wanted = false;
		s_off = StereoOff::Mode;
		printf("[c3d] stereo: off for the run (mode)\n");
	}
	// The mode byte before the targets. enabled = cfg.stereo as it is now (0 without a file: 2D on the menus until the
	// first hook 3a applies the preference).
	set_mode(s_wanted && cfg.stereo, "present_init");
	printf("[c3d] stereo: right-eye objects %s (%s), vram_cap %u KiB\n", s_wanted ? "allocated" : "not allocated",
	       has_file ? "debug-c3d file" : "no file", stereo_vram_cap(cfg, s_wanted));
	fflush(stdout);
	return s_wanted;
}

bool stereo_objects_wanted()
{
	return s_wanted && (s_off == StereoOff::None || s_off == StereoOff::Uniform);
}

bool stereo_usable()
{
	return s_wanted && s_off == StereoOff::None;
}

StereoOff stereo_off_reason()
{
	return s_off;
}

void stereo_turn_off(StereoOff why)
{
	if (s_off != StereoOff::None)
		return;
	s_off = why;
	printf("[c3d] stereo: off for the run (%s)\n", stereo_off_name(why));
	fflush(stdout);
	set_mode(false, "off for the run");
}

void stereo_follow(const Config& cfg)
{
	set_mode(stereo_usable() && cfg.stereo, "Renderer preference");
}

void stereo_end()
{
	set_mode(false, "citro3d released");
}

bool stereo_is_3d()
{
	return gfxIs3D();
}

float stereo_slider(const Config& cfg)
{
	if (cfg.stereo_slider >= 0)
		return static_cast<float>(cfg.stereo_slider) / 100.0f;
	return osGet3DSliderState();   // libctru os.h:313, 0.0 .. 1.0
}

} // namespace back
} // namespace a1_c3d
