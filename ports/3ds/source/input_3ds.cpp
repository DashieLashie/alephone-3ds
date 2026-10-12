/*
	Aleph One for the Nintendo 3DS: input glue. See ports/3ds/include/a1_3ds_platform.h
	and docs/3ds/STAGE5.md.

	- Touch screen: SDL's N3DS driver reports the bottom screen as an SDL
	  touch device, but sends no mouse events (it has no window on that
	  screen), so the engine never sees a touch. An SDL event filter turns
	  finger events into mouse events: an absolute pointer over the top
	  screen's menu area, or relative motion for mouse look.
	- HOME menu / sleep: an APT hook pauses SDL audio and sends the engine
	  the window focus events it already handles (focus lost pauses a game).
	- Text entry: the system keyboard (swkbd), opened by the engine's text
	  fields. SDL's own swkbd hook (SDL_StartTextInput) is bypassed: it opens
	  a blocking keyboard whenever a text field merely gains focus, with
	  empty initial text, and its result is appended to the field.
	- Bottom screen: a legend of the controls, or, with the debug-input flag
	  file, a log of the raw SDL input events (also written to log.txt).
	  In game with the HUD or the map on the bottom screen (docs/3ds/STAGE7.md)
	  bottom_3ds.cpp draws it instead (bottom_game()), and a touch on its
	  buttons strip presses a button instead of moving the pointer or the view.
	  With the debug-audio flag file, the last four rows show the audio
	  statistics (audio_3ds.cpp, decoders_3ds.cpp). If the DSP could not be
	  started (no dspfirm.cdc), a notice explaining how to dump the firmware
	  replaces the legend once, until a button is pressed.
	- debug-input-script: a test script that injects buttons, sticks and
	  touches at the libctru HID level (the functions SDL's N3DS joystick and
	  touch code call), for testing without a person at the console.

	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; either version 3 of the License, or
	(at your option) any later version.
*/
#include "a1_3ds_platform.h"

#include <3ds.h>
#include <SDL2/SDL.h>

#include <stdio.h>
#include <string.h>
#include <sys/iosupport.h>
#include <sys/stat.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <deque>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace a1_3ds {

// source/a1_3ds_platform.cpp
double elapsed_ms();
void save_screens(const std::string& tag);
// source/bottom_3ds.cpp
int bottom_button_at(float x, float y);
void bottom_button_set(int button, bool down);
void bottom_force_redraw();

