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
#include "Crosshairs.h"            // M6 overlays test key
#include "shell.h"                 // screen_printf

#include "filter.h"
#include "c3d_rasterizer.h"
#include "../back/context.h"
#include "../back/draw.h"
#include "../back/view.h"
#include "../back/view_shared.h"
#include "../back/overlay.h"
#include "../back/shade.h"
#include "../back/stereo.h"
#include "../core/bmp.h"
#include "../core/calibrate.h"
#include "../core/classify.h"
#include "../core/config.h"
#include "../core/diff.h"
#include "../core/direct_state.h"
#include "../core/fade_fit.h"
#include "../core/overlay_bands.h"
#include "../core/shade_lut.h"
#include "../core/stereo.h"
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

// RenderOther/screen_shared.h (in screen.cpp's TU, not static): the overlays the M6 test key switches on.
extern bool displaying_fps;
extern bool ShowPosition;
// RenderOther/screen.cpp: the colour curves of the current fade (M6b; read at hook 3a, never written).
extern uint16 current_gamma_r[256];
extern uint16 current_gamma_g[256];
extern uint16 current_gamma_b[256];

namespace a1_c3d {

namespace {

constexpr unsigned kCalFrames = 30;
constexpr unsigned kMaxDumpFiles = 32;

// --- state (main thread only) ---

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
	       "c3d_rect_unsupported=%u c3d_land_polys=%u c3d_land_spans=%u c3d_land_rows=%d-%d c3d_land_unsupported=%u c3d_static_polys=%u "
	       "c3d_static_rects=%u c3d_static_unsupported=%u c3d_solid_skipped=%u\n", s_diff_done,
	       s_diff_checks, static_cast<unsigned long long>(s_diff_differ_total), s_diff_dropped, s_c3d_compared,
	       s_c3d_discarded, s_c3d_skipped, s_c3d_drawn, s_c3d_accept_skips[1], s_c3d_accept_skips[2],
	       s_c3d_accept_skips[3], s_c3d_accept_skips[4], s_c3d_ties, s_c3d_tie_mismatch, s_c3d_rast.span_polygons(),
	       s_c3d_rast.span_quads(), s_c3d_rast.span_quads_max(), s_c3d_rast.span_ring_refused(),
	       s_c3d_rast.lit_polys(), s_c3d_rast.shadeless_polys(), s_c3d_rast.neg_ambient_polys(),
	       s_c3d_rast.flash_checks(), s_c3d_rast.m_max(), s_c3d_rast.flash_over(), s_c3d_ambient_wrap,
	       s_c3d_rast.rects(), s_c3d_rast.tinted_rects(), s_c3d_rast.rect_skipped(), s_c3d_rast.rect_unsupported(),
	       s_c3d_rast.land_polys(), s_c3d_rast.land_spans(), s_c3d_rast.land_row_min(), s_c3d_rast.land_row_max(),
	       s_c3d_rast.land_unsupported(), s_c3d_rast.static_polys(),
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
constexpr int kSkipCount = static_cast<int>(back::SkipReason::kCount) + 2;   // + config, stereo_size
constexpr int kSkipConfig = static_cast<int>(back::SkipReason::kCount);
constexpr int kSkipStereoSize = kSkipConfig + 1;   // M9: stereo allocated and the view does not fit the 512x256 textures
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
			if (!landscape_supported(p, view_, true) || p.vertex_count > MAXIMUM_VERTICES_PER_SCREEN_POLYGON)
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

// --- M6 direct mode (design 7, M6 specification) ---

// Why a frame in mode direct was not direct: the policy's blocks (kDirectBlockCount), then the front end's own.
enum FrontReason { kReasonPlay = kDirectBlockCount, kReasonOverlay, kReasonPolicy, kReasonCount };
const char* reason_name(int r)
{
	static const char* const extra[] = {"play", "overlay", "no_policy"};
	if (r < kDirectBlockCount)
		return direct_block_name(1u << r);
	return extra[r - kDirectBlockCount];
}

struct DirectFront
{
	unsigned policy = 0;
	bool policy_set = false;
	bool overlay_ok = false;
	bool this_frame = false;          // direct_frame(): this frame's world view is on the GPU
	DirectState st;
	const void* dest0 = nullptr;
	int dw = 0, dh = 0;
	unsigned frames = 0, direct = 0, readback = 0, sw = 0, cal = 0;
	unsigned reason[kReasonCount] = {};
	unsigned policy_calls = 0, policy_blocked[kDirectBlockCount] = {};   // every frame hook 3a saw (map and terminal
	                                                                      // frames never reach render_frame)
	unsigned seeds = 0, syncs = 0, sync_failed = 0, discard_stale = 0, forced_sw = 0, new_dest = 0;
	unsigned fallback_sw = 0;         // M8: ineligible frames drawn by SW (fallback sw), not read back
	unsigned switch_off = 0;          // M8: the renderer was switched off after frames of mode direct
	bool in_mode = false;             // the last play frame was one of mode direct (direct_leave)
	unsigned sync_skipped[static_cast<int>(SyncSkip::kCount)] = {};
	bool seed_checked = false;
	int last_mode = -2;               // -1 direct, else a reason; for the mode-change lines
	unsigned mode_lines = 0, mode_changes = 0;
	bool overlays_done = false;
	uint64_t last_msg_ms = 0;
	// M6b (design 7, M6b specification "Counters"): this frame's fit kind (None: no fade quad), direct frames with a fade
	// quad, frames whose curves did not fit (they stay blocked=gamma), the worst error among accepted fits, distinct
	// curve sets fitted, "[c3d] fade:" lines printed and dropped (after 200), and whether a fade is showing.
	FadeKind fade_this = FadeKind::None;
	unsigned fade_affine = 0, fade_fold = 0, fade_static = 0, fade_nofit = 0, fade_err = 0, fade_changes = 0;
	unsigned fade_lines = 0, fade_lines_dropped = 0, fade_nonoise = 0;
	uint64_t fade_fit_ticks = 0, fade_fit_max = 0;   // A1_PROFILE: the fits' time (svcGetSystemTick)
	bool fade_seen = false;
	FadeKind fade_last_kind = FadeKind::None;   // the kind of the last fit logged
} s_d;
FadeCache s_fade_cache;

// M9 (design 7, "M9 specification", "The ramp", "Counters and log lines"). In this commit the slider is not read (part 2 wires
// it): the strength stays 0, so a stereo frame (stereo_force) is the full path at zero parallax, which is T2.
constexpr bool kStereoSliderWired = false;
struct StereoFront
{
	StereoRamp ramp;
	bool seen = false;               // a stereo-capable direct frame ran since the last hook 3a (s_stereo_seen)
	bool prev = false, cur = false;  // the previous / this frame (hook 3a to hook 3a) ran as a stereo frame: the right clear
	unsigned stereo = 0, mono = 0, not3d = 0, ramp_resets = 0, discards = 0;
	float p_now = 0.0f, p_max = 0.0f, P_now = 0.0f, slider = 0.0f;
	int last = -1;                   // 1 stereo, 0 mono, for the change lines
	unsigned lines = 0, changes = 0;
} s_s;

// Every change between stereo and mono direct frames, once (at most 20 lines, then counted), as the mode changes.
void note_stereo(bool stereo)
{
	const int now = stereo ? 1 : 0;
	if (now == s_s.last)
		return;
	s_s.last = now;
	++s_s.changes;
	if (s_s.lines < 20)
	{
		++s_s.lines;
		printf("[c3d] stereo: frame %u %s (p %.3f)\n", s_frame, stereo ? "stereo" : "mono", s_s.p_now);
	}
}

void stereo_abort()
{
	if (s_s.ramp.p > 0.0f)
		++s_s.ramp_resets;
	stereo_reset(s_s.ramp);
	back::stereo_frame_set(false);
}

int stereo_counts_text(char* buf, size_t size)
{
	const int n = snprintf(buf, size,
	                       "stereo=%u mono=%u p=%.3f pmax=%.3f P=%.2f slider=%.2f not3d=%u ramp_resets=%u off=%s fill=%d ",
	                       s_s.stereo, s_s.mono, static_cast<double>(s_s.p_now), static_cast<double>(s_s.p_max),
	                       static_cast<double>(s_s.P_now), static_cast<double>(s_s.slider), s_s.not3d, s_s.ramp_resets,
	                       back::stereo_off_name(back::stereo_off_reason()), back::config().stereo_fill ? 1 : 0);
	if (n > 0 && n < static_cast<int>(size))
		back::draw_stereo_text(buf + n, size - n);
	return n;
}

// The kind of frame play mode makes this frame, logged on a change (at most 20 lines, then counted).
void note_mode(int mode)
{
	if (mode == s_d.last_mode)
		return;
	s_d.last_mode = mode;
	++s_d.mode_changes;
	if (s_d.mode_lines < 20)
	{
		++s_d.mode_lines;
		printf("[c3d] direct: frame %u %s%s\n", s_frame, mode < 0 ? "direct" : "readback/sw: ", mode < 0 ? "" : reason_name(mode));
	}
}

C3dAvail c3d_avail()
{
	if (!back::is_up() || back::release_pending())
		return C3dAvail::Off;
	if (back::gate_closed())
		return C3dAvail::Gate;
	return C3dAvail::Ok;
}

// Before a frame SW draws into dest in mode direct: the sync of 5.x's rules (the texture holds a newer view). Then the
// state says SW has drawn (direct_after(Sw)).
void direct_sw_frame(bitmap_definition* dest, ViewKind kind = ViewKind::Sw)
{
	const DirectPlan plan = direct_plan(s_d.st, kind, s_play.off ? C3dAvail::Off : c3d_avail());
	if (plan.sync)
	{
		if (back::view_sync(reinterpret_cast<uint8_t* const*>(dest->row_addresses)).ok)
			++s_d.syncs;
		else
			++s_d.sync_failed;
	}
	else if (plan.skip != SyncSkip::None)
		++s_d.sync_skipped[static_cast<int>(plan.skip)];
	direct_after(s_d.st, kind, false);
}

// M8: the renderer was switched off (the Renderer preference) after frames of mode direct. SW draws the next frame into
// dest, which is stale where the last frame was direct: the sync first, as before any SW frame of mode direct. Only a world
// frame calls this: off and on again with no world frame between leaves in_mode set and counts no switch_off, which is
// still right (the texture does hold the newest view). The view texture and the overlay surface stay allocated.
void direct_leave(bitmap_definition* dest)
{
	if (!s_d.in_mode)
		return;
	s_d.in_mode = false;
	++s_d.switch_off;
	if (dest->row_addresses[0] != s_d.dest0 || dest->width != s_d.dw || dest->height != s_d.dh)
	{
		direct_forget_texture(s_d.st);   // a new dest: the texture's view is not its picture, and the sizes may differ
		s_d.st.dest_view = true;
		return;
	}
	direct_sw_frame(dest);
}

// M8: the Renderer preference, applied before each frame's first decision (hook 3a, then render_frame).
const Config& frame_config()
{
	back::config_preference(graphics_preferences->c3d_renderer != 0);
	return back::config();
}

int direct_counts_text(char* buf, size_t size)
{
	int n = snprintf(buf, size,
	                 "frames=%u direct=%u readback=%u sw=%u cal=%u seeds=%u syncs=%u sync_failed=%u "
	                 "sync_skipped=%u/%u/%u(gate/off/discard) discard_stale=%u forced_sw=%u fallback_sw=%u switch_off=%u "
	                 "new_dest=%u mode_changes=%u why=",
	                 s_d.frames, s_d.direct, s_d.readback, s_d.sw, s_d.cal, s_d.seeds, s_d.syncs, s_d.sync_failed,
	                 s_d.sync_skipped[1], s_d.sync_skipped[2], s_d.sync_skipped[3], s_d.discard_stale, s_d.forced_sw,
	                 s_d.fallback_sw, s_d.switch_off, s_d.new_dest, s_d.mode_changes);
	bool any = false;
	for (int r = 0; r < kReasonCount && n > 0 && n < static_cast<int>(size); ++r)
		if (s_d.reason[r])
		{
			n += snprintf(buf + n, size - n, "%s%s:%u", any ? "," : "", reason_name(r), s_d.reason[r]);
			any = true;
		}
	if (!any && n > 0 && n < static_cast<int>(size))
		n += snprintf(buf + n, size - n, "-");
	if (n > 0 && n < static_cast<int>(size))
		n += snprintf(buf + n, size - n, " policy=%u blocked=", s_d.policy_calls);
	any = false;
	for (int r = 0; r < kDirectBlockCount && n > 0 && n < static_cast<int>(size); ++r)
		if (s_d.policy_blocked[r])
		{
			n += snprintf(buf + n, size - n, "%s%s:%u", any ? "," : "", direct_block_name(1u << r), s_d.policy_blocked[r]);
			any = true;
		}
	if (!any && n > 0 && n < static_cast<int>(size))
		n += snprintf(buf + n, size - n, "-");
	if (n > 0 && n < static_cast<int>(size))
		n += snprintf(buf + n, size - n,
		              " fade_affine=%u fade_fold=%u fade_static=%u fade_nofit=%u fade_nonoise=%u fade_err=%u fade_changes=%u "
		              "fade_lines_dropped=%u fade_fit_ms=%.3f/%.3f ",
		              s_d.fade_affine, s_d.fade_fold, s_d.fade_static, s_d.fade_nofit, s_d.fade_nonoise, s_d.fade_err,
		              s_d.fade_changes, s_d.fade_lines_dropped,
		              s_d.fade_changes ? s_d.fade_fit_ticks / 268111.856 / s_d.fade_changes : 0.0,
		              s_d.fade_fit_max / 268111.856);
	if (n > 0 && n < static_cast<int>(size))
		n += back::overlay_stats_text(buf + n, static_cast<unsigned>(size - n));
	return n;
}

// M6b: "[c3d] fade: curves changed, fit ...". The steps of a fade share 200 lines, then are counted; its first fit (after
// the identity), a change of kind and the return to the identity (`event`) always print, up to 2000 lines, so a test
// log shows every fade that ran.
void fade_log(const char* what, const FadeFit* fit, bool event)
{
	if (s_d.fade_lines >= (event ? 2000u : 200u))
	{
		++s_d.fade_lines_dropped;
		return;
	}
	++s_d.fade_lines;
	if (fit && fit->kind == FadeKind::Fold)
		printf("[c3d] fade: curves changed, fit %s A=%u,%u,%u B=%u,%u,%u s1=%u%u%u K1=%u,%u,%u s2=%u%u%u K2=%u,%u,%u "
		       "trust=%u,%u,%u wrap=%u,%u,%u err=%u frame=%u\n", what, fit->ch[0].a8, fit->ch[1].a8, fit->ch[2].a8,
		       fit->ch[0].b8, fit->ch[1].b8, fit->ch[2].b8, fit->ch[0].s1, fit->ch[1].s1, fit->ch[2].s1, fit->ch[0].k1,
		       fit->ch[1].k1, fit->ch[2].k1, fit->ch[0].s2, fit->ch[1].s2, fit->ch[2].s2, fit->ch[0].k2, fit->ch[1].k2,
		       fit->ch[2].k2, fit->trust[0], fit->trust[1], fit->trust[2], fit->wrap_from[0], fit->wrap_from[1],
		       fit->wrap_from[2], fit->err, s_frame);
	else if (fit && fit->kind == FadeKind::Static)
		printf("[c3d] fade: curves changed, fit %s mask=%u A=%u,%u,%u B=%u,%u,%u frame=%u\n", what, fit->mask,
		       fit->ch[0].a8, fit->ch[1].a8, fit->ch[2].a8, fit->ch[0].b8, fit->ch[1].b8, fit->ch[2].b8, s_frame);
	else if (fit && fit->kind == FadeKind::None)
		printf("[c3d] fade: curves changed, fit %s err=%u first=%u,%u,%u/%u,%u,%u/%u,%u,%u frame=%u\n", what, fit->err,
		       current_gamma_r[0] >> 8, current_gamma_r[128] >> 8, current_gamma_r[255] >> 8, current_gamma_g[0] >> 8,
		       current_gamma_g[128] >> 8, current_gamma_g[255] >> 8, current_gamma_b[0] >> 8, current_gamma_b[128] >> 8,
		       current_gamma_b[255] >> 8, s_frame);
	else if (fit)
		printf("[c3d] fade: curves changed, fit %s A=%u,%u,%u B=%u,%u,%u err=%u frame=%u\n", what, fit->ch[0].a8,
		       fit->ch[1].a8, fit->ch[2].a8, fit->ch[0].b8, fit->ch[1].b8, fit->ch[2].b8, fit->err, s_frame);
	else
		printf("[c3d] fade: curves changed, fit %s frame=%u\n", what, s_frame);
}

// M6b (design 7, M6b specification "The mechanism"), at hook 3a after the frame's fade state was reset: with fades 1,
// renderer c3d, mode direct and kDirectGamma the only block, the engine's three curves are fitted (core/fade_fit.h,
// cached); a fit clears the bit and hands the fit and its curves to this frame's composite. Returns the mask to store.
unsigned fade_policy(const Config& cfg, unsigned blocks)
{
	// Citro3d must be up (critic, M6b code review minor 2): after a release noise_get would build a texture nobody frees.
	if (!cfg.fades || !cfg.renderer_c3d || cfg.mode != Mode::Direct || !back::is_up())
		return blocks;
	if (!(blocks & kDirectGamma))
	{
		if (s_d.fade_seen)
		{
			// The curves are back to the identity (using_default_gamma): visible in the log, and the next fade is
			// logged even if its curves equal the last fitted ones.
			s_d.fade_seen = false;
			s_fade_cache.invalidate();
			s_d.fade_last_kind = FadeKind::None;
			fade_log("identity", nullptr, true);
		}
		return blocks;
	}
	if (blocks != kDirectGamma)
		return blocks;
	bool changed = false;
#ifdef A1_PROFILE
	const uint64_t t0 = a1_3ds::prof::ticks();
#endif
	const FadeFit& fit = s_fade_cache.get(current_gamma_r, current_gamma_g, current_gamma_b, cfg.fade_tol,
	                                      cfg.fade_parts, &changed);
	if (changed)
	{
#ifdef A1_PROFILE
		const uint64_t dt = a1_3ds::prof::ticks() - t0;
		s_d.fade_fit_ticks += dt;
		if (dt > s_d.fade_fit_max)
			s_d.fade_fit_max = dt;
#endif
		++s_d.fade_changes;
		fade_log(fade_kind_name(fit.kind), &fit, !s_d.fade_seen || fit.kind != s_d.fade_last_kind);
		s_d.fade_last_kind = fit.kind;
	}
	s_d.fade_seen = true;
	if (fit.kind == FadeKind::None)
	{
		++s_d.fade_nofit;
		return blocks;
	}
	// Part 3 needs the noise texture: fetched here, outside any frame (round 3, minor 7: resident_build is a CPU fill and
	// a flush); without linear memory for it the frame falls back instead of going direct with no noise.
	if (fit.kind == FadeKind::Static && !back::noise_get())
	{
		++s_d.fade_nonoise;
		return blocks;
	}
	if (fit.kind != FadeKind::Static && fit.err > s_d.fade_err)
		s_d.fade_err = fit.err;
	FadeFrame frame;
	frame.fit = fit;
	const uint16_t* curves[3] = {current_gamma_r, current_gamma_g, current_gamma_b};
	for (int ch = 0; ch < 3; ++ch)
	{
		fade_reference(curves[ch], fit, ch, frame.ref[ch]);
		for (int i = 0; i < kFadeEntries; ++i)
			frame.sw[ch][i] = static_cast<uint8_t>(curves[ch][i] >> 8);
	}
	back::fade_frame_set(&frame);
	s_d.fade_this = fit.kind;
	return 0;
}

// The overlays test key (M6 specification "Test-only keys"): the engine's own switches, once; a message every 2 s.
void overlays_tick(const Config& cfg)
{
	if (!cfg.overlays)
		return;
	if (!s_d.overlays_done)
	{
		s_d.overlays_done = true;
		displaying_fps = cfg.overlays == 1;
		ShowPosition = true;
		Crosshairs_SetActive(true);
		printf("[c3d] overlays %u: fps %d, position 1, crosshair 1, messages %d\n", cfg.overlays, displaying_fps ? 1 : 0,
		       cfg.overlays == 1 ? 1 : 0);
	}
	const uint64_t now = machine_tick_count();
	if (cfg.overlays == 1 && now - s_d.last_msg_ms >= 2000)
	{
		s_d.last_msg_ms = now;
		screen_printf("c3d overlay test");
	}
}
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
	                "skip=%u/%u/%u/%u/%u/%u(down/gate/size/pref/config/stereo_size) offs=%u ons=%u max_run=%u sentinel=%llu play_checks=%u "
	                "copied=%llu empty_frames=%u static_frames=%u static_checks=%u static_px=%llu",
	                p.frames, p.c3d, p.sw, p.overflow, p.off_frames, p.scan[0], p.scan[1], p.scan[2], p.scan[3], p.scan[4],
	                p.scan[5], p.discard[0], p.discard[1], p.discard[2], p.discard[3], p.discard[4], p.discard[5],
	                p.skip[0], p.skip[1], p.skip[2], p.skip[3], p.skip[4], p.skip[5], p.offs, p.ons, p.max_run, p.sentinel, p.checks,
	                p.copied, p.empty_frames, p.static_frames, p.static_checks, p.static_px);
}

