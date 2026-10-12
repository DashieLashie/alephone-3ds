/*
	Aleph One for the Nintendo 3DS: the frame profiler (A1_PROFILE builds only;
	see ports/3ds/include/a1_3ds_profile.h and docs/3ds/PERF.md).

	- Scoped timers (svcGetSystemTick) around the main thread's per-frame
	  work; exclusive times, summed per displayed game frame.
	- Every second of play: ms per frame for each zone on the bottom screen
	  (and in log.txt with "log 1" in debug-perf).
	- Benchmark: with sdmc:/3ds/alephone/debug-perf, BEGIN NEW GAME starts
	  the level it names; the profiler waits `settle` seconds of play, then
	  averages `measure` seconds and logs one "[prof] BENCH" line. With
	  "quit 1" the game then quits (as HOME > Close).

	debug-perf (one setting per line, '#' starts a comment):

	  level 0       level index (0 = first level)
	  settle 8      seconds of play before measuring
	  measure 20    seconds measured
	  quit 1        quit after the BENCH line
	  log 1         also log every 1-second window

	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; either version 3 of the License, or
	(at your option) any later version.
*/
#include "a1_3ds_profile.h"
#include "a1_3ds_platform.h"

#include <3ds.h>

#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <fstream>
#include <sstream>
#include <string>

namespace a1_3ds {

// source/input_3ds.cpp
void perf_overlay_show(const char* const rows[], int count);
void request_quit();
// source/a1_3ds_platform.cpp: both screens to sdmc:/3ds/alephone/screen-<tag>-*.bmp
void save_screens(const std::string& tag);
// source/audio_3ds.cpp
uint64_t audio_busy_ticks();

namespace prof {

namespace {

const char kDebugPerfFile[] = "debug-perf";
const char* const kZoneNames[kZoneCount] = {
	"world", "render", "hud", "blit", "present", "idle", "overlay"
};

constexpr uint64_t kTicksPerSecond = SYSCLOCK_ARM11;

double to_ms(uint64_t t)
{
	return static_cast<double>(t) * 1000.0 / kTicksPerSecond;
}

// --- settings -----------------------------------------------------------------

struct Settings
{
	bool loaded = false;
	bool present = false;   // debug-perf exists
	int level = -1;
	double settle_s = 8;
	double measure_s = 20;
	bool quit = false;
	bool log_windows = false;
};
Settings s_cfg;

const Settings& settings()
{
	if (s_cfg.loaded)
		return s_cfg;
	s_cfg.loaded = true;
	std::ifstream in(user_dir() + "/" + kDebugPerfFile);
	if (!in)
		return s_cfg;
	s_cfg.present = true;
	std::string line;
	while (std::getline(in, line))
	{
		line = line.substr(0, line.find('#'));
		std::istringstream ls(line);
		std::string key;
		double value = 0;
		if (!(ls >> key >> value))
			continue;
		if (key == "level")
			s_cfg.level = static_cast<int>(value);
		else if (key == "settle")
			s_cfg.settle_s = value;
		else if (key == "measure")
			s_cfg.measure_s = value;
		else if (key == "quit")
			s_cfg.quit = value != 0;
		else if (key == "log")
			s_cfg.log_windows = value != 0;
		else
			printf("[prof] debug-perf: unknown setting \"%s\"\n", key.c_str());
	}
	printf("[prof] debug-perf: level %d, settle %.1f s, measure %.1f s, quit %d, log %d\n",
	       s_cfg.level, s_cfg.settle_s, s_cfg.measure_s, s_cfg.quit, s_cfg.log_windows);
	return s_cfg;
}

// --- accumulation ---------------------------------------------------------------

// Open zones (main thread), innermost last.
struct Open
{
	Zone zone;
	uint64_t start;
	uint64_t children;   // time of the zones nested in it
};
Open s_open[16];
int s_depth = 0;
uint64_t s_zone[kZoneCount] = {};     // exclusive ticks since the window began

struct Totals
{
	uint64_t wall = 0;                // first frame_done of the window to the last
	uint32_t frames = 0;
	uint64_t max_frame = 0;
	uint64_t zone[kZoneCount] = {};
	uint64_t audio = 0;               // audio callback time (audio thread)