namespace {

// If this file exists in the user dir, every input event is logged
// (log.txt, and on the bottom screen instead of the legend).
const char kDebugInputFlag[] = "debug-input";
// If this file exists, the bottom screen's last rows show audio statistics
// (a1_3ds_platform.cpp also logs them every 5 s).
const char kDebugAudioFlag[] = "debug-audio";
// Test script; see the comment above run_script_line().
const char kInputScriptFile[] = "debug-input-script";

bool file_exists(const std::string& path)
{
	struct stat st;
	return stat(path.c_str(), &st) == 0;
}

bool s_debug_input = false;
bool s_debug_audio = false;

// --- Bottom screen: legend or event log --------------------------------------

PrintConsole s_bottom;
const devoptab_t* s_console_out = nullptr;   // the console's stdout device
// The game draws the bottom screen (bottom_game(true)): the console must not,
// as it keeps a pointer to the framebuffer it was set up with.
bool s_bottom_game = false;
std::deque<std::string> s_overlay_lines;     // debug-input: last lines shown

const char kLegend[] =
	"\x1b[1;1H\x1b[36;1mAleph One\x1b[0m for Nintendo 3DS\n"
	"\n"
	"\x1b[33mTouch screen\x1b[0m\n"
	" Menus: tap and drag. This screen\n"
	"        covers the top screen's menu.\n"
	" Game:  drag to look (Controls >\n"
	"        Touch Screen Aiming).\n"
	"\n"
	"\x1b[33mDefault controls\x1b[0m\n"
	" Circle Pad   move, sidestep\n"
	" C-Stick      turn, look up/down\n"
	" ZR / ZL      fire / alternate fire\n"
	" R            action (doors, switches)\n"
	" L            walk (always run is on)\n"
	" D-Pad L/R    previous / next weapon\n"
	" D-Pad U/D    zoom the map\n"
	" SELECT       map\n"
	" X            recenter view\n"
	" Y            auxiliary trigger\n"
	" START        quit menu / unbind\n"
	"\n"
	"\x1b[33mMenus\x1b[0m\n"
	" D-Pad move, A select, B back\n"
	"\n"
	"Change them in Preferences > Controls.\n"
	"HUD/map here: Graphics > Bottom Screen";

// The missing-DSP notice: not shown yet, showing, dismissed (once per run).
enum class DspNotice { Pending, Showing, Dismissed };
DspNotice s_dsp_notice = DspNotice::Pending;

const char kDspFirmwareNotice[] =
	"\x1b[1;1H\x1b[31;1mNo sound: DSP firmware missing\x1b[0m\n"
	"\n"
	"Aleph One runs without sound: the\n"
	"file sdmc:/3ds/dspfirm.cdc is not on\n"
	"the SD card, and the 3DS needs it to\n"
	"play sound from homebrew. Create it\n"
	"once with Luma3DS:\n"
	"\n"
	" 1. Press L + D-Pad Down + SELECT to\n"
	"    open the Rosalina menu.\n"
	" 2. Miscellaneous options >\n"
	"    Dump DSP firmware.\n"
	" 3. Close Rosalina (B) and restart\n"
	"    Aleph One.\n"
	"\n"
	"\x1b[32mPress any button to continue.\x1b[0m";

void console_write(const char* text)
{
	if (!s_console_out || s_bottom_game)
		return;
	PrintConsole* previous = consoleSelect(&s_bottom);
	s_console_out->write_r(_REENT, nullptr, text, strlen(text));
	consoleSelect(previous);
}

// The console draws straight into the bottom framebuffer (single-buffered
// after consoleInit). Flush it and present it; SDL only ever presents the
// top screen, and a HOME menu or applet visit may have replaced the bottom
// screen's configuration.
void bottom_present()
{
	if (!s_console_out || s_bottom_game)
		return;
	gfxFlushBuffers();
	gfxScreenSwapBuffers(GFX_BOTTOM, false);
}

void audio_overlay_draw();

#ifdef A1_PROFILE
// The profiler's rows (profile_3ds.cpp), shown instead of the legend.
std::vector<std::string> s_perf_rows;

void perf_rows_draw()
{
	for (size_t i = 0; i < s_perf_rows.size(); ++i)
	{
		char pos[16];
		snprintf(pos, sizeof pos, "\x1b[%d;1H", static_cast<int>(i) + 1);
		console_write(pos);
		console_write(s_perf_rows[i].c_str());
		console_write("\x1b[K");   // clear to the end of the row
	}
}
#endif

void bottom_redraw()
{
	if (!s_console_out || s_bottom_game)
		return;
	console_write("\x1b[2J");
	if (s_dsp_notice == DspNotice::Showing)
	{
		std::string detail;
		if (dsp_problem(&detail) == DspProblem::NoFirmware)
			console_write(kDspFirmwareNotice);
		else
		{
			console_write("\x1b[1;1H\x1b[31;1mNo sound: the DSP did not start\x1b[0m\n\n");
			console_write(detail.substr(0, 39 * 3).c_str());
			console_write("\n\nAleph One runs without sound. If\n"
			              "sdmc:/3ds/dspfirm.cdc is damaged, dump\n"
			              "it again: Rosalina menu (L + D-Pad\n"
			              "Down + SELECT) > Miscellaneous\n"
			              "options > Dump DSP firmware.\n\n"
			              "\x1b[32mPress any button to continue.\x1b[0m");
		}
	}
	else if (s_debug_input)
	{
		console_write("\x1b[1;1H\x1b[36;1mdebug-input\x1b[0m: raw SDL events\n");
		for (const auto& line : s_overlay_lines)
		{
			console_write(line.c_str());
			console_write("\n");
		}
	}
	else
#ifdef A1_PROFILE
		perf_rows_draw();
#else
		console_write(kLegend);
#endif
	if (s_debug_audio)
		audio_overlay_draw();
	bottom_present();
}

// (Re)attaches the console to the bottom screen: RGB565, single-buffered,
// its framebuffer pointer taken from libctru.
void console_attach()
{
	// consoleInit() points stdout/stderr at the console; keep the log tee.
	const devoptab_t* saved_out = devoptab_list[STD_OUT];
	const devoptab_t* saved_err = devoptab_list[STD_ERR];
	PrintConsole* previous = consoleSelect(&s_bottom);   // consoleInit selects s_bottom
	consoleInit(GFX_BOTTOM, &s_bottom);
	// libctru installs its console device only on the first consoleInit().
	if (!s_console_out)
		s_console_out = devoptab_list[STD_OUT];
	devoptab_list[STD_OUT] = saved_out;
	devoptab_list[STD_ERR] = saved_err;
	if (previous && previous != &s_bottom)
		consoleSelect(previous);
}

void bottom_init()
{
	console_attach();
	bottom_redraw();
}

void overlay_add(const std::string& line)
{
	if (!s_console_out)
		return;
	const size_t kRows = s_debug_audio ? 25 : 29, kCols = 39;
	s_overlay_lines.push_back(line.substr(0, kCols));
	bool scrolled = false;
	while (s_overlay_lines.size() > kRows)
	{
		s_overlay_lines.pop_front();
		scrolled = true;
	}
	if (scrolled || s_debug_audio)
		bottom_redraw();
	else
	{
		console_write(s_overlay_lines.back().c_str());
		console_write("\n");
		bottom_present();
	}
}

// --- Audio statistics (debug-audio) ---------------------------------------------

// Rows 27-30, below the legend (25 rows) and the debug-input log (rows
// 2-26 with this overlay). Each row is padded to 39 columns: writing the
// 40th column of row 30 would scroll the console.
std::string s_audio_rows[4];
double s_audio_last_ms = -1e9;
uint32_t s_audio_prev_calls = 0;
float s_audio_prev_busy = 0;
double s_dec_prev_ms = 0, s_dec_prev_audio = 0;

void audio_overlay_draw()
{
	for (int i = 0; i < 4; ++i)
	{
		char pos[16];
		snprintf(pos, sizeof pos, "\x1b[%d;1H", 27 + i);
		console_write(pos);
		std::string row = s_audio_rows[i].substr(0, 39);
		row.resize(39, ' ');
		console_write(i == 0 ? "\x1b[36m" : "");
		console_write(row.c_str());
		console_write("\x1b[0m");
	}
}

// Main thread, from __wrap_hidScanInput: refresh once a second.
void audio_overlay_update()
{
	const double now = elapsed_ms();
	if (now - s_audio_last_ms < 1000.0)
		return;
	s_audio_last_ms = now;

	char buf[4][64];
	const AudioStats a = audio_stats();
	const DecodeStats d = decode_stats();
	if (a.open)
	{
		const uint32_t n = a.calls - s_audio_prev_calls;
		const float avg = n ? (a.busy_ms - s_audio_prev_busy) / n : 0.0f;
		snprintf(buf[0], sizeof buf[0], "audio %d Hz %d smp %.1f ms/buf", a.rate, a.samples, a.budget_ms);
		snprintf(buf[1], sizeof buf[1], "mix avg %.1f max %.1f ms load %.0f%%", avg, a.max_ms,
		         a.budget_ms > 0 ? 100.0f * avg / a.budget_ms : 0.0f);
		snprintf(buf[2], sizeof buf[2], "underruns %lu %.2f/min %.1f min late %lu",
		         static_cast<unsigned long>(a.underruns),
		         a.played_s > 0 ? a.underruns * 60.0 / a.played_s : 0.0, a.played_s / 60.0,
		         static_cast<unsigned long>(a.late));
		s_audio_prev_calls = a.calls;
		s_audio_prev_busy = a.busy_ms;
	}
	else
	{
		snprintf(buf[0], sizeof buf[0], "audio: no device");
		snprintf(buf[1], sizeof buf[1], "%s", dsp_problem() == DspProblem::NoFirmware ? "DSP firmware missing" : "");
		buf[2][0] = 0;
	}
	if (d.last_file.empty())
		snprintf(buf[3], sizeof buf[3], "music: none decoded");
	else
	{
		// Decode time per second of audio produced, over the last second.
		const double audio = d.audio_s - s_dec_prev_audio;
		const double ms = d.decode_ms - s_dec_prev_ms;
		snprintf(buf[3], sizeof buf[3], "%s %d %s dec %.1f%% max %.1fms",
		         d.last_codec.substr(0, d.last_codec.find(' ')).c_str(), d.last_rate,
		         d.last_channels == 2 ? "st" : "mo", audio > 0 ? ms / (10.0 * audio) : 0.0, d.max_call_ms);
		s_dec_prev_audio = d.audio_s;
		s_dec_prev_ms = d.decode_ms;
	}
	for (int i = 0; i < 4; ++i)
		s_audio_rows[i] = buf[i];
	audio_overlay_draw();
	bottom_present();
}

// --- Raw event log (debug-input) ------------------------------------------------

// SDL's N3DS joystick: button i is libctru's KEY_* bit i (hidKeysDown()),
// axes 0/1 the Circle Pad, 2/3 the C-Stick (SDL_sysjoystick.c).
const char* hid_bit_name(int bit)
{
	static const char* names[32] = {
		"A", "B", "SELECT", "START", "D-Right", "D-Left", "D-Up", "D-Down",
		"R", "L", "X", "Y", "bit12", "bit13", "ZL", "ZR",
		"bit16", "bit17", "bit18", "bit19", "TOUCH", "bit21", "bit22", "bit23",
		"CStick-Right", "CStick-Left", "CStick-Up", "CStick-Down",
		"CPad-Right", "CPad-Left", "CPad-Up", "CPad-Down",
	};
	return bit >= 0 && bit < 32 ? names[bit] : "?";
}

const char* joy_axis_name(int axis)
{
	static const char* names[] = {"CirclePad-X", "CirclePad-Y", "CStick-X", "CStick-Y"};
	return axis >= 0 && axis < 4 ? names[axis] : "?";
}

void log_input(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void log_input(const char* fmt, ...)
{
	char buf[160];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof buf, fmt, ap);
	va_end(ap);
	printf("[3ds] input +%.1f ms: %s\n", elapsed_ms(), buf);
	overlay_add(buf);
}

void log_event(const SDL_Event& e)
{
	switch (e.type)
	{
		case SDL_JOYBUTTONDOWN:
		case SDL_JOYBUTTONUP:
			log_input("JOYBUTTON%s %d (%s)", e.type == SDL_JOYBUTTONDOWN ? "DOWN" : "UP",
			          e.jbutton.button, hid_bit_name(e.jbutton.button));
			break;
		case SDL_JOYAXISMOTION:
			log_input("JOYAXIS %d (%s) %d", e.jaxis.axis, joy_axis_name(e.jaxis.axis), e.jaxis.value);
			break;
		case SDL_CONTROLLERBUTTONDOWN:
		case SDL_CONTROLLERBUTTONUP:
			log_input("CTRLBUTTON%s %s", e.type == SDL_CONTROLLERBUTTONDOWN ? "DOWN" : "UP",
			          SDL_GameControllerGetStringForButton(static_cast<SDL_GameControllerButton>(e.cbutton.button)));
			break;
		case SDL_CONTROLLERAXISMOTION:
			log_input("CTRLAXIS %s %d",
			          SDL_GameControllerGetStringForAxis(static_cast<SDL_GameControllerAxis>(e.caxis.axis)),
			          e.caxis.value);
			break;
		case SDL_FINGERDOWN:
		case SDL_FINGERUP:
		case SDL_FINGERMOTION:
			log_input("FINGER%s %.0f,%.0f (bottom px)",
			          e.type == SDL_FINGERDOWN ? "DOWN" : e.type == SDL_FINGERUP ? "UP" : "MOTION",
			          e.tfinger.x * GSP_SCREEN_HEIGHT_BOTTOM, e.tfinger.y * GSP_SCREEN_WIDTH);
			break;
		case SDL_MOUSEMOTION:
			log_input("MOUSEMOTION %d,%d rel %d,%d", e.motion.x, e.motion.y, e.motion.xrel, e.motion.yrel);
			break;
		case SDL_MOUSEBUTTONDOWN:
		case SDL_MOUSEBUTTONUP:
			log_input("MOUSEBUTTON%s %d at %d,%d", e.type == SDL_MOUSEBUTTONDOWN ? "DOWN" : "UP",
			          e.button.button, e.button.x, e.button.y);
			break;
		case SDL_KEYDOWN:
		case SDL_KEYUP:
			log_input("KEY%s scancode %d", e.type == SDL_KEYDOWN ? "DOWN" : "UP", e.key.keysym.scancode);
			break;
		case SDL_TEXTINPUT:
			log_input("TEXTINPUT \"%s\"", e.text.text);
			break;
		case SDL_JOYDEVICEADDED:
		case SDL_JOYDEVICEREMOVED:
		case SDL_CONTROLLERDEVICEADDED:
		case SDL_CONTROLLERDEVICEREMOVED:
			log_input("device event 0x%x, index/instance %d", e.type, e.jdevice.which);
			break;
		case SDL_WINDOWEVENT:
			if (e.window.event == SDL_WINDOWEVENT_FOCUS_LOST || e.window.event == SDL_WINDOWEVENT_FOCUS_GAINED ||
			    e.window.event == SDL_WINDOWEVENT_EXPOSED)
				log_input("WINDOWEVENT %s",
				          e.window.event == SDL_WINDOWEVENT_FOCUS_LOST ? "FOCUS_LOST" :
				          e.window.event == SDL_WINDOWEVENT_FOCUS_GAINED ? "FOCUS_GAINED" : "EXPOSED");
			break;
		case SDL_QUIT:
			log_input("QUIT");
			break;
		default:
			break;
	}
}

// --- Touch screen ---------------------------------------------------------------

bool s_touch_look = false;
bool s_finger_down = false;        // a finger is down and being tracked
bool s_pointer_button = false;     // Pointer mode sent a button-down not yet released
bool s_touch_blocked = false;      // ignore touches until the finger is lifted
bool s_touch_block_pending = false; // decide at the next scan (after a keyboard / HOME visit)
float s_last_tx = 0, s_last_ty = 0;   // bottom-screen pixels
float s_look_rem_x = 0, s_look_rem_y = 0;
int s_pointer_x = 0, s_pointer_y = 0; // window pixels
int s_touch_button = -1;           // the finger is on this bottom-screen button

SDL_Window* main_window()
{
	SDL_Window* w = SDL_GetKeyboardFocus();
	return w ? w : SDL_GetMouseFocus();
}

void push_mouse_motion(SDL_Window* w, int x, int y, int xrel, int yrel)
{
	SDL_Event m;
	SDL_zero(m);
	m.type = SDL_MOUSEMOTION;
	m.motion.windowID = SDL_GetWindowID(w);
	m.motion.which = 0;
	m.motion.state = s_pointer_button ? SDL_BUTTON_LMASK : 0;
	m.motion.x = x;
	m.motion.y = y;
	m.motion.xrel = xrel;
	m.motion.yrel = yrel;
	SDL_PushEvent(&m);
}

void push_mouse_button(SDL_Window* w, bool down)
{
	SDL_Event b;
	SDL_zero(b);
	b.type = down ? SDL_MOUSEBUTTONDOWN : SDL_MOUSEBUTTONUP;
	b.button.windowID = SDL_GetWindowID(w);
	b.button.which = 0;
	b.button.button = SDL_BUTTON_LEFT;
	b.button.state = down ? SDL_PRESSED : SDL_RELEASED;
	b.button.clicks = 1;
	b.button.x = s_pointer_x;
	b.button.y = s_pointer_y;
	SDL_PushEvent(&b);
}

// The part of the window the engine's logical screen (640x480 for menus)
// is drawn into: SDL letterboxes it to 320x240 in the middle of the
// 400x240 top screen, which is exactly the bottom screen's size.
void content_rect(SDL_Window* w, float& x0, float& y0, float& x1, float& y1)
{
	int ww = 0, wh = 0;
	SDL_GetWindowSize(w, &ww, &wh);
	x0 = y0 = 0;
	x1 = ww;
	y1 = wh;
	SDL_Renderer* r = SDL_GetRenderer(w);
	int lw = 0, lh = 0;
	if (r)
		SDL_RenderGetLogicalSize(r, &lw, &lh);
	if (lw > 0 && lh > 0)
	{
		int ax, ay, bx, by;
		SDL_RenderLogicalToWindow(r, 0, 0, &ax, &ay);
		SDL_RenderLogicalToWindow(r, lw, lh, &bx, &by);
		x0 = ax;
		y0 = ay;
		x1 = bx;
		y1 = by;
	}
}

void release_pointer(SDL_Window* w)
{
	if (s_pointer_button)
	{
		s_pointer_button = false;
		if (w)
			push_mouse_button(w, false);
	}
}

void handle_finger(const SDL_TouchFingerEvent& f, Uint32 type)
{
	SDL_Window* w = main_window();
	const float tx = f.x * GSP_SCREEN_HEIGHT_BOTTOM;   // 0..320
	const float ty = f.y * GSP_SCREEN_WIDTH;           // 0..240

	// A finger that went down on a bottom-screen button stays a button
	// press until it is lifted.
	if (s_touch_button >= 0)
	{
		if (type == SDL_FINGERUP)
		{
			bottom_button_set(s_touch_button, false);
			s_touch_button = -1;
		}
		return;
	}
	if (type == SDL_FINGERDOWN && s_bottom_game && !s_touch_blocked)
	{
		const int button = bottom_button_at(tx, ty);
		if (button >= 0)
		{
			s_touch_button = button;
			bottom_button_set(button, true);
			return;
		}
	}

	if (type == SDL_FINGERUP)
	{
		s_finger_down = false;
		s_touch_blocked = false;
		release_pointer(w);
		return;
	}
	if (s_touch_blocked || !w)
		return;
	if (type == SDL_FINGERDOWN)
	{
		s_finger_down = true;
		s_last_tx = tx;
		s_last_ty = ty;
		s_look_rem_x = s_look_rem_y = 0;
	}
	else if (!s_finger_down)
		return;

	if (s_touch_look)
	{
		// Relative motion in bottom-screen pixels, sent as window pixels;
		// SDL's renderer then scales it to logical pixels. The engine gets
		// 2 logical pixels per bottom-screen pixel: with the 640x480 logical
		// screen that is SDL's scaling (renderer scale 0.5), with the 400x240
		// one (STAGE7.md, scale 1) the motion is doubled here. With the
		// default mouse sensitivity (0.25) a drag across the whole screen
		// turns about 220 degrees.
		float k = 1.0f;
		if (SDL_Renderer* r = SDL_GetRenderer(w))
		{
			float sx = 0, sy = 0;
			SDL_RenderGetScale(r, &sx, &sy);
			if (sx > 0)
				k = 2.0f * sx;
		}
		s_look_rem_x += (tx - s_last_tx) * k;
		s_look_rem_y += (ty - s_last_ty) * k;
		s_last_tx = tx;
		s_last_ty = ty;
		const int dx = static_cast<int>(s_look_rem_x);
		const int dy = static_cast<int>(s_look_rem_y);
		if (dx || dy)
		{
			s_look_rem_x -= dx;
			s_look_rem_y -= dy;
			int ww = 0, wh = 0;
			SDL_GetWindowSize(w, &ww, &wh);
			push_mouse_motion(w, ww / 2, wh / 2, dx, dy);
		}
		return;
	}

	float x0, y0, x1, y1;
	content_rect(w, x0, y0, x1, y1);
	const int x = static_cast<int>(x0 + f.x * (x1 - x0));
	const int y = static_cast<int>(y0 + f.y * (y1 - y0));
	const int xrel = x - s_pointer_x, yrel = y - s_pointer_y;
	s_pointer_x = x;
	s_pointer_y = y;
	if (type == SDL_FINGERDOWN || xrel || yrel)
		push_mouse_motion(w, x, y, xrel, yrel);
	if (type == SDL_FINGERDOWN)
	{
		s_pointer_button = true;
		push_mouse_button(w, true);
	}
}

// Runs inside SDL_PushEvent(), i.e. on the thread sending the event: the
// main thread for input (SDL_PumpEvents). Pushing more events from here is
// fine (SDL's N3DS mutexes are recursive).
int SDLCALL event_filter(void*, SDL_Event* e)
{
	if (s_debug_input)
		log_event(*e);
	switch (e->type)
	{
		case SDL_FINGERDOWN:
		case SDL_FINGERMOTION:
		case SDL_FINGERUP:
			handle_finger(e->tfinger, e->type);
			return 0;   // the engine does not use finger events
		default:
			return 1;
	}
}

// --- HOME menu, sleep, applets ------------------------------------------------

aptHookCookie s_apt_cookie;
bool s_in_home = false, s_asleep = false;
bool s_audio_paused_by_us = false;
bool s_exit_requested = false;
double s_suspend_ms = 0;

void push_window_event(Uint8 what)
{
	SDL_Window* w = main_window();
	if (!w)
		return;
	SDL_Event e;
	SDL_zero(e);
	e.type = SDL_WINDOWEVENT;
	e.window.event = what;
	e.window.windowID = SDL_GetWindowID(w);
	SDL_PushEvent(&e);
}

void on_suspend(const char* why)
{
	s_suspend_ms = elapsed_ms();
	audio_set_suspended(true);   // not an underrun: the DSP stops by design
	if (SDL_WasInit(SDL_INIT_AUDIO) && SDL_GetAudioStatus() == SDL_AUDIO_PLAYING)
	{
		SDL_PauseAudio(1);
		s_audio_paused_by_us = true;
	}
	printf("[3ds] +%.1f ms: suspended (%s)%s\n", s_suspend_ms, why,
	       s_audio_paused_by_us ? "; SDL audio paused" : "");
	fflush(stdout);
	// The engine's focus-lost handler pauses a game in progress (and its
	// OpenAL sources) and stops the main menu's demo timer.
	push_window_event(SDL_WINDOWEVENT_FOCUS_LOST);
	release_pointer(main_window());
	s_finger_down = false;
	if (s_touch_button >= 0)
	{
		bottom_button_set(s_touch_button, false);
		s_touch_button = -1;
	}
}

void on_resume(const char* why)
{
	const double now = elapsed_ms();
	printf("[3ds] +%.1f ms: resumed (%s) after %.1f s%s\n", now, why, (now - s_suspend_ms) / 1000.0,
	       s_audio_paused_by_us ? "; SDL audio resumed" : "");
	audio_set_suspended(false);
	if (s_audio_paused_by_us)
	{
		SDL_PauseAudio(0);
		s_audio_paused_by_us = false;
	}
	push_window_event(SDL_WINDOWEVENT_FOCUS_GAINED);
	push_window_event(SDL_WINDOWEVENT_EXPOSED);   // dialogs redraw on this
	s_touch_block_pending = true;   // a touch that closed the HOME menu is not a tap
	bottom_redraw();
	if (s_bottom_game)
		bottom_force_redraw();   // the HOME menu may have replaced the bottom screen
}

// Called by libctru inside aptMainLoop() (SDL_PumpEvents() on the main
// thread) and around library applets such as the keyboard.
void apt_hook(APT_HookType hook, void*)
{
	const bool was_suspended = s_in_home || s_asleep;
	switch (hook)
	{
		case APTHOOK_ONSUSPEND:
			s_in_home = true;
			break;
		case APTHOOK_ONSLEEP:
			s_asleep = true;
			break;
		case APTHOOK_ONRESTORE:
			s_in_home = false;
			break;
		case APTHOOK_ONWAKEUP:
			s_asleep = false;
			break;
		case APTHOOK_ONEXIT:
			s_exit_requested = true;
			printf("[3ds] +%.1f ms: the system asked the application to close\n", elapsed_ms());
			fflush(stdout);
			return;
		default:
			return;
	}
	const bool suspended = s_in_home || s_asleep;
	const char* why = (hook == APTHOOK_ONSLEEP || hook == APTHOOK_ONWAKEUP) ? "sleep" : "HOME menu or applet";
	if (suspended && !was_suspended)
		on_suspend(why);
	else if (!suspended && was_suspended)
		on_resume(why);
}

// --- Test script (debug-input-script) -------------------------------------------

// Buttons and sticks are injected where SDL reads them: --wrap on
// hidScanInput, hidKeysDown, hidKeysUp, hidCircleRead, irrstCstickRead
// (= hidCstickRead) and hidTouchRead (ports/3ds/CMakeLists.txt). Without a
// script the wrappers only add the post-keyboard debounce below.

struct Script
{
	std::vector<std::string> lines;
	size_t next = 0;
	bool running = false;
	double wait_until = 0;
	u32 held = 0, prev_held = 0;
	u32 down = 0, up = 0;          // for the current scan
	bool circle_set = false, cstick_set = false;
	circlePosition circle{0, 0}, cstick{0, 0};
	bool touching = false;
	float touch_x = 0, touch_y = 0;
	// drag in progress
	bool dragging = false;
	float dx0 = 0, dy0 = 0, dx1 = 0, dy1 = 0;
	double drag_t0 = 0, drag_ms = 0;
	bool have_swkbd = false, swkbd_cancel = false;
	std::string swkbd_text;
	bool quit = false;
} s_script;

u32 s_suppress = 0;               // buttons held across a keyboard visit
// SDL takes the first value it is given for a joystick axis as the axis's
// resting position and sends no event for it; the deflection only arrives
// when the stick moves again. SDL's N3DS driver reports an axis only when
// the HID value changes, so a stick resting at exactly 0 loses its first
// push. input_init() sets these so the next stick read reports 1 HID unit
// (0.6% of the range) once: SDL records that as the resting position.
bool s_prime_circle = false, s_prime_cstick = false;
bool s_suppress_pending = false;

u32 parse_button(const std::string& name)
{
	static const struct { const char* name; u32 key; } keys[] = {
		{"A", KEY_A}, {"B", KEY_B}, {"X", KEY_X}, {"Y", KEY_Y},
		{"SELECT", KEY_SELECT}, {"START", KEY_START},
		{"L", KEY_L}, {"R", KEY_R}, {"ZL", KEY_ZL}, {"ZR", KEY_ZR},
		{"UP", KEY_DUP}, {"DOWN", KEY_DDOWN}, {"LEFT", KEY_DLEFT}, {"RIGHT", KEY_DRIGHT},
	};
	for (const auto& k : keys)
		if (name == k.name)
			return k.key;
	printf("[3ds] script: unknown button \"%s\"\n", name.c_str());
	return 0;
}

u32 parse_buttons(std::istringstream& in)
{
	u32 mask = 0;
	std::string name;
	while (in >> name)
		mask |= parse_button(name);
	return mask;
}

void script_log(const std::string& what)
{
	printf("[3ds] script +%.1f ms: %s\n", elapsed_ms(), what.c_str());
}

// One command per line; blank lines and '#' comments are skipped. The
// script starts when the main menu is first displayed. Coordinates are
// bottom-screen pixels (0-319, 0-239); stick values are raw HID units
// (-160..160; up is positive y, as libctru reports it).
//
//   wait MS                    do nothing for MS milliseconds
//   press BUTTON...            hold buttons (A B X Y L R ZL ZR SELECT START UP DOWN LEFT RIGHT)
//   release BUTTON...          let go of them
//   tap BUTTON [MS]            press, wait MS (default 100), release, wait 100
//   circle DX DY | cstick DX DY    hold a stick there ("circle 0 0" centres it)
//   touch X Y                  finger down at, or moved to, X Y
//   untouch                    lift the finger
//   drag X0 Y0 X1 Y1 MS        finger down at X0 Y0, moved to X1 Y1 over MS ms (stays down)
//   home MS | sleep MS         HOME menu / sleep for MS ms: the APT hooks run and the
//                              main thread blocks, as on a console
//   swkbd TEXT | swkbd-cancel  the next keyboard returns TEXT / is cancelled
//   screens TAG                save both screens as screen-TAG-{top,bottom}.bmp
//   log TEXT                   write TEXT to the log
//   quit                       as if HOME > Close was chosen
bool run_script_line(const std::string& raw)
{
	std::string line = raw;
	const size_t hash = line.find('#');
	if (hash != std::string::npos)
		line.erase(hash);
	std::istringstream in(line);
	std::string cmd;
	if (!(in >> cmd))
		return true;
	Script& s = s_script;
	const double now = elapsed_ms();
	script_log(line);
	if (cmd == "wait")
	{
		double ms = 0;
		in >> ms;
		s.wait_until = now + ms;
	}
	else if (cmd == "press")
		s.held |= parse_buttons(in);
	else if (cmd == "release")
		s.held &= ~parse_buttons(in);
	else if (cmd == "tap")
	{
		std::string name;
		double ms = 100;
		in >> name >> ms;
		const u32 key = parse_button(name);
		s.held |= key;
		// expand into: wait ms; release; wait 100
		s.lines.insert(s.lines.begin() + s.next,
		               {"wait " + std::to_string(static_cast<int>(ms)), "release " + name, "wait 100"});
	}
	else if (cmd == "circle" || cmd == "cstick")
	{
		int x = 0, y = 0;
		in >> x >> y;
		circlePosition& p = cmd == "circle" ? s.circle : s.cstick;
		p.dx = x;
		p.dy = y;
		(cmd == "circle" ? s.circle_set : s.cstick_set) = true;
	}
	else if (cmd == "touch")
	{
		in >> s.touch_x >> s.touch_y;
		s.touching = true;
	}
	else if (cmd == "untouch")
	{
		s.touching = false;
		s.dragging = false;
	}
	else if (cmd == "drag")
	{
		in >> s.dx0 >> s.dy0 >> s.dx1 >> s.dy1 >> s.drag_ms;
		s.dragging = true;
		s.drag_t0 = now;
		s.touching = true;
		s.touch_x = s.dx0;
		s.touch_y = s.dy0;
		s.wait_until = now + s.drag_ms + 50;
	}
	else if (cmd == "home" || cmd == "sleep")
	{
		double ms = 1000;
		in >> ms;
		const bool home = cmd == "home";
		apt_hook(home ? APTHOOK_ONSUSPEND : APTHOOK_ONSLEEP, nullptr);
		svcSleepThread(static_cast<s64>(ms * 1000000.0));
		apt_hook(home ? APTHOOK_ONRESTORE : APTHOOK_ONWAKEUP, nullptr);
	}
	else if (cmd == "swkbd")
	{
		std::getline(in >> std::ws, s.swkbd_text);
		s.have_swkbd = true;
		s.swkbd_cancel = false;
	}
	else if (cmd == "swkbd-cancel")
	{
		s.have_swkbd = true;
		s.swkbd_cancel = true;
	}
	else if (cmd == "screens")
	{
		std::string tag = "script";
		in >> tag;
		save_screens(tag);
	}
	else if (cmd == "log")
		;   // already logged above
	else if (cmd == "quit")
	{
		s.quit = true;
		s_exit_requested = true;
	}
	else
		printf("[3ds] script: unknown command \"%s\"\n", cmd.c_str());
	return true;
}

// Called once per hidScanInput(), i.e. once per SDL_PumpEvents().
void script_step()
{
	Script& s = s_script;
	if (!s.running)
		return;
	const double now = elapsed_ms();
	if (s.dragging)
	{
		const double t = s.drag_ms > 0 ? std::min(1.0, (now - s.drag_t0) / s.drag_ms) : 1.0;
		s.touch_x = static_cast<float>(s.dx0 + (s.dx1 - s.dx0) * t);
		s.touch_y = static_cast<float>(s.dy0 + (s.dy1 - s.dy0) * t);
		if (t >= 1.0)
			s.dragging = false;
	}
	while (now >= s.wait_until && s.next < s.lines.size())
		run_script_line(s.lines[s.next++]);
	if (s.next >= s.lines.size() && now >= s.wait_until && !s.dragging)
	{
		script_log("end of script");
		s.running = false;
	}
}

} // namespace

