/*
	Aleph One for the Nintendo 3DS, citro3d back end: a1_c3d::render_frame(), the M0 harness
	(docs/3ds/CITRO3D_DESIGN.md, 1.2, 1.5, 8.1 and section 7, M0 specification):

	  - the readback calibration (first 30 world frames, back/view.cpp)
	  - the diff frame: one traversal recorded while SW draws it into scratch A, every comparison a
	    replay of the recording, the real frame the last replay (into dest)
	  - null mode: a counting rasteriser sees the traversal; the caller then draws in SW
	  - the once-a-second "[c3d] frame stats" line and the final null / DIFF summaries

	Engine headers and the plain headers of back/ and core/ only: no <3ds.h>, no <citro3d.h>.
	Main thread only (render_view's thread).

	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; either version 3 of the License, or
	(at your option) any later version.
*/
#include "a1_c3d.h"

#include "cseries.h"
#include "render.h"
#include "Rasterizer.h"
#include "Rasterizer_SW.h"
#include "textures.h"
#include "preferences.h"
#include "scottish_textures.h"
#include "low_level_textures.h"   // texture_random_seed()

#include "filter.h"
#include "c3d_rasterizer.h"
#include "../back/context.h"
#include "../back/draw.h"
#include "../back/view.h"
#include "../core/bmp.h"
#include "../core/calibrate.h"
#include "../core/classify.h"
#include "../core/config.h"
#include "../core/diff.h"
#include "../core/stats.h"

#include "a1_3ds_platform.h"
#ifdef A1_PROFILE
#include "a1_3ds_profile.h"
#endif

#include <stdio.h>

#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace a1_c3d {

namespace {

constexpr unsigned kCalFrames = 30;
constexpr unsigned kMaxDumpFiles = 32;

// --- state (main thread only) ---

int s_active = -1;                  // -1 = not decided; 0/1 = whether debug-c3d asks for anything
unsigned s_frame = 0;               // world frames render_frame saw (after the fast path and the depth test)
uint64_t s_stats_ms = 0;

// calibration
unsigned s_cal_done = 0, s_cal_skipped = 0;
unsigned s_cal_orient_ok = 0, s_cal_frame_ok = 0, s_cal_channels_ok = 0, s_cal_alpha_ok = 0;
unsigned long long s_cal_mismatched = 0, s_cal_sentinel = 0;
bool s_cal_summary = false;

// diff
unsigned s_diff_done = 0;           // diff frames compared
unsigned s_diff_dropped = 0;        // diff frames dropped (recorder overflow)
unsigned s_diff_checks = 0;
uint64_t s_diff_differ_total = 0;
unsigned s_dump_files = 0;
bool s_diff_summary_printed = false;
unsigned s_c3d_compared = 0, s_c3d_discarded = 0, s_c3d_skipped = 0;
unsigned long long s_c3d_ties = 0, s_c3d_tie_mismatch = 0;
unsigned s_c3d_drawn = 0, s_c3d_accept_skips[5] = {0, 0, 0, 0, 0};
C3dRasterizer s_c3d_rast;

// null
unsigned s_null_frames = 0;
bool s_logged_mode = false;
unsigned s_pair_table_full = 0;

short s_depth_logged = 0;           // the unsupported (bit_depth, w, h) last logged
int s_size_logged_w = 0, s_size_logged_h = 0;
bool s_unsupported_logged = false;

struct NullStats
{
	Samples polys, rects, vertices, indices, changes, pairs;
};
std::unique_ptr<NullStats> s_null_stats;

Recorder s_recorder;

// A scratch bitmap with dest's geometry: a bitmap_definition (+ row addresses) over its own pixels.
struct Scratch
{
	bitmap_definition_buffer def;
	std::vector<uint32_t> pixels;
	int w = 0, h = 0;

