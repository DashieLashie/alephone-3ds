/*
	Aleph One for the Nintendo 3DS, citro3d back end: a1_c3d::render_frame(), the M0 harness
	(docs/3ds/CITRO3D_DESIGN.md, 1.2, 1.5, 8.1 and section 7, M0 specification):

	  - the readback calibration (first 30 world frames, back/view.cpp)
	  - the diff frame: one traversal recorded while SW draws it into scratch A, every comparison a
	    replay of the recording, the real frame the last replay (into dest)
	  - null mode: a counting rasteriser sees the traversal; the caller then draws in SW
	  - M4 play frames (section 7, M4 specification "Play mode"): the traversal recorded once, scanned, drawn by
	    citro3d and read back into dest; anything C3D cannot draw (or a discard) is the recording replayed in SW
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
#include "../core/shade_lut.h"
#include "../core/stats.h"

#include "a1_3ds_platform.h"
#ifdef A1_PROFILE
#include "a1_3ds_profile.h"
#endif

#include <stdio.h>
#include <string.h>

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
unsigned s_c3d_ambient_wrap = 0;   // M3: polygons excluded because SW's short index wraps (FilterRasterizer)
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
Scratch s_a, s_b, s_c;   // s_c: M5's static coverage (the drop forced to 0)

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
	       "c3d_skipped=%u c3d_drawn=%u c3d_accept_skips=degenerate:%u,bounds:%u,walk:%u,area:%u c3d_ties=%llu c3d_tie_mismatch=%llu "
	       "c3d_span_polygons=%u c3d_span_quads=%u c3d_span_quads_max=%d c3d_span_ring_refused=%u "
	       "c3d_lit_polys=%u c3d_shadeless_polys=%u c3d_neg_ambient_polys=%u c3d_flash_checks=%u c3d_m_max=%ld "
	       "c3d_flash_over=%u c3d_ambient_wrap=%u c3d_rects=%u c3d_tinted_rects=%u c3d_rect_skipped=%u "
	       "c3d_rect_unsupported=%u c3d_land_polys=%u c3d_land_spans=%u c3d_land_unsupported=%u c3d_static_polys=%u "
	       "c3d_static_rects=%u c3d_static_unsupported=%u c3d_solid_skipped=%u\n", s_diff_done,
	       s_diff_checks, static_cast<unsigned long long>(s_diff_differ_total), s_diff_dropped, s_c3d_compared,
	       s_c3d_discarded, s_c3d_skipped, s_c3d_drawn, s_c3d_accept_skips[1], s_c3d_accept_skips[2],
	       s_c3d_accept_skips[3], s_c3d_accept_skips[4], s_c3d_ties, s_c3d_tie_mismatch, s_c3d_rast.span_polygons(),
	       s_c3d_rast.span_quads(), s_c3d_rast.span_quads_max(), s_c3d_rast.span_ring_refused(),
	       s_c3d_rast.lit_polys(), s_c3d_rast.shadeless_polys(), s_c3d_rast.neg_ambient_polys(),
	       s_c3d_rast.flash_checks(), s_c3d_rast.m_max(), s_c3d_rast.flash_over(), s_c3d_ambient_wrap,
	       s_c3d_rast.rects(), s_c3d_rast.tinted_rects(), s_c3d_rast.rect_skipped(), s_c3d_rast.rect_unsupported(),
	       s_c3d_rast.land_polys(), s_c3d_rast.land_spans(), s_c3d_rast.land_unsupported(), s_c3d_rast.static_polys(),
	       s_c3d_rast.static_rects(), s_c3d_rast.static_unsupported(), s_c3d_rast.solid_skipped());
	fflush(stdout);
}

// M5 (design 7, M5 specification "Diff checks"): SW's static coverage C into scratch C: the static class replayed
// in SW with every drop forced to 0, over a clear 0.
void static_cover(const Config& cfg, uint16 s0, Rasterizer_SW_Class* swr, RasterizerClass* sw, int w, int h)
{
	s_c.make(w, h);
	texture_random_seed() = s0;
	s_c.clear(0);
	swr->screen = s_c.get();
	FilterRasterizer fc(sw, kStatic, cfg.fullbright, true, cfg.textures);
	fc.set_static_cover(true);
	s_recorder.replay(&fc);
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

	// M4 (8.1, design 7 "M4 specification", "Diff checks"): tinted draws over a grey clear and compares "pixel !=
	// clear"; the frame compares every pixel either side drew (Coverage::All); the other classes clear to 0 and use
	// the alpha byte.
	const bool tinted = c == kTinted;
	const bool stat = c == kStatic;
	const uint32_t clear_argb = tinted ? 0xFF808080u : 0u;
	const Coverage cov = tinted ? Coverage::NotClear : (c == kFrame ? Coverage::All : Coverage::TopByte);
	// M5: landscape lines at their own tolerance (landscape_tol; 0 = diff_tol).
	const int tol = static_cast<int>(c == kLandscape && cfg.landscape_tol ? cfg.landscape_tol : cfg.diff_tol);
	if (stat)
		static_cover(cfg, s0, swr, sw, w, h);
	texture_random_seed() = s0;
	s_a.clear(clear_argb);
	swr->screen = s_a.get();
	FilterRasterizer fa(sw, c, cfg.fullbright, true, cfg.textures);
	s_recorder.replay(&fa);
	s_c3d_ambient_wrap += fa.ambient_wrap();

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
		vb = back::view_begin(w, h, tinted ? 0x808080FFu : 0u);
		if (vb.kind == back::ViewBegin::Kind::Ok)
		{
			texture_random_seed() = s0;
			s_b.clear(0);
			s_c3d_rast.set_target(w, h);
			s_c3d_rast.set_options(cfg.textures, cfg.uv_half, cfg.lit, cfg.rect_bias);
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
	if (stat)
	{
		// M5: the density of A and of B inside SW's coverage C, B outside it (c3d-diff.py gates the pooled density).
		const StaticResult sr = diff_static(s_a.image(), s_b.image(), s_c.image());
		printf("%s\n", static_line(s_frame, sr, fa.excluded(), sentinel).c_str());
		++s_c3d_compared;
		if (cfg.dump && (first || sr.out_nonedge > 0))
			dump_pair(s_frame, cls);
		return;
	}
	const DiffResult r = diff_images(s_a.image(), s_b.image(), cov, clear_argb, tol, s_c3d_rast.tie_mask());
	printf("%s\n", diff_line_c3d(s_frame, cls, r, tol, fa.excluded(), sentinel).c_str());
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

// --- play mode (design 7, M4 specification "Play mode") ---

enum class ScanReason : int { None = -1, Landscape, Static, Other, Bitmap, Wrap, Flash, kCount };
const char* const kScanNames[] = {"landscape", "static", "other", "bitmap", "wrap", "flash"};
constexpr int kSkipCount = static_cast<int>(back::SkipReason::kCount) + 1;   // + config
constexpr int kSkipConfig = static_cast<int>(back::SkipReason::kCount);
constexpr int kDiscardCount = static_cast<int>(back::DiscardReason::kCount);
constexpr unsigned kOffAfter = 10;

// Replays nothing: finds the first reason C3D cannot draw a recorded frame (step 2), with the same predicates the
// C3D rasterizer applies (c3d_supports, M3's ambient wrap and flash limit, rect_support; M5: landscape_supported,
// static_supported, _solid skipped). Also counts the static primitives (the play check's exclusion).
class PlayScan : public RasterizerClass
{
public:
	ScanReason reason = ScanReason::None;
	unsigned statics = 0;

	void SetView(view_data& View) override { view_ = &View; }
	void texture_horizontal_polygon(polygon_definition& p) override { polygon(Entry::Horizontal, p); }
	void texture_vertical_polygon(polygon_definition& p) override { polygon(Entry::Vertical, p); }
	void texture_rectangle(rectangle_definition& r) override
	{
		statics += r.transfer_mode == _static_transfer;
		if (reason != ScanReason::None)
			return;
		const int32_t M = view_ ? static_cast<int32_t>(view_->maximum_depth_intensity) : 0;
		switch (rect_support(r, M, true))
		{
		case RectSupport::Ok: break;
		case RectSupport::Static: reason = ScanReason::Static; break;
		case RectSupport::Other: reason = ScanReason::Other; break;
		case RectSupport::Bitmap: reason = ScanReason::Bitmap; break;
		case RectSupport::Wrap: reason = ScanReason::Wrap; break;
		}
	}

private:
	void polygon(Entry e, const polygon_definition& p)
	{
		const Transfer t = to_transfer(p.transfer_mode);
		statics += t == Transfer::Static;
		if (reason != ScanReason::None || p.transfer_mode == _solid_transfer)   // M5: _solid, SW draws nothing
			return;
		// M5: landscapes and static polygons are drawn; the names remain for the ones C3D cannot draw (play frames are
		// textures 1 and lit 1).
		if (t == Transfer::BigLandscaped)
		{
			if (!landscape_supported(p, true) || p.vertex_count > MAXIMUM_VERTICES_PER_SCREEN_POLYGON)
				reason = ScanReason::Landscape;
			return;
		}
		if (t == Transfer::Static)
		{
			if (!static_supported(e, p, true, true) || p.vertex_count > MAXIMUM_VERTICES_PER_SCREEN_POLYGON)
				reason = ScanReason::Static;
			return;
		}
		if (t != Transfer::Textured || p.vertex_count > MAXIMUM_VERTICES_PER_SCREEN_POLYGON)
		{
			reason = ScanReason::Other;
			return;
		}
		const bitmap_definition* tex = p.texture;
		const bool shadeless = (p.flags & _SHADELESS_BIT) != 0;
		if (!tex || !c3d_supports(e, t, tex->width, tex->height, tex->bytes_per_row == NONE, shadeless,
		                          (tex->flags & _TRANSPARENT_BIT) != 0, true))
		{
			reason = ScanReason::Bitmap;
			return;
		}
		if (shadeless)
			return;
		if (shade_ambient_wraps(static_cast<int32_t>(p.ambient_shade)))
			reason = ScanReason::Wrap;
		else if (view_ && static_cast<int32_t>(view_->maximum_depth_intensity) > kFlashMax)
			reason = ScanReason::Flash;
	}
	view_data* view_ = nullptr;
};

struct PlayState
{
	unsigned frames = 0, c3d = 0, sw = 0, overflow = 0, off_frames = 0;
	unsigned scan[static_cast<int>(ScanReason::kCount)] = {};
	unsigned discard[kDiscardCount] = {};
	unsigned skip[kSkipCount] = {};
	unsigned run = 0, max_run = 0, offs = 0, ons = 0;
	unsigned long long sentinel = 0, copied = 0;
	unsigned checks = 0, empty_frames = 0;   // empty: C3D play frames that copied no pixel
	unsigned static_frames = 0;              // M5: C3D play frames with static primitives
	unsigned static_checks = 0;              // M5: play checks with SW's static coverage excluded
	unsigned long long static_px = 0;        // M5: the pixels those checks left out
	bool off = false;
	uint32_t off_gen = 0;
	bool logged_first = false;
} s_play;
C3dRasterizer s_play_rast;   // its own counters: the DIFF summary's stay the diff checks'
std::vector<uint8_t> s_play_mask;   // M5: the play check's exclusion (SW's static coverage)

bool resource_discard(back::DiscardReason r)
{
	return r != back::DiscardReason::Unsupported;
}

void play_discard(back::DiscardReason r)
{
	++s_play.discard[static_cast<int>(r)];
	if (!resource_discard(r))
		return;   // content, not trouble: the run is unchanged
	++s_play.run;
	if (s_play.run > s_play.max_run)
		s_play.max_run = s_play.run;
	if (s_play.run >= kOffAfter && !s_play.off)
	{
		s_play.off = true;
		s_play.off_gen = back::texcache().generation();
		++s_play.offs;
		printf("[c3d] play: off until the next level (%u discards in a row, last %s)\n", s_play.run,
		       back::discard_name(r));
		fflush(stdout);
	}
}

int play_counts_text(char* buf, size_t size)
{
	const PlayState& p = s_play;
	return snprintf(buf, size,
	                "frames=%u c3d=%u sw=%u overflow=%u off_frames=%u scan=%u/%u/%u/%u/%u/%u"
	                "(landscape/static/other/bitmap/wrap/flash) discard=%u/%u/%u/%u/%u/%u(unsup/ring/cmdbuf/gx/begin/tex) "
	                "skip=%u/%u/%u/%u/%u(down/gate/size/pref/config) offs=%u ons=%u max_run=%u sentinel=%llu play_checks=%u "
	                "copied=%llu empty_frames=%u static_frames=%u static_checks=%u static_px=%llu",
	                p.frames, p.c3d, p.sw, p.overflow, p.off_frames, p.scan[0], p.scan[1], p.scan[2], p.scan[3], p.scan[4],
	                p.scan[5], p.discard[0], p.discard[1], p.discard[2], p.discard[3], p.discard[4], p.discard[5],
	                p.skip[0], p.skip[1], p.skip[2], p.skip[3], p.skip[4], p.offs, p.ons, p.max_run, p.sentinel, p.checks,
	                p.copied, p.empty_frames, p.static_frames, p.static_checks, p.static_px);
}

// Design 7, M4 "Play mode". True: the frame is in dest (C3D or the SW replay). False: the caller draws it in SW
// (no recording was made).
bool run_play_frame(const Config& cfg, DrawFn draw, void* ctx, bitmap_definition* dest, RasterizerClass* sw)
{
	PlayState& P = s_play;
	++P.frames;
	if (!P.logged_first)
	{
		P.logged_first = true;
		printf("[c3d] renderer c3d: play frames drawn by citro3d (readback mode, M5)\n");
	}
	if (P.off)
	{
		// collections_changed() (hook 2a: every level load) bumps the texture cache's generation.
		if (back::texcache().generation() == P.off_gen)
		{
			++P.off_frames;
			++P.sw;
			return false;
		}
		P.off = false;
		P.run = 0;
		++P.ons;
		printf("[c3d] play: on again (collections changed)\n");
		fflush(stdout);
	}
	// (0) the cheap refusals: SW draws the frame directly.
	const int w = dest->width, h = dest->height;
	back::SkipReason why = back::SkipReason::Down;
	int skip = -1;
	if (!cfg.textures || !cfg.lit)
		skip = kSkipConfig;
	else if (graphics_preferences->software_alpha_blending != _sw_alpha_off)
		skip = static_cast<int>(back::SkipReason::Pref);
	else if (!back::view_play_ready(w, h, &why))
		skip = static_cast<int>(why);
	if (skip >= 0)
	{
		++P.skip[skip];
		++P.sw;
		return false;
	}

	Rasterizer_SW_Class* swr = static_cast<Rasterizer_SW_Class*>(sw);
	const uint16 s0 = texture_random_seed();
	auto sw_replay = [&]() {
		texture_random_seed() = s0;
		swr->screen = dest;
		s_recorder.replay(sw);
		++P.sw;
	};
	back::ViewBegin vb{back::ViewBegin::Kind::Skipped, back::SkipReason::Down, back::DiscardReason::Unsupported};
	back::ViewEnd ve{false, back::DiscardReason::Begin};
	unsigned sentinel = 0, copied = 0, play_statics = 0;
	const bool check = cfg.play_check && (P.c3d + 1) % cfg.play_check == 0;
	{
#ifdef A1_PROFILE
		A1_PROF_SCOPE(C3dDraw);
#endif
		// (1) the one traversal, recorded (C rand() of the weapon flutter is its only side effect, weapons.cpp:2922).
		s_recorder.clear();
		s_recorder.forward = nullptr;
		draw(ctx, &s_recorder);
		if (s_recorder.overflowed())
		{
			++P.overflow;
			texture_random_seed() = s0;
			swr->screen = dest;
			draw(ctx, sw);
			++P.sw;
			return true;
		}
		// (2) the scan.
		PlayScan scan;
		s_recorder.replay(&scan);
		play_statics = scan.statics;
		if (scan.reason != ScanReason::None)
		{
			++P.scan[static_cast<int>(scan.reason)];
			sw_replay();
			return true;
		}
		// play_check: A starts as dest's previous frame, as dest does.
		// Through row_addresses[y], as view_end_play writes (critic M4 code review, minor 8).
		if (check)
		{
			s_a.make(w, h);
			for (int y = 0; y < h; ++y)
				memcpy(&s_a.pixels[static_cast<size_t>(y) * w], dest->row_addresses[y], static_cast<size_t>(w) * 4);
		}
		// (3) the C3D frame, read back into dest.
		vb = back::view_begin(w, h, 0);
		if (vb.kind == back::ViewBegin::Kind::Ok)
		{
			s_play_rast.set_target(w, h);
			s_play_rast.set_options(cfg.textures, cfg.uv_half, cfg.lit, cfg.rect_bias);
			s_recorder.replay(&s_play_rast);
			ve = back::view_end_play(reinterpret_cast<uint8_t* const*>(dest->row_addresses), &sentinel, &copied);
		}
	}
	if (vb.kind == back::ViewBegin::Kind::Skipped)
	{
		++P.skip[static_cast<int>(vb.skip)];
		sw_replay();
		return true;
	}
	if (vb.kind == back::ViewBegin::Kind::Discarded)
	{
		play_discard(vb.discard);
		sw_replay();
		return true;
	}
	if (!ve.ok)
	{
		// (4) a discard: nothing was copied into dest.
		play_discard(ve.discard);
		sw_replay();
		return true;
	}
	++P.c3d;
	P.static_frames += play_statics != 0;
	P.run = 0;
	P.sentinel += sentinel;
	P.copied += copied;
	P.empty_frames += copied == 0;
	if (check)
	{
		// SW's replay over the same previous frame, against what the play path left in dest.
		texture_random_seed() = s0;
		swr->screen = s_a.get();
		s_recorder.replay(sw);
		swr->screen = dest;
		texture_random_seed() = s0;   // as after SW's own traversal: the replay above consumed what it did
		s_b.make(w, h);
		for (int y = 0; y < h; ++y)
			memcpy(&s_b.pixels[static_cast<size_t>(y) * w], dest->row_addresses[y], static_cast<size_t>(w) * 4);
		// M5 (design 7, M5 specification "Diff checks", play): a frame with static is compared without SW's static
		// coverage (the noise differs per pixel by design); the line's `excluded` is then the pixels left out.
		unsigned static_px = 0;
		DiffResult r;
		if (play_statics)
		{
			static_cover(cfg, s0, swr, sw, w, h);
			swr->screen = dest;
			texture_random_seed() = s0;
			s_play_mask.resize(static_cast<size_t>(w) * h);
			for (size_t i = 0; i < s_play_mask.size(); ++i)
				s_play_mask[i] = s_c.pixels[i] != 0;
			r = diff_images_excluding(s_a.image(), s_b.image(), 0, static_cast<int>(cfg.diff_tol), s_play_mask.data(),
			                          &static_px);
			++P.static_checks;
			P.static_px += static_px;
		}
		else
			r = diff_images(s_a.image(), s_b.image(), Coverage::All, 0, static_cast<int>(cfg.diff_tol));
		printf("%s\n", diff_line_c3d(s_frame, "play", r, static_cast<int>(cfg.diff_tol), static_px, sentinel).c_str());
		++P.checks;
	}
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
	if (s_play.frames)
	{
		char buf[768];
		play_counts_text(buf, sizeof buf);
		printf("[c3d] play %s\n", buf);
	}
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
		if (cfg.mode == Mode::Readback && cfg.play)
			return run_play_frame(cfg, draw, ctx, dest, sw);
		if (!s_logged_mode)
		{
			s_logged_mode = true;
			printf("[c3d] renderer c3d: play frames are software (%s)\n",
			       cfg.mode == Mode::Direct ? "mode direct: M6" : "play 0");
		}
	}
	return false;
}

namespace back {

void frame_final_stats()
{
	log_null_summary();
	static bool play_final = false;
	if (s_play.frames && !play_final)
	{
		play_final = true;
		char buf[768];
		play_counts_text(buf, sizeof buf);
		printf("[c3d] play final %s rects=%u tinted_rects=%u rect_skipped=%u rect_unsupported=%u land_polys=%u "
		       "land_spans=%u land_unsupported=%u static_polys=%u static_rects=%u static_unsupported=%u solid_skipped=%u\n",
		       buf, s_play_rast.rects(), s_play_rast.tinted_rects(), s_play_rast.rect_skipped(),
		       s_play_rast.rect_unsupported(), s_play_rast.land_polys(), s_play_rast.land_spans(),
		       s_play_rast.land_unsupported(), s_play_rast.static_polys(), s_play_rast.static_rects(),
		       s_play_rast.static_unsupported(), s_play_rast.solid_skipped());
		fflush(stdout);
	}
	if ((s_diff_done || s_diff_dropped || s_diff_checks) && !s_diff_summary_printed)
		diff_summary();
}

} // namespace back

} // namespace a1_c3d