// --- HID wrappers -------------------------------------------------------------

extern "C" {
void __real_hidScanInput(void);
u32 __real_hidKeysDown(void);
u32 __real_hidKeysUp(void);
void __real_hidCircleRead(circlePosition* pos);
void __real_irrstCstickRead(circlePosition* pos);
void __real_hidTouchRead(touchPosition* pos);

void __wrap_hidScanInput(void)
{
	__real_hidScanInput();
	if (s_suppress_pending)
	{
		s_suppress = hidKeysHeld();
		s_suppress_pending = false;
	}
	else
		s_suppress &= hidKeysHeld();   // forget buttons once released

	if (s_debug_audio && s_console_out)
		audio_overlay_update();

	Script& s = s_script;
	script_step();
	u32 held = s.held | (s.touching ? KEY_TOUCH : 0);
	if (s_touch_block_pending)
	{
		// Back from the keyboard or the HOME menu: a finger still on the
		// screen pressed something there, not here.
		s_touch_blocked = ((hidKeysHeld() | held) & KEY_TOUCH) != 0;
		s_touch_block_pending = false;
	}
	s.down = held & ~s.prev_held;
	s.up = s.prev_held & ~held;
	s.prev_held = held;
	// The no-sound notice: shown once the DSP failed, dismissed by any
	// button or a tap (real or scripted). The press that dismisses it is not
	// passed on, so it does not also act on the menu.
	if (s_console_out && s_dsp_notice == DspNotice::Pending && dsp_problem() != DspProblem::None)
	{
		s_dsp_notice = DspNotice::Showing;
		printf("[3ds] +%.1f ms: showing the no-sound (DSP) notice on the bottom screen\n", elapsed_ms());
		bottom_redraw();
	}
	else if (s_dsp_notice == DspNotice::Showing &&
	         (((__real_hidKeysDown() | s.down) & ~KEY_TOUCH) || ((hidKeysHeld() | held) & KEY_TOUCH)))
	{
		s_suppress |= hidKeysHeld() & ~KEY_TOUCH;
		s.down = 0;
		if ((hidKeysHeld() | held) & KEY_TOUCH)
			s_touch_blocked = true;
		s_dsp_notice = DspNotice::Dismissed;
		printf("[3ds] +%.1f ms: no-sound notice dismissed\n", elapsed_ms());
		bottom_redraw();
	}
	if (s.quit)
	{
		// What SDL does while aptMainLoop() returns false.
		SDL_Event q;
		SDL_zero(q);
		q.type = SDL_QUIT;
		SDL_PushEvent(&q);
	}
}

u32 __wrap_hidKeysDown(void)
{
	return (__real_hidKeysDown() & ~s_suppress) | s_script.down;
}

u32 __wrap_hidKeysUp(void)
{
	return __real_hidKeysUp() | s_script.up;
}

void __wrap_hidCircleRead(circlePosition* pos)
{
	__real_hidCircleRead(pos);
	if (s_script.circle_set)
		*pos = s_script.circle;
	if (s_prime_circle)
	{
		s_prime_circle = false;
		pos->dx = pos->dy = 1;
	}
}

void __wrap_irrstCstickRead(circlePosition* pos)
{
	__real_irrstCstickRead(pos);
	if (s_script.cstick_set)
		*pos = s_script.cstick;
	if (s_prime_cstick)
	{
		s_prime_cstick = false;
		pos->dx = pos->dy = 1;
	}
}

void __wrap_hidTouchRead(touchPosition* pos)
{
	__real_hidTouchRead(pos);
	if (s_script.touching)
	{
		// (0, 0) means "not touched" to SDL; keep scripted touches off it.
		pos->px = static_cast<u16>(std::max(1.0f, std::min(319.0f, s_script.touch_x)));
		pos->py = static_cast<u16>(std::max(1.0f, std::min(239.0f, s_script.touch_y)));
	}
}

// The engine calls SDL_StartTextInput() when a text field gains focus. On
// the N3DS driver that opens a blocking system keyboard at once; here it
// only enables text events, and the text field opens the keyboard itself
// when it is clicked (a1_3ds::text_input()).
void __wrap_SDL_StartTextInput(void)
{
	SDL_EventState(SDL_TEXTINPUT, SDL_ENABLE);
	SDL_EventState(SDL_TEXTEDITING, SDL_ENABLE);
}
} // extern "C"