	void add(const Totals& o)
	{
		wall += o.wall;
		frames += o.frames;
		max_frame = std::max(max_frame, o.max_frame);
		for (int z = 0; z < kZoneCount; ++z)
			zone[z] += o.zone[z];
		audio += o.audio;
	}
};

uint64_t s_last_frame = 0;            // tick of the previous frame_done
uint64_t s_window_start = 0;
uint64_t s_audio_base = 0;
Totals s_window;
// The overlay is drawn between two windows, so it is in neither's frame
// times; the next window reports what it cost.
uint64_t s_overlay_ticks = 0;

// Benchmark state.
enum class Bench { Off, Armed, Settling, Measuring, Done };
Bench s_bench = Bench::Off;
uint64_t s_bench_t0 = 0;
Totals s_bench_totals;
uint32_t s_bench_gaps = 0;            // pauses/stalls (> 0.5 s) while measuring

// "ms per frame" for one zone of a set of totals.
double per_frame(const Totals& t, uint64_t ticks)
{
	return t.frames ? to_ms(ticks) / t.frames : 0.0;
}

double other_ms(const Totals& t)
{
	uint64_t zones = 0;
	for (int z = 0; z < kZoneCount; ++z)
		if (z != Overlay)   // drawn between windows, outside `wall`
			zones += t.zone[z];
	return t.wall > zones ? per_frame(t, t.wall - zones) : 0.0;
}

// One line with every figure, for the log.
std::string describe(const Totals& t)
{
	char buf[512];
	int n = snprintf(buf, sizeof buf, "frames=%lu fps=%.2f frame=%.2f max=%.2f",
	                 static_cast<unsigned long>(t.frames),
	                 t.wall ? t.frames * static_cast<double>(kTicksPerSecond) / t.wall : 0.0,
	                 per_frame(t, t.wall), to_ms(t.max_frame));
	for (int z = 0; z < kZoneCount; ++z)
		n += snprintf(buf + n, sizeof buf - n, " %s=%.2f", kZoneNames[z], per_frame(t, t.zone[z]));
	snprintf(buf + n, sizeof buf - n, " other=%.2f audio=%.2f", other_ms(t), per_frame(t, t.audio));
	return buf;
}

void draw_overlay(const Totals& t)
{
	char rows[8][48];
	const double fps = t.wall ? t.frames * static_cast<double>(kTicksPerSecond) / t.wall : 0.0;
	snprintf(rows[0], sizeof rows[0], "\x1b[36;1mA1_PROFILE\x1b[0m ms per frame, last 1 s");
	snprintf(rows[1], sizeof rows[1], "fps %5.1f  frame %5.1f  max %5.1f", fps, per_frame(t, t.wall),
	         to_ms(t.max_frame));
	snprintf(rows[2], sizeof rows[2], "world   %5.1f    render  %5.1f", per_frame(t, t.zone[World]),
	         per_frame(t, t.zone[Render]));
	snprintf(rows[3], sizeof rows[3], "hud     %5.1f    blit    %5.1f", per_frame(t, t.zone[Hud]),
	         per_frame(t, t.zone[Blit]));
	snprintf(rows[4], sizeof rows[4], "present %5.1f    idle    %5.1f", per_frame(t, t.zone[Present]),
	         per_frame(t, t.zone[Idle]));
	snprintf(rows[5], sizeof rows[5], "other   %5.1f    audio   %5.1f", other_ms(t), per_frame(t, t.audio));
	switch (s_bench)
	{
		case Bench::Off:
		case Bench::Armed:
			snprintf(rows[6], sizeof rows[6], "%s", settings().present ? "bench: waiting for the level" : "");
			break;
		case Bench::Settling:
			snprintf(rows[6], sizeof rows[6], "bench L%d: settling %.0f/%.0f s", settings().level,
			         to_ms(s_last_frame - s_bench_t0) / 1000.0, settings().settle_s);
			break;
		case Bench::Measuring:
			snprintf(rows[6], sizeof rows[6], "bench L%d: measuring %.0f/%.0f s", settings().level,
			         to_ms(s_bench_totals.wall) / 1000.0, settings().measure_s);
			break;
		case Bench::Done:
			snprintf(rows[6], sizeof rows[6], "bench L%d: done, %.1f fps", settings().level,
			         s_bench_totals.wall ? s_bench_totals.frames * static_cast<double>(kTicksPerSecond) /
			                                   s_bench_totals.wall : 0.0);
			break;
	}
	snprintf(rows[7], sizeof rows[7], "(audio: audio thread; core 1 on hw)");
	const char* ptrs[8];
	for (int i = 0; i < 8; ++i)
		ptrs[i] = rows[i];
	perf_overlay_show(ptrs, 8);
}

void start_window(uint64_t now)
{
	s_window = Totals();
	s_window_start = now;
	std::fill(s_zone, s_zone + kZoneCount, 0);
	s_zone[Overlay] = s_overlay_ticks;
	s_overlay_ticks = 0;
	s_audio_base = audio_busy_ticks();
}

// A complete window: hand it to the benchmark and the overlay.
void finish_window(uint64_t now)
{
	s_window.wall = now - s_window_start;
	std::copy(s_zone, s_zone + kZoneCount, s_window.zone);
	s_window.audio = audio_busy_ticks() - s_audio_base;
	const Settings& cfg = settings();

	if (s_bench == Bench::Settling && to_ms(now - s_bench_t0) >= cfg.settle_s * 1000.0)
	{
		s_bench = Bench::Measuring;
		printf("[prof] bench: level %d, measuring %.1f s\n", cfg.level, cfg.measure_s);
	}
	else if (s_bench == Bench::Measuring)
	{
		s_bench_totals.add(s_window);
		if (to_ms(s_bench_totals.wall) >= cfg.measure_s * 1000.0)
		{
			s_bench = Bench::Done;
			printf("[prof] BENCH level=%d seconds=%.1f gaps=%lu %s\n", cfg.level,
			       to_ms(s_bench_totals.wall) / 1000.0, static_cast<unsigned long>(s_bench_gaps),
			       describe(s_bench_totals).c_str());
			fflush(stdout);
			// What the measured spot looked like (top) and the overlay (bottom).
			draw_overlay(s_window);
			save_screens("perf-L" + std::to_string(cfg.level));
			if (cfg.quit)
				request_quit();
		}
	}
	if (cfg.log_windows)
		printf("[prof] window %s\n", describe(s_window).c_str());

	const uint64_t t0 = svcGetSystemTick();
	draw_overlay(s_window);
	s_overlay_ticks = svcGetSystemTick() - t0;
}

} // namespace

uint64_t ticks()
{
	return svcGetSystemTick();
}

void push(Zone zone)
{
	if (s_depth < static_cast<int>(sizeof s_open / sizeof s_open[0]))
		s_open[s_depth] = Open{zone, svcGetSystemTick(), 0};
	++s_depth;
}

void pop()
{
	if (s_depth <= 0)
		return;
	--s_depth;
	if (s_depth >= static_cast<int>(sizeof s_open / sizeof s_open[0]))
		return;
	const Open& o = s_open[s_depth];
	const uint64_t elapsed = svcGetSystemTick() - o.start;
	s_zone[o.zone] += elapsed - std::min(elapsed, o.children);
	if (s_depth > 0)
		s_open[s_depth - 1].children += elapsed;
}

void frame_done()
{
	const uint64_t now = svcGetSystemTick();
	// Half a second without a frame: level loading, a pause, a menu or the
	// HOME menu. Start over instead of averaging it in.
	if (s_last_frame == 0 || now - s_last_frame > kTicksPerSecond / 2)
	{
		if (s_bench == Bench::Measuring)
			++s_bench_gaps;
		if (s_bench == Bench::Armed)
		{
			s_bench = Bench::Settling;
			s_bench_t0 = now;
			printf("[prof] bench: level %d running, settling %.1f s\n", settings().level, settings().settle_s);
		}
		s_last_frame = now;
		start_window(now);
		return;
	}
	s_window.max_frame = std::max(s_window.max_frame, now - s_last_frame);
	++s_window.frames;
	s_last_frame = now;
	if (now - s_window_start >= kTicksPerSecond)
	{
		finish_window(now);
		start_window(svcGetSystemTick());
		s_last_frame = s_window_start;   // the overlay's time is not a frame's
	}
}

int bench_level()
{
	const Settings& cfg = settings();
	if (cfg.level < 0 || s_bench != Bench::Off)
		return -1;   // one benchmark per run
	s_bench = Bench::Armed;
	printf("[prof] bench: starting level %d instead of the first\n", cfg.level);
	return cfg.level;
}

bool bench_armed()
{
	return s_bench != Bench::Off;
}

} // namespace prof
} // namespace a1_3ds