// Design 7, M4 "Play mode". True: the frame is in dest (C3D or the SW replay) or, M6, on the GPU (direct_frame()).
// False: the caller draws it in SW (no recording was made).
bool run_play_frame(const Config& cfg, DrawFn draw, void* ctx, bitmap_definition* dest, RasterizerClass* sw)
{
	PlayState& P = s_play;
	++P.frames;
	if (!P.logged_first)
	{
		P.logged_first = true;
		printf("[c3d] renderer c3d: play frames drawn by citro3d (%s mode, M6)\n",
		       cfg.mode == Mode::Direct ? "direct" : "readback");
	}
	const int w = dest->width, h = dest->height;
	// M6: mode direct with its texture target. The view state follows dest: a new dest holds zeros, not the last view.
	const bool dmode = cfg.mode == Mode::Direct && back::view_direct_ok();
	if (dmode)
	{
		++s_d.frames;
		s_d.in_mode = true;
		if (dest->row_addresses[0] != s_d.dest0 || w != s_d.dw || h != s_d.dh)
		{
			s_d.dest0 = dest->row_addresses[0];
			s_d.dw = w;
			s_d.dh = h;
			direct_forget_texture(s_d.st);
			s_d.st.dest_view = true;
			++s_d.new_dest;
		}
	}
	// A frame SW draws into dest (return false: the caller draws it): in mode direct the sync first and the state after.
	auto sw_frame = [&](int reason) {
		if (dmode)
		{
			direct_sw_frame(dest);
			++s_d.sw;
			++s_d.reason[reason];
			note_mode(reason);
		}
	};
	if (P.off)
	{
		// collections_changed() (hook 2a: every level load) bumps the texture cache's generation.
		if (back::texcache().generation() == P.off_gen)
		{
			++P.off_frames;
			++P.sw;
			sw_frame(kReasonPlay);
			return false;
		}
		P.off = false;
		P.run = 0;
		++P.ons;
		printf("[c3d] play: on again (collections changed)\n");
		fflush(stdout);
	}
	// (0) the cheap refusals: SW draws the frame directly.
	back::SkipReason why = back::SkipReason::Down;
	int skip = -1;
	if (!cfg.textures || !cfg.lit)
		skip = kSkipConfig;
	else if (graphics_preferences->software_alpha_blending != _sw_alpha_off)
		skip = static_cast<int>(back::SkipReason::Pref);
	else if (cfg.fallback_sw && cfg.mode == Mode::Direct && !back::view_direct_ok())
		skip = kSkipConfig;   // M8: no texture target, so no direct frame: SW rather than readback
	else if (dmode && back::stereo_objects_wanted() && (w > 512 || h > 256))
		skip = kSkipStereoSize;   // M9: the textures are never deleted for a size change; SW draws what does not fit them
	else if (!back::view_play_ready(w, h, &why))
		skip = static_cast<int>(why);
	const bool forced_sw = dmode && cfg.sw_every && P.frames % cfg.sw_every == 0;
	if (skip >= 0 || forced_sw)
	{
		if (skip >= 0)
			++P.skip[skip];
		else
			++s_d.forced_sw;
		++P.sw;
		sw_frame(kReasonPlay);
		return false;
	}

	// M6: direct when the engine's policy allows it (hook 3a) and the overlay surface exists; else readback.
	int not_direct = -1;
	if (dmode)
	{
		if (!s_d.policy_set)
			not_direct = kReasonPolicy;
		else if (s_d.policy)
			not_direct = direct_block_index(s_d.policy);
		else if (!s_d.overlay_ok)
			not_direct = kReasonOverlay;
	}
	// M8: fallback sw, the Renderer preference's choice. A frame that may not be direct (Classic's not_native, a fade, a
	// non-default Brightness...) costs more read back than drawn by SW (G2-b), so SW draws it.
	if (dmode && not_direct >= 0 && cfg.fallback_sw)
	{
		++s_d.fallback_sw;
		++P.sw;
		sw_frame(not_direct);
		return false;
	}
	const bool direct = dmode && not_direct < 0;
	const ViewKind kind = direct ? ViewKind::Direct : ViewKind::Readback;

	// M9: the ramp, once per frame decided direct. Capable: stereo enabled and usable, the right screen target exists, not
	// a calibration or the 1024x512 probe. A stereo frame: p > 0, or stereo_force (the full path even at strength 0).
	bool stereo_capable = false, stereo_frame = false;
	float stereo_p = 0.0f;
	if (direct)
	{
		stereo_capable = cfg.stereo && back::stereo_usable() && back::present_right_ready() && !cfg.calibrate && !cfg.g3a;
		s_s.slider = kStereoSliderWired && stereo_capable ? back::stereo_slider(cfg) : 0.0f;
		stereo_p = stereo_step(s_s.ramp, stereo_capable, s_s.slider, cfg.stereo_ramp);
		stereo_frame = stereo_capable && (stereo_p > 0.0f || cfg.stereo_force);
		if (stereo_frame && !back::stereo_is_3d())
		{
			// In 2D mode the right framebuffer is the left one: a right-target transfer would overwrite the left image.
			++s_s.not3d;
			stereo_frame = false;
			stereo_abort();
			stereo_p = 0.0f;
		}
	}

	Rasterizer_SW_Class* swr = static_cast<Rasterizer_SW_Class*>(sw);
	const uint16 s0 = texture_random_seed();
	auto sw_replay = [&]() {
		texture_random_seed() = s0;
		swr->screen = dest;
		s_recorder.replay(sw);
		++P.sw;
	};
	// A frame whose C3D side failed after the recording, redrawn by SW into dest: in mode direct dest was stale where the
	// last frame was direct (counted: only uncovered pixels can differ), and the texture holds a partial frame.
	auto discarded_replay = [&](bool c3d_drew) {
		if (dmode)
		{
			if (!s_d.st.dest_view)
				++s_d.discard_stale;
			if (c3d_drew)
				direct_after(s_d.st, kind, true);
			else
				direct_sw_frame(dest);
			++s_d.sw;
			++s_d.reason[kReasonPlay];
			note_mode(kReasonPlay);
		}
		sw_replay();
	};
	back::ViewBegin vb{back::ViewBegin::Kind::Skipped, back::SkipReason::Down, back::DiscardReason::Unsupported};
	back::ViewEnd ve{false, back::DiscardReason::Begin};
	unsigned sentinel = 0, copied = 0, play_statics = 0;
	const bool check = cfg.play_check && kind == ViewKind::Readback && (P.c3d + 1) % cfg.play_check == 0;
	bool seeded = false;
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
			if (dmode)
				sw_frame(kReasonPlay);
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
			if (dmode)
				sw_frame(kReasonPlay);
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
		// (3) the C3D frame: M5's readback (cleared, covered pixels copied); M6's direct (no clear, no readback) or mode
		// direct's readback (no clear, every pixel copied), each seeded first when the texture lost the view.
		back::ViewOptions opt;
		if (dmode)
		{
			const DirectPlan plan = direct_plan(s_d.st, kind, C3dAvail::Ok);
			opt.clear = false;
			if (plan.seed)
			{
				opt.seed_rows = reinterpret_cast<const uint8_t* const*>(dest->row_addresses);
				seeded = true;
			}
			opt.sentinel = false;   // mode direct's readback copies every pixel and checks no sentinel (critic, M6 code minor 1)
			opt.stereo = stereo_frame;
			opt.P = stereo_pixels(stereo_p, cfg.stereo_k);
			opt.w0 = static_cast<float>(cfg.stereo_w0);
			opt.near = static_cast<float>(cfg.stereo_near) / 100.0f;
			opt.fill = cfg.stereo_fill;
			opt.clear_right = !s_s.prev;
		}
		if (direct && cfg.calibrate)
		{
			// The frame-indexed test (M6 specification "Calibration"): no world, the present that will show it.
			if (back::view_direct_calibrate(back::present_counter() + 1, w, h))
			{
				++s_d.cal;
				vb.kind = back::ViewBegin::Kind::Ok;
				ve.ok = true;
			}
		}
		else
		{
			vb = back::view_begin(w, h, 0, opt);
			if (vb.kind == back::ViewBegin::Kind::Ok && stereo_frame && !vb.stereo)
			{
				// The right-eye objects could not be made (stereo is off for the run): this frame is flat.
				stereo_frame = false;
				stereo_abort();
			}
			if (vb.kind == back::ViewBegin::Kind::Ok)
			{
				s_play_rast.set_target(w, h);
				s_play_rast.set_options(cfg.textures, cfg.uv_half, cfg.lit, cfg.rect_bias);
				s_recorder.replay(&s_play_rast);
				uint8_t* const* rows = reinterpret_cast<uint8_t* const*>(dest->row_addresses);
				if (direct)
					ve = back::view_end_direct();
				else if (dmode)
					ve = back::view_end_play_full(rows, &sentinel, &copied);
				else
					ve = back::view_end_play(rows, &sentinel, &copied);
			}
		}
	}
	if (vb.kind == back::ViewBegin::Kind::Skipped)
	{
		if (stereo_capable)
			stereo_abort();
		++P.skip[static_cast<int>(vb.skip)];
		discarded_replay(false);
		return true;
	}
	if (vb.kind == back::ViewBegin::Kind::Discarded)
	{
		if (stereo_capable)
			stereo_abort();
		play_discard(vb.discard);
		discarded_replay(true);
		return true;
	}
	if (!ve.ok)
	{
		// (4) a discard: nothing was copied into dest. M9: a discarded frame resets the ramp (a replay discard counts apart).
		if (stereo_capable)
		{
			stereo_abort();
			s_s.discards += stereo_frame;
		}
		play_discard(ve.discard);
		discarded_replay(true);
		return true;
	}
	++P.c3d;
	P.static_frames += play_statics != 0;
	P.run = 0;
	P.sentinel += sentinel;
	P.copied += copied;
	P.empty_frames += copied == 0 && !direct;
	if (dmode)
	{
		s_d.seeds += seeded;
		direct_after(s_d.st, kind, false);
		if (direct)
		{
			++s_d.direct;
			s_d.fade_affine += s_d.fade_this == FadeKind::Affine;
			s_d.fade_fold += s_d.fade_this == FadeKind::Fold;
			s_d.fade_static += s_d.fade_this == FadeKind::Static;
			s_d.this_frame = true;
			note_mode(-1);
			// M9: the ramp's bookkeeping for a direct frame that was drawn; the composite draws two eyes iff stereo.
			s_s.seen = s_s.seen || stereo_capable;
			s_s.p_now = stereo_frame ? stereo_p : 0.0f;
			s_s.P_now = stereo_frame ? stereo_pixels(stereo_p, cfg.stereo_k) : 0.0f;
			if (s_s.p_now > s_s.p_max)
				s_s.p_max = s_s.p_now;
			if (stereo_frame)
			{
				++s_s.stereo;
				s_s.cur = true;
				back::stereo_frame_set(true);
			}
			else
				++s_s.mono;
			note_stereo(stereo_frame);
			if (cfg.calibrate)
				direct_forget_texture(s_d.st);   // it holds the calibration picture, not the view (critic, M6 code minor 5)
		}
		else
		{
			++s_d.readback;
			++s_d.reason[not_direct];
			note_mode(not_direct);
		}
		if (seeded && cfg.seed_check && !s_d.seed_checked)
		{
			// The seed's orientation against the readback's (M6 specification): dest seeded and read straight back.
			// Before the composite reads the texture: the check's frame re-seeds it from the same dest, then the view
			// is drawn again by the next frame; this frame's direct picture is the check's (dest + nothing drawn).
			s_d.seed_checked = true;
			unsigned differ = 0;
			if (back::view_seed_check(reinterpret_cast<const uint8_t* const*>(dest->row_addresses), &differ))
				printf("[c3d] seed_check: %u of %d pixels differ after seed + readback\n", differ, w * h);
			else
				printf("[c3d] seed_check: not run\n");
			direct_forget_texture(s_d.st);   // the texture now holds dest, not this frame's view
			s_d.this_frame = false;           // so this frame goes the readback way: dest must hold it
			if (direct)
			{
				// The world is in the texture no more: redraw it in SW into dest (the view this frame shows), and count the
				// frame as SW (critic, M6 code minor 5).
				sw_replay();
				direct_after(s_d.st, ViewKind::Sw, false);
				--s_d.direct;
				s_d.fade_affine -= s_d.fade_this == FadeKind::Affine;
				s_d.fade_fold -= s_d.fade_this == FadeKind::Fold;
				s_d.fade_static -= s_d.fade_this == FadeKind::Static;
				--P.c3d;
				// M9: a stereo decision turned into SW: the ramp restarts and the handed-over flag goes.
				s_s.cur = false;
				if (stereo_frame)
					--s_s.stereo;
				else
					--s_s.mono;
				stereo_abort();
				s_s.seen = false;
				++s_d.sw;
				++s_d.reason[kReasonPlay];
				note_mode(kReasonPlay);
			}
		}
	}
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
	if (back::config().quiet)
		return;
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
		char buf[1024];
		play_counts_text(buf, sizeof buf);
		printf("[c3d] play %s\n", buf);
	}
	if (s_d.frames || s_d.policy_calls)
	{
		char buf[1024];
		direct_counts_text(buf, sizeof buf);
		printf("[c3d] direct %s\n", buf);
		if (s_s.stereo + s_s.mono)
		{
			stereo_counts_text(buf, sizeof buf);
			printf("[c3d] stereo: %s\n", buf);
		}
	}
	fflush(stdout);
}

} // namespace