// --- Public API -------------------------------------------------------------------

void input_script_start()
{
	static bool started = false;
	if (started)
		return;
	started = true;
	std::ifstream in(user_dir() + "/" + kInputScriptFile);
	if (!in)
		return;
	std::string line;
	while (std::getline(in, line))
		s_script.lines.push_back(line);
	s_script.running = true;
	s_script.wait_until = 0;
	script_log("started (" + std::to_string(s_script.lines.size()) + " lines from " + kInputScriptFile + ")");
}

void input_init()
{
	s_debug_input = file_exists(user_dir() + "/" + kDebugInputFlag);
	s_debug_audio = file_exists(user_dir() + "/" + kDebugAudioFlag);

	// What SDL reports (docs/3ds/STAGE5.md, "SDL's view of the controls").
	const int n = SDL_NumJoysticks();
	printf("[3ds] input: %d joystick(s)%s\n", n, s_debug_input ? "; debug-input: logging every input event" : "");
	for (int i = 0; i < n; ++i)
	{
		char guid[64];
		SDL_JoystickGetGUIDString(SDL_JoystickGetDeviceGUID(i), guid, sizeof guid);
		printf("[3ds] input: joystick %d \"%s\", GUID %s, game controller: %s\n", i,
		       SDL_JoystickNameForIndex(i), guid, SDL_IsGameController(i) ? "yes" : "no");
		if (SDL_GameController* c = SDL_GameControllerOpen(i))   // adds a reference
		{
			SDL_Joystick* j = SDL_GameControllerGetJoystick(c);
			printf("[3ds] input:   %d buttons, %d axes, %d hats\n", SDL_JoystickNumButtons(j),
			       SDL_JoystickNumAxes(j), SDL_JoystickNumHats(j));
			if (char* mapping = SDL_GameControllerMapping(c))
			{
				printf("[3ds] input:   mapping %s\n", mapping);
				SDL_free(mapping);
			}
			SDL_GameControllerClose(c);
		}
	}
	printf("[3ds] input: %d touch device(s)\n", SDL_GetNumTouchDevices());

	s_prime_circle = s_prime_cstick = true;
	SDL_SetEventFilter(event_filter, nullptr);
	aptHook(&s_apt_cookie, apt_hook, nullptr);
	bottom_init();
}

