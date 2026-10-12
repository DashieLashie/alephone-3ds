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

class Decoder;          // Sound/Decoder.h
class FileSpecifier;    // Files/FileHandler.h

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

// --- Video (docs/3ds/PERF.md; implementation: ports/3ds/source/video_3ds.cpp) ---

// Puts the engine's 640x480 screen surface (XRGB8888 or ARGB8888) on the
// top screen without SDL: halved to 320x240 (2x2 averages, as SDL's linear
// scaling does), centred, rotated and converted into the framebuffer in one pass,
// then swapped at the next VBlank. Waits for a VBlank first if the previous
// swap has not happened yet. Returns false, having done nothing, for any
// other size or format; the caller then presents through SDL.
// A 400x240 surface (the in-game screen with the HUD or the map on the
// bottom screen, docs/3ds/STAGE7.md) goes to the top screen 1:1.
bool present_screen(const void* pixels, int pitch, int width, int height, uint32_t sdl_format);

// A 320x240 XRGB8888 picture onto the bottom screen (RGB565), swapped at the
// next VBlank. The bottom screen must be in game mode (bottom_game(true)).
bool present_bottom(const void* pixels, int pitch);

// 2x2 box average of a 32-bit picture: `w` x `h` output pixels from 2w x 2h
// source pixels, as present_screen() halves the 640x480 screen.
void half_blit(const void* src, int src_pitch, int w, int h, void* dst, int dst_pitch);

// --- Bottom screen (docs/3ds/STAGE7.md; implementation: ports/3ds/source/bottom_3ds.cpp,
// input_3ds.cpp) ---

// Who draws the bottom screen: the legend console (menus, dialogs, Classic)
// or the game (HUD or map, and the touch buttons). Cheap to call every frame.
void bottom_game(bool game);
bool bottom_is_game();

// The Bottom Screen preference (graphics preferences, "bottom_screen").
enum BottomMode { BottomHud = 0, BottomMap = 1, BottomClassic = 2 };

// End of each rendered game frame (RenderOther/screen.cpp). `hud_changed`:
// the HUD buffer was redrawn since the last call.
void bottom_frame(BottomMode mode, bool hud_changed);

// The touch buttons pressed since the last call, as action flags
// (Misc/vbl.cpp parse_keymap). Main thread.
uint32_t take_touch_action_flags();

// --- Audio (docs/3ds/STAGE6.md; implementation: ports/3ds/source/audio_3ds.cpp) ---
// The AudioBackend itself is declared in Sound/AudioBackend.h
// (a1_3ds::create_audio_backend).

// Why there is no sound device, if there is none: ndspInit() failed inside
// SDL_OpenAudio(), most often because sdmc:/3ds/dspfirm.cdc is missing.
// `detail` receives SDL's error text.
enum class DspProblem { None, NoFirmware, Other };
DspProblem dsp_problem(std::string* detail = nullptr);

// The HOME menu, sleep or an applet suspended (true) or resumed (false) the
// application; underruns and late callbacks are not counted meanwhile.
void audio_set_suspended(bool suspended);

// Running totals for the debug-audio overlay. max_ms is the longest
// callback since the previous audio_stats() call.
struct AudioStats
{
	bool open = false;
	int rate = 0, samples = 0;
	float budget_ms = 0;        // play time of one buffer
	uint32_t calls = 0;         // callbacks so far
	float busy_ms = 0;          // total time spent in them
	float max_ms = 0;
	uint32_t late = 0;          // callbacks > 1.5 buffers after the previous one
	uint32_t over_budget = 0;   // callbacks longer than a buffer
	uint32_t underruns = 0;     // the DSP ran out of queued audio
	double played_s = 0;        // audio queued while not suspended
};
AudioStats audio_stats();

// --- Music decoding (implementation: ports/3ds/source/decoders_3ds.cpp) ---

// Ogg Vorbis (Tremor) and MP3 (mpg123) decoders, tried by Sound/Decoder.cpp
// after libsndfile (built without codecs). Returns a new Decoder, or
// nullptr if the file is neither.
Decoder* open_decoder(FileSpecifier& file);

// Decoding totals, for the overlay: the last file opened, and the time spent
// decoding against the audio produced (all decoders, all threads).
struct DecodeStats
{
	std::string last_file;      // name only
	std::string last_codec;     // "Ogg Vorbis (Tremor)", "MP3 (mpg123)"
	int last_rate = 0, last_channels = 0;
	bool last_in_memory = false;
	double decode_ms = 0;
	double audio_s = 0;         // audio decoded
	float max_call_ms = 0;      // longest Decode() since the previous reset
};
DecodeStats decode_stats(bool reset_max = true);

} // namespace a1_3ds

#endif