bool render_frame(view_data& view, bitmap_definition* dest, RasterizerClass* sw, DrawFn draw, void* ctx)
{
	(void)view;
	const Config& cfg = frame_config();
	if (!cfg.diff && !cfg.readback_cal && !cfg.renderer_c3d)
	{
		direct_leave(dest);
		return false;
	}

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

	overlays_tick(cfg);
	const bool dmode = cfg.renderer_c3d && cfg.mode == Mode::Direct && back::view_direct_ok();
	if (cfg.readback_cal)
	{
		const unsigned before = s_cal_done + s_cal_skipped;
		run_calibration(cfg, dest->width, dest->height);
		if (dmode && s_cal_done + s_cal_skipped != before)
			direct_forget_texture(s_d.st);   // a calibration frame ran: it cleared the texture target
	}

	if (cfg.diff && s_frame % cfg.diff == 0 &&
	    (cfg.diff_frames == 0 || s_diff_done + s_diff_dropped < cfg.diff_frames))
	{
		if (dmode)
			direct_sw_frame(dest, ViewKind::Clearing);   // the real frame is SW's replay into dest; the target is cleared
		return run_diff_frame(cfg, draw, ctx, dest, sw);
	}

	if (cfg.renderer_c3d)
	{
		if (cfg.mode == Mode::Null)
		{
			run_null(draw, ctx);
			return false;
		}
		if ((cfg.mode == Mode::Readback || cfg.mode == Mode::Direct) && cfg.play)
			return run_play_frame(cfg, draw, ctx, dest, sw);
		if (!s_logged_mode)
		{
			s_logged_mode = true;
			printf("[c3d] renderer c3d: play frames are software (play 0)\n");
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
		char buf[1024];
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
	static bool direct_final = false;
	if ((s_d.frames || s_d.policy_calls) && !direct_final)
	{
		direct_final = true;
		char buf[1024];
		direct_counts_text(buf, sizeof buf);
		printf("[c3d] direct final %s bottom_pending=%u\n", buf, overlay_bottom_pending());
		if (s_s.stereo + s_s.mono)
		{
			stereo_counts_text(buf, sizeof buf);
			printf("[c3d] stereo final: %s changes=%u discards=%u\n", buf, s_s.changes, s_s.discards);
		}
		fflush(stdout);
	}
}

} // namespace back

// --- M6 hook 3 (include/a1_c3d.h) ---

void direct_policy(unsigned blocks)
{
	const Config& cfg = frame_config();
	// M9: frames that never reached a stereo-capable direct frame (map, terminal, SW, readback, menus, a discard) restart
	// the ramp; the stereo flag handed to the composite belongs to one frame; the right-clear rule's "previous frame" is set.
	if (!s_s.seen)
	{
		if (s_s.ramp.p > 0.0f)
			++s_s.ramp_resets;
		stereo_reset(s_s.ramp);
	}
	s_s.seen = false;
	s_s.prev = s_s.cur;
	s_s.cur = false;
	back::stereo_frame_set(false);
	// M6b: the fade belongs to this frame only (reset first), then the fit may clear kDirectGamma: the cleared mask is
	// what is stored, counted and used for overlay_ok (design 7, M6b "Order in direct_policy").
	back::fade_frame_set(nullptr);
	s_d.fade_this = FadeKind::None;
	blocks = fade_policy(cfg, blocks);
	s_d.policy = blocks;
	s_d.policy_set = true;
	++s_d.policy_calls;
	if (blocks)
		++s_d.policy_blocked[direct_block_index(blocks)];
	s_d.this_frame = false;
	back::overlay_drop();   // a hand-out the last present did not consume (the gate skipped it)
	// The overlay surface must exist before a frame is decided direct (critic, M6 round 1 minor 5).
	s_d.overlay_ok = !blocks && cfg.renderer_c3d && cfg.mode == Mode::Direct && back::is_up() && back::overlay_ensure();
}

bool direct_frame()
{
	return s_d.this_frame;
}

SDL_Surface* overlay_begin(int n, const OverlayBand* bands)
{
	if (!s_d.this_frame)
		return nullptr;
	Band in[kMaxBands * 2];
	int k = 0;
	for (int i = 0; i < n && k < kMaxBands * 2; ++i)
		in[k++] = Band{bands[i].y0, bands[i].y1};
	const int cal_row = back::config().calibrate ? 8 * static_cast<int>((back::present_counter() + 1) % 30) : -1;
	SDL_Surface* surf;
	{
#ifdef A1_PROFILE
		A1_PROF_SCOPE(C3dOverlay);
#endif
		surf = back::overlay_begin(in, k, cal_row);
	}
	if (!surf)
		s_d.this_frame = false;   // cannot happen (overlay_ok): the frame is then blitted from dest
	return surf;
}

const SDL_Surface* overlay_target()
{
	return back::overlay_surface_now();
}

} // namespace a1_c3d