void set_touch_look(bool look)
{
	if (look == s_touch_look)
		return;
	release_pointer(main_window());
	s_touch_look = look;
	// A finger already down belongs to the old mode (e.g. the tap that
	// resumed the game): ignore it until it is lifted.
	if (s_finger_down)
		s_touch_blocked = true;
	s_finger_down = false;
	if (s_debug_input)
		log_input("touch mode: %s", look ? "look" : "pointer");
}

bool exit_requested()
{
	return s_exit_requested;
}

void bottom_game(bool game)
{
	if (game == s_bottom_game || !s_console_out)
		return;
	if (s_touch_button >= 0)
	{
		bottom_button_set(s_touch_button, false);
		s_touch_button = -1;
	}
	if (game)
	{
		// Double-buffered, so a HUD update never shows half drawn. The
		// screen stays RGB565 (consoleInit's format): half the bytes of RGBA8.
		s_bottom_game = true;
		gfxSetDoubleBuffering(GFX_BOTTOM, true);
		bottom_force_redraw();
	}
	else
	{
		s_bottom_game = false;
		console_attach();   // single-buffered again, with the console's framebuffer
		bottom_redraw();
	}
	if (s_debug_input)
		log_input("bottom screen: %s", game ? "game" : "legend");
}

bool bottom_is_game()
{
	return s_bottom_game;
}