	void make(int width, int height)
	{
		if (w == width && h == height && !def.empty())
			return;
		w = width;
		h = height;
		pixels.assign(static_cast<size_t>(width) * height, 0);
		def = bitmap_definition_buffer(height);
		bitmap_definition* d = def.get();
		d->width = static_cast<int16>(width);
		d->height = static_cast<int16>(height);
		d->bytes_per_row = static_cast<int16>(width * 4);
		d->flags = 0;
		d->bit_depth = 32;
		d->row_addresses[0] = reinterpret_cast<pixel8*>(pixels.data());
		precalculate_bitmap_row_addresses(d);
	}
	void clear(uint32_t value) { pixels.assign(pixels.size(), value); }
	bitmap_definition* get() { return def.get(); }
	Image32 image() const { return Image32{pixels.data(), w, h, w}; }
};
Scratch s_a, s_b;

bool write_file(const std::string& name, const std::vector<uint8_t>& bytes)
{
	if (bytes.empty())
		return false;
	std::ofstream out(a1_3ds::user_dir() + "/" + name, std::ios::binary | std::ios::trunc);
	if (!out)
		return false;
	out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
	return static_cast<bool>(out);
}

// --- calibration ---

void run_calibration(const Config& cfg, int w, int h)
{
	if (s_cal_done >= kCalFrames)
		return;
	CalResult res;
	const uint32_t* px = nullptr;
	int cw = w, ch = h;   // the size used: readback_cal_resize shrinks the odd frames
	if (!back::view_calibrate(s_cal_done, w, h, res, &px, &cw, &ch))
	{
		++s_cal_skipped;
		return;
	}
	printf("%s\n", cal_line(s_cal_done, res).c_str());
	s_cal_orient_ok += res.orientation == Orientation::Ok;
	s_cal_frame_ok += res.current_frame_ok;
	s_cal_channels_ok += res.channels_ok;
	s_cal_alpha_ok += res.alpha_ok;
	s_cal_mismatched += res.mismatched;
	s_cal_sentinel += res.sentinel;
	if (cfg.dump && px && (s_cal_done == 0 || s_cal_done == kCalFrames - 1))
	{
		char name[48];
		snprintf(name, sizeof name, "c3d-rbcal-%u.bmp", s_cal_done);
		write_file(name, bmp_encode(px, cw, ch, cw));
	}
	++s_cal_done;
	if (s_cal_done == kCalFrames && !s_cal_summary)
	{
		s_cal_summary = true;
		printf("[c3d] readback_cal summary: %u frames, orientation ok %u, current frame ok %u, channels ok %u, "
		       "alpha ok %u, mismatched pixels %llu (sentinel %llu), skipped %u\n", s_cal_done, s_cal_orient_ok,
		       s_cal_frame_ok, s_cal_channels_ok, s_cal_alpha_ok, s_cal_mismatched, s_cal_sentinel, s_cal_skipped);
	}
	fflush(stdout);
}

// --- diff ---

void dump_pair(unsigned frame, const char* cls)
{
	if (s_dump_files + 2 > kMaxDumpFiles)
		return;
	char name[96];
	snprintf(name, sizeof name, "c3d-diff-%u-%s-sw.bmp", frame, cls);
	write_file(name, bmp_encode(s_a.pixels.data(), s_a.w, s_a.h, s_a.w));
	snprintf(name, sizeof name, "c3d-diff-%u-%s-c3d.bmp", frame, cls);
	write_file(name, bmp_encode(s_b.pixels.data(), s_b.w, s_b.h, s_b.w));
	s_dump_files += 2;
}

void check(const Config& cfg, unsigned frame, const char* cls, Coverage cov, uint32_t clear, bool first_diff_frame)
{
	const DiffResult r = diff_images(s_a.image(), s_b.image(), cov, clear, 0);
	printf("%s\n", diff_line(frame, cls, r, 0).c_str());
	++s_diff_checks;
	s_diff_differ_total += r.differ;
	if (cfg.dump && (first_diff_frame || r.differ > 0))
		dump_pair(frame, cls);
}

void diff_summary()
{
	if (s_diff_summary_printed)
		return;
	s_diff_summary_printed = true;
	printf("[c3d] DIFF summary frames=%u checks=%u differ_total=%llu dropped=%u c3d_compared=%u c3d_discarded=%u "
	       "c3d_skipped=%u c3d_drawn=%u c3d_accept_skips=degenerate:%u,bounds:%u,walk:%u,area:%u c3d_ties=%llu c3d_tie_mismatch=%llu\n", s_diff_done,
	       s_diff_checks, static_cast<unsigned long long>(s_diff_differ_total), s_diff_dropped, s_c3d_compared,
	       s_c3d_discarded, s_c3d_skipped, s_c3d_drawn, s_c3d_accept_skips[1], s_c3d_accept_skips[2],
	       s_c3d_accept_skips[3], s_c3d_accept_skips[4], s_c3d_ties, s_c3d_tie_mismatch);
	fflush(stdout);
}

// One C3D check (M1): side A = SW replay of filter(class, c3d_only) into scratch A, side B = one C3D view
// frame fed by the same replay. Logs one line.
void c3d_check(const Config& cfg, int c, uint16 s0, Rasterizer_SW_Class* swr, RasterizerClass* sw, bool first)
{
	const char* cls = class_name(c);
	const int w = s_a.w, h = s_a.h;

	// With software_alpha_blending on, SW blends walls and floors (scottish_textures.cpp:386-399, 610-627):
	// C3D draws them opaque, so the check is not made (M2 specification, "What C3D draws in M2").
	if (graphics_preferences->software_alpha_blending != _sw_alpha_off)
	{
		++s_diff_checks;
		++s_c3d_skipped;
		printf("[c3d] DIFF frame=%u class=%s skipped (%s)\n", s_frame, cls,
		       back::skip_name(back::view_skip(back::SkipReason::Pref).skip));
		return;
	}

	texture_random_seed() = s0;
	s_a.clear(0);
	swr->screen = s_a.get();
	FilterRasterizer fa(sw, c, cfg.fullbright, true, cfg.textures);
	s_recorder.replay(&fa);

	// Zone c3d_draw (design 7, M1 "Zones"): the C3D side from view_begin to view_end, the front end's
	// replay through C3dRasterizer included; the readback and its retirement wait inside view_end are
	// timed in the nested c3d_readback / c3d_wait. One scope per check, none per polygon.
	back::ViewBegin vb;
	back::ViewEnd ve{false, back::DiscardReason::Begin};
	unsigned sentinel = 0;
	{
#ifdef A1_PROFILE
		A1_PROF_SCOPE(C3dDraw);
#endif
		vb = back::view_begin(w, h);
		if (vb.kind == back::ViewBegin::Kind::Ok)
		{
			texture_random_seed() = s0;
			s_b.clear(0);
			s_c3d_rast.set_target(w, h);
			s_c3d_rast.set_options(cfg.textures, cfg.uv_half);
			FilterRasterizer fb(&s_c3d_rast, c, cfg.fullbright, true, cfg.textures);
			s_recorder.replay(&fb);
			ve = back::view_end(s_b.pixels.data(), s_b.w, &sentinel);
		}
	}
	++s_diff_checks;
	if (vb.kind == back::ViewBegin::Kind::Skipped)
	{
		++s_c3d_skipped;
		printf("[c3d] DIFF frame=%u class=%s skipped (%s)\n", s_frame, cls, back::skip_name(vb.skip));
		return;
	}
	if (vb.kind == back::ViewBegin::Kind::Discarded)
	{
		++s_c3d_discarded;
		printf("[c3d] DIFF frame=%u class=%s discarded (%s)\n", s_frame, cls, back::discard_name(vb.discard));
		return;
	}

	s_c3d_drawn += s_c3d_rast.drawn();
	for (int i = 1; i < 5; ++i)
		s_c3d_accept_skips[i] += s_c3d_rast.skipped(static_cast<Accept>(i));
	if (!ve.ok)
	{
		++s_c3d_discarded;
		printf("[c3d] DIFF frame=%u class=%s discarded (%s)\n", s_frame, cls, back::discard_name(ve.discard));
		return;
	}
	const DiffResult r = diff_images(s_a.image(), s_b.image(), Coverage::TopByte, 0, static_cast<int>(cfg.diff_tol),
	                                s_c3d_rast.tie_mask());
	printf("%s\n", diff_line_c3d(s_frame, cls, r, static_cast<int>(cfg.diff_tol), fa.excluded(), sentinel).c_str());
	++s_c3d_compared;
	s_c3d_ties += r.ties;
	s_c3d_tie_mismatch += r.tie_mismatch;
	s_diff_differ_total += r.differ;
	if (cfg.dump && (first || r.cov_mismatch_nonedge > 0))
		dump_pair(s_frame, cls);
}

// Design 7, M0 "Diff frame". Returns true: the real frame is in dest. False: dropped, nothing written to dest.
bool run_diff_frame(const Config& cfg, DrawFn draw, void* ctx, bitmap_definition* dest, RasterizerClass* sw)
{
#ifdef A1_PROFILE
	A1_PROF_SCOPE(C3dDiff);
#endif
	const bool first = s_diff_done == 0 && s_diff_dropped == 0;
	const int w = dest->width, h = dest->height;
	s_a.make(w, h);
	s_b.make(w, h);
	Rasterizer_SW_Class* swr = static_cast<Rasterizer_SW_Class*>(sw);
	const uint16 s0 = texture_random_seed();

	// (1) the one traversal: SW draws into A while the recorder copies everything.
	s_a.clear(0);
	swr->screen = s_a.get();
	s_recorder.clear();
	s_recorder.forward = sw;
	draw(ctx, &s_recorder);
	s_recorder.forward = nullptr;

	// (2) the replay of everything into B must equal the direct draw in A.
	if (s_recorder.overflowed())
	{
		swr->screen = dest;
		texture_random_seed() = s0;
		++s_diff_dropped;
		printf("[c3d] DIFF frame=%u dropped: the recording overflowed (%lu events)\n", s_frame,
		       static_cast<unsigned long>(Recorder::kCapacity));
		return false;
	}
	texture_random_seed() = s0;
	s_b.clear(0);
	swr->screen = s_b.get();
	s_recorder.replay(sw);
	check(cfg, s_frame, "replay", Coverage::TopByte, 0, first);

	// (3) per selected class: the same filtered replay into A and into B (renderer sw: both SW; renderer c3d:
	// B is a C3D view frame).
	for (int c = 0; c <= kFrame; ++c)
	{
		if (!(cfg.classes & (1u << c)))
			continue;
		if (cfg.renderer_c3d)
		{
			c3d_check(cfg, c, s0, swr, sw, first);
			continue;
		}
		const bool tinted = c == kTinted;
		const uint32_t clear = tinted ? 0xFF808080u : 0u;
		const Coverage cov = (tinted || c == kStatic) ? Coverage::NotClear : Coverage::TopByte;
		for (int side = 0; side < 2; ++side)
		{
			Scratch& target = side == 0 ? s_a : s_b;
			texture_random_seed() = s0;
			target.clear(clear);
			swr->screen = target.get();
			FilterRasterizer filter(sw, c, cfg.fullbright);
			s_recorder.replay(&filter);
		}
		check(cfg, s_frame, class_name(c), cov, clear, first);
	}

	// (4) the real frame: everything, over the previous frame's pixels, as SW draws it.
	texture_random_seed() = s0;
	swr->screen = dest;
	s_recorder.replay(sw);

	++s_diff_done;
	fflush(stdout);
	if (cfg.diff_frames && s_diff_done + s_diff_dropped >= cfg.diff_frames)
		diff_summary();
	return true;
}

// --- null mode ---

// Only from frame_final_stats(): Samples::percentile() sorts a copy of its values, which does not
// belong in a timed render zone. Once.
void log_null_summary()
{
	static bool printed = false;
	if (printed || !s_null_stats || s_null_frames == 0)
		return;
	printed = true;
	const NullStats& n = *s_null_stats;
	printf("[c3d] null frames=%u polys max=%u p99=%u rects max=%u p99=%u vertices max=%u p99=%u "
	       "indices max=%u p99=%u state_changes max=%u p99=%u pairs max=%u p99=%u pair_table_full=%u\n",
	       s_null_frames, n.polys.max(), n.polys.percentile(99), n.rects.max(), n.rects.percentile(99),
	       n.vertices.max(), n.vertices.percentile(99), n.indices.max(), n.indices.percentile(99),
	       n.changes.max(), n.changes.percentile(99), n.pairs.max(), n.pairs.percentile(99), s_pair_table_full);
	fflush(stdout);
}

void run_null(DrawFn draw, void* ctx)
{
	static NullRasterizer rast;
	if (!s_null_stats)
		s_null_stats.reset(new NullStats());
	{
#ifdef A1_PROFILE
		A1_PROF_SCOPE(C3dNull);
#endif
		draw(ctx, &rast);
	}
	const NullCounts& c = rast.counts();
	NullStats& n = *s_null_stats;
	n.polys.add(c.polygons);
	n.rects.add(c.rectangles);
	n.vertices.add(c.vertices);
	n.indices.add(c.indices);
	n.changes.add(c.state_changes);
	n.pairs.add(c.pairs);
	if (c.pair_table_full)
		++s_pair_table_full;
	++s_null_frames;
}

void stats_tick()
{
	const uint64_t now = machine_tick_count();
	if (s_stats_ms == 0)
		s_stats_ms = now;
	if (now - s_stats_ms < 1000)
		return;
	s_stats_ms = now;
	printf("[c3d] frame stats frames=%u diff=%u dropped=%u null=%u cal=%u cal_skipped=%u\n", s_frame, s_diff_done,
	       s_diff_dropped, s_null_frames, s_cal_done, s_cal_skipped);
	fflush(stdout);
}

} // namespace

