/*
	Aleph One for the Nintendo 3DS: platform glue used by engine sources
	under #ifdef A1_PLATFORM_3DS (implementation: ports/3ds/source/a1_3ds_platform.cpp).

	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; either version 3 of the License, or
	(at your option) any later version.

	Deliberately free of <3ds.h>: libctru's typedefs (u8, Handle, ...) must
	not leak into engine translation units.
*/
#ifndef A1_3DS_PLATFORM_H
#define A1_3DS_PLATFORM_H

#include <cstddef>
#include <cstdint>
#include <string>

namespace a1_3ds {

// --- Paths (docs/3ds/STAGE4.md) ---------------------------------------------

// sdmc:/3ds/alephone: preferences, saves, logs, caches, scenario.txt.
const std::string& user_dir();

// The scenario directory: sdmc:/3ds/alephone/Marathon2 by default, or the
// first non-comment line of sdmc:/3ds/alephone/scenario.txt.
const std::string& scenario_dir();

// Where scenario_dir() came from ("default" or "scenario.txt line N").
const std::string& scenario_dir_source();

// Read-only engine resources (MML, Plugins/Default_Theme): SDL_GetBasePath(),
// i.e. "romfs:/".
const std::string& resource_dir();

// --- Boot ------------------------------------------------------------------

// First call in main(). Creates user_dir(), redirects stdout/stderr to
// user_dir()/log.txt (and to the 3dslink host when launched with 3dslink),
// logs memory, and checks scenario_dir(). Returns false after showing the
// error screen if the game cannot start; main() must then return.
// Also gives argv a program name when there is none: emulators (Azahar)
// start a .3dsx with argc == 0, and the engine reads argv[0].
bool boot_begin(int& argc, char**& argv);

// Logs a timestamped boot milestone with a memory snapshot.
void boot_mark(const char* what);

// Called each time the main menu is displayed; the first call logs time to
// menu, memory and the video setup.
void main_menu_displayed();

// --- Errors ----------------------------------------------------------------

// Shows a readable error on the bottom screen (and a pointer to it on the top
// screen if SDL video is not up yet) until START or A is pressed, or the
// system asks the application to close. Also writes it to the log.
void error_screen(const std::string& title, const std::string& body);

// --- Entropy (docs/3ds/STAGE5-PREREQS.md item 2) -----------------------------

// devkitARM's std::random_device has no entropy source: it is mt19937 with
// the default seed, so it returns the same numbers in every run. Use these
// instead. Bytes come from the ps:ps service (PS_GenerateRandomBytes); if it
// is not available, from a mix of the system tick, the clock and a counter
// (distinct within a run and, through the clock, across runs; not for
// cryptography). Thread-safe.
void random_bytes(void* out, size_t len);

// A UniformRandomBitGenerator over random_bytes(), e.g. for
// boost::uuids::basic_random_generator<a1_3ds::Urng>.
struct Urng
{
	using result_type = uint32_t;
	static constexpr result_type min() { return 0; }
	static constexpr result_type max() { return UINT32_MAX; }
	result_type operator()()
	{
		result_type v;
		random_bytes(&v, sizeof v);
		return v;
	}
};

// --- Input (docs/3ds/STAGE5.md; implementation: ports/3ds/source/input_3ds.cpp) ---

// Call once after SDL_Init() and initialize_joystick(). Logs the controller
// SDL reports, installs the touch screen translation (an SDL event filter),
// the HOME menu / sleep handling (an APT hook) and the bottom screen legend.
void input_init();

// Touch screen mode. Pointer (the default): the bottom screen maps onto the
// top screen's menu area and a tap is a left click. Look: finger motion
// becomes relative mouse motion (mouse look) and taps do nothing. The engine
// selects Look while gameplay input is active and mouse aiming is on
// (Input/mouse_sdl.cpp), Pointer otherwise.
void set_touch_look(bool look);

// True once the system asked the application to close (HOME menu > Close,
// or the power button). SDL then sends SDL_QUIT on every event poll.
bool exit_requested();

// The system keyboard (swkbd). `text` is UTF-8 in both directions: the
// initial text on entry, the result on return. Returns false (and leaves
// `text` alone) if the user cancelled. `max_chars` is the most characters
// the field accepts.
enum class TextKind { Normal, Number, Password };
bool text_input(const std::string& hint, std::string& text, size_t max_chars, TextKind kind);

} // namespace a1_3ds

#endif