#ifdef A1_PROFILE
// profile_3ds.cpp: the benchmark is over (debug-perf "quit 1").
void request_quit()
{
	s_script.quit = true;   // SDL_QUIT on every hidScanInput, as the script's "quit"
	s_exit_requested = true;
}

// profile_3ds.cpp, once a second during play: the rows replace the legend.
// The debug-input log and the no-sound notice take precedence.
void perf_overlay_show(const char* const rows[], int count)
{
	s_perf_rows.assign(rows, rows + count);
	if (!s_console_out || s_debug_input || s_dsp_notice == DspNotice::Showing)
		return;
	perf_rows_draw();
	bottom_present();
}
#endif

bool text_input(const std::string& hint, std::string& text, size_t max_chars, TextKind kind)
{
	printf("[3ds] keyboard: \"%s\", initial \"%s\", up to %u characters\n", hint.c_str(),
	       kind == TextKind::Password ? "(hidden)" : text.c_str(), static_cast<unsigned>(max_chars));

	bool ok = false;
	if (s_script.have_swkbd)
	{
		s_script.have_swkbd = false;
		if (!s_script.swkbd_cancel)
		{
			text = s_script.swkbd_text.substr(0, max_chars);
			ok = true;
		}
		printf("[3ds] keyboard: scripted, %s\n", ok ? "confirmed" : "cancelled");
	}
	else
	{
		SwkbdState kb;
		swkbdInit(&kb, kind == TextKind::Number ? SWKBD_TYPE_NUMPAD : SWKBD_TYPE_NORMAL, 2,
		          static_cast<int>(std::max<size_t>(max_chars, 1)));
		swkbdSetHintText(&kb, hint.c_str());
		swkbdSetInitialText(&kb, text.c_str());
		swkbdSetValidation(&kb, SWKBD_ANYTHING, 0, 0);
		if (kind == TextKind::Password)
			swkbdSetPasswordMode(&kb, SWKBD_PASSWORD_HIDE_DELAY);
		// swkbd returns UTF-8 converted from UTF-16: up to 3 bytes per character.
		std::vector<char> buf(max_chars * 3 + 1, 0);
		const SwkbdButton button = swkbdInputText(&kb, buf.data(), buf.size());
		ok = button == SWKBD_BUTTON_CONFIRM;
		if (ok)
			text = buf.data();
		printf("[3ds] keyboard: %s (button %d, result %d)\n", ok ? "confirmed" : "cancelled",
		       static_cast<int>(button), static_cast<int>(swkbdGetResult(&kb)));
	}
	// Whatever confirmed the keyboard (A, START, a tap on OK) may still be
	// held: do not let it act on the dialog as well.
	s_suppress_pending = true;
	s_touch_block_pending = true;
	bottom_redraw();
	return ok;
}

} // namespace a1_3ds