bool render_frame(view_data& view, bitmap_definition* dest, RasterizerClass* sw, DrawFn draw, void* ctx)
{
	(void)view;
	if (s_active < 0)
	{
		const Config& c = back::config();
		s_active = (c.diff || c.readback_cal || c.renderer_c3d) ? 1 : 0;
	}
	if (!s_active)
		return false;
	const Config& cfg = back::config();

	// Design 1.5: bit_depth, view side. Logged once per change.
	if (bit_depth != 32 || dest->bit_depth != 32 || dest->width > 1024 || dest->height > 1024)
	{
		if (!s_unsupported_logged || s_depth_logged != bit_depth || s_size_logged_w != dest->width ||
		    s_size_logged_h != dest->height)
		{
			s_unsupported_logged = true;
			s_depth_logged = bit_depth;
			s_size_logged_w = dest->width;
			s_size_logged_h = dest->height;
			printf("[c3d] render_frame: bit_depth %d, surface %dx%d at %d bpp is not supported (32 bpp, at most "
			       "1024 per side): software\n", bit_depth, dest->width, dest->height, dest->bit_depth);
		}
		return false;
	}

	++s_frame;
	stats_tick();

	if (cfg.readback_cal)
		run_calibration(cfg, dest->width, dest->height);

	if (cfg.diff && s_frame % cfg.diff == 0 &&
	    (cfg.diff_frames == 0 || s_diff_done + s_diff_dropped < cfg.diff_frames))
		return run_diff_frame(cfg, draw, ctx, dest, sw);

	if (cfg.renderer_c3d)
	{
		if (cfg.mode == Mode::Null)
		{
			run_null(draw, ctx);
			return false;
		}
		if (!s_logged_mode)
		{
			s_logged_mode = true;
			printf("[c3d] renderer c3d: play frames are software before %s\n",
			       cfg.mode == Mode::Direct ? "M6 (mode direct)" : "M4 (mode readback)");
		}
	}
	return false;
}

namespace back {

void frame_final_stats()
{
	log_null_summary();
	if ((s_diff_done || s_diff_dropped || s_diff_checks) && !s_diff_summary_printed)
		diff_summary();
}

} // namespace back

} // namespace a1_c3d
