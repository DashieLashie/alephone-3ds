/*
	Aleph One for the Nintendo 3DS, citro3d back end (docs/3ds/CITRO3D_DESIGN.md,
	1.4 "Port-internal"): what the port's own files call. Compiled in only with
	-DA1_RENDER_C3D=ON (ports/3ds/CMakeLists.txt). No <3ds.h>, no engine headers.

	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; either version 3 of the License, or
	(at your option) any later version.
*/
#ifndef A1_C3D_PORT_H
#define A1_C3D_PORT_H

#include <cstdint>

// The GPU present (design 5.1). Called first by a1_3ds::present_screen() with the
// engine's screen surface. Initialises citro3d on the first call. Returns true if
// citro3d put the picture on the top screen (any surface it is given); false, having
// done nothing, if citro3d is not in use (debug-c3d "present cpu", init failed, or
// ownership was given up): the caller then presents on the CPU.
bool a1_c3d_present(const void* pixels, int pitch, int width, int height, uint32_t sdl_format);

// Gives the top screen back: ends any open frame, retires the queue, detaches the
// output and shuts citro3d down. Idempotent; returns at once when citro3d is not up.
// Safe from any thread: off the main thread it makes no citro3d call but hands the release
// to the main thread and waits for it, bounded. Returns true if citro3d no longer owns the
// top screen, false if it still does (the hand-off timed out): keep off the top screen.
bool a1_c3d_release_top();

// present_screen() is about to return false: counted if citro3d is up (design 5.1).
void a1_c3d_note_false_return();

#endif
