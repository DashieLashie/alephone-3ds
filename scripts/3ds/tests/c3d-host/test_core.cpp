// Host tests for render_c3d/core: the debug-c3d parser (P1, piece 5, B-a and M0 keys), core/gate.h
// (docs/3ds/CITRO3D_DESIGN.md 5.6.1, 8.4), and the M0 pieces (7, "M0 specification"): classify.h
// (class mapping, class list), diff.* (the 8.1 metric and its log line), bmp.* (32-bit BMP),
// gx_queue.h (2.4's worst case with the readback's +1), stats.* (nearest-rank percentile, the
// bounded sample array, the pair table) and calibrate.* (the readback calibration's check); and the M1
// pieces (7, "M1 specification"): geometry.* (the w gates (a)-(c) against a transliteration of the SW
// mapper's world_x, the coverage model against a transliteration of SW's per-column coverage, the
// acceptance test, fan indices, batching key, colour hash, command-buffer arithmetic, reserve test),
// the alpha quad of the calibration, GxQueue's split and runtime budget, and the M1 config keys; and the M2
// pieces (7, "M2 specification", parts 1-2): tex_convert.* (every kind with shapes.cpp-style 32-bit
// tables), tiling.* (against tex3ds, byte for byte), texcache.* (a fake allocator) and the M2 config keys;
// parts 3-4: the horizontal coverage model, the M2 support predicate, the texcoords against transliterations
// of SW's wall and floor mappers (the 98 % reachability gate), and the keys textures / uv_half / diff_tol.
#include "bmp.h"
#include "calibrate.h"
#include "classify.h"
#include "config.h"
#include "diff.h"
#include "gate.h"
#include "geometry.h"
#include "gx_queue.h"
#include "ring.h"
#include "stats.h"
#include "tex_convert.h"
#include "texcache.h"
#include "tiling.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <random>
#include <set>
#include <string>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

using namespace a1_c3d;

static int s_fail = 0, s_checks = 0;
#define CHECK(c)                                                              \
	do                                                                        \
	{                                                                         \
		++s_checks;                                                           \
		if (!(c))                                                             \
		{                                                                     \
			++s_fail;                                                         \
			std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);          \
		}                                                                     \
	} while (0)

static void test_config()
{
	Config d;
	CHECK(d.suspend_gate && d.retire_wait_ms == 500 && !d.force_abandon && !d.close_frame);
	CHECK(d.present_gpu && d.test_vhalt == 0);
	CHECK(!d.two_frame);   // plan B-a: off by default

	// two_frame (design 5.7.11)
	Config t;
	std::vector<std::string> tproblems;
	config_parse(t, "present gpu\ntwo_frame 1   # frames U and D\npresent_diff 30\n", tproblems);
	CHECK(tproblems.empty() && t.two_frame && t.present_gpu && t.present_diff == 30);
	CHECK(config_apply(t, "two_frame", "0") && !t.two_frame);
	CHECK(!config_apply(t, "two_frame", "2") && !t.two_frame);
	CHECK(!config_apply(t, "two_frame", "yes") && !config_apply(t, "two_frame", ""));

	Config c;
	std::vector<std::string> problems;
	config_parse(c,
	             "# diag\n"
	             "present gpu\n"
	             "suspend_gate 0   # off\n"
	             "retire_wait_ms 5000\n"
	             "close_frame 1\n"
	             "force_abandon 1\n"
	             "\n",
	             problems);
	CHECK(problems.empty());
	CHECK(!c.suspend_gate && c.retire_wait_ms == 5000 && c.close_frame && c.force_abandon);

	// Bad values leave the setting alone and are reported.
	Config b;
	problems.clear();
	config_parse(b, "suspend_gate 2\nretire_wait_ms -1\nretire_wait_ms 600001\nretire_wait_ms abc\n"
	                "close_frame\nforce_abandon yes\nsuspend_gates 1\n", problems);
	CHECK(problems.size() == 7);
	CHECK(b.suspend_gate && b.retire_wait_ms == 500 && !b.close_frame && !b.force_abandon);

	Config z;
	CHECK(config_apply(z, "retire_wait_ms", "0") && z.retire_wait_ms == 0);   // 0 = unbounded
	CHECK(config_apply(z, "retire_wait_ms", "600000") && z.retire_wait_ms == 600000);

	// The P1 keys still parse.
	Config p;
	problems.clear();
	config_parse(p, "present cpu\ncalibrate 1\nalt_load 1\nodd_surface 1\ntest_vhalt 4\npresent_diff 30\n", problems);
	CHECK(problems.empty() && !p.present_gpu && p.calibrate && p.alt_load && p.odd_surface && p.test_vhalt == 4 &&
	      p.present_diff == 30);
}

static void test_gate_inputs()
{
	GateInputs g;
	CHECK(!g.closed());
	g.gpu_right = false;
	CHECK(g.closed());
	g = GateInputs();
	g.should_close = true;
	CHECK(g.closed());
	g = GateInputs();
	g.suspended = true;
	CHECK(g.closed());
	g.gpu_right = false;
	g.should_close = true;
	CHECK(g.closed());
}

static void test_tracker()
{
	GateTracker t;
	using E = GateTracker::Event;
	CHECK(t.on_present(false, false) == E::None);
	CHECK(t.total_skipped() == 0 && !t.is_closed());
	// A closing: the event once, then counts.
	CHECK(t.on_present(true, true) == E::Closed);
	CHECK(t.on_present(true, true) == E::None);
	CHECK(t.on_present(true, true) == E::None);
	CHECK(t.is_closed() && t.episode_skipped() == 3 && t.total_skipped() == 3 && t.episodes() == 1);
	CHECK(t.on_present(false, false) == E::Reopened);
	CHECK(t.episode_skipped() == 3 && !t.is_closed());
	CHECK(t.on_present(false, false) == E::None);   // only once
	// A second episode restarts the episode count, not the total.
	CHECK(t.on_present(true, true) == E::Closed);
	CHECK(t.episode_skipped() == 1 && t.total_skipped() == 4 && t.episodes() == 2);
	CHECK(t.on_present(false, false) == E::Reopened && t.episode_skipped() == 1);

	// suspend_gate 0: the condition is watched, nothing is skipped.
	GateTracker w;
	CHECK(w.on_present(true, false) == E::Closed);
	CHECK(w.on_present(true, false) == E::None);
	CHECK(w.total_skipped() == 0 && w.episode_skipped() == 0);
	CHECK(w.on_present(false, false) == E::Reopened);
}

// A scripted fake queue and clock: retires after `busy_tries` failed tries.
struct Fake
{
	uint64_t now = 100;
	int tries = 0, sleeps = 0, busy_tries;
	explicit Fake(int b) : busy_tries(b) {}
	bool try_once() { return ++tries > busy_tries; }
	void sleep_ms(int ms) { now += ms; ++sleeps; }
	uint64_t clock() { return now; }
};

static RetireResult run(Fake& f, uint32_t limit, uint32_t& waited)
{
	return retire_wait([&] { return f.try_once(); }, [&](int ms) { f.sleep_ms(ms); }, [&] { return f.clock(); },
	                   limit, waited);
}

static void test_retire_wait()
{
	uint32_t w = 99;
	Fake a(0);   // retires at once: one try, no sleep
	CHECK(run(a, 500, w) == RetireResult::Retired && a.tries == 1 && a.sleeps == 0 && w == 0);

	Fake b(7);   // 7 busy tries: 7 sleeps of 1 ms, retired at 7 ms
	CHECK(run(b, 500, w) == RetireResult::Retired && b.tries == 8 && b.sleeps == 7 && w == 7);

	Fake c(100000);   // never: gives up at the bound, having slept (never spun)
	CHECK(run(c, 500, w) == RetireResult::TimedOut && w >= 500 && c.sleeps >= 499 && c.sleeps <= 501);
	CHECK(c.tries == c.sleeps + 1);   // exactly one sleep between two tries

	Fake d(100000);   // a bound of 1 ms still tries once
	CHECK(run(d, 1, w) == RetireResult::TimedOut && d.tries >= 1 && d.sleeps <= 1);

	Fake e(499);   // retires on the last try before the bound
	CHECK(run(e, 500, w) == RetireResult::Retired && w == 499);
}

static void test_config_m0()
{
	Config d;
	CHECK(!d.renderer_c3d && d.mode == Mode::Readback && d.diff == 0 && d.diff_frames == 20);
	CHECK(d.classes == kAllClassesMask && !d.fullbright && !d.dump && !d.readback_cal && !d.readback_cal_resize);

	Config c;
	std::vector<std::string> problems;
	config_parse(c,
	             "renderer c3d\nmode null\ndiff 5   # every fifth\ndiff_frames 0\n"
	             "classes walls,floors,frame\nfullbright 1\ndump 1\nreadback_cal 1\nreadback_cal_resize 1\n",
	             problems);
	CHECK(problems.empty());
	CHECK(c.renderer_c3d && c.mode == Mode::Null && c.diff == 5 && c.diff_frames == 0);
	CHECK(c.classes == ((1u << kWalls) | (1u << kFloors) | (1u << kFrame)));
	CHECK(c.fullbright && c.dump && c.readback_cal && c.readback_cal_resize);
	CHECK(config_apply(c, "readback_cal_resize", "0") && !c.readback_cal_resize);
	CHECK(config_apply(c, "mode", "direct") && c.mode == Mode::Direct);
	CHECK(config_apply(c, "mode", "readback") && c.mode == Mode::Readback);
	CHECK(config_apply(c, "renderer", "sw") && !c.renderer_c3d);
	CHECK(config_apply(c, "classes", "all") && c.classes == kAllClassesMask);
	CHECK(config_apply(c, "diff", "100000") && c.diff == 100000);
	CHECK(config_apply(c, "diff_frames", "1000000") && c.diff_frames == 1000000);

	// Bad values reject and leave the setting alone.
	Config b;
	CHECK(!config_apply(b, "renderer", "gpu") && !b.renderer_c3d);
	CHECK(!config_apply(b, "mode", "fast") && b.mode == Mode::Readback);
	CHECK(!config_apply(b, "diff", "100001") && b.diff == 0);
	CHECK(!config_apply(b, "diff", "-1") && !config_apply(b, "diff", ""));
	CHECK(!config_apply(b, "diff_frames", "1000001") && b.diff_frames == 20);
	CHECK(!config_apply(b, "fullbright", "2") && !b.fullbright);
	CHECK(!config_apply(b, "dump", "yes") && !b.dump);
	CHECK(!config_apply(b, "readback_cal", "") && !b.readback_cal);
	CHECK(!config_apply(b, "readback_cal_resize", "2") && !b.readback_cal_resize);
	CHECK(config_apply(b, "classes", "walls"));
	const uint32_t only_walls = b.classes;
	CHECK(only_walls == (1u << kWalls));
	CHECK(!config_apply(b, "classes", "walls,bogus") && b.classes == only_walls);
	CHECK(!config_apply(b, "classes", "walls,,floors") && b.classes == only_walls);
	CHECK(!config_apply(b, "classes", "walls,") && !config_apply(b, "classes", ",walls"));
	CHECK(!config_apply(b, "classes", "") && !config_apply(b, "classes", "Walls"));
	CHECK(!config_apply(b, "classes", "walls floors") && b.classes == only_walls);
}

static void test_classify()
{
	CHECK(kAllClassesMask == 0x1FFu);
	CHECK(classify(Entry::Vertical, Transfer::Textured, false) == kWalls);
	CHECK(classify(Entry::Vertical, Transfer::BigLandscaped, false) == kLandscape);
	CHECK(classify(Entry::Vertical, Transfer::Static, false) == kStatic);
	CHECK(classify(Entry::Horizontal, Transfer::Textured, false) == kFloors);
	CHECK(classify(Entry::Horizontal, Transfer::BigLandscaped, false) == kLandscape);
	CHECK(classify(Entry::Horizontal, Transfer::Static, false) == kStatic);
	CHECK(classify(Entry::Rectangle, Transfer::Tinted, false) == kTinted);
	CHECK(classify(Entry::Rectangle, Transfer::Tinted, true) == kTinted);
	CHECK(classify(Entry::Rectangle, Transfer::Static, true) == kStatic);
	CHECK(classify(Entry::Rectangle, Transfer::Textured, false) == kSprites);
	CHECK(classify(Entry::Rectangle, Transfer::Textured, true) == kWeapons);
	CHECK(classify(Entry::Rectangle, Transfer::Other, false) == kSprites);
	CHECK(classify(Entry::Rectangle, Transfer::Other, true) == kWeapons);
	CHECK(classify(Entry::Vertical, Transfer::Other, false) == kOther);
	CHECK(classify(Entry::Horizontal, Transfer::Other, true) == kOther);
	CHECK(classify(Entry::Vertical, Transfer::Tinted, false) == kOther);
	CHECK(classify(Entry::Horizontal, Transfer::Tinted, false) == kOther);

	CHECK(std::strcmp(class_name(kWalls), "walls") == 0 && std::strcmp(class_name(kFloors), "floors") == 0);
	CHECK(std::strcmp(class_name(kLandscape), "landscape") == 0 && std::strcmp(class_name(kStatic), "static") == 0);
	CHECK(std::strcmp(class_name(kSprites), "sprites") == 0 && std::strcmp(class_name(kWeapons), "weapons") == 0);
	CHECK(std::strcmp(class_name(kTinted), "tinted") == 0 && std::strcmp(class_name(kOther), "other") == 0);
	CHECK(std::strcmp(class_name(kFrame), "frame") == 0);

	uint32_t m = 0;
	CHECK(parse_classes("all", m) && m == kAllClassesMask);
	CHECK(parse_classes("sprites,weapons", m) && m == ((1u << kSprites) | (1u << kWeapons)));
	CHECK(parse_classes("frame", m) && m == (1u << kFrame));
	m = 7;
	CHECK(!parse_classes("walls,nope", m) && m == 7);
	CHECK(!parse_classes("", m) && m == 7);
}

// A 16x8 image, `v` everywhere.
static std::vector<uint32_t> flat(int w, int h, uint32_t v) { return std::vector<uint32_t>(static_cast<size_t>(w) * h, v); }

static void test_diff()
{
	const int w = 16, h = 8;
	// Identical images: everything covered is common except the edge band.
	std::vector<uint32_t> a = flat(w, h, 0);
	for (int y = 2; y < 6; ++y)
		for (int x = 4; x < 12; ++x)
			a[y * w + x] = 0xFF102030u;   // an 8x4 block: 32 covered, 2 x 6 = 12 interior pixels
	std::vector<uint32_t> b = a;
	Image32 ia{a.data(), w, h, w}, ib{b.data(), w, h, w};
	DiffResult r = diff_images(ia, ib, Coverage::TopByte, 0, 0);
	CHECK(r.covered_a == 32 && r.covered_b == 32);
	CHECK(r.cov_mismatch_edge == 0 && r.cov_mismatch_nonedge == 0);
	CHECK(r.common == 12 && r.within_tol == 12 && r.sum_delta == 0 && r.max_delta == 0 && r.differ == 0);
	CHECK(r.mean() == 0.0 && r.within_pct() == 100.0);

	// Deltas: max over channels; a change in an interior pixel counts, one on the edge does not.
	b[3 * w + 6] = 0xFF102030u + 0x000500u - 0x000000u + 0x070000u;   // R +7, G +5: delta 7
	b[3 * w + 7] = 0xFF102030u + 0x2;                                   // B +2
	b[2 * w + 4] = 0xFF000000u;                                         // edge pixel: differs, not common
	r = diff_images(ia, ib, Coverage::TopByte, 0, 2);
	CHECK(r.common == 12 && r.differ == 3);
	CHECK(r.sum_delta == 9 && r.max_delta == 7 && r.within_tol == 11);
	CHECK(r.mean() == 9.0 / 12.0);
	CHECK(r.within_pct() == 100.0 * 11 / 12);
	r = diff_images(ia, ib, Coverage::TopByte, 0, 7);
	CHECK(r.within_tol == 12);

	// Coverage mismatches: B missing an interior pixel (non-edge for A) and an edge pixel.
	std::vector<uint32_t> c = a;
	c[3 * w + 6] = 0;     // interior in A
	c[2 * w + 4] = 0;     // corner of the block, an edge pixel in A
	Image32 ic{c.data(), w, h, w};
	r = diff_images(ia, ic, Coverage::TopByte, 0, 0);
	CHECK(r.covered_a == 32 && r.covered_b == 30);
	CHECK(r.cov_mismatch_nonedge == 1 && r.cov_mismatch_edge == 1);
	CHECK(r.common == 11 && r.differ == 2);

	// B covers a pixel A does not: far from the block (non-edge), and next to it (edge).
	std::vector<uint32_t> d = a;
	d[0] = 0xFF000001u;       // A uncovered, neighbours uncovered
	d[2 * w + 3] = 0xFF000001u;   // A uncovered, neighbour covered
	Image32 id{d.data(), w, h, w};
	r = diff_images(ia, id, Coverage::TopByte, 0, 0);
	CHECK(r.covered_b == 34 && r.cov_mismatch_nonedge == 1 && r.cov_mismatch_edge == 1);

	// NotClear: coverage is "not the clear value", alpha plays no part.
	const uint32_t clear = 0xFF808080u;
	std::vector<uint32_t> e = flat(w, h, clear), f = flat(w, h, clear);
	e[4 * w + 4] = f[4 * w + 4] = 0xFF123456u;
	f[4 * w + 5] = 0xFF123456u;
	Image32 ie{e.data(), w, h, w}, iff{f.data(), w, h, w};
	r = diff_images(ie, iff, Coverage::NotClear, clear, 0);
	CHECK(r.covered_a == 1 && r.covered_b == 2 && r.cov_mismatch_edge == 1 && r.cov_mismatch_nonedge == 0);
	CHECK(r.common == 0 && r.differ == 1 && r.mean() == 0.0 && r.within_pct() == 100.0);
	r = diff_images(ie, iff, Coverage::TopByte, 0, 0);   // all alpha 0xFF: everything covered in both
	CHECK(r.covered_a == static_cast<unsigned>(w * h) && r.common == static_cast<unsigned>(w * h));   // no edge in A: nothing differs in coverage

	// A pitch larger than the width, and different pitches for the two images.
	std::vector<uint32_t> g(static_cast<size_t>(20) * h, 0xDEADBEEFu), k(static_cast<size_t>(w) * h, 0xFF000000u);
	for (int y = 0; y < h; ++y)
		for (int x = 0; x < w; ++x)
			g[y * 20 + x] = 0xFF000000u;
	Image32 ig{g.data(), w, h, 20}, ik{k.data(), w, h, w};
	r = diff_images(ig, ik, Coverage::TopByte, 0, 0);
	CHECK(r.differ == 0 && r.covered_a == static_cast<unsigned>(w * h));

	// The log line, exactly.
	DiffResult l;
	l.covered_a = 100; l.covered_b = 98; l.cov_mismatch_edge = 4; l.cov_mismatch_nonedge = 1;
	l.common = 80; l.within_tol = 60; l.sum_delta = 120; l.max_delta = 9; l.differ = 17; l.band = 64;
	CHECK(diff_line(12, "walls", l, 4) ==
	      "[c3d] DIFF frame=12 class=walls covered=100/98 cov_mismatch=4/1 common=80 within_tol=75.00% (tol 4) "
	      "mean=1.50 max=9 differ=17 band=64");
	CHECK(diff_line(0, "frame", DiffResult(), 0) ==
	      "[c3d] DIFF frame=0 class=frame covered=0/0 cov_mismatch=0/0 common=0 within_tol=100.00% (tol 0) "
	      "mean=0.00 max=0 differ=0 band=0");
	// M1: the C3D fields are appended after band, so an M0 parser (key=value pairs) still reads the rest.
	CHECK(diff_line_c3d(12, "walls", l, 4, 3, 0) ==
	      "[c3d] DIFF frame=12 class=walls covered=100/98 cov_mismatch=4/1 common=80 within_tol=75.00% (tol 4) "
	      "mean=1.50 max=9 differ=17 band=64 b=c3d excluded=3 sentinel=0 ties=0 tie_mismatch=0");
	l.ties = 5; l.tie_mismatch = 2;
	CHECK(diff_line_c3d(12, "walls", l, 4, 3, 0).find(" sentinel=0 ties=5 tie_mismatch=2") != std::string::npos);
	CHECK(diff_line_c3d(1, "walls", DiffResult(), 0, 0, 7).find(" band=0 b=c3d excluded=0 sentinel=7 ties=0 tie_mismatch=0") !=
	      std::string::npos);

	// The tie mask (M1 piece 5): a one-pixel hole in A's coverage that B fills is a non-edge mismatch... except
	// that the hole's neighbours differ in coverage so it is edge anyway; use a pixel deep inside a covered
	// block instead: B lacks it. Without a mask it is non-edge; with the mask it is an edge and a tie mismatch.
	{
		const int tw = 8, th = 8;
		std::vector<uint32_t> ta = flat(tw, th, 0xFF000000u), tb = ta;
		tb[3 * tw + 4] = 0;   // B does not cover (4,3); A covers all
		std::vector<uint8_t> mask(static_cast<size_t>(tw) * th, 0);
		const Image32 ita{ta.data(), tw, th, tw}, itb{tb.data(), tw, th, tw};
		DiffResult t0 = diff_images(ita, itb, Coverage::TopByte, 0, 0);
		CHECK(t0.cov_mismatch_nonedge == 1 && t0.cov_mismatch_edge == 0 && t0.ties == 0 && t0.tie_mismatch == 0);
		DiffResult t1 = diff_images(ita, itb, Coverage::TopByte, 0, 0, mask.data());   // an all-zero mask = no ties
		CHECK(t1.cov_mismatch_nonedge == 1 && t1.ties == 0 && t1.band == t0.band);
		mask[3 * tw + 4] = 1;
		mask[5 * tw + 5] = 1;   // a tie pixel where A and B agree
		DiffResult t2 = diff_images(ita, itb, Coverage::TopByte, 0, 0, mask.data());
		CHECK(t2.cov_mismatch_nonedge == 0 && t2.cov_mismatch_edge == 1 && t2.ties == 2 && t2.tie_mismatch == 1);
		CHECK(t2.band == t0.band + 2);
		CHECK(t2.common == t0.common - 1);   // the agreeing tie pixel leaves the common set
	}

	// The band: the 8x4 block of `a` in a 16x8 image has a ring of 10x6 - 8x4 = 28 outside pixels with a
	// covered neighbour, plus its 20 own border pixels (every one but the 12 interior ones): 48.
	{
		const int bw = 16, bh = 8;
		std::vector<uint32_t> ba = flat(bw, bh, 0);
		for (int y = 2; y < 6; ++y)
			for (int x = 4; x < 12; ++x)
				ba[y * bw + x] = 0xFF102030u;
		Image32 iba{ba.data(), bw, bh, bw};
		const DiffResult br = diff_images(iba, iba, Coverage::TopByte, 0, 0);
		CHECK(br.band == 48 && br.common == 12);
		std::vector<uint32_t> none = flat(bw, bh, 0);
		Image32 inone{none.data(), bw, bh, bw};
		CHECK(diff_images(inone, inone, Coverage::TopByte, 0, 0).band == 0);   // nothing covered: no edge
	}
}

static void test_bmp()
{
	const int w = 3, h = 2, pitch = 5;
	std::vector<uint32_t> px(static_cast<size_t>(pitch) * h, 0xCCCCCCCCu);
	for (int y = 0; y < h; ++y)
		for (int x = 0; x < w; ++x)
			px[y * pitch + x] = 0x80000000u | (static_cast<uint32_t>(x) << 16) | (static_cast<uint32_t>(y) << 8) | 0x11;
	px[0] = 0xAA112233u;                          // top-left
	px[(h - 1) * pitch + (w - 1)] = 0x44556677u;  // bottom-right
	const std::vector<uint8_t> b = bmp_encode(px.data(), w, h, pitch);
	auto u16 = [&](size_t o) { return static_cast<unsigned>(b[o] | (b[o + 1] << 8)); };
	auto u32 = [&](size_t o) { return u16(o) | (u16(o + 2) << 16); };
	CHECK(b.size() == 54u + w * h * 4);
	CHECK(b[0] == 'B' && b[1] == 'M');
	CHECK(u32(2) == b.size() && u32(10) == 54);
	CHECK(u32(14) == 40 && u32(18) == static_cast<unsigned>(w) && u32(22) == static_cast<unsigned>(h));
	CHECK(u16(26) == 1 && u16(28) == 32 && u32(30) == 0);
	CHECK(u32(34) == static_cast<unsigned>(w * h * 4));
	// The first file row is the image's last row; pixel (0,0) is the first of the last file row.
	const size_t last_row = 54 + static_cast<size_t>(h - 1) * w * 4;
	CHECK(b[last_row + 0] == 0x33 && b[last_row + 1] == 0x22 && b[last_row + 2] == 0x11 && b[last_row + 3] == 0xAA);
	const size_t last_px = 54 + static_cast<size_t>(w - 1) * 4;   // (w-1, h-1): last pixel of the first file row
	CHECK(b[last_px + 0] == 0x77 && b[last_px + 1] == 0x66 && b[last_px + 2] == 0x55 && b[last_px + 3] == 0x44);
	// A middle pixel (1, 0): x=1 y=0 -> 0x80010011: A 0x80, R 0x01, G 0x00, B 0x11, in the last file row.
	CHECK(b[last_row + 4] == 0x11 && b[last_row + 5] == 0x00 && b[last_row + 6] == 0x01 && b[last_row + 7] == 0x80);
	CHECK(bmp_encode(px.data(), 0, 2, pitch).empty() && bmp_encode(px.data(), 2, 0, pitch).empty());
}

static void test_gx_queue_readback()
{
	// The readback transfer is inside the view frame since revise 1: it is one more take() (two with a
	// non-empty command buffer) and the frame's total counts it.
	GxQueue q;
	q.begin_frame();
	for (int i = 0; i < GxQueue::kMaxBudget - 2; ++i)
		CHECK(q.take(false));
	CHECK(q.take(true));   // the readback transfer, splitting the command list
	CHECK(!q.take(false) && q.refused() == 1);
	CHECK(q.end_frame(0) == GxQueue::kMaxBudget + 1);
	CHECK(q.last_total() == 25 && q.high_water() == 25 && q.high_water() < 32);

	// A later, smaller frame does not lower the high-water mark.
	q.begin_frame();
	CHECK(q.take(true));
	CHECK(q.end_frame(0) == 3 && q.last_total() == 3 && q.high_water() == 25);
}

static void test_gx_queue_m1()
{
	// The flushing split is exactly 1 whatever the buffer holds; the transfer after it costs 1: a C3D view
	// frame is clear 1 + split 1 + transfer 1 = 3 entries (M1 specification, "GX queue").
	GxQueue q;
	q.begin_frame();
	CHECK(q.budget() == 24 && GxQueue::kMaxBudget == 24);
	CHECK(q.take(false));        // the clear, before the first draw
	CHECK(q.take_split());       // the flushing split (the buffer holds draws)
	CHECK(q.take(false));        // the transfer finds an empty buffer
	CHECK(q.used() == 3 && q.refused() == 0);
	CHECK(q.end_frame(0) == 3 + GxQueue::kFrameEndEntries && q.high_water() == 4);

	// A non-empty buffer at a plain take still costs 2, the split never does.
	q.begin_frame();
	CHECK(q.take(true) && q.used() == 2);
	CHECK(q.take_split() && q.used() == 3);

	// Runtime budget: 4 passes the view frame, 2 refuses the transfer (clear, split, then no room).
	GxQueue q4;
	q4.set_budget(4);
	q4.begin_frame();
	CHECK(q4.take(false) && q4.take_split() && q4.take(false) && q4.refused() == 0);
	GxQueue q2;
	q2.set_budget(2);
	q2.begin_frame();
	CHECK(q2.take(false) && q2.take_split() && !q2.take(false) && q2.refused() == 1 && q2.used() == 2);
	// Never above 24, never below 1; set_budget keeps the count of the frame in progress.
	GxQueue qc;
	qc.set_budget(100);
	CHECK(qc.budget() == 24);
	qc.set_budget(0);
	CHECK(qc.budget() == 1);
	qc.set_budget(-5);
	CHECK(qc.budget() == 1);
	qc.begin_frame();
	CHECK(qc.take(false) && !qc.take_split() && !qc.take(false) && qc.refused() == 2);

	// The worst case with the split at 1: 24 + 1 for C3D_FrameEnd = 25 < 32; 24 entries then a split is refused.
	GxQueue qw;
	qw.begin_frame();
	for (int i = 0; i < 23; ++i)
		CHECK(qw.take(false));
	CHECK(qw.take_split() && !qw.take_split() && qw.used() == 24);
	CHECK(qw.end_frame(0) == 25 && qw.high_water() == 25 && qw.high_water() < 32);
}

static void test_stats()
{
	Samples s;
	CHECK(s.percentile(99) == 0 && s.max() == 0 && s.count() == 0);
	s.add(42);
	CHECK(s.percentile(1) == 42 && s.percentile(99) == 42 && s.percentile(100) == 42 && s.max() == 42);

	Samples h;
	for (uint32_t v = 100; v >= 1; --v)   // 1..100, added in descending order
		h.add(v);
	CHECK(h.count() == 100 && h.max() == 100);
	CHECK(h.percentile(99) == 99 && h.percentile(100) == 100 && h.percentile(50) == 50);
	CHECK(h.percentile(1) == 1 && h.percentile(0) == 1);

	Samples five;
	for (uint32_t v : {10u, 50u, 20u, 40u, 30u})
		five.add(v);
	CHECK(five.percentile(50) == 30 && five.percentile(99) == 50 && five.percentile(20) == 10 &&
	      five.percentile(21) == 20);

	// The cap: values beyond the capacity update max and count, not the stored set.
	Samples cap(4);
	for (uint32_t v : {1u, 2u, 3u, 4u, 1000u})
		cap.add(v);
	CHECK(cap.count() == 5 && cap.stored() == 4 && cap.max() == 1000);
	CHECK(cap.percentile(100) == 4);

	// PairTable.
	PairTable t;
	t.begin_frame();
	CHECK(t.count() == 0 && !t.full());
	CHECK(t.insert(1, 2) && t.insert(2, 1) && t.insert(1, 3));
	CHECK(!t.insert(1, 2) && !t.insert(2, 1));   // repeats
	CHECK(t.count() == 3 && !t.full());
	t.begin_frame();   // a new frame forgets everything
	CHECK(t.count() == 0);
	CHECK(t.insert(1, 2) && t.count() == 1);

	t.begin_frame();   // fill it
	for (uintptr_t i = 0; i < PairTable::kSlots; ++i)
		CHECK(t.insert(i, i * 7 + 1));
	CHECK(t.count() == 1024 && !t.full());
	CHECK(!t.insert(5, 5 * 7 + 1) && !t.full());   // a repeat is not "full"
	CHECK(!t.insert(99999, 1) && t.full() && t.count() == 1024);
	t.begin_frame();
	CHECK(!t.full() && t.count() == 0 && t.insert(99999, 1));

	// Many frames (the generation advances; stale entries from earlier frames never match).
	PairTable m;
	bool ok = true;
	for (int f = 0; f < 5000; ++f)
	{
		m.begin_frame();
		ok = ok && m.insert(7, 8) && !m.insert(7, 8) && m.count() == 1;
	}
	CHECK(ok);
}

static void test_calibrate()
{
	CHECK(rgba_to_argb(0xFF0000FFu) == 0xFFFF0000u);   // red RGBA -> red ARGB
	CHECK(rgba_to_argb(0x12345600u) == 0xFF123456u);
	CHECK(cal_clear_argb(0x1234) == 0xFF344080u);
	CHECK(cal_clear_rgba(0x34) == 0x344080FFu);
	CHECK(rgba_to_argb(cal_clear_rgba(77)) == cal_clear_argb(77));
	for (unsigned k = 0; k < 256; ++k)
	{
		const uint32_t c = cal_clear_argb(k);
		CHECK(c != kCalTopLeft && c != kCalTopRight && c != kCalBottomLeft && c != kCalBottomRight);
	}
	CHECK(kCalQuad == 16);
	CHECK(std::strcmp(orientation_name(Orientation::Ok), "ok") == 0);
	CHECK(std::strcmp(orientation_name(Orientation::RowsFlipped), "rows flipped") == 0);
	CHECK(std::strcmp(orientation_name(Orientation::ColsFlipped), "columns flipped") == 0);
	CHECK(std::strcmp(orientation_name(Orientation::Rotated180), "rotated 180") == 0);
	CHECK(std::strcmp(orientation_name(Orientation::Transposed), "transposed") == 0);
	CHECK(std::strcmp(orientation_name(Orientation::Unknown), "unknown") == 0);

	const int sizes[2][2] = {{400, 240}, {480, 240}};
	for (const auto& sz : sizes)
	{
		const int w = sz[0], h = sz[1];
		const uint32_t R = kCalTopLeft, G = kCalTopRight, B = kCalBottomLeft, W = kCalBottomRight;
		struct Case { uint32_t tl, tr, bl, br; Orientation o; };
		const Case cases[] = {
			{R, G, B, W, Orientation::Ok},          {B, W, R, G, Orientation::RowsFlipped},
			{G, R, W, B, Orientation::ColsFlipped}, {W, B, G, R, Orientation::Rotated180},
			{R, B, G, W, Orientation::Transposed},  {R, R, R, R, Orientation::Unknown},
		};
		for (const Case& cs : cases)
		{
			const unsigned k = 17;
			std::vector<uint32_t> img = flat(w, h, cal_clear_argb(k));
			auto quad = [&](int x0, int y0, uint32_t v) {
				for (int y = y0; y < y0 + kCalQuad; ++y)
					for (int x = x0; x < x0 + kCalQuad; ++x)
						img[static_cast<size_t>(y) * w + x] = v;
			};
			quad(0, 0, cs.tl);
			quad(w - kCalQuad, 0, cs.tr);
			quad(0, h - kCalQuad, cs.bl);
			quad(w - kCalQuad, h - kCalQuad, cs.br);
			// The alpha quad where this orientation puts it (top centre for the unknown one).
			const int ax = cs.o == Orientation::Transposed ? 0 : w / 2 - 8;
			const int ay = (cs.o == Orientation::RowsFlipped || cs.o == Orientation::Rotated180) ? h - kCalQuad
			               : cs.o == Orientation::Transposed ? h / 2 - 8 : 0;
			quad(ax, ay, kCalAlphaQuad);
			const CalResult r = cal_check(img.data(), w, h, w, k);
			CHECK(r.alpha_ok);
			CHECK(r.orientation == cs.o);
			CHECK(r.observed_k == 17 && r.current_frame_ok == (cs.o != Orientation::Unknown));   // unknown: corners mismatch
			CHECK(r.corner[0] == cs.tl && r.corner[1] == cs.tr && r.corner[2] == cs.bl && r.corner[3] == cs.br);
			CHECK(r.centre == cal_clear_argb(k));
			CHECK(r.channels_ok == (cs.o != Orientation::Unknown));
			if (cs.o == Orientation::Ok)
				CHECK(cal_line(k, r) ==
				      "[c3d] readback_cal k=17 orientation ok current frame ok mismatched 0 (sentinel 0) "
				      "channels ok alpha ok corners 0xFFFF0000 0xFF00FF00 0xFF0000FF 0xFFFFFFFF");
		}
	}

	// A perfect image: every pixel accounted for, nothing stale, nothing left of the sentinel.
	const int w = 400, h = 240;
	auto perfect = [&](unsigned k) {
		std::vector<uint32_t> im = flat(w, h, cal_clear_argb(k));
		auto quad = [&](int x0, int y0, uint32_t v) {
			for (int y = y0; y < y0 + kCalQuad; ++y)
				for (int x = x0; x < x0 + kCalQuad; ++x)
					im[static_cast<size_t>(y) * w + x] = v;
		};
		quad(0, 0, kCalTopLeft);
		quad(w - kCalQuad, 0, kCalTopRight);
		quad(0, h - kCalQuad, kCalBottomLeft);
		quad(w - kCalQuad, h - kCalQuad, kCalBottomRight);
		quad(w / 2 - 8, 0, kCalAlphaQuad);
		return im;
	};
	{
		std::vector<uint32_t> im = perfect(17);
		CalResult p = cal_check(im.data(), w, h, w, 17);
		CHECK(p.current_frame_ok && p.mismatched == 0 && p.sentinel == 0 && p.orientation == Orientation::Ok);
		CHECK(cal_line(17, p) ==
		      "[c3d] readback_cal k=17 orientation ok current frame ok mismatched 0 (sentinel 0) channels ok "
		      "alpha ok corners 0xFFFF0000 0xFF00FF00 0xFF0000FF 0xFFFFFFFF");

		// One stale pixel (the previous frame's clear) away from every sample point.
		im[100 * w + 100] = cal_clear_argb(16);
		p = cal_check(im.data(), w, h, w, 17);
		CHECK(!p.current_frame_ok && p.mismatched == 1 && p.sentinel == 0);
		CHECK(p.orientation == Orientation::Ok && p.observed_k == 17);   // the five samples are fine

		// A row the transfer never wrote: the sentinel (also 0xRRGGBB of another k than 17's) is left in it.
		im = perfect(17);
		for (int x = 0; x < w; ++x)
			im[50 * w + x] = cal_sentinel_argb(17);
		p = cal_check(im.data(), w, h, w, 17);
		CHECK(!p.current_frame_ok && p.mismatched == static_cast<unsigned>(w) && p.sentinel == static_cast<unsigned>(w));
		CHECK(cal_line(17, p).find("mismatched 400 (sentinel 400)") != std::string::npos);
		// ... in a quad, and in a row that is also stale: only the sentinel pixels count as sentinel.
		im[3 * w + 3] = cal_sentinel_argb(17);
		im[60 * w + 60] = cal_clear_argb(16);
		p = cal_check(im.data(), w, h, w, 17);
		CHECK(p.mismatched == static_cast<unsigned>(w) + 2 && p.sentinel == static_cast<unsigned>(w) + 1);

		// The sentinel is no clear and no corner colour for any k, and differs from k to k.
		for (unsigned k = 0; k < 256; ++k)
		{
			const uint32_t sn = cal_sentinel_argb(k);
			CHECK(sn != cal_clear_argb(k) && sn != kCalTopLeft && sn != kCalTopRight && sn != kCalBottomLeft &&
			      sn != kCalBottomRight);
			CHECK(rgba_to_argb(cal_sentinel_rgba(k)) == sn && sn != cal_sentinel_argb(k + 1));
		}

		// Rows flipped: still reported as an orientation, and the image is current (0 mismatched against
		// the flipped layout); the orientation, not the mismatch count, tells it apart.
		std::vector<uint32_t> fl = perfect(17);
		for (int y = 0; y < h / 2; ++y)
			for (int x = 0; x < w; ++x)
				std::swap(fl[static_cast<size_t>(y) * w + x], fl[static_cast<size_t>(h - 1 - y) * w + x]);
		p = cal_check(fl.data(), w, h, w, 17);
		CHECK(p.orientation == Orientation::RowsFlipped && p.channels_ok && p.mismatched == 0 && p.current_frame_ok);
		fl[100 * w + 100] = cal_sentinel_argb(17);
		p = cal_check(fl.data(), w, h, w, 17);
		CHECK(p.orientation == Orientation::RowsFlipped && p.mismatched == 1 && p.sentinel == 1);
	}

	// Current frame wrong: a stale frame's k, or something that is no clear at all.
	std::vector<uint32_t> img = perfect(16);
	CalResult r = cal_check(img.data(), w, h, w, 17);
	CHECK(r.orientation == Orientation::Ok && !r.current_frame_ok && r.observed_k == 16);
	CHECK(cal_line(17, r) ==
	      "[c3d] readback_cal k=17 orientation ok current frame NO (centre k=16) mismatched 94720 (sentinel 0) "
	      "channels ok alpha ok corners "
	      "0xFFFF0000 0xFF00FF00 0xFF0000FF 0xFFFFFFFF");
	img[(h / 2) * w + w / 2] = 0xFF123456u;
	r = cal_check(img.data(), w, h, w, 17);
	CHECK(!r.current_frame_ok && r.observed_k == -1);
	CHECK(cal_line(17, r) ==
	      "[c3d] readback_cal k=17 orientation ok current frame NO (centre 0xFF123456) mismatched 94720 "
	      "(sentinel 0) channels ok alpha ok corners "
	      "0xFFFF0000 0xFF00FF00 0xFF0000FF 0xFFFFFFFF");
	CHECK(cal_check(img.data(), w, h, w, 0x111).observed_k == -1);   // k is taken mod 256 by the clear
	img = perfect(0x111);   // k is taken mod 256 by the clear
	CHECK(cal_check(img.data(), w, h, w, 0x111).current_frame_ok);

	// Channels swapped (R and B exchanged): the corners are no longer the expected colours.
	auto swap_rb = [](uint32_t c) { return (c & 0xFF00FF00u) | ((c & 0xFF) << 16) | ((c >> 16) & 0xFF); };
	img = perfect(17);
	for (uint32_t& p : img)
		if (p != cal_clear_argb(17))
			p = swap_rb(p);
	r = cal_check(img.data(), w, h, w, 17);
	CHECK(!r.channels_ok && r.orientation == Orientation::Unknown);
	CHECK(cal_line(17, r).find("channels NO") != std::string::npos);

	// A pitch wider than the image.
	std::vector<uint32_t> wide(static_cast<size_t>(512) * h, 0xDEADBEEFu);
	for (int y = 0; y < h; ++y)
		for (int x = 0; x < w; ++x)
			wide[static_cast<size_t>(y) * 512 + x] = cal_clear_argb(3);
	for (int y = 0; y < kCalQuad; ++y)
		for (int x = 0; x < kCalQuad; ++x)
		{
			wide[static_cast<size_t>(y) * 512 + x] = kCalTopLeft;
			wide[static_cast<size_t>(y) * 512 + (w - 1 - x)] = kCalTopRight;
			wide[static_cast<size_t>(h - 1 - y) * 512 + x] = kCalBottomLeft;
			wide[static_cast<size_t>(h - 1 - y) * 512 + (w - 1 - x)] = kCalBottomRight;
			wide[static_cast<size_t>(y) * 512 + (w / 2 - 8 + x)] = kCalAlphaQuad;
		}
	r = cal_check(wide.data(), w, h, 512, 3);
	CHECK(r.orientation == Orientation::Ok && r.current_frame_ok && r.channels_ok && r.mismatched == 0);
}

static void test_calibrate_m1()
{
	// The keeping conversion: 0xRRGGBBAA -> 0xAARRGGBB, alpha untouched (the forcing one sets it to 0xFF).
	CHECK(rgba_to_argb_keep(0x20609080u) == kCalAlphaQuad);
	CHECK(rgba_to_argb_keep(0x12345678u) == 0x78123456u);
	CHECK(rgba_to_argb_keep(0xFF0000FFu) == 0xFFFF0000u && rgba_to_argb_keep(0x00000000u) == 0u);
	CHECK(rgba_to_argb_keep(0xAABBCC00u) == 0x00AABBCCu && rgba_to_argb(0xAABBCC00u) == 0xFFAABBCCu);
	for (unsigned k = 0; k < 256; ++k)   // the sentinel has alpha 0xFF: both forms agree
		CHECK(rgba_to_argb_keep(cal_sentinel_rgba(k)) == cal_sentinel_argb(k));

	// A wrong alpha quad is a mismatch and alpha NO; a perfect image (alpha 0xFF elsewhere) is alpha ok.
	const int w = 400, h = 240;
	const unsigned k = 9;
	std::vector<uint32_t> im = flat(w, h, cal_clear_argb(k));
	auto quad = [&](int x0, int y0, uint32_t v) {
		for (int y = y0; y < y0 + kCalQuad; ++y)
			for (int x = x0; x < x0 + kCalQuad; ++x)
				im[static_cast<size_t>(y) * w + x] = v;
	};
	quad(0, 0, kCalTopLeft);
	quad(w - kCalQuad, 0, kCalTopRight);
	quad(0, h - kCalQuad, kCalBottomLeft);
	quad(w - kCalQuad, h - kCalQuad, kCalBottomRight);
	CalResult r = cal_check(im.data(), w, h, w, k);   // no alpha quad drawn (M0's image)
	CHECK(!r.alpha_ok && r.mismatched == 256 && !r.current_frame_ok && r.orientation == Orientation::Ok);
	CHECK(cal_line(k, r).find("channels ok alpha NO corners") != std::string::npos);
	quad(w / 2 - 8, 0, kCalAlphaQuad);
	r = cal_check(im.data(), w, h, w, k);
	CHECK(r.alpha_ok && r.mismatched == 0 && r.current_frame_ok);
	im[3 * w + w / 2] = 0xFF206090u;   // alpha forced to 0xFF: the forcing conversion's signature
	r = cal_check(im.data(), w, h, w, k);
	CHECK(!r.alpha_ok && r.mismatched == 1 && !r.current_frame_ok);
	im[3 * w + w / 2] = kCalAlphaQuad;
	im[5 * w + 5 + 100] = 0x80123456u;   // alpha elsewhere is wrong too: a mismatch, alpha still ok
	r = cal_check(im.data(), w, h, w, k);
	CHECK(r.alpha_ok && r.mismatched == 1);
	// The quad's edges: x in [192, 208), y in [0, 16), so (191, 0) and (208, 0) and (192, 16) are clear.
	im = flat(w, h, cal_clear_argb(k));
	quad(0, 0, kCalTopLeft);
	quad(w - kCalQuad, 0, kCalTopRight);
	quad(0, h - kCalQuad, kCalBottomLeft);
	quad(w - kCalQuad, h - kCalQuad, kCalBottomRight);
	quad(w / 2 - 8, 1, kCalAlphaQuad);   // one row too low
	r = cal_check(im.data(), w, h, w, k);
	CHECK(!r.alpha_ok && r.mismatched == 2 * 16);

	// All four orientations (and the unknown one, top centre): the quad follows the orientation.
	const uint32_t R = kCalTopLeft, G = kCalTopRight, B = kCalBottomLeft, W = kCalBottomRight;
	struct Case { uint32_t c[4]; Orientation o; int qx, qy; };
	const Case cases[] = {
		{{R, G, B, W}, Orientation::Ok, w / 2 - 8, 0},
		{{B, W, R, G}, Orientation::RowsFlipped, w / 2 - 8, h - 16},
		{{G, R, W, B}, Orientation::ColsFlipped, w / 2 - 8, 0},
		{{W, B, G, R}, Orientation::Rotated180, w / 2 - 8, h - 16},
		{{R, B, G, W}, Orientation::Transposed, 0, h / 2 - 8},
	};
	for (const Case& cs : cases)
	{
		im = flat(w, h, cal_clear_argb(k));
		quad(0, 0, cs.c[0]);
		quad(w - kCalQuad, 0, cs.c[1]);
		quad(0, h - kCalQuad, cs.c[2]);
		quad(w - kCalQuad, h - kCalQuad, cs.c[3]);
		quad(cs.qx, cs.qy, kCalAlphaQuad);
		r = cal_check(im.data(), w, h, w, k);
		CHECK(r.orientation == cs.o && r.alpha_ok && r.mismatched == 0 && r.current_frame_ok && r.channels_ok);
		// The quad on the other side (the wrong place for this orientation) is alpha NO with 2 x 256 mismatches
		// (a quad missing where it belongs, one where it does not) unless both sides coincide.
		im = flat(w, h, cal_clear_argb(k));
		quad(0, 0, cs.c[0]);
		quad(w - kCalQuad, 0, cs.c[1]);
		quad(0, h - kCalQuad, cs.c[2]);
		quad(w - kCalQuad, h - kCalQuad, cs.c[3]);
		quad(cs.qx, cs.qy == 0 ? h - kCalQuad : 0, kCalAlphaQuad);
		if (cs.o != Orientation::Transposed)
		{
			r = cal_check(im.data(), w, h, w, k);
			CHECK(r.orientation == cs.o && !r.alpha_ok && r.mismatched == 512);
		}
	}
}

// ---------------------------------------------------------------------------------------------
// Transliterations of the software mapper (Source_Files/RenderMain/scottish_textures.cpp) for the
// geometry tests. They live here, not in core/.

namespace sw
{
// scottish_textures.cpp:906-954 (_pretexture_vertical_polygon_lines), the part that yields world_x, for
// the column x0 (screen_x = x0 - hw), TEXBITS 7. int32 arithmetic as in the original; the one product that
// can leave int32 (:954) is done in 64 bits and reported, not wrapped.
struct WorldX
{
	int32_t world_x;       // after the negation of :955
	int32_t tx;            // fixed point 16.16
	int32_t n_r, d_r;      // the shifted numerator and denominator that tx was divided from
	int64_t product;       // tx * vector.i, whose int32 truncation :954 shifts
};

static WorldX world_x(int world_to_screen_x, int hw, int x0, int ox, int oy, int vi, int vj)
{
	const int32_t screen_x = x0 - hw;
	int32_t tx_numerator = world_to_screen_x * oy - screen_x * ox;
	int32_t tx_denominator = screen_x * vi - world_to_screen_x * vj;
	int32_t an = tx_numerator, ad = tx_denominator;
	const int bits = 7;   // VERTICAL_TEXTURE_WIDTH_BITS
	while (an > ((1 << (31 - bits)) - 1) || an < -(1 << (31 - bits)))
		an >>= 1, ad >>= 1;
	if (!ad)
		ad = 1;
	while (an > INT16_MAX || an < INT16_MIN)
		an >>= 1, ad >>= 1;
	if (!ad)
		ad = 1;
	WorldX r;
	r.n_r = an;
	r.d_r = ad;
	r.tx = static_cast<int32_t>((static_cast<int64_t>(an) * 65536) / ad);   // INTEGER_TO_FIXED(an) / ad
	r.product = static_cast<int64_t>(r.tx) * vi;
	r.world_x = ox + (static_cast<int32_t>(r.product) >> 16);
	if (r.world_x < 0)
		r.world_x = -r.world_x;
	return r;
}

// scottish_textures.cpp:1231-1290 (build_y_table), on shorts as in the original. NULL = refused.
static short* build_y_table(short* table, short x0, short y0, short x1, short y1)
{
	short dx, dy, adx, ady;
	short x, y;
	short d, delta_d, d_max;
	short* record;
	auto sgn = [](short v) -> short { return v < 0 ? -1 : (v > 0 ? 1 : 0); };

	dx = x1 - x0, adx = std::abs(dx), dx = sgn(dx);
	dy = y1 - y0, ady = std::abs(dy), dy = sgn(dy);
	if (dx >= 0)
	{
		if (dy >= 0)
		{
			x = x0, y = y0;
			record = table;
		}
		else
		{
			x = x1, y = y1;
			record = table + adx;
		}
		if (adx >= ady)
		{
			d = adx - ady, delta_d = -2 * ady, d_max = 2 * adx;
			while ((adx -= 1) >= 0)
			{
				if (d < 0)
					y += 1, d += d_max;
				if (dy >= 0)
					*record++ = y;
				else
					*--record = y;
				x += dx, d += delta_d;
			}
		}
		else
		{
			d = ady - adx, delta_d = -2 * adx, d_max = 2 * ady;
			while ((ady -= 1) >= 0)
			{
				if (d < 0)
				{
					x += dx, d += d_max, adx -= 1;
					if (dy >= 0)
						*record++ = y;
					else
						*--record = y;
				}
				y += 1, d += delta_d;
			}
			if (adx == 1)
			{
				if (dy >= 0)
					*record++ = y;
				else
					*--record = y;
			}
		}
	}
	else
		return nullptr;
	(void)x;   // the original tracks x without reading it
	return table;
}

enum Result { Drawn, RefusedBounds, RefusedDegenerate, RefusedWalk };

// scottish_textures.cpp:429-520 (texture_vertical_polygon) down to the tables, then the row loop of
// low_level_textures.h:322-326: `cov` (w x h, zeroed by the caller) gets 1 for every written pixel.
// `wrote_outside` counts rows the original would write outside [0, h).
static Result polygon_coverage(const std::vector<Point2>& pts, int w, int h, std::vector<uint8_t>& cov,
                               unsigned& wrote_outside)
{
	const int n = static_cast<int>(pts.size());
	int highest = 0, lowest = 0;
	for (int v = 1; v < n; ++v)
	{
		if (pts[v].x < pts[highest].x)
			highest = v;
		if (pts[v].x > pts[lowest].x)
			lowest = v;
	}
	for (int v = 0; v < n; ++v)
		if (!(pts[v].x >= 0 && pts[v].x <= w && pts[v].y >= 0 && pts[v].y <= h))
			return RefusedBounds;
	if (highest == lowest)
		return RefusedDegenerate;

	std::vector<short> left_table(8192, -1), right_table(8192, -1);
	short left_line_count = 0, right_line_count = 0, total_line_count;
	short aggregate_left = 0, aggregate_right = 0;
	short left_vertex = static_cast<short>(highest), right_vertex = static_cast<short>(highest), vertex;
	total_line_count = static_cast<short>(pts[lowest].x - pts[highest].x);
	const short aggregate_total = total_line_count;
	while (total_line_count > 0)
	{
		if (left_line_count <= 0)
		{
			do
			{
				vertex = (left_vertex == n - 1) ? 0 : static_cast<short>(left_vertex + 1);
				left_line_count = static_cast<short>(pts[vertex].x - pts[left_vertex].x);
				if (!build_y_table(left_table.data() + aggregate_left, static_cast<short>(pts[left_vertex].x),
				                   static_cast<short>(pts[left_vertex].y), static_cast<short>(pts[vertex].x),
				                   static_cast<short>(pts[vertex].y)))
					return RefusedWalk;
				aggregate_left = static_cast<short>(aggregate_left + left_line_count);
				left_vertex = vertex;
			} while (!left_line_count);
		}
		if (right_line_count <= 0)
		{
			do
			{
				vertex = right_vertex ? static_cast<short>(right_vertex - 1) : static_cast<short>(n - 1);
				right_line_count = static_cast<short>(pts[vertex].x - pts[right_vertex].x);
				if (!build_y_table(right_table.data() + aggregate_right, static_cast<short>(pts[right_vertex].x),
				                   static_cast<short>(pts[right_vertex].y), static_cast<short>(pts[vertex].x),
				                   static_cast<short>(pts[vertex].y)))
					return RefusedWalk;
				aggregate_right = static_cast<short>(aggregate_right + right_line_count);
				right_vertex = vertex;
			} while (!right_line_count);
		}
		const short delta = std::min(left_line_count, right_line_count);
		total_line_count = static_cast<short>(total_line_count - delta);
		left_line_count = static_cast<short>(left_line_count - delta);
		right_line_count = static_cast<short>(right_line_count - delta);
	}
	for (int k = 0; k < aggregate_total; ++k)
	{
		const int x = pts[highest].x + k;
		const int y0 = left_table[k], y1 = right_table[k];
		for (int y = y0; y < y1; ++y)
		{
			if (y < 0 || y >= h || x >= w)
				++wrote_outside;
			else
				cov[static_cast<size_t>(y) * w + x] = 1;
		}
	}
	return Drawn;
}
} // namespace sw

static void test_wall_w()
{
	// Basics: a wall in the plane x = 1000 (v.i = 0) is at w = 1000 in every column.
	for (int sx = 0; sx <= 400; sx += 50)
		CHECK(wall_w(200, 256, 1000, 0, 0, 1000, sx) == 1000.0f);
	// |w|: a negative world x is negated (:955).
	CHECK(wall_w(200, 256, -1000, 0, 0, 1000, 123) == 1000.0f);
	// The denominator guard (:943, 950): column sx = hw, vector.j = 0 -> denominator 0 -> 1, no inf / NaN.
	const float g = wall_w(200, 256, 1000, 5000, 7, 0, 200);
	CHECK(std::isfinite(g) && g == std::fabs(1000.0f + static_cast<float>(256 * 5000) * 7.0f));
	// The clamp, apart: [16, 32767].
	CHECK(clamp_w(0.0f) == 16.0f && clamp_w(15.9f) == 16.0f && clamp_w(16.0f) == 16.0f);
	CHECK(clamp_w(500.5f) == 500.5f && clamp_w(32767.0f) == 32767.0f && clamp_w(32768.0f) == 32767.0f);
	CHECK(clamp_w(1e30f) == 32767.0f && clamp_w(-5.0f) == 16.0f);
	CHECK(wall_w_clamped(200, 256, 10, 0, 0, 1000, 123) == 16.0f && wall_w_clamped(200, 256, 40000, 0, 0, 1000, 5) == 32767.0f);

	// The three gates over generated walls and columns (design 7, M1 "Host tests"). hw = 200 (a 400-px view),
	// wx 160-320, |o| <= 40 000, t <= 64; three profiles of the vector's size. Restricted to SW's domain:
	// samples with |tx * v.i| >= 2^31 or |o.x + t * v.i| > 32 767 are excluded and counted.
	std::mt19937_64 rng(0xC3D001u);
	auto uni = [&](int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(rng); };
	const int hw = 200;
	unsigned long generated = 0, out_t = 0, excl_wrap = 0, excl_range = 0, judged_a = 0, judged_b = 0, judged_c = 0;
	unsigned fail_a = 0, fail_b = 0, fail_c = 0;
	double worst_a = 0, worst_b = 0;
	const int o_max[3] = {12000, 40000, 40000}, v_max[3] = {2000, 8000, 32767};
	for (int i = 0; i < 900000; ++i)
	{
		const int pr = i % 3;
		const int wx = uni(160, 320);
		const int ox = uni(-o_max[pr], o_max[pr]), oy = uni(-o_max[pr], o_max[pr]);
		const int vi = uni(-v_max[pr], v_max[pr]), vj = uni(-v_max[pr], v_max[pr]);
		const int sx = uni(0, 400);
		++generated;
		// Double-precision 2.2 (the reference for gate (a)).
		const double s = sx - hw;
		const double num = static_cast<double>(wx) * oy - s * ox;
		double den = s * vi - static_cast<double>(wx) * vj;
		if (den == 0)
			den = 1;
		const double t_d = num / den;
		const double x_d = ox + t_d * vi;
		if (std::fabs(t_d) > 64)
		{
			++out_t;
			continue;
		}
		const sw::WorldX sw = sw::world_x(wx, hw, sx, ox, oy, vi, vj);
		if (std::llabs(sw.product) >= (1LL << 31))
		{
			++excl_wrap;
			continue;
		}
		if (std::fabs(x_d) > 32767.0)
		{
			++excl_range;
			continue;
		}
		const double w_f = wall_w(hw, wx, ox, oy, vi, vj, sx);
		const double w_d = std::fabs(x_d);
		const double sw_w = sw.world_x;
		// (a) the accuracy gate.
		++judged_a;
		const double err_a = std::fabs(w_f - w_d);
		worst_a = std::max(worst_a, err_a / (1e-4 * w_d + 0.05));
		if (err_a > 1e-4 * w_d + 0.05)
		{
			if (fail_a++ < 5)
				std::printf("  gate (a): wx %d o (%d,%d) v (%d,%d) sx %d: w_f %.4f w_d %.4f\n", wx, ox, oy, vi, vj, sx, w_f, w_d);
		}
		// SW's own quantisation bound.
		const double t_sw = std::fabs(t_d);
		const double q = 1.0 + std::abs(vi) * (t_sw * ((sw.n_r ? 1.0 / std::abs(sw.n_r) : 0.0) + 1.0 / std::abs(sw.d_r)) + 1.0 / 65536);
		if (q <= 0.5e-3 * sw_w)
		{
			// (b) the kickoff's rel <= 1e-3 vs SW world_x, where SW is precise enough to be judged.
			++judged_b;
			worst_b = std::max(worst_b, std::fabs(w_f - sw_w) / (1e-3 * sw_w));
			if (std::fabs(w_f - sw_w) > 1e-3 * sw_w)
			{
				if (fail_b++ < 5)
					std::printf("  gate (b): wx %d o (%d,%d) v (%d,%d) sx %d: w_f %.4f sw %d q %.3f\n", wx, ox, oy, vi, vj, sx, w_f, sw.world_x, q);
			}
		}
		else
		{
			// (c) elsewhere within SW's own bound plus the float error.
			++judged_c;
			if (std::fabs(w_f - sw_w) > q + err_a)
			{
				if (fail_c++ < 5)
					std::printf("  gate (c): wx %d o (%d,%d) v (%d,%d) sx %d: w_f %.4f sw %d q %.3f\n", wx, ox, oy, vi, vj, sx, w_f, sw.world_x, q);
			}
		}
	}
	std::printf("w gates: %lu samples generated; %lu outside the generator's t <= 64, %lu excluded (|tx*v.i| >= 2^31), "
	            "%lu excluded (|world x| > 32767); judged: (a) %lu, (b) %lu, (c) %lu (a + b + c = %lu)\n",
	            generated, out_t, excl_wrap, excl_range, judged_a, judged_b, judged_c, judged_b + judged_c);
	std::printf("w gates: failures (a) %u (worst %.3f of the bound), (b) %u (worst %.3f of the bound), (c) %u\n",
	            fail_a, worst_a, fail_b, worst_b, fail_c);
	CHECK(judged_a > 100000 && judged_b > 10000 && judged_c > 10000 && judged_b + judged_c == judged_a);
	CHECK(fail_a == 0 && fail_b == 0 && fail_c == 0);
}

static void test_fan_and_key()
{
	uint16_t idx[48];
	std::memset(idx, 0xFF, sizeof idx);
	CHECK(fan_indices(idx, 2, 5) == 0 && fan_indices(idx, 0, 5) == 0 && idx[0] == 0xFFFF);
	CHECK(fan_indices(idx, 3, 10) == 3 && idx[0] == 10 && idx[1] == 11 && idx[2] == 12 && idx[3] == 0xFFFF);
	CHECK(fan_indices(idx, 4, 0) == 6);
	const uint16_t quad[6] = {0, 1, 2, 0, 2, 3};
	CHECK(std::memcmp(idx, quad, sizeof quad) == 0 && idx[6] == 0xFFFF);
	for (int n = 3; n <= 16; ++n)   // 3(n-2) indices, every triangle (base, base+i, base+i+1), all inside [base, base+n)
	{
		const unsigned base = 1000 + n;
		CHECK(fan_indices(idx, n, base) == 3 * (n - 2));
		bool ok = true;
		for (int i = 0; i < n - 2; ++i)
			ok = ok && idx[3 * i] == base && idx[3 * i + 1] == base + i + 1 && idx[3 * i + 2] == base + i + 2;
		CHECK(ok);
	}
	CHECK(fan_indices(idx, 16, 65520) == 42 && idx[41] == 65535);   // u16 at the end of the range

	int tex_a, tex_b, sh_a, sh_b;
	const DrawKey k{kWalls, &tex_a, &sh_a, 0, 3, 0};
	DrawKey k2 = k;
	CHECK(k == k2 && !(k != k2));
	k2 = k; k2.cls = kFloors;                CHECK(!(k == k2) && k != k2);
	k2 = k; k2.texture = &tex_b;             CHECK(!(k == k2));
	k2 = k; k2.shading_tables = &sh_b;       CHECK(!(k == k2));
	k2 = k; k2.transfer_mode = 1;            CHECK(!(k == k2));
	k2 = k; k2.transfer_data = 4;            CHECK(!(k == k2));
	k2 = k; k2.shadeless = 0x0002;           CHECK(!(k == k2));

	// c3d_supports (M2 predicate): (entry, transfer, width, height, rle, shadeless, transparent, textures).
	auto sup = [](Entry e, Transfer t, int w, int h, bool rle, bool shadeless, bool transp, bool tex) {
		return c3d_supports(e, t, w, h, rle, shadeless, transp, tex);
	};
	for (bool tex : {false, true})
	{
		CHECK(sup(Entry::Vertical, Transfer::Textured, 128, 128, false, false, false, tex));
		CHECK(sup(Entry::Horizontal, Transfer::Textured, 128, 128, false, false, false, tex));
		CHECK(sup(Entry::Vertical, Transfer::Textured, 256, 256, false, false, false, tex));
		CHECK(sup(Entry::Horizontal, Transfer::Textured, 512, 512, false, false, false, tex));
		CHECK(!sup(Entry::Horizontal, Transfer::Textured, 1024, 1024, false, false, false, tex));   // above vram_cap
		CHECK(!sup(Entry::Vertical, Transfer::Textured, 64, 64, false, false, false, tex));
		CHECK(!sup(Entry::Vertical, Transfer::Textured, 128, 256, false, false, false, tex));       // not square
		CHECK(!sup(Entry::Vertical, Transfer::Textured, 192, 192, false, false, false, tex));       // not a power of two
		CHECK(!sup(Entry::Vertical, Transfer::Textured, 128, 128, true, false, false, tex));        // RLE (sprites)
		CHECK(!sup(Entry::Vertical, Transfer::Textured, 128, 128, false, true, false, tex));        // shadeless (M3)
		CHECK(!sup(Entry::Horizontal, Transfer::Textured, 128, 128, false, true, false, tex));
		CHECK(!sup(Entry::Rectangle, Transfer::Textured, 128, 128, false, false, false, tex));
		CHECK(!sup(Entry::Vertical, Transfer::BigLandscaped, 128, 128, false, false, false, tex));
		CHECK(!sup(Entry::Horizontal, Transfer::BigLandscaped, 128, 128, false, false, false, tex));
		CHECK(!sup(Entry::Vertical, Transfer::Static, 128, 128, false, false, false, tex));
		CHECK(!sup(Entry::Vertical, Transfer::Tinted, 128, 128, false, false, false, tex));
		CHECK(!sup(Entry::Vertical, Transfer::Other, 128, 128, false, false, false, tex));
	}
	// A _TRANSPARENT_BIT wall: drawn with textures (the alpha test drops texel 0), M1's exclusion without; a
	// floor never skips texel 0, so it is supported either way.
	CHECK(sup(Entry::Vertical, Transfer::Textured, 128, 128, false, false, true, true));
	CHECK(!sup(Entry::Vertical, Transfer::Textured, 128, 128, false, false, true, false));
	CHECK(sup(Entry::Horizontal, Transfer::Textured, 128, 128, false, false, true, false));
	CHECK(sup(Entry::Horizontal, Transfer::Textured, 128, 128, false, false, true, true));
}

static void test_colour_and_cmdbuf()
{
	// The colour hash never produces the sentinel's G and B (so no RGB the sentinel can have, for any k).
	std::set<uint32_t> distinct;
	bool never = true;
	for (unsigned s = 0; s < 65536; ++s)
	{
		const Rgb c = shape_colour(static_cast<uint16_t>(s));
		never = never && !(c.g == 0xC3 && c.b == 0xA5);
		for (unsigned k : {0u, 1u, 77u, 255u})
		{
			const uint32_t argb = 0xFF000000u | (static_cast<uint32_t>(c.r) << 16) | (static_cast<uint32_t>(c.g) << 8) | c.b;
			never = never && argb != cal_sentinel_argb(k);
		}
		distinct.insert((static_cast<uint32_t>(c.r) << 16) | (static_cast<uint32_t>(c.g) << 8) | c.b);
	}
	CHECK(never);
	CHECK(distinct.size() > 60000);   // a hash of 65 536 ids: nearly all different
	std::printf("shape_colour: %zu distinct colours over 65536 shapes\n", distinct.size());
	// The forced case: find a shape whose raw hash is the forbidden pair, if any, is covered by the loop above;
	// the function is deterministic.
	CHECK(shape_colour(1234).r == shape_colour(1234).r && shape_colour(1).b == shape_colour(1).b);

	// Command-buffer arithmetic. P0 = buffer start, P1 = write position before C3D_FrameEnd.
	uint32_t buf[16384];
	const uint32_t size = 16384;
	CmdBufWords c = cmdbuf_words(buf + 4, buf + 1000, false, 0.9f, size);   // empty at P1: usage is stale
	CHECK(c.frame_words == 996 && c.end_words == 0);
	// Offset non-zero: end = lroundf(usage * size) - frame words. usage 0.0625 * 16384 = 1024.
	c = cmdbuf_words(buf + 4, buf + 1000, true, 0.0625f, size);
	CHECK(c.frame_words == 996 && c.end_words == 28);
	// lroundf: 1023.6 -> 1024, 1023.4 -> 1023 (usage = words / size).
	c = cmdbuf_words(buf, buf + 1000, true, 1023.6f / 16384.0f, size);
	CHECK(c.end_words == 24);
	c = cmdbuf_words(buf, buf + 1000, true, 1023.4f / 16384.0f, size);
	CHECK(c.end_words == 23);
	// Rounding half away from zero (lroundf): 1000.5 -> 1001.
	c = cmdbuf_words(buf, buf + 1000, true, 1000.5f / 16384.0f, size);
	CHECK(c.end_words == 1 || c.end_words == 0);   // float(1000.5/16384) * 16384 is 1000.5 or a hair under
	// A usage below the frame's own words (a stale or odd value) records 0, not a wrap-around.
	c = cmdbuf_words(buf, buf + 1000, true, 0.01f, size);
	CHECK(c.frame_words == 1000 && c.end_words == 0);
	// P1 before P0 (cannot happen): 0.
	c = cmdbuf_words(buf + 10, buf + 5, true, 0.5f, size);
	CHECK(c.frame_words == 0);
	// The offset-zero case: nothing recorded for the end whatever the usage says.
	c = cmdbuf_words(buf, buf, false, 1.0f, size);
	CHECK(c.frame_words == 0 && c.end_words == 0);
	c = cmdbuf_words(buf, buf + 8, false, 1.0f, size);
	CHECK(c.frame_words == 8 && c.end_words == 0);

	// The reserve test: size - offset >= draw + end + extra.
	CHECK(reserve_ok(131072, 0, kDrawReserveWords, kEndReserveWords, 0));
	CHECK(kDrawReserveWords == 508 && kEndReserveWords == 20);
	CHECK(reserve_ok(4096, 2048, 1024, 1024, 0) && !reserve_ok(4096, 2049, 1024, 1024, 0));
	CHECK(reserve_ok(4096, 2048 - 5, 1024, 1024, 5) && !reserve_ok(4096, 2048 - 5, 1024, 1024, 6));
	CHECK(!reserve_ok(131072, 0, 1024, 1024, 1000000));   // reserve_extra forces a refusal
	CHECK(!reserve_ok(8192, 8193, 0, 0, 0) && reserve_ok(8192, 8192, 0, 0, 0));   // offset beyond size: refused
	CHECK(!reserve_ok(0xFFFFFFFFu, 0, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu));   // no 32-bit wrap
	CHECK(reserve_ok(8192, 0, 4096, 4096, 0) && !reserve_ok(8192, 0, 4096, 4097, 0));
	CHECK(sizeof(Vertex) == 32 && offsetof(Vertex, u0) == 12 && offsetof(Vertex, r) == 28);
}

static void test_config_m1()
{
	Config d;
	CHECK(d.cmdbuf_kib == 512 && d.gxq_budget == 24 && d.ring_vertices == 1024 && d.ring_indices == 2048);
	CHECK(d.reserve_extra == 0 && d.film.empty());

	Config c;
	CHECK(config_apply(c, "cmdbuf_kib", "32") && c.cmdbuf_kib == 32);
	CHECK(config_apply(c, "cmdbuf_kib", "16") && c.cmdbuf_kib == 16);
	CHECK(config_apply(c, "cmdbuf_kib", "4096") && c.cmdbuf_kib == 4096);
	CHECK(!config_apply(c, "cmdbuf_kib", "15") && c.cmdbuf_kib == 4096);
	CHECK(!config_apply(c, "cmdbuf_kib", "4097") && !config_apply(c, "cmdbuf_kib", "") && !config_apply(c, "cmdbuf_kib", "-1"));
	CHECK(config_apply(c, "gxq_budget", "4") && c.gxq_budget == 4);
	CHECK(config_apply(c, "gxq_budget", "1") && config_apply(c, "gxq_budget", "24") && c.gxq_budget == 24);
	CHECK(!config_apply(c, "gxq_budget", "0") && !config_apply(c, "gxq_budget", "25") && c.gxq_budget == 24);
	CHECK(config_apply(c, "ring_vertices", "64") && c.ring_vertices == 64);
	CHECK(config_apply(c, "ring_vertices", "32768") && !config_apply(c, "ring_vertices", "32769") && !config_apply(c, "ring_vertices", "63"));
	CHECK(config_apply(c, "ring_indices", "64") && config_apply(c, "ring_indices", "65536") && c.ring_indices == 65536);
	CHECK(!config_apply(c, "ring_indices", "65537") && !config_apply(c, "ring_indices", "63"));
	CHECK(config_apply(c, "reserve_extra", "0") && config_apply(c, "reserve_extra", "1000000") && c.reserve_extra == 1000000);
	CHECK(!config_apply(c, "reserve_extra", "1000001") && !config_apply(c, "reserve_extra", "x"));

	// film: the rest of the line, trimmed; spaces kept inside; '#' starts a comment.
	Config f;
	std::vector<std::string> problems;
	config_parse(f,
	             "renderer c3d\n"
	             "film   M2 L01 Waterloo Waterpark.46857.filA   \n"
	             "diff 2\n",
	             problems);
	CHECK(problems.empty() && f.film == "M2 L01 Waterloo Waterpark.46857.filA" && f.diff == 2 && f.renderer_c3d);
	config_parse(f, "film\tc3d-film.filA   # a comment, not part of the name\n", problems);
	CHECK(problems.empty() && f.film == "c3d-film.filA");
	config_parse(f, "film sdmc:/3ds/alephone/my film.filA\n", problems);
	CHECK(problems.empty() && f.film == "sdmc:/3ds/alephone/my film.filA");
	config_parse(f, "film  /films/a b  c.filA\r\n", problems);
	CHECK(problems.empty() && f.film == "/films/a b  c.filA");   // inner runs of spaces kept, the CR trimmed
	config_parse(f, "film a#b.filA\n", problems);   // '#' is not supported inside a path: the name is cut there
	CHECK(problems.empty() && f.film == "a");
	// An empty value is rejected and leaves the old one.
	config_parse(f, "film\n", problems);
	CHECK(problems.size() == 1 && f.film == "a");
	config_parse(f, "film    # nothing but a comment\n", problems);
	CHECK(problems.size() == 2 && f.film == "a");
	problems.clear();
	// The other keys still read one token, and the new ones work in a file.
	Config m;
	config_parse(m, "cmdbuf_kib 32\ngxq_budget 4  # the queue\nring_vertices 64\nring_indices 128\nreserve_extra 5000\nmode readback\n", problems);
	CHECK(problems.empty() && m.cmdbuf_kib == 32 && m.gxq_budget == 4 && m.ring_vertices == 64 && m.ring_indices == 128);
	CHECK(m.reserve_extra == 5000 && m.mode == Mode::Readback);
	config_parse(m, "gxq_budget 25\nring_vertices 10\n", problems);
	CHECK(problems.size() == 2 && m.gxq_budget == 4 && m.ring_vertices == 64);
}

// A random convex polygon (or a degenerate one) of 3-8 integer vertices for the coverage model.
static std::vector<Point2> random_polygon(std::mt19937_64& rng, int W, int H)
{
	auto uni = [&](int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(rng); };
	// Box size log-uniform: many small boxes (many exact ties), some as large as the view.
	const double lg = std::uniform_real_distribution<double>(std::log(3.0), std::log(static_cast<double>(W)))(rng);
	const int bw = std::min(W, static_cast<int>(std::exp(lg))), bh = std::min(H, std::max(2, static_cast<int>(std::exp(lg) * 0.6)));
	const int bx = uni(0, W - bw), by = uni(0, H - bh);
	const int count = uni(3, 8);
	std::vector<Point2> pts;
	for (int i = 0; i < count; ++i)
		pts.push_back({bx + uni(0, bw), by + uni(0, bh)});
	std::vector<Point2> poly;
	if (uni(0, 9) == 0)
		poly = pts;   // anything goes: duplicates, collinear, concave, wound either way
	else
	{
		// Convex hull, counter-clockwise in math orientation (monotone chain, collinear dropped).
		std::vector<Point2> p = pts;
		std::sort(p.begin(), p.end(), [](const Point2& a, const Point2& b) { return a.x != b.x ? a.x < b.x : a.y < b.y; });
		p.erase(std::unique(p.begin(), p.end(), [](const Point2& a, const Point2& b) { return a.x == b.x && a.y == b.y; }), p.end());
		auto cross = [](const Point2& o, const Point2& a, const Point2& b) {
			return static_cast<long long>(a.x - o.x) * (b.y - o.y) - static_cast<long long>(a.y - o.y) * (b.x - o.x);
		};
		std::vector<Point2> hull;
		if (p.size() >= 3)
		{
			hull.resize(2 * p.size());
			size_t k = 0;
			for (size_t i = 0; i < p.size(); ++i)
			{
				while (k >= 2 && cross(hull[k - 2], hull[k - 1], p[i]) <= 0)
					--k;
				hull[k++] = p[i];
			}
			for (size_t i = p.size() - 1, t = k + 1; i > 0; --i)
			{
				while (k >= t && cross(hull[k - 2], hull[k - 1], p[i - 1]) <= 0)
					--k;
				hull[k++] = p[i - 1];
			}
			hull.resize(k - 1);
		}
		poly = hull.size() >= 3 ? hull : pts;
		// Sometimes insert the midpoint of an edge (a collinear vertex), as clipping can produce.
		if (poly.size() < 8 && hull.size() >= 3 && uni(0, 3) == 0)
		{
			const size_t e = static_cast<size_t>(uni(0, static_cast<int>(poly.size()) - 1));
			const Point2 a = poly[e], b = poly[(e + 1) % poly.size()];
			if (((a.x + b.x) % 2) == 0 && ((a.y + b.y) % 2) == 0)
				poly.insert(poly.begin() + static_cast<long>(e) + 1, Point2{(a.x + b.x) / 2, (a.y + b.y) / 2});
		}
	}
	if (uni(0, 1))
		std::reverse(poly.begin(), poly.end());
	const int rot = uni(0, static_cast<int>(poly.size()) - 1);
	std::rotate(poly.begin(), poly.begin() + rot, poly.end());
	return poly;
}

// Every vertex on one side of (or on) every edge: `clockwise` = the side of positive shoelace area in
// y-down space (cross >= 0 below); otherwise either side. Repeated vertices are fine (zero-length edges skip).
static bool is_convex(const std::vector<Point2>& poly, bool clockwise)
{
	const int n = static_cast<int>(poly.size());
	bool pos = false, neg = false;
	for (int i = 0; i < n; ++i)
	{
		const Point2 &a = poly[i], &b = poly[(i + 1) % n];
		for (int j = 0; j < n; ++j)
		{
			const long long cr = static_cast<long long>(b.x - a.x) * (poly[j].y - a.y) - static_cast<long long>(b.y - a.y) * (poly[j].x - a.x);
			pos = pos || cr > 0;
			neg = neg || cr < 0;
		}
	}
	return clockwise ? !neg : !(pos && neg);
}

namespace sw
{
// scottish_textures.cpp:1173-1223 (build_x_table), on shorts as in the original: for each row of the edge
// (x0,y0)-(x1,y1), y0 < y1, the x the horizontal mapper starts or stops the row at. NULL = refused (dy < 0).
static short* build_x_table(short* table, short x0, short y0, short x1, short y1)
{
	short dx, dy, adx, ady;
	short x;
	short d, delta_d, d_max;
	short* record;
	auto sgn = [](short v) -> short { return v < 0 ? -1 : (v > 0 ? 1 : 0); };

	dx = x1 - x0, adx = std::abs(dx), dx = sgn(dx);
	dy = y1 - y0, ady = std::abs(dy), dy = sgn(dy);
	if (dy > 0)
	{
		x = x0;
		record = table;
		if (adx >= ady)
		{
			d = adx - ady, delta_d = -2 * ady, d_max = 2 * adx;
			while ((adx -= 1) >= 0)
			{
				if (d < 0)
					d += d_max, *record++ = x, ady -= 1;
				x += dx, d += delta_d;
			}
			if (ady == 1)
				*record++ = x;
		}
		else
		{
			d = ady - adx, delta_d = -2 * adx, d_max = 2 * ady;
			while ((ady -= 1) >= 0)
			{
				if (d < 0)
					x += dx, d += d_max;
				*record++ = x;
				d += delta_d;
			}
		}
	}
	else if (dy < 0)
		return nullptr;
	return table;
}

// scottish_textures.cpp:213-300 (texture_horizontal_polygon) down to the tables, then the row loop of
// low_level_textures.h:200-225: `cov` gets 1 for every written pixel of rows [x0, x1). If `rows` is given it
// receives, per written row r (from the highest vertex), the (y, x0, x1) of the row. `wrote_outside` as above.
struct HRow
{
	int y, x0, x1;
};
static Result polygon_coverage_h_rows(const std::vector<Point2>& pts, int w, int h, std::vector<uint8_t>& cov,
                                      unsigned& wrote_outside, std::vector<HRow>* rows)
{
	const int n = static_cast<int>(pts.size());
	int highest = 0, lowest = 0;
	for (int v = 0; v < n; ++v)
	{
		if (!(pts[v].x >= 0 && pts[v].x <= w && pts[v].y >= 0 && pts[v].y <= h))
			return RefusedBounds;
		if (pts[v].y < pts[highest].y)
			highest = v;
		else if (pts[v].y > pts[lowest].y)
			lowest = v;
	}
	if (highest == lowest)
		return RefusedDegenerate;

	std::vector<short> left_table(8192, -1), right_table(8192, -1);
	short left_line_count = 0, right_line_count = 0, total_line_count;
	short aggregate_left = 0, aggregate_right = 0;
	short left_vertex = static_cast<short>(highest), right_vertex = static_cast<short>(highest), vertex;
	total_line_count = static_cast<short>(pts[lowest].y - pts[highest].y);
	const short aggregate_total = total_line_count;
	while (total_line_count > 0)
	{
		if (left_line_count <= 0)
		{
			do
			{
				vertex = left_vertex ? static_cast<short>(left_vertex - 1) : static_cast<short>(n - 1);
				left_line_count = static_cast<short>(pts[vertex].y - pts[left_vertex].y);
				if (!build_x_table(left_table.data() + aggregate_left, static_cast<short>(pts[left_vertex].x),
				                   static_cast<short>(pts[left_vertex].y), static_cast<short>(pts[vertex].x),
				                   static_cast<short>(pts[vertex].y)))
					return RefusedWalk;
				aggregate_left = static_cast<short>(aggregate_left + left_line_count);
				left_vertex = vertex;
			} while (!left_line_count);
		}
		if (right_line_count <= 0)
		{
			do
			{
				vertex = (right_vertex == n - 1) ? 0 : static_cast<short>(right_vertex + 1);
				right_line_count = static_cast<short>(pts[vertex].y - pts[right_vertex].y);
				if (!build_x_table(right_table.data() + aggregate_right, static_cast<short>(pts[right_vertex].x),
				                   static_cast<short>(pts[right_vertex].y), static_cast<short>(pts[vertex].x),
				                   static_cast<short>(pts[vertex].y)))
					return RefusedWalk;
				aggregate_right = static_cast<short>(aggregate_right + right_line_count);
				right_vertex = vertex;
			} while (!right_line_count);
		}
		const short delta = std::min(left_line_count, right_line_count);
		total_line_count = static_cast<short>(total_line_count - delta);
		left_line_count = static_cast<short>(left_line_count - delta);
		right_line_count = static_cast<short>(right_line_count - delta);
	}
	for (int r = 0; r < aggregate_total; ++r)
	{
		const int y = pts[highest].y + r;
		const int x0 = left_table[r], x1 = right_table[r];
		if (rows)
			rows->push_back({y, x0, x1});
		for (int x = x0; x < x1; ++x)
		{
			if (y < 0 || y >= h || x < 0 || x >= w)
				++wrote_outside;
			else
				cov[static_cast<size_t>(y) * w + x] = 1;
		}
	}
	return Drawn;
}
static Result polygon_coverage_h(const std::vector<Point2>& pts, int w, int h, std::vector<uint8_t>& cov,
                                 unsigned& wrote_outside)
{
	return polygon_coverage_h_rows(pts, w, h, cov, wrote_outside, nullptr);
}
} // namespace sw

using CoverFn = sw::Result (*)(const std::vector<Point2>&, int, int, std::vector<uint8_t>&, unsigned&);

// Random polygons, concave and degenerate included: the acceptance status is the SW walk's, and the pixels
// are the interior's by the pixel-centre rule, apart from the exact ties (counted apart).
static void coverage_model(Orient orient, CoverFn cover, uint64_t seed, const char* label)
{
	const int W = 400, H = 240;
	std::mt19937_64 rng(seed);
	unsigned long n_poly = 0, n_draw = 0, n_walk = 0, n_bounds = 0, n_degen = 0, n_area = 0;
	unsigned long walk_mismatch = 0, area_sw_wrote = 0, refused_sw_wrote = 0, outside_writes = 0;
	unsigned long identical_px = 0, tie_px = 0, tie_sw_covered = 0, tie_sw_clear = 0, bad_px = 0, drawn_cmp = 0, empty_drawn = 0;
	std::vector<uint8_t> cov(static_cast<size_t>(W) * H);
	for (int it = 0; it < 12000; ++it)
	{
		std::vector<Point2> poly = random_polygon(rng, W, H);
		if (it % 40 == 7)   // out of the bounds now and then
			poly[0].x = (it & 1) ? -1 : W + 1;
		const int n = static_cast<int>(poly.size());
		const Accept a = accept_polygon(poly.data(), n, W, H, orient);
		++n_poly;
		n_draw += a == Accept::Draw; n_walk += a == Accept::Walk; n_bounds += a == Accept::Bounds;
		n_degen += a == Accept::Degenerate; n_area += a == Accept::Area;

		std::fill(cov.begin(), cov.end(), 0);
		unsigned wrote_outside = 0;
		const sw::Result sr = cover(poly, W, H, cov, wrote_outside);
		outside_writes += wrote_outside;
		size_t written = 0;
		for (uint8_t v : cov)
			written += v;

		// The statuses that precede the area agree with SW's refusals one for one.
		const bool sw_refused = sr != sw::Drawn;
		if ((a == Accept::Bounds) != (sr == sw::RefusedBounds) || (a == Accept::Degenerate) != (sr == sw::RefusedDegenerate) ||
		    (a == Accept::Walk) != (sr == sw::RefusedWalk))
			++walk_mismatch;
		if (sw_refused && written)
			++refused_sw_wrote;
		if (a == Accept::Area)
		{
			// "SW then has y1 < y0 in every column and writes nothing" -- for the convex ones. A concave polygon of
			// negative area can still have pixels where its tables cross; those are not what the engine feeds.
			if (written && is_convex(poly, false))
				++area_sw_wrote;
			continue;
		}
		if (a != Accept::Draw)
			continue;

		// Ideal pixel-centre rasterisation of the fan = the polygon's interior, in doubled coordinates.
		// Only for convex polygons (all cross products of consecutive edges the same sign).
		if (!is_convex(poly, true))
			continue;
		// Polygons with a repeated vertex are fan-degenerate but still convex: zero-length edges are skipped.
		int minx = W, maxx = 0, miny = H, maxy = 0;
		for (const Point2& p : poly)
			minx = std::min(minx, p.x), maxx = std::max(maxx, p.x), miny = std::min(miny, p.y), maxy = std::max(maxy, p.y);
		++drawn_cmp;
		const unsigned long tie_before = tie_px;
		unsigned covered_here = 0;
		for (int y = miny; y < std::min(maxy + 1, H); ++y)
			for (int x = minx; x < std::min(maxx + 1, W); ++x)
			{
				const long long px = 2LL * x + 1, py = 2LL * y + 1;
				bool neg = false, zero = false;
				for (int i = 0; i < n; ++i)
				{
					const Point2 &p0 = poly[i], &p1 = poly[(i + 1) % n];
					if (p0.x == p1.x && p0.y == p1.y)
						continue;
					const long long cr = (2LL * (p1.x - p0.x)) * (py - 2LL * p0.y) - (2LL * (p1.y - p0.y)) * (px - 2LL * p0.x);
					neg = neg || cr < 0;
					zero = zero || cr == 0;
				}
				const bool swc = cov[static_cast<size_t>(y) * W + x] != 0;
				if (neg)
				{
					// Outside the polygon: SW must not have written it.
					if (swc)
						++bad_px;
					else
						++identical_px;
				}
				else if (zero)
				{
					++tie_px;
					(swc ? tie_sw_covered : tie_sw_clear) += 1;
				}
				else
				{
					covered_here += 1;
					if (swc)
						++identical_px;
					else
						++bad_px;
				}
			}
		// The same definition as tie_pixels (M1 piece 5): the counts agree polygon by polygon.
		if (tie_pixels(poly.data(), n, W, H, nullptr, nullptr) != static_cast<int>(tie_px - tie_before))
			++bad_px;
		// SW wrote nothing outside the box, either.
		size_t in_box = 0;
		for (int y = miny; y < std::min(maxy + 1, H); ++y)
			for (int x = minx; x < std::min(maxx + 1, W); ++x)
				in_box += cov[static_cast<size_t>(y) * W + x];
		if (in_box != written)
			++bad_px;
		if (!covered_here)
			++empty_drawn;
	}
	std::printf("%s: %lu polygons: %lu drawn (acceptance), %lu walk, %lu bounds, %lu degenerate, %lu area;"
	            " %lu convex drawn compared, %lu of them with no strictly interior pixel\n",
	            label, n_poly, n_draw, n_walk, n_bounds, n_degen, n_area, drawn_cmp, empty_drawn);
	std::printf("%s: %lu pixels identical, %lu exact-tie pixels counted apart (SW covered %lu, SW clear %lu), "
	            "%lu mismatches\n", label, identical_px, tie_px, tie_sw_covered, tie_sw_clear, bad_px);
	CHECK(n_draw > 2000 && n_walk > 0 && n_bounds > 0 && n_degen > 0 && n_area > 1000);
	std::printf("%s: acceptance vs SW refusals differ %lu; SW wrote after a refusal %lu, after area <= 0 %lu, outside the view %lu\n",
	            label, walk_mismatch, refused_sw_wrote, area_sw_wrote, outside_writes);
	CHECK(walk_mismatch == 0 && refused_sw_wrote == 0 && area_sw_wrote == 0 && outside_writes == 0);
	CHECK(bad_px == 0 && drawn_cmp > 1500);
	CHECK(tie_px > 0);
}

static void test_acceptance_and_coverage()
{
	const int W = 400, H = 240;
	// Hand-made cases.
	auto acc = [&](std::vector<Point2> v) { return accept_polygon(v.data(), static_cast<int>(v.size()), W, H); };
	CHECK(acc({{0, 0}, {10, 0}, {10, 10}, {0, 10}}) == Accept::Draw);        // clockwise on screen (y down)
	CHECK(acc({{0, 0}, {0, 10}, {10, 10}, {10, 0}}) == Accept::Area);        // counter-clockwise
	CHECK(acc({{0, 0}, {400, 0}, {400, 240}, {0, 240}}) == Accept::Draw);    // the whole view: the bounds are inclusive
	CHECK(acc({{-1, 0}, {10, 0}, {10, 10}}) == Accept::Bounds);
	CHECK(acc({{0, 0}, {401, 0}, {10, 10}}) == Accept::Bounds);
	CHECK(acc({{0, 0}, {10, -1}, {10, 10}}) == Accept::Bounds);
	CHECK(acc({{0, 0}, {10, 241}, {10, 10}}) == Accept::Bounds);
	CHECK(acc({{5, 0}, {5, 10}, {5, 20}}) == Accept::Degenerate);            // min x == max x
	CHECK(acc({{-5, 0}, {5, 10}, {5, 20}}) == Accept::Bounds);               // the bounds test comes first
	CHECK(acc({{0, 0}, {5, 5}}) == Accept::Degenerate);
	CHECK(acc({{0, 0}, {10, 5}, {20, 10}}) == Accept::Area);                 // collinear: zero area
	CHECK(acc({{0, 0}, {20, 0}, {0, 10}}) == Accept::Draw);                  // clockwise on screen
	CHECK(acc({{0, 0}, {0, 10}, {20, 0}}) == Accept::Area);                  // counter-clockwise on screen
	CHECK(acc({{0, 0}, {20, 0}, {0, 10}, {0, 5}}) == Accept::Draw);          // a collinear vertex on the left edge
	// A walk build_y_table refuses: a concave "S" whose clockwise walk from the leftmost vertex runs back in x.
	CHECK(acc({{0, 0}, {10, 0}, {5, 5}, {10, 10}, {0, 10}}) == Accept::Draw);        // concave but walkable
	CHECK(acc({{0, 0}, {10, 0}, {5, 5}, {20, 10}, {0, 10}}) == Accept::Walk);        // 1 -> 2 runs back in x

	coverage_model(Orient::Vertical, sw::polygon_coverage, 0xC3D002u, "coverage model");
}

// The same for rows: texture_horizontal_polygon against accept_polygon(Horizontal) + pixel-centre rasterisation
// (M2 specification, "Floor coverage rule").
static void test_acceptance_and_coverage_horizontal()
{
	const int W = 400, H = 240;
	auto acc = [&](std::vector<Point2> v) { return accept_polygon(v.data(), static_cast<int>(v.size()), W, H, Orient::Horizontal); };
	CHECK(acc({{0, 0}, {10, 0}, {10, 10}, {0, 10}}) == Accept::Draw);        // clockwise on screen (y down)
	CHECK(acc({{0, 0}, {0, 10}, {10, 10}, {10, 0}}) == Accept::Area);        // counter-clockwise
	CHECK(acc({{0, 0}, {400, 0}, {400, 240}, {0, 240}}) == Accept::Draw);
	CHECK(acc({{-1, 0}, {10, 0}, {10, 10}}) == Accept::Bounds);
	CHECK(acc({{0, 0}, {10, 241}, {10, 10}}) == Accept::Bounds);
	CHECK(acc({{0, 5}, {10, 5}, {20, 5}}) == Accept::Degenerate);            // min y == max y
	CHECK(acc({{0, 0}, {10, 5}, {20, 10}}) == Accept::Area);                 // collinear
	CHECK(acc({{0, 0}, {20, 0}, {0, 10}}) == Accept::Draw);
	CHECK(acc({{0, 0}, {0, 10}, {20, 0}}) == Accept::Area);
	coverage_model(Orient::Horizontal, sw::polygon_coverage_h, 0xC3D003u, "horizontal coverage model");
}

// tie_pixels vs a brute-force scan of every pixel centre against every edge, on generated polygons; and
// against the coverage model's definition (the centre on an edge line and inside-or-on every other edge).
static void tie_cb(void* ctx, int k, int m) { static_cast<std::vector<std::pair<int, int>>*>(ctx)->push_back({k, m}); }

static void test_tie_pixels()
{
	const int W = 40, H = 30;
	std::mt19937_64 rng(0x71E5u);
	unsigned long polys = 0, ties = 0, model_ties = 0, convex = 0;
	for (int it = 0; it < 20000; ++it)
	{
		std::vector<Point2> poly = random_polygon(rng, W, H);
		if (it % 17 == 3)
			poly[0].x = -3;   // clipping: vertices outside the view
		const int n = static_cast<int>(poly.size());
		std::vector<std::pair<int, int>> got;
		const int cnt = tie_pixels(poly.data(), n, W, H, tie_cb, &got);
		CHECK(cnt == static_cast<int>(got.size()));
		std::vector<uint8_t> have(static_cast<size_t>(W) * H, 0), want(static_cast<size_t>(W) * H, 0);
		for (auto& g : got)
		{
			CHECK(g.first >= 0 && g.first < W && g.second >= 0 && g.second < H);
			++have[static_cast<size_t>(g.second) * W + g.first];
		}
		// Brute force: per edge, every pixel centre in the x-span, doubled coordinates.
		for (int i = 0; i < n; ++i)
		{
			Point2 p0 = poly[i], p1 = poly[(i + 1) % n];
			if (p0.x > p1.x)
				std::swap(p0, p1);
			for (int y = 0; y < H; ++y)
				for (int x = 0; x < W; ++x)
				{
					if (x < p0.x || x >= p1.x)
						continue;
					const long long cr = (2LL * (p1.x - p0.x)) * (2LL * y + 1 - 2LL * p0.y) -
					                     (2LL * (p1.y - p0.y)) * (2LL * x + 1 - 2LL * p0.x);
					if (cr == 0)
						++want[static_cast<size_t>(y) * W + x];
				}
		}
		CHECK(have == want);
		++polys;
		for (uint8_t v : have)
			ties += v;
		// The coverage model's definition, for convex polygons inside the view: counts must match.
		if (poly[0].x < 0 || accept_polygon(poly.data(), n, W, H) != Accept::Draw || !is_convex(poly, true))
			continue;   // the coverage test's population: accepted convex polygons
		bool inside_view = true;
		for (const Point2& p : poly)
			inside_view = inside_view && p.x >= 0 && p.x <= W && p.y >= 0 && p.y <= H;
		if (!inside_view)
			continue;
		++convex;
		unsigned long model = 0, mine = 0;
		for (int y = 0; y < H; ++y)
			for (int x = 0; x < W; ++x)
			{
				const long long px = 2LL * x + 1, py = 2LL * y + 1;
				bool neg = false, zero = false;
				for (int e = 0; e < n; ++e)
				{
					const Point2 &q0 = poly[e], &q1 = poly[(e + 1) % n];
					if (q0.x == q1.x && q0.y == q1.y)
						continue;
					const long long cr = (2LL * (q1.x - q0.x)) * (py - 2LL * q0.y) - (2LL * (q1.y - q0.y)) * (px - 2LL * q0.x);
					neg = neg || cr < 0;
					zero = zero || cr == 0;
				}
				if (!neg && zero)
					++model;
			}
		for (uint8_t v : have)
			mine += v != 0;
		model_ties += model;
		CHECK(model == mine);
	}
	std::printf("tie pixels: %lu polygons, %lu tie pixels (per edge, brute force identical); %lu convex in-view polygons: "
	            "coverage-model exact-tie count %lu == tie_pixels count\n", polys, ties, convex, model_ties);
	CHECK(ties > 0 && convex > 500);
	// The film's sliver: its top edge (346,96)-(347,97) passes through the centre (346.5, 96.5).
	const Point2 sl[4] = {{346, 96}, {347, 97}, {347, 125}, {346, 127}};
	std::vector<std::pair<int, int>> got;
	tie_pixels(sl, 4, 400, 240, tie_cb, &got);
	bool has = false;
	for (auto& g : got)
		has = has || (g.first == 346 && g.second == 96);
	CHECK(has);
}

static void test_ring()
{
	RingState r;
	r.configure(100, 200);
	r.begin_frame();
	CHECK(r.half() == 0 && r.vertices_used() == 0 && r.index_used() == 0);
	CHECK(r.fits(100, 200) && !r.fits(101, 0) && !r.fits(0, 201));
	CHECK(r.take(4, 6) == 0 && r.take(3, 3) == 4);
	CHECK(r.vertices_used() == 7 && r.index_used() == 9);
	CHECK(r.fits(93, 191) && !r.fits(94, 0) && !r.fits(0, 192));
	r.begin_frame();
	CHECK(r.half() == 1 && r.vertices_used() == 0 && r.index_used() == 0);
	CHECK(r.take(10, 24) == 0);
	r.begin_frame();
	CHECK(r.half() == 0);
	CHECK(r.take(2, 3) == 0);
	// high water is per half and survives the flip
	CHECK(r.high_water_vertices(0) == 7 && r.high_water_indices(0) == 9);
	CHECK(r.high_water_vertices(1) == 10 && r.high_water_indices(1) == 24);
	// a full half fits exactly, one more does not
	RingState f;
	f.configure(64, 64);
	f.begin_frame();
	f.take(64, 64);
	CHECK(!f.fits(1, 0) && !f.fits(0, 1) && f.fits(0, 0));
}

// ---------------------------------------------------------------------------------------------
// M2 parts 1-2: texture conversion, tiling, the texture cache (design 7, "M2 specification")

static void test_config_m2()
{
	Config d;
	CHECK(d.vram_cap == 3584 && d.linear_cap == 4096 && d.promote_max == 8);
	Config c;
	CHECK(config_apply(c, "vram_cap", "0") && c.vram_cap == 0);
	CHECK(config_apply(c, "vram_cap", "512") && config_apply(c, "vram_cap", "4096") && c.vram_cap == 4096);
	CHECK(!config_apply(c, "vram_cap", "4097") && !config_apply(c, "vram_cap", "-1") && !config_apply(c, "vram_cap", "") && c.vram_cap == 4096);
	CHECK(config_apply(c, "linear_cap", "64") && c.linear_cap == 64);
	CHECK(config_apply(c, "linear_cap", "16384") && c.linear_cap == 16384);
	CHECK(!config_apply(c, "linear_cap", "63") && !config_apply(c, "linear_cap", "16385") && !config_apply(c, "linear_cap", "x") && c.linear_cap == 16384);
	CHECK(config_apply(c, "promote_max", "0") && c.promote_max == 0);
	CHECK(config_apply(c, "promote_max", "8") && c.promote_max == 8);
	CHECK(!config_apply(c, "promote_max", "9") && !config_apply(c, "promote_max", "-1") && c.promote_max == 8);
	Config p;
	std::vector<std::string> problems;
	config_parse(p, "vram_cap 512  # KiB\nlinear_cap 1024\npromote_max 0\nvram_cap 99999\n", problems);
	CHECK(problems.size() == 1 && p.vram_cap == 512 && p.linear_cap == 1024 && p.promote_max == 0);
}

// --- tables built like shapes.cpp's build_shading_tables32 (number_of_shading_tables = 256, SDL_MapRGB
// on pixel_format_32 = ARGB8888: 0xFF000000 | R << 16 | G << 8 | B)

struct TestColor { unsigned r, g, b; bool selfLum; };

static std::vector<uint32_t> build_tables32(const TestColor* colors, const uint8_t* remap)
{
	std::vector<uint32_t> t(256 * 256, 0);
	for (unsigned i = 0; i < 256; ++i)
		for (unsigned level = 0; level < 256; ++level)
		{
			const TestColor& c = colors[remap ? remap[i] : i];
			const unsigned mult = c.selfLum ? (128 + (level >> 1)) : level;   // shapes.cpp:1946
			t[256 * level + i] = 0xFF000000u | (((c.r * mult / 255) >> 8) << 16) | (((c.g * mult / 255) >> 8) << 8) |
			                     ((c.b * mult / 255) >> 8);
		}
	return t;
}

// 256 colours: a spread, plus the special cases at 250..253 (0 is a normal colour with G = 255).
static std::vector<TestColor> make_colors()
{
	std::vector<TestColor> c(256);
	for (unsigned i = 0; i < 256; ++i)
		c[i] = TestColor{i * 257u, (255 - i) * 257u, ((i * 7) & 255) * 257u, i % 5 == 0};
	c[250] = TestColor{0, 0, 0, false};          // black normal
	c[251] = TestColor{0, 0, 0, true};           // black self-luminous
	c[252] = TestColor{0x0100, 0, 0, true};      // dark self-luminous: level 0 rounds to black
	c[253] = TestColor{0x4000, 0x2000, 0, true}; // self-luminous with a visible level 0
	c[254] = TestColor{0xFFFF, 0xFFFF, 0xFFFF, false};
	c[255] = TestColor{0x1234, 0x5678, 0x9ABC, false};
	return c;
}

// Independent expectation, from the colour itself (top level 255 or 0), not from the table.
static uint32_t expect_word(const TestColor& c, TexKind kind, bool zero_index)
{
	auto ch = [&](unsigned v, unsigned mult) { return (v * mult / 255) >> 8; };
	const unsigned top = 255;                                  // the top table: multiplier 255 either way
	const unsigned lvl0 = c.selfLum ? 128 : 0;
	const bool lum = ch(c.r, lvl0) || ch(c.g, lvl0) || ch(c.b, lvl0);
	switch (kind)
	{
		case TexKind::Normal:
		case TexKind::NormalKeyed:
		{
			const unsigned a = (kind == TexKind::NormalKeyed && zero_index) ? 0 : (lum ? 128 : 255);
			return (ch(c.r, top) << 24) | (ch(c.g, top) << 16) | (ch(c.b, top) << 8) | a;
		}
		default:
			return 0;
	}
}

struct TestBitmap
{
	unsigned W;
	std::vector<uint8_t> data;
	std::vector<const uint8_t*> rows;
	explicit TestBitmap(unsigned w) : W(w), data(w * w), rows(w)
	{
		for (unsigned j = 0; j < w; ++j)
		{
			rows[j] = &data[j * w];
			for (unsigned s = 0; s < w; ++s)
				data[j * w + s] = static_cast<uint8_t>(s * 131 + j * 7 + (s >> 3) * (j >> 2) + (j == 3 && s < 8 ? 250 + (s & 3) : 0));
		}
		data[0] = 0;                          // index 0 at (0, 0) and a few more
		data[w * 5 + 9] = 0;
		data[w * (w - 1) + (w - 1)] = 0;
	}
	TexBitmap view() const { return TexBitmap{static_cast<int>(W), static_cast<int>(W), rows.data(), false}; }
};

static uint32_t read_word(const std::vector<uint8_t>& out, unsigned W, unsigned j, unsigned s)
{
	const uint8_t* p = &out[tile_index(W, tile_memory_row(W, j), s) * 4];   // little-endian u32
	const uint32_t w = static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
	                   (static_cast<uint32_t>(p[3]) << 24);
	return w;
}

static void test_tex_supported()
{
	CHECK(tex_supported(128, 128, false) && tex_supported(256, 256, false) && tex_supported(512, 512, false));
	CHECK(!tex_supported(128, 128, true));                      // RLE: sprites, M4
	CHECK(!tex_supported(64, 64, false) && !tex_supported(1024, 1024, false) && !tex_supported(0, 0, false));
	CHECK(!tex_supported(192, 192, false) && !tex_supported(130, 130, false) && !tex_supported(-128, -128, false));
	CHECK(!tex_supported(128, 256, false) && !tex_supported(256, 128, false) && !tex_supported(270, 512, false));
	const TexBitmap b{512, 512, nullptr, false};
	CHECK(tex_supported(b));
	CHECK(tex_bytes(128, TexKind::Normal) == 65536 && tex_bytes(512, TexKind::NormalKeyed) == 1048576);
	CHECK(tex_bytes(128, TexKind::Mask) == 16384);
	std::vector<uint8_t> out(16);
	const std::vector<uint32_t> t(kShadingTableEntries, 0);
	CHECK(!tex_convert(b, t.data(), TexKind::Normal, out.data()));   // rows null
	TestBitmap odd(128);
	CHECK(!tex_convert(odd.view(), nullptr, TexKind::Normal, out.data()));   // tables needed
	CHECK(!tex_convert(TexBitmap{64, 64, odd.rows.data(), false}, t.data(), TexKind::Normal, out.data()));
}

static void test_tex_convert()
{
	const std::vector<TestColor> colors = make_colors();
	const std::vector<uint32_t> tables = build_tables32(colors.data(), nullptr);

	// the table shape the comments claim: 0xFF000000 | rgb, black normal is 0xFF000000 at every level
	CHECK(tables[256 * 255 + 250] == 0xFF000000u && tables[250] == 0xFF000000u);
	CHECK(tables[251] == 0xFF000000u && tables[256 * 255 + 251] == 0xFF000000u);   // black SL
	CHECK(tables[252] == 0xFF000000u && tables[256 * 255 + 252] == 0xFF010000u);   // dark SL: black at 0, not at 255
	CHECK((tables[253] & 0xFFFFFF) != 0);                                         // visible SL at level 0
	CHECK(tables[256 * 255 + 254] == 0xFFFFFFFFu);

	for (unsigned W : {128u, 256u, 512u})
	{
		const TestBitmap bmp(W);
		for (TexKind kind : {TexKind::Normal, TexKind::NormalKeyed})
		{
			std::vector<uint8_t> out(tex_bytes(W, kind), 0xEE);
			CHECK(tex_convert(bmp.view(), tables.data(), kind, out.data()));
			unsigned bad = 0, keyed0 = 0, lum = 0;
			for (unsigned j = 0; j < W; ++j)
				for (unsigned s = 0; s < W; ++s)
				{
					const unsigned i = bmp.rows[j][s];
					const uint32_t want = expect_word(colors[i], kind, i == 0);
					const uint32_t got = read_word(out, W, j, s);
					bad += want != got;
					keyed0 += (got & 0xFF) == 0;
					lum += (got & 0xFF) == 128;
				}
			CHECK(bad == 0);
			CHECK(kind == TexKind::Normal ? keyed0 == 0 : keyed0 >= 3);
			CHECK(lum > 0);
		}
		// normal vs normal_keyed on the same bitmap: identical except A at index 0
		std::vector<uint8_t> a(tex_bytes(W, TexKind::Normal)), b(a.size());
		tex_convert(bmp.view(), tables.data(), TexKind::Normal, a.data());
		tex_convert(bmp.view(), tables.data(), TexKind::NormalKeyed, b.data());
		unsigned diffs = 0, bad = 0;
		for (unsigned j = 0; j < W; ++j)
			for (unsigned s = 0; s < W; ++s)
			{
				const uint32_t wa = read_word(a, W, j, s), wb = read_word(b, W, j, s);
				if (wa != wb)
				{
					++diffs;
					bad += !(bmp.rows[j][s] == 0 && (wb & 0xFF) == 0 && (wa & ~0xFFu) == (wb & ~0xFFu));
				}
			}
		CHECK(diffs >= 3 && bad == 0);
	}

	// special colours, texel by texel (word = R<<24 | G<<16 | B<<8 | A)
	CHECK(tex_palette_word(tables.data(), TexKind::Normal, 250) == 0x000000FFu);         // black normal: A 255
	CHECK(tex_palette_word(tables.data(), TexKind::Normal, 251) == 0x000000FFu);         // black SL reads normal
	CHECK(tex_palette_word(tables.data(), TexKind::Normal, 252) == 0x010000FFu);         // dark SL: level 0 black -> A 255
	CHECK(tex_palette_word(tables.data(), TexKind::Normal, 253) == ((0x40u << 24) | (0x20u << 16) | 128u));   // SL: A 128
	CHECK(tex_palette_word(tables.data(), TexKind::Normal, 254) == 0xFFFFFFFFu);
	CHECK(tex_palette_word(tables.data(), TexKind::Normal, 255) == ((0x12u << 24) | (0x56u << 16) | (0x9Au << 8) | 255u));
	CHECK(tex_palette_word(tables.data(), TexKind::NormalKeyed, 0) == (0xFFu << 16));      // G = 255, A 0 (index 0 keyed)
	CHECK((tex_palette_word(tables.data(), TexKind::Normal, 0) & 0xFF) == 128);            // colour 0 is SL, G = 255

	// an alternate CLUT is another table pointer: colours come from the remapped entries
	uint8_t remap[256];
	for (unsigned i = 0; i < 256; ++i)
		remap[i] = static_cast<uint8_t>(255 - i);
	const std::vector<uint32_t> alt = build_tables32(colors.data(), remap);
	CHECK(alt != tables);
	CHECK(tex_palette_word(alt.data(), TexKind::Normal, 5) == expect_word(colors[250], TexKind::Normal, false));
	CHECK(tex_palette_word(alt.data(), TexKind::Normal, 4) == expect_word(colors[251], TexKind::Normal, false));
	CHECK(tex_palette_word(alt.data(), TexKind::Normal, 3) == expect_word(colors[252], TexKind::Normal, false));
	{
		const TestBitmap bmp(128);
		std::vector<uint8_t> o1(tex_bytes(128, TexKind::Normal)), o2(o1.size());
		tex_convert(bmp.view(), tables.data(), TexKind::Normal, o1.data());
		tex_convert(bmp.view(), alt.data(), TexKind::Normal, o2.data());
		CHECK(o1 != o2);
		unsigned bad = 0;
		for (unsigned j = 0; j < 128; ++j)
			for (unsigned s = 0; s < 128; ++s)
				bad += read_word(o2, 128, j, s) != expect_word(colors[255 - bmp.rows[j][s]], TexKind::Normal, bmp.rows[j][s] == 0);   // Normal: no keying
		CHECK(bad == 0);
	}

	// shadeless (infravision): the single tint table, RGB = table[i], A 255 (0 for index 0 if keyed). The
	// tint table of shapes.cpp:2279-2301 is a collection-wide table whose entry i is a grey-ish tint.
	std::vector<uint32_t> tint(kShadingTableEntries, 0);
	for (unsigned i = 0; i < 256; ++i)
		tint[i] = 0xFF000000u | ((i ^ 0x5A) << 16) | ((255 - i) << 8) | ((i * 3) & 255);
	for (unsigned i : {0u, 1u, 77u, 200u, 255u})
	{
		const uint32_t rgb = tint[i] & 0xFFFFFF;
		const uint32_t w = (rgb & 0xFF0000u) << 8 | (rgb & 0xFF00u) << 8 | (rgb & 0xFFu) << 8;
		CHECK(tex_palette_word(tint.data(), TexKind::Shadeless, i) == (w | 255u));
		CHECK(tex_palette_word(tint.data(), TexKind::ShadelessKeyed, i) == (w | (i == 0 ? 0u : 255u)));
	}
	{
		const TestBitmap bmp(128);
		std::vector<uint8_t> o(tex_bytes(128, TexKind::ShadelessKeyed));
		CHECK(tex_convert(bmp.view(), tint.data(), TexKind::ShadelessKeyed, o.data()));
		unsigned bad = 0;
		for (unsigned j = 0; j < 128; ++j)
			for (unsigned s = 0; s < 128; ++s)
				bad += read_word(o, 128, j, s) != tex_palette_word(tint.data(), TexKind::ShadelessKeyed, bmp.rows[j][s]);
		CHECK(bad == 0);
	}

	// mask (tinted rectangles): A8, 1 byte per texel, Morton tiled; the tables (the global tint) never matter
	{
		const TestBitmap bmp(256);
		std::vector<uint8_t> o(tex_bytes(256, TexKind::Mask), 0x77), o2(o.size(), 0x66);
		CHECK(tex_convert(bmp.view(), nullptr, TexKind::Mask, o.data()));
		CHECK(tex_convert(bmp.view(), tables.data(), TexKind::Mask, o2.data()));
		CHECK(o == o2 && o.size() == 256u * 256u);
		unsigned bad = 0, zeros = 0;
		for (unsigned j = 0; j < 256; ++j)
			for (unsigned s = 0; s < 256; ++s)
			{
				const uint8_t want = bmp.rows[j][s] == 0 ? 0 : 255;
				zeros += want == 0;
				bad += o[tile_index(256, 255 - j, s)] != want;
			}
		CHECK(bad == 0 && zeros >= 3);
	}
}

// --- tiling

static void test_morton_and_tile_index()
{
	CHECK(morton8(1, 0) == 1 && morton8(0, 1) == 2 && morton8(2, 0) == 4 && morton8(0, 2) == 8);
	CHECK(morton8(4, 0) == 16 && morton8(0, 4) == 32 && morton8(7, 7) == 63 && morton8(3, 0) == 5);
	std::set<unsigned> seen;
	for (unsigned y = 0; y < 8; ++y)
		for (unsigned x = 0; x < 8; ++x)
			seen.insert(morton8(x, y));
	CHECK(seen.size() == 64 && *seen.begin() == 0 && *seen.rbegin() == 63);
	// tiles in row order across the width
	CHECK(tile_index(16, 0, 0) == 0 && tile_index(16, 0, 8) == 64 && tile_index(16, 8, 0) == 128 && tile_index(16, 8, 8) == 192);
	CHECK(tile_index(16, 0, 1) == 1 && tile_index(16, 1, 0) == 2 && tile_index(16, 1, 1) == 3 && tile_index(16, 8, 1) == 129);
	CHECK(tile_index(128, 127, 127) == 128 * 128 - 1);
	// a bijection for every W
	for (unsigned W : {8u, 16u, 128u})
	{
		std::vector<char> hit(W * W, 0);
		for (unsigned m = 0; m < W; ++m)
			for (unsigned s = 0; s < W; ++s)
				hit[tile_index(W, m, s)] = 1;
		CHECK(std::count(hit.begin(), hit.end(), 1) == static_cast<long>(W * W));
	}
	CHECK(tile_memory_row(128, 0) == 127 && tile_memory_row(128, 127) == 0 && tile_memory_row(8, 3) == 4);
	// tile_image for 1 and 4 byte texels
	std::vector<uint8_t> lin(16 * 16), til(16 * 16);
	for (unsigned i = 0; i < lin.size(); ++i)
		lin[i] = static_cast<uint8_t>(i);
	tile_image(lin.data(), 16, 1, til.data());
	CHECK(til[tile_index(16, 9, 3)] == lin[9 * 16 + 3] && til[2] == lin[16] && til[64] == lin[8]);
}

// --- a minimal PNG writer (8-bit RGBA, stored deflate blocks) for the tex3ds test

static uint32_t crc32_of(const uint8_t* p, size_t n, uint32_t crc = 0)
{
	static uint32_t table[256];
	if (!table[1])
		for (uint32_t i = 0; i < 256; ++i)
		{
			uint32_t c = i;
			for (int k = 0; k < 8; ++k)
				c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
			table[i] = c;
		}
	crc = ~crc;
	for (size_t i = 0; i < n; ++i)
		crc = table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
	return ~crc;
}

static uint32_t adler32_of(const uint8_t* p, size_t n)
{
	uint32_t a = 1, b = 0;
	for (size_t i = 0; i < n; ++i)
	{
		a = (a + p[i]) % 65521;
		b = (b + a) % 65521;
	}
	return (b << 16) | a;
}

static void put_be32(std::vector<uint8_t>& v, uint32_t x)
{
	for (int sh = 24; sh >= 0; sh -= 8)
		v.push_back(static_cast<uint8_t>(x >> sh));
}

static void png_chunk(std::vector<uint8_t>& png, const char* type, const std::vector<uint8_t>& data)
{
	put_be32(png, static_cast<uint32_t>(data.size()));
	std::vector<uint8_t> body(type, type + 4);
	body.insert(body.end(), data.begin(), data.end());
	png.insert(png.end(), body.begin(), body.end());
	put_be32(png, crc32_of(body.data(), body.size()));
}

// rgba: W rows of W pixels, bytes R, G, B, A per pixel, row 0 first.
static std::vector<uint8_t> make_png(unsigned W, const std::vector<uint8_t>& rgba)
{
	std::vector<uint8_t> raw;
	for (unsigned r = 0; r < W; ++r)
	{
		raw.push_back(0);   // filter: none
		raw.insert(raw.end(), rgba.begin() + r * W * 4, rgba.begin() + (r + 1) * W * 4);
	}
	std::vector<uint8_t> z = {0x78, 0x01};
	for (size_t pos = 0; pos < raw.size() || pos == 0;)
	{
		const size_t n = std::min<size_t>(65535, raw.size() - pos);
		const bool last = pos + n == raw.size();
		z.push_back(last ? 1 : 0);
		z.push_back(static_cast<uint8_t>(n)); z.push_back(static_cast<uint8_t>(n >> 8));
		z.push_back(static_cast<uint8_t>(~n)); z.push_back(static_cast<uint8_t>(~n >> 8));
		z.insert(z.end(), raw.begin() + pos, raw.begin() + pos + n);
		pos += n;
		if (last)
			break;
	}
	put_be32(z, adler32_of(raw.data(), raw.size()));
	std::vector<uint8_t> png = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
	std::vector<uint8_t> ihdr;
	put_be32(ihdr, W); put_be32(ihdr, W);
	ihdr.push_back(8); ihdr.push_back(6); ihdr.push_back(0); ihdr.push_back(0); ihdr.push_back(0);
	png_chunk(png, "IHDR", ihdr);
	png_chunk(png, "IDAT", z);
	png_chunk(png, "IEND", {});
	return png;
}

static bool write_file(const std::string& path, const std::vector<uint8_t>& d)
{
	FILE* f = std::fopen(path.c_str(), "wb");
	if (!f)
		return false;
	const bool ok = std::fwrite(d.data(), 1, d.size(), f) == d.size();
	return std::fclose(f) == 0 && ok;
}

static bool read_file(const std::string& path, std::vector<uint8_t>& d)
{
	FILE* f = std::fopen(path.c_str(), "rb");
	if (!f)
		return false;
	d.clear();
	uint8_t buf[65536];
	size_t n;
	while ((n = std::fread(buf, 1, sizeof buf, f)) > 0)
		d.insert(d.end(), buf, buf + n);
	std::fclose(f);
	return true;
}

// Runs tex3ds -f rgba8 -z none -r on `rgba` (W x W, row 0 first); returns the texel bytes after the
// 4-byte compression header (checked here). False: tex3ds failed or the header is wrong.
static bool run_tex3ds(const std::string& dir, unsigned W, const std::vector<uint8_t>& rgba, std::vector<uint8_t>& texels)
{
	const std::string in = dir + "/in.png", out = dir + "/out.bin";
	if (!write_file(in, make_png(W, rgba)))
		return false;
	const std::string cmd = "'/opt/devkitpro/tools/bin/tex3ds' -f rgba8 -z none -r -o '" + out + "' '" + in + "' > '" + dir + "/log.txt' 2>&1";
	if (std::system(cmd.c_str()) != 0)
		return false;
	std::vector<uint8_t> raw;
	if (!read_file(out, raw) || raw.size() != 4 + static_cast<size_t>(W) * W * 4)
		return false;
	const uint32_t size = raw[1] | (raw[2] << 8) | (raw[3] << 16);
	if (raw[0] != 0x00 || size != W * W * 4)   // compression type 0 (none), 24-bit little-endian size
		return false;
	texels.assign(raw.begin() + 4, raw.end());
	return true;
}

static void test_tiling_tex3ds()
{
	const char* tex3ds = "/opt/devkitpro/tools/bin/tex3ds";
	if (access(tex3ds, X_OK) != 0)
	{
		std::printf("SKIPPED tex3ds tests: %s not found\n", tex3ds);
		return;
	}
	const char* tmp = std::getenv("TMPDIR");
	std::string tmpl = std::string(tmp && *tmp ? tmp : "/tmp") + "/c3d-tex3ds.XXXXXX";
	std::vector<char> dirbuf(tmpl.begin(), tmpl.end());
	dirbuf.push_back(0);
	if (!mkdtemp(dirbuf.data()))
	{
		CHECK(false);
		return;
	}
	const std::string dir = dirbuf.data();

	// (1) the pure tiler: a unique RGBA value per texel (A != 0: tex3ds zeroes the RGB of A == 0 texels).
	// The PNG's row r is our source row W-1-r; our tiled output must equal tex3ds's bytes exactly.
	for (unsigned W : {8u, 16u, 128u})
	{
		std::vector<uint8_t> rgba(W * W * 4);
		auto src = [&](unsigned j, unsigned s) -> uint32_t   // R<<24|G<<16|B<<8|A of source texel (s, j)
		{
			const unsigned n = j * W + s;
			return ((n & 255u) << 24) | (((n >> 8) & 255u) << 16) | (((n * 37 + 11) & 255u) << 8) | (1 + n * 5 % 255);
		};
		for (unsigned r = 0; r < W; ++r)
			for (unsigned x = 0; x < W; ++x)
			{
				const uint32_t w = src(W - 1 - r, x);
				uint8_t* p = &rgba[(r * W + x) * 4];
				p[0] = w >> 24; p[1] = w >> 16; p[2] = w >> 8; p[3] = w;
			}
		std::vector<uint8_t> t3x, ours(W * W * 4);
		const bool ran = run_tex3ds(dir, W, rgba, t3x);
		CHECK(ran);
		for (unsigned j = 0; j < W; ++j)
			for (unsigned s = 0; s < W; ++s)
			{
				const uint32_t w = src(j, s);
				uint8_t* p = &ours[tile_index(W, tile_memory_row(W, j), s) * 4];
				p[0] = w; p[1] = w >> 8; p[2] = w >> 16; p[3] = w >> 24;   // bytes A, B, G, R
			}
		const bool same = ran && t3x == ours;
		CHECK(same);
		std::printf("tex3ds tiling W=%u: %s\n", W, same ? "byte-identical" : "MISMATCH");
		if (ran && W == 16)   // the reviewer's anchors: texel 0 = PNG (0,0), texel 2 = (0,1), texel 128 = (0,8)
		{
			auto pix = [&](unsigned x, unsigned y) { return std::vector<uint8_t>(&rgba[(y * W + x) * 4], &rgba[(y * W + x) * 4] + 4); };
			auto texel = [&](unsigned n) { return std::vector<uint8_t>{t3x[n * 4 + 3], t3x[n * 4 + 2], t3x[n * 4 + 1], t3x[n * 4]}; };
			CHECK(texel(0) == pix(0, 0) && texel(2) == pix(0, 1) && texel(1) == pix(1, 0) && texel(128) == pix(0, 8));
			CHECK(tile_index(16, 0, 0) == 0 && tile_index(16, 1, 0) == 2 && tile_index(16, 8, 0) == 128);
		}
	}

	// (2) the whole path: tex_convert's output for a bitmap (W = 128, 256) equals tex3ds's for the PNG of
	// the colours it should produce (normal kind, no keying, so A != 0). Source row j = PNG row W-1-j.
	const std::vector<TestColor> colors = make_colors();
	const std::vector<uint32_t> tables = build_tables32(colors.data(), nullptr);
	for (unsigned W : {128u, 256u})
	{
		const TestBitmap bmp(W);
		std::vector<uint8_t> rgba(W * W * 4), ours(tex_bytes(W, TexKind::Normal)), t3x;
		for (unsigned r = 0; r < W; ++r)
			for (unsigned x = 0; x < W; ++x)
			{
				const uint32_t w = tex_palette_word(tables.data(), TexKind::Normal, bmp.rows[W - 1 - r][x]);
				uint8_t* p = &rgba[(r * W + x) * 4];
				p[0] = w >> 24; p[1] = w >> 16; p[2] = w >> 8; p[3] = w;
			}
		CHECK(tex_convert(bmp.view(), tables.data(), TexKind::Normal, ours.data()));
		const bool ran = run_tex3ds(dir, W, rgba, t3x);
		CHECK(ran);
		const bool same = ran && t3x == ours;
		CHECK(same);
		std::printf("tex3ds conversion W=%u: %s\n", W, same ? "byte-identical" : "MISMATCH");
	}
	std::remove((dir + "/in.png").c_str());
	std::remove((dir + "/out.bin").c_str());
	std::remove((dir + "/log.txt").c_str());
	rmdir(dir.c_str());
}

// --- the cache, with a fake allocator

class FakeAlloc : public TexAllocator
{
public:
	struct Rec { TexPool pool; uint32_t bytes; };
	std::map<void*, Rec> live;
	uintptr_t next = 0x1000;
	int fail_next = 0;            // fail the next N allocations
	unsigned bad_release = 0, allocs = 0, releases = 0;
	void* alloc(TexPool pool, uint32_t bytes) override
	{
		if (fail_next > 0)
		{
			--fail_next;
			return nullptr;
		}
		void* h = reinterpret_cast<void*>(next);
		next += 0x100;
		live[h] = Rec{pool, bytes};
		++allocs;
		return h;
	}
	void release(TexPool pool, void* handle) override
	{
		auto it = live.find(handle);
		if (it == live.end() || it->second.pool != pool)
		{
			++bad_release;
			return;
		}
		live.erase(it);
		++releases;
	}
	uint32_t total(TexPool pool) const
	{
		uint32_t n = 0;
		for (const auto& kv : live)
			if (kv.second.pool == pool)
				n += kv.second.bytes;
		return n;
	}
};

// A cache frame: on_retired (after "frame_begin"), begin_frame, then the promotion loop; returns the copies.
static std::vector<TexPromotion> run_frame(TexCache& c, GxQueue& gx, uint32_t frame, bool cmdbuf_not_empty = false)
{
	c.on_retired();
	c.begin_frame(frame);
	gx.begin_frame();
	std::vector<TexPromotion> out;
	TexPromotion p;
	while (c.next_promotion(gx, cmdbuf_not_empty, p))
		out.push_back(p);
	return out;
}

static int bm[4096];   // addresses to use as bitmap/table keys
static TexKey K(int i, const void* tables = &bm[4095], TexKind k = TexKind::Normal) { return TexKey{&bm[i], tables, k}; }

static TexCacheConfig cfg_of(unsigned linear_kib, unsigned vram_kib, unsigned promote_max)
{
	TexCacheConfig c;
	c.linear_cap_kib = linear_kib;
	c.vram_cap_kib = vram_kib;
	c.promote_max = promote_max;
	return c;
}

static void test_cache_keys_generations()
{
	FakeAlloc fa;
	TexCache c(fa, cfg_of(4096, 3584, 8));
	GxQueue gx;
	run_frame(c, gx, 1);
	const TexKey a = K(1), b = K(1, &bm[7]), d = K(1, &bm[4095], TexKind::NormalKeyed);
	CHECK(c.lookup(a) == nullptr);
	TexSlot* sa = c.create(a, 128);
	TexSlot* sb = c.create(b, 128);   // another CLUT: its own entry
	TexSlot* sd = c.create(d, 128);   // normal vs normal_keyed coexist
	CHECK(sa && sb && sd && sa != sb && sa != sd && sb != sd);
	CHECK(c.lookup(a) == sa && c.lookup(b) == sb && c.lookup(d) == sd && c.lookup(K(2)) == nullptr);
	CHECK(sa->bytes == 65536 && sa->W == 128 && sa->pool == TexPool::Linear && sa->handle);
	CHECK(c.used_bytes(TexPool::Linear) == 3 * 65536 && fa.total(TexPool::Linear) == 3 * 65536);
	CHECK(c.live_count(TexPool::Linear) == 3 && c.stats().conversions == 3);
	TexSlot* sm = c.create(K(3, nullptr, TexKind::Mask), 128);
	CHECK(sm && sm->bytes == 16384 && c.used_bytes(TexPool::Linear) == 3 * 65536 + 16384);

	// collections_changed: lookups miss at once, nothing is freed until a retire
	CHECK(c.bump_generation() == 4);
	CHECK(c.generation() == 1 && c.stats().collections_changed == 1);
	CHECK(c.lookup(a) == nullptr && c.lookup(b) == nullptr);
	CHECK(fa.live.size() == 4 && fa.releases == 0 && c.retiring_count() == 4 && c.live_count(TexPool::Linear) == 0);
	// a bitmap reallocated at the same address misses and gets a fresh entry (different slot, the old
	// one still being held)
	TexSlot* na = c.create(a, 128);
	CHECK(na && na != sa && na != sb && na != sd && na->handle != sa->handle);
	CHECK(c.lookup(a) == na);
	CHECK(fa.live.size() == 5 && fa.releases == 0);
	// frames_begin returned -> on_retired frees the old generation
	CHECK(c.on_retired() == 4);
	CHECK(fa.live.size() == 1 && fa.releases == 4 && c.retiring_count() == 0 && c.stats().deferred_frees == 4);
	CHECK(c.used_bytes(TexPool::Linear) == 65536 && fa.bad_release == 0);
	CHECK(c.lookup(a) == na);
	// bumping twice before a retire is fine
	c.bump_generation();
	c.bump_generation();
	CHECK(c.generation() == 3 && c.on_retired() == 1 && fa.live.empty());
	// stable addresses: slot i is where it was
	CHECK(&c.slot(0) == &c.slot(0) && (&c.slot(1) - &c.slot(0)) == 1);

	// release_all frees everything in every state
	c.begin_frame(2);
	c.create(K(5), 128);
	c.create(K(6), 128);
	c.bump_generation();
	c.create(K(7), 128);
	c.release_all();
	CHECK(fa.live.empty() && c.used_bytes(TexPool::Linear) == 0 && fa.bad_release == 0 && c.retiring_count() == 0);
	CHECK(!c.stats_line().empty());
}

static void test_cache_lru()
{
	FakeAlloc fa;
	TexCache c(fa, cfg_of(192, 0, 8));   // three 128x128 textures; no VRAM
	GxQueue gx;
	run_frame(c, gx, 1);
	CHECK(c.create(K(1), 128) && c.create(K(2), 128) && c.create(K(3), 128));
	CHECK(c.pending_count() == 0);   // the VRAM cap cannot hold one: never pending
	// everything was used this frame: nothing evictable -> failure, the live ones stay
	CHECK(c.create(K(4), 128) == nullptr);
	CHECK(c.stats().discards_tex == 1 && c.stats().evictions == 0 && c.live_count(TexPool::Linear) == 3);
	run_frame(c, gx, 2);
	c.lookup(K(1));   // 1 is used in frame 2, so 2 is the LRU (then 3)
	TexSlot* s4 = c.create(K(4), 128);
	CHECK(s4 && c.stats().evictions == 1);
	CHECK(c.lookup(K(2)) == nullptr);   // lookup() of the evicted one is also a miss (and does not touch)
	CHECK(c.lookup(K(1)) && c.lookup(K(3)) && c.lookup(K(4)));
	// a miss that follows: frame 2 slots 1, 3 (just looked up), 4 are all current -> failure
	CHECK(c.create(K(5), 128) == nullptr && c.stats().discards_tex == 2);
	run_frame(c, gx, 3);
	TexSlot* s5 = c.create(K(5), 128);   // LRU: 1/3/4 all last used in frame 2; the oldest creation (1) goes
	CHECK(s5 && c.lookup(K(1)) == nullptr && c.lookup(K(3)) && c.lookup(K(4)));
	CHECK(fa.bad_release == 0 && c.used_bytes(TexPool::Linear) == 192 * 1024);

	// a texture bigger than the pool can never be made; no slot is lost
	TexSlot* big = c.create(K(6), 256);
	CHECK(big == nullptr && c.live_count(TexPool::Linear) == 3);

	// allocator failure with room in the cap is counted (fragmentation or the real heap)
	FakeAlloc fb;
	TexCache d(fb, cfg_of(4096, 0, 0));
	run_frame(d, gx, 1);
	fb.fail_next = 1;
	CHECK(d.create(K(1), 128) == nullptr);
	CHECK(d.stats().alloc_failed_free == 1 && d.stats().discards_tex == 1);
	CHECK(d.create(K(1), 128) != nullptr && d.stats().alloc_failed_free == 1);
}

static void test_cache_slot_array_full()
{
	FakeAlloc fa;
	TexCache c(fa, cfg_of(4096, 0, 0));
	GxQueue gx;
	run_frame(c, gx, 1);
	std::vector<TexSlot*> slots;
	for (int i = 0; i < static_cast<int>(kTexSlots); ++i)
		slots.push_back(c.create(K(i), 8));   // 256-byte textures
	CHECK(std::count(slots.begin(), slots.end(), nullptr) == 0);
	CHECK(c.create(K(1000), 8) == nullptr);          // full and all current: discard
	run_frame(c, gx, 2);
	TexSlot* n = c.create(K(1000), 8);                 // full, previous frame: evict the LRU (oldest)
	CHECK(n && n == slots[0] && c.lookup(K(0)) == nullptr && c.lookup(K(1)) == slots[1]);
	CHECK(c.stats().evictions == 1 && fa.bad_release == 0);
}

static void test_cache_promotion()
{
	// order, promote_max, the GX rule
	{
		FakeAlloc fa;
		TexCache c(fa, cfg_of(4096, 3584, 2));
		GxQueue gx;
		run_frame(c, gx, 1);
		TexSlot* s[5];
		for (int i = 0; i < 5; ++i)
			s[i] = c.create(K(i), 128);
		CHECK(c.pending_count() == 5);
		std::vector<TexPromotion> p = run_frame(c, gx, 2);
		CHECK(p.size() == 2 && p[0].slot == s[0] && p[1].slot == s[1]);   // oldest first, promote_max 2
		CHECK(gx.used() == 2 && c.stats().promotions_done == 2 && c.stats().promotions_deferred == 0);
		CHECK(p[0].src == s[0]->handle && p[0].dst == s[0]->vram_handle && p[0].bytes == 65536);
		CHECK(c.pending_count() == 3 && s[0]->handle != s[0]->vram_handle);
		// the linear texture is still sampled in the frame of the copy; it is on the deferred list
		CHECK(s[0]->pool == TexPool::Linear && c.deferred_block_count() == 2);
		CHECK(c.used_bytes(TexPool::Vram) == 2 * 65536 && c.used_bytes(TexPool::Linear) == 5 * 65536);
		void* vh = s[0]->vram_handle;
		p = run_frame(c, gx, 3);   // on_retired: frees the two linear sources, switches 0 and 1 to VRAM
		CHECK(s[0]->handle == vh && s[0]->pool == TexPool::Vram && s[0]->vram_handle == nullptr);
		CHECK(c.used_bytes(TexPool::Linear) == 3 * 65536 && fa.bad_release == 0);
		CHECK(p.size() == 2 && p[0].slot == s[2] && p[1].slot == s[3]);
		p = run_frame(c, gx, 4);
		CHECK(p.size() == 1 && p[0].slot == s[4]);
		p = run_frame(c, gx, 5);
		CHECK(p.empty() && c.pending_count() == 0 && c.stats().promotions_done == 5);
		CHECK(c.live_count(TexPool::Vram) == 5 && c.live_count(TexPool::Linear) == 0 && fa.total(TexPool::Linear) == 0);
		CHECK(c.stats().deferred_frees == 5);
	}
	// gxq_budget 4: used + 1 + 3 <= 4 admits one per frame; the rest are deferred, not discarded
	{
		FakeAlloc fa;
		TexCache c(fa, cfg_of(4096, 3584, 8));
		GxQueue gx;
		gx.set_budget(4);
		run_frame(c, gx, 1);
		for (int i = 0; i < 4; ++i)
			c.create(K(i), 128);
		for (uint32_t f = 2; f <= 5; ++f)
		{
			std::vector<TexPromotion> p = run_frame(c, gx, f);
			CHECK(p.size() == 1 && gx.used() == 1);
			// the frame's own entries still fit: clear + split + readback = 3 more
			CHECK(gx.take(false) && gx.take_split() && gx.take(false) && gx.refused() == 0);
		}
		CHECK(c.stats().promotions_done == 4 && c.stats().promotions_deferred == 3 && c.pending_count() == 0);
		run_frame(c, gx, 6);
		CHECK(c.stats().promotions_deferred == 3);   // nothing waiting: not a deferral
		// budget 3 never admits one (used 0 + 1 + 3 > 3)
		FakeAlloc fb;
		TexCache d(fb, cfg_of(4096, 3584, 8));
		GxQueue g3;
		g3.set_budget(3);
		run_frame(d, g3, 1);
		d.create(K(1), 128);
		CHECK(run_frame(d, g3, 2).empty() && d.stats().promotions_deferred == 1 && d.pending_count() == 1);
	}
	// a non-empty command buffer costs 2 entries; the rule still uses 1, a failed take is a deferral
	{
		FakeAlloc fa;
		TexCache c(fa, cfg_of(4096, 3584, 8));
		GxQueue gx;
		gx.set_budget(5);
		run_frame(c, gx, 1);
		c.create(K(1), 128);
		c.create(K(2), 128);
		std::vector<TexPromotion> p = run_frame(c, gx, 2, true);   // rule: 0+1+3 <= 5; take costs 2; then 2+1+3 > 5
		CHECK(p.size() == 1 && gx.used() == 2 && c.stats().promotions_deferred == 1 && fa.bad_release == 0);
		GxQueue tight;
		tight.set_budget(4);
		FakeAlloc fb;
		TexCache e(fb, cfg_of(4096, 3584, 8));
		run_frame(e, tight, 1);
		e.create(K(1), 128);
		p = run_frame(e, tight, 2, true);   // rule passes (4 <= 4), take(true) costs 2 <= 4: ok, used 2
		CHECK(p.size() == 1 && tight.used() == 2);
	}
	// promote_max 0: nothing ever pending, nothing promoted
	{
		FakeAlloc fa;
		TexCache c(fa, cfg_of(4096, 3584, 0));
		GxQueue gx;
		run_frame(c, gx, 1);
		c.create(K(1), 128);
		CHECK(c.pending_count() == 0 && run_frame(c, gx, 2).empty() && c.stats().promotions_done == 0);
		CHECK(c.stats().promotions_deferred == 0);
	}
}

static void test_cache_eviction_rules()
{
	// never evict a slot whose copy was issued this frame; a merely waiting slot goes (and leaves the queue)
	{
		FakeAlloc fa;
		TexCache c(fa, cfg_of(128, 1024, 1));   // two linear textures
		GxQueue gx;
		run_frame(c, gx, 1);
		TexSlot* a = c.create(K(1), 128);
		TexSlot* b = c.create(K(2), 128);
		CHECK(a && b && c.pending_count() == 2);
		std::vector<TexPromotion> p = run_frame(c, gx, 2);   // promote_max 1: A's copy is issued
		CHECK(p.size() == 1 && p[0].slot == a);
		// the linear cap is full (A's source is still held until the retire): B waits, is older than the
		// current frame, and is evicted for C
		TexSlot* cc = c.create(K(3), 128);
		CHECK(cc && c.lookup(K(2)) == nullptr && c.pending_count() == 1);   // B dropped from the queue; C pending
		CHECK(c.stats().evictions == 1 && a->vram_handle);
		// D: A is protected (copy this frame), C was created this frame -> failure (discard tex), A intact
		CHECK(c.create(K(4), 128) == nullptr && c.stats().discards_tex == 1);
		CHECK(c.lookup(K(1)) == a && a->vram_handle && fa.bad_release == 0);
		// next frame the source is freed, A is in VRAM; C (older than frame 3) can be promoted
		p = run_frame(c, gx, 3);
		CHECK(a->pool == TexPool::Vram && p.size() == 1 && p[0].slot == cc);
	}
	// VRAM full: the LRU VRAM slot is evicted for a promotion unless it was used as recently
	{
		FakeAlloc fa;
		TexCache c(fa, cfg_of(4096, 64, 8));   // one 128x128 in VRAM
		GxQueue gx;
		run_frame(c, gx, 1);
		TexSlot* a = c.create(K(1), 128);
		run_frame(c, gx, 2);
		run_frame(c, gx, 3);   // A is in VRAM now (promoted in frame 2, retired at 3)
		CHECK(a->pool == TexPool::Vram && c.used_bytes(TexPool::Vram) == 65536);
		c.lookup(K(1));   // A used in frame 3
		TexSlot* b = c.create(K(2), 128);   // B created in frame 3
		CHECK(b && b->pool == TexPool::Linear);
		std::vector<TexPromotion> p = run_frame(c, gx, 4);   // A (3) is not older than B (3): B stays linear
		CHECK(p.empty() && c.pending_count() == 1 && c.lookup(K(1)) == a && c.stats().evictions == 0);
		run_frame(c, gx, 5);
		c.lookup(K(2));   // B used in frame 5 (A last used in 4)
		p = run_frame(c, gx, 6);   // B (5) is newer than A (4): A is evicted from VRAM, B promoted
		CHECK(p.size() == 1 && p[0].slot == b && c.stats().evictions == 1 && c.lookup(K(1)) == nullptr);
		CHECK(c.used_bytes(TexPool::Vram) == 65536 && fa.bad_release == 0);
	}
	// bump while a copy is in flight: no double free, everything released after one retire
	{
		FakeAlloc fa;
		TexCache c(fa, cfg_of(4096, 3584, 8));
		GxQueue gx;
		run_frame(c, gx, 1);
		c.create(K(1), 128);
		c.create(K(2), 128);
		std::vector<TexPromotion> p = run_frame(c, gx, 2);
		CHECK(p.size() == 2);
		c.bump_generation();
		CHECK(c.pending_count() == 0 && c.retiring_count() == 2);
		c.on_retired();
		CHECK(fa.live.empty() && fa.bad_release == 0 && c.used_bytes(TexPool::Linear) == 0 && c.used_bytes(TexPool::Vram) == 0);
	}
	// an unreachable promotion (oversized for the VRAM cap) never queues
	{
		FakeAlloc fa;
		TexCache c(fa, cfg_of(4096, 128, 8));
		GxQueue gx;
		run_frame(c, gx, 1);
		c.create(K(1), 256);   // 256 KiB > 128 KiB VRAM cap
		CHECK(c.pending_count() == 0);
	}
}

// The linear cap filled without any promotion getting through: LRU eviction, never a failure.
static void fill_linear(unsigned promote_max, int gx_budget)
{
	FakeAlloc fa;
	TexCache c(fa, cfg_of(256, 3584, promote_max));   // four 128x128 in linear
	GxQueue gx;
	gx.set_budget(gx_budget);
	unsigned made = 0;
	for (uint32_t f = 1; f <= 40; ++f)
	{
		run_frame(c, gx, f);
		for (int k = 0; k < 2; ++k)
			made += c.create(K(static_cast<int>(f * 2 + k)), 128) != nullptr;
		CHECK(c.used_bytes(TexPool::Linear) <= 256 * 1024);
	}
	CHECK(made == 80);
	CHECK(c.stats().discards_tex == 0 && c.stats().evictions > 0 && fa.bad_release == 0);
	CHECK(c.stats().alloc_failed_free == 0);
	if (promote_max == 0)
		CHECK(c.stats().promotions_done == 0 && c.pending_count() == 0);
	else
		CHECK(c.stats().promotions_done > 0 && c.stats().promotions_deferred > 0);
}

static void test_cache_linear_full()
{
	fill_linear(0, 24);   // promote_max 0
	fill_linear(8, 4);    // gxq_budget 4: one promotion a frame, the waiting ones are evicted
}

static void test_cache_stats()
{
	FakeAlloc fa;
	TexCache c(fa, cfg_of(8192, 3584, 8));
	GxQueue gx;
	c.bump_generation();   // a generation change opens the first-10-frames window
	run_frame(c, gx, 1);
	for (int i = 0; i < 10; ++i)
		c.create(K(i), 128);   // 640 KiB in frame 1: over the soft budget (512 KiB)
	run_frame(c, gx, 2);
	c.create(K(20), 256);
	const TexStats& s = c.stats();
	CHECK(s.conversions == 11 && s.bytes_converted == 10 * 65536 + 262144);
	CHECK(s.frame_bytes_max == 10 * 65536 && s.over_budget_frames == 1);
	CHECK(s.gen_window_bytes[0] == 10 * 65536 && s.gen_window_bytes[1] == 262144 && s.gen_window_bytes[2] == 0);
	const std::string line = c.stats_line();
	CHECK(line.find("[c3d] tex:") == 0 && line.find("gen 1") != std::string::npos && line.find("discards 0") != std::string::npos);
	CHECK(line.find('\n') == std::string::npos);
}


// ---------------------------------------------------------------------------------------------
// M2 part 4: the texcoords against the transliterated SW mappers (design 7, M2 specification "Host tests").
// For every pixel SW covers, the texel (column, row) the integer mappers read, against the texel the GPU would
// sample at the pixel centre: perspective-correct interpolation (double) of the vertex attributes
// wall_attrs / floor_attrs give over the fan triangle that contains the centre, floor(u * W) mod W.

namespace sw
{
using std::int32_t;
using std::uint32_t;

static int32_t mul32(int32_t a, int32_t b) { return static_cast<int32_t>(static_cast<uint32_t>(a) * static_cast<uint32_t>(b)); }
static int32_t add32(int32_t a, int32_t b) { return static_cast<int32_t>(static_cast<uint32_t>(a) + static_cast<uint32_t>(b)); }

// scottish_textures.cpp:891-1001 (_pretexture_vertical_polygon_lines) for the column sx whose first written
// row is y0; texel (column, row) of pixel row m is (col, (texture_y + (m - y0) * dy) >> (32 - bits)).
struct WallColumn
{
	unsigned col;
	uint32_t texture_y, texture_dy;
};

static WallColumn wall_column(int bits, const TexView& v, const WallSurf& o, int sx, int y0)
{
	const int32_t screen_x = sx - v.hw;
	const int32_t dz0 = mul32(v.wy, o.oz);
	const int32_t unadjusted_ty_denominator = mul32(v.wy, o.vk);
	int32_t an = add32(mul32(v.wx, o.oy), -mul32(screen_x, o.ox));
	int32_t ad = add32(mul32(screen_x, o.vi), -mul32(v.wx, o.vj));
	while (an > ((1 << (31 - bits)) - 1) || an < -(1 << (31 - bits)))
		an >>= 1, ad >>= 1;
	if (!ad)
		ad = 1;
	WallColumn r;
	r.col = static_cast<unsigned>((an * (1 << bits)) / ad) & ((1u << bits) - 1);
	while (an > INT16_MAX || an < INT16_MIN)
		an >>= 1, ad >>= 1;
	if (!ad)
		ad = 1;
	const int32_t tx = (an * 65536) / ad;
	int32_t world_x = o.ox + (static_cast<int32_t>(static_cast<int64_t>(tx) * o.vi) >> 16);
	if (world_x < 0)
		world_x = -world_x;
	const int16_t screen_y0 = static_cast<int16_t>(v.hh - y0 + v.dtp);
	int32_t ty_numerator = add32(mul32(world_x, screen_y0), -dz0);
	int32_t ty_denominator = unadjusted_ty_denominator;
	while (ty_numerator > INT16_MAX || ty_numerator < INT16_MIN)
		ty_numerator >>= 1, ty_denominator >>= 1;
	if (!ty_denominator)
		ty_denominator = 1;
	const int32_t ty = (ty_numerator * 65536) / ty_denominator;
	int32_t adjusted_world_x = world_x;
	int32_t adjusted_ty_denominator = unadjusted_ty_denominator >> 8;
	while (adjusted_world_x > INT16_MAX)
		adjusted_world_x >>= 1, adjusted_ty_denominator >>= 1;
	if (!adjusted_ty_denominator)
		adjusted_ty_denominator = 1;
	const int32_t ty_delta = -((adjusted_world_x * 65536) / adjusted_ty_denominator);
	r.texture_y = static_cast<uint32_t>(ty) << 16;
	r.texture_dy = static_cast<uint32_t>(ty_delta) << 8;
	return r;
}

// scottish_textures.cpp:1003-1097 (_pretexture_horizontal_polygon_lines) for the row y0 whose first written
// pixel is x0. Pixel k of the row: column = (sx + (k - x0) * dx) >> (32 - bits), row = the top `bits` bits of
// (sy + (k - x0) * dy) (low_level_textures.h:222).
struct FloorRow
{
	uint32_t sx, sy, dx, dy;
};

static FloorRow floor_row(const TexView& v, const FloorSurf& o, int x0, int y0)
{
	const bool higher_precision = o.oz > -1024 && o.oz < 1024;
	int32_t hcosine = v.cos_t, hsine = v.sin_t;
	if (higher_precision)
		hcosine = mul32(hcosine, o.oz), hsine = mul32(hsine, o.oz);
	const int32_t dhcosine = mul32(v.wy, hcosine), dhsine = mul32(v.wy, hsine);
	const int16_t screen_x = static_cast<int16_t>(x0 - v.hw);
	int16_t screen_y = static_cast<int16_t>(v.hh - y0 + v.dtp);
	if (!screen_y)
		screen_y = 1;
	int32_t source_x, source_y, source_dx, source_dy;
	const int32_t ox10 = static_cast<int32_t>(static_cast<uint32_t>(o.ox) << 10);
	const int32_t oy10 = static_cast<int32_t>(static_cast<uint32_t>(o.oy) << 10);
	if (higher_precision)
	{
		source_x = add32((add32(dhcosine, -mul32(screen_x, hsine))) / screen_y, ox10);
		source_dx = -hsine / screen_y;
		source_y = add32((add32(mul32(screen_x, hcosine), dhsine)) / screen_y, oy10);
		source_dy = hcosine / screen_y;
	}
	else
	{
		source_x = add32(mul32((add32(dhcosine, -mul32(screen_x, hsine))) / screen_y, o.oz), ox10);
		source_dx = -mul32(hsine, o.oz) / screen_y;
		source_y = add32(mul32((add32(mul32(screen_x, hcosine), dhsine)) / screen_y, o.oz), oy10);
		source_dy = mul32(hcosine, o.oz) / screen_y;
	}
	const int bits = 12 - o.scale;   // HORIZONTAL_FREE_BITS - (flags & _SCALE_BITS)
	FloorRow r;
	r.sx = static_cast<uint32_t>(source_x) << bits;
	r.dx = static_cast<uint32_t>(source_dx) << bits;
	r.sy = static_cast<uint32_t>(source_y) << bits;
	r.dy = static_cast<uint32_t>(source_dy) << bits;
	return r;
}
} // namespace sw

// The texel the GPU samples at the pixel centre (cx, cy): false when the centre is outside every fan triangle.
static bool gpu_sample(const Point2* pts, const TexAttr* at, int n, double cx, double cy, double& s, double& t)
{
	for (int i = 1; i + 1 < n; ++i)
	{
		const Point2 &a = pts[0], &b = pts[i], &c = pts[i + 1];
		const double det = static_cast<double>(b.x - a.x) * (c.y - a.y) - static_cast<double>(c.x - a.x) * (b.y - a.y);
		if (det == 0)
			continue;
		const double l1 = ((cx - a.x) * (c.y - a.y) - (c.x - a.x) * (cy - a.y)) / det;
		const double l2 = ((b.x - a.x) * (cy - a.y) - (cx - a.x) * (b.y - a.y)) / det;
		const double l0 = 1.0 - l1 - l2;
		if (l0 < -1e-9 || l1 < -1e-9 || l2 < -1e-9)
			continue;
		const double iw0 = l0 / at[0].w, iw1 = l1 / at[i].w, iw2 = l2 / at[i + 1].w;
		const double iw = iw0 + iw1 + iw2;
		s = (iw0 * at[0].s + iw1 * at[i].s + iw2 * at[i + 1].s) / iw;
		t = (iw0 * at[0].t + iw1 * at[i].t + iw2 * at[i + 1].t) / iw;
		return true;
	}
	return false;
}

static unsigned texel_of(double u, unsigned W)
{
	const long long k = static_cast<long long>(std::floor(u * W));
	return static_cast<unsigned>(((k % static_cast<long long>(W)) + W) % W);
}

struct TexAgree
{
	unsigned long total = 0, exact = 0, within1 = 0;   // within1: both coordinates within one texel (wrapping)
	double pct() const { return total ? 100.0 * exact / total : 100.0; }
	double pct1() const { return total ? 100.0 * within1 / total : 100.0; }
	void add(unsigned sc, unsigned sr, unsigned gc, unsigned gr, unsigned W)
	{
		++total;
		if (sc == gc && sr == gr)
			++exact;
		auto near1 = [W](unsigned a, unsigned b) { const unsigned d = (a + W - b) % W; return d <= 1 || d == W - 1; };
		if (near1(sc, gc) && near1(sr, gr))
			++within1;
	}
};

static void test_texcoords_vs_sw()
{
	std::mt19937_64 rng(0xC3D004u);
	auto uni = [&](int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(rng); };
	const int VW = 400, VH = 240;
	const double deltas[2] = {0.0, -0.5};
	TexAgree wall[2], floor_[2], floor_h_[2];
	TexAgree wall_b[3][2], floor_b[3][2];
	TexAgree sw_vs_exact_w, sw_vs_exact_f, gpu_vs_exact_w, gpu_vs_exact_f;   // the exact map at the pixel's corner (informative)   // by TEXBITS 7, 8, 9 (informative)
	unsigned long walls_made = 0, walls_used = 0, floors_made = 0, floors_used = 0, floors_h_used = 0, nonfinite = 0, degenerate = 0;
	std::vector<uint8_t> cov(static_cast<size_t>(VW) * VH);

	auto make_view = [&]() {
		TexView v;
		v.hw = VW / 2, v.hh = VH / 2;
		v.wx = uni(160, 320);
		v.wy = std::max(100, static_cast<int>(v.wx * std::uniform_real_distribution<double>(0.8, 1.2)(rng)));
		v.dtp = uni(-60, 60);
		const int yaw = uni(0, 511);
		v.cos_t = static_cast<int>(std::lround(1024.0 * std::cos(2.0 * M_PI * yaw / 512.0)));
		v.sin_t = static_cast<int>(std::lround(1024.0 * std::sin(2.0 * M_PI * yaw / 512.0)));
		return v;
	};

	for (int it = 0; it < 6000; ++it)
	{
		const TexView v = make_view();
		const int bits = uni(7, 9);
		const unsigned W = 1u << bits;

		// ---- a wall, as RenderRasterize.cpp:525-548 makes it ----
		{
			++walls_made;
			const int sx0 = uni(4, VW - 4), sx1 = uni(4, VW - 4);
			if (std::abs(sx0 - sx1) < 6)
				goto floors;
			const int d0 = uni(150, 6000), d1 = uni(150, 6000);
			const int p0i = d0, p0j = (sx0 - v.hw) * d0 / v.wx, p1i = d1, p1j = (sx1 - v.hw) * d1 / v.wx;
			const int h0 = -uni(100, 1200), h1 = h0 + uni(256, 2048);   // bottom, top (relative to the eye)
			const int scale = 1024 << uni(0, 2);
			const int dx = p1i - p0i, dy = p1j - p0j;
			int divisor = static_cast<int>(std::lround(std::sqrt(static_cast<double>(dx) * dx + static_cast<double>(dy) * dy)));
			if (divisor == 0)
				divisor = 1;
			const int x0off = uni(0, scale - 1) & (scale - 1), y0off = uni(0, scale - 1) & (scale - 1);
			WallSurf o;
			o.vi = (scale * dx) / divisor;
			o.vj = (scale * dy) / divisor;
			o.vk = -scale;
			o.ox = p0i - (x0off * dx) / divisor;
			o.oy = p0j - (x0off * dy) / divisor;
			o.oz = h1 + y0off;
			auto project_y = [&](int depth, int z) {
				const double Y = v.hh + v.dtp - static_cast<double>(v.wy) * z / depth;
				return std::min(VH, std::max(0, static_cast<int>(std::lround(Y))));
			};
			const bool p0_left = sx0 < sx1;
			const int xl = p0_left ? sx0 : sx1, xr = p0_left ? sx1 : sx0;
			const int dl = p0_left ? d0 : d1, dr = p0_left ? d1 : d0;
			std::vector<Point2> poly = {{xl, project_y(dl, h1)}, {xr, project_y(dr, h1)}, {xr, project_y(dr, h0)}, {xl, project_y(dl, h0)}};
			if (accept_polygon(poly.data(), 4, VW, VH) != Accept::Draw || !is_convex(poly, true))
				goto floors;
			TexAttr at[2][4];
			if (wall_attrs(v, o, poly.data(), 4, deltas[0], at[0]) != TexResult::Ok ||
			    wall_attrs(v, o, poly.data(), 4, deltas[1], at[1]) != TexResult::Ok)
			{
				++nonfinite;
				goto floors;
			}
			std::fill(cov.begin(), cov.end(), 0);
			unsigned outside = 0;
			if (sw::polygon_coverage(poly, VW, VH, cov, outside) != sw::Drawn)
				goto floors;
			++walls_used;
			for (int k = 0; k < VW; ++k)
			{
				int y0 = -1;
				sw::WallColumn col{};
				for (int m = 0; m < VH; ++m)
				{
					if (!cov[static_cast<size_t>(m) * VW + k])
						continue;
					if (y0 < 0)
					{
						y0 = m;
						col = sw::wall_column(bits, v, o, k, y0);
					}
					const unsigned sw_row = (col.texture_y + static_cast<uint32_t>(m - y0) * col.texture_dy) >> (32 - bits);
					for (int di = 0; di < 2; ++di)
					{
						double gs, gt;
						if (!gpu_sample(poly.data(), at[di], 4, k + 0.5, m + 0.5, gs, gt))
							continue;
						// walls: s = row in the column, t = column
						wall[di].add(col.col, sw_row, texel_of(gt, W), texel_of(gs, W), W);
						wall_b[bits - 7][di].add(col.col, sw_row, texel_of(gt, W), texel_of(gs, W), W);
						if (di == 1)
						{
							const TexAttrD e = wall_vertex(v, o, k, m);
							sw_vs_exact_w.add(col.col, sw_row, texel_of(e.t, W), texel_of(e.s, W), W);
							gpu_vs_exact_w.add(texel_of(gt, W), texel_of(gs, W), texel_of(e.t, W), texel_of(e.s, W), W);
						}
					}
				}
			}
		}
	floors:
		// ---- a floor or ceiling, as RenderRasterize.cpp:450-452 makes it ----
		{
			++floors_made;
			FloorSurf o;
			const int mag = uni(64, 2048);
			o.oz = (it & 1) ? mag : -mag;
			o.ox = uni(0, 32767);
			o.oy = uni(0, 32767);
			o.scale = uni(0, 2);
			const int yh = v.hh + v.dtp;   // the horizon row (screen_y == 0)
			// Ground distance is |oz| * wy / |sy'|; maps are some 32 WORLD_ONE across: keep rows no farther than
			// that, except in the "horizon" variant (every 20th: rows on the horizon row itself).
			const bool near_horizon = (it % 20) == 3;
			const int off_min = near_horizon ? 0 : std::max(1, (std::abs(o.oz) * v.wy + 32767) / 32768);
			std::vector<Point2> poly = random_polygon(rng, VW, VH);
			// Map y in [0, VH] onto the rows between the horizon (offset off_min) and the view's edge.
			const int lo = o.oz < 0 ? yh + off_min : 0, hi = o.oz < 0 ? VH : yh - off_min;
			if (hi - lo < 4)
				continue;
			for (Point2& p : poly)
				p.y = lo + (p.y * (hi - lo) + VH / 2) / VH;
			if (accept_polygon(poly.data(), static_cast<int>(poly.size()), VW, VH, Orient::Horizontal) != Accept::Draw ||
			    !is_convex(poly, true))
				continue;
			const int n = static_cast<int>(poly.size());
			TexAttr at[2][16];
			bool bad = false;
			for (int di = 0; di < 2; ++di)
			{
				const TexResult tr = floor_attrs(v, o, poly.data(), n, deltas[di], at[di]);
				if (tr == TexResult::Degenerate)
					++degenerate, bad = true;
				else if (tr != TexResult::Ok)
					++nonfinite, bad = true;
			}
			if (bad)
				continue;
			std::fill(cov.begin(), cov.end(), 0);
			unsigned outside = 0;
			if (sw::polygon_coverage_h(poly, VW, VH, cov, outside) != sw::Drawn)
				continue;
			(near_horizon ? floors_h_used : floors_used) += 1;
			for (int m = 0; m < VH; ++m)
			{
				int x0 = -1;
				sw::FloorRow row{};
				for (int k = 0; k < VW; ++k)
				{
					if (!cov[static_cast<size_t>(m) * VW + k])
						continue;
					if (x0 < 0)
					{
						x0 = k;
						row = sw::floor_row(v, o, x0, m);
					}
					const uint32_t ux = row.sx + static_cast<uint32_t>(k - x0) * row.dx;
					const uint32_t uy = row.sy + static_cast<uint32_t>(k - x0) * row.dy;
					const unsigned sw_col = ux >> (32 - bits), sw_row = uy >> (32 - bits);
					for (int di = 0; di < 2; ++di)
					{
						double gs, gt;
						if (!gpu_sample(poly.data(), at[di], n, k + 0.5, m + 0.5, gs, gt))
							continue;
						// floors: s = column, t = row
						(near_horizon ? floor_h_ : floor_)[di].add(sw_col, sw_row, texel_of(gs, W), texel_of(gt, W), W);
						if (!near_horizon)
							floor_b[bits - 7][di].add(sw_col, sw_row, texel_of(gs, W), texel_of(gt, W), W);
						if (di == 1 && !near_horizon)
						{
							TexAttrD e;
							floor_vertex(v, o, k, m, e);
							sw_vs_exact_f.add(sw_col, sw_row, texel_of(e.s, W), texel_of(e.t, W), W);
							gpu_vs_exact_f.add(texel_of(gs, W), texel_of(gt, W), texel_of(e.s, W), texel_of(e.t, W), W);
						}
					}
				}
			}
		}
	}
	std::printf("texcoords vs SW: %lu walls generated, %lu compared; %lu floors generated, %lu compared (+ %lu on the horizon row), "
	            "%lu non-finite, %lu degenerate\n", walls_made, walls_used, floors_made, floors_used, floors_h_used, nonfinite, degenerate);
	for (int di = 0; di < 2; ++di)
	{
		std::printf("texcoords vs SW, delta %5.1f: walls %.3f%% exact (%.3f%% within 1 texel) of %lu px; floors %.3f%% exact (%.3f%% within 1) of %lu px;"
		            " horizon-row floors %.3f%% of %lu px (informative)\n",
		            deltas[di], wall[di].pct(), wall[di].pct1(), wall[di].total, floor_[di].pct(), floor_[di].pct1(),
		            floor_[di].total, floor_h_[di].pct(), floor_h_[di].total);
	}
	for (int b = 0; b < 3; ++b)
		std::printf("texcoords vs SW, W=%u, delta -0.5: walls %.3f%% (delta 0: %.3f%%), floors %.3f%% (delta 0: %.3f%%)\n", 128u << b,
		            wall_b[b][1].pct(), wall_b[b][0].pct(), floor_b[b][1].pct(), floor_b[b][0].pct());
	std::printf("texcoords vs SW (informative): against the exact map at each pixel's corner, SW's integers agree %.3f%% (walls) / %.3f%% (floors);"
	            " the GPU at delta -0.5 agrees %.3f%% / %.3f%%\n", sw_vs_exact_w.pct(), sw_vs_exact_f.pct(), gpu_vs_exact_w.pct(), gpu_vs_exact_f.pct());
	const double best_wall = std::max(wall[0].pct(), wall[1].pct()), best_floor = std::max(floor_[0].pct(), floor_[1].pct());
	std::printf("texcoords vs SW: best delta for walls %s (%.3f%%), for floors %s (%.3f%%); gate 98%%\n",
	            wall[1].pct() >= wall[0].pct() ? "-0.5" : "0", best_wall, floor_[1].pct() >= floor_[0].pct() ? "-0.5" : "0", best_floor);
	CHECK(walls_used > 1000 && floors_used > 1000 && wall[1].total > 100000 && floor_[1].total > 100000);
	CHECK(best_wall >= 98.0);    // the reachability gate (Review log "M2 pre-implementation", round 2)
	CHECK(best_floor >= 98.0);
}


static void test_config_m2_front()
{
	Config d;
	CHECK(!d.textures && d.uv_half && d.diff_tol == 2);
	Config c;
	CHECK(config_apply(c, "textures", "1") && c.textures && config_apply(c, "textures", "0") && !c.textures);
	CHECK(!config_apply(c, "textures", "2") && !config_apply(c, "textures", "yes") && !config_apply(c, "textures", "") && !c.textures);
	CHECK(config_apply(c, "uv_half", "0") && !c.uv_half && config_apply(c, "uv_half", "1") && c.uv_half);
	CHECK(!config_apply(c, "uv_half", "-1") && !config_apply(c, "uv_half", "half") && c.uv_half);
	CHECK(config_apply(c, "diff_tol", "0") && c.diff_tol == 0 && config_apply(c, "diff_tol", "255") && c.diff_tol == 255);
	CHECK(!config_apply(c, "diff_tol", "256") && !config_apply(c, "diff_tol", "-1") && !config_apply(c, "diff_tol", "") && c.diff_tol == 255);
	Config p;
	std::vector<std::string> problems;
	config_parse(p, "textures 1\nuv_half 0 # shift off\ndiff_tol 4\ndiff_tol 300\ntextures maybe\n", problems);
	CHECK(problems.size() == 2 && p.textures && !p.uv_half && p.diff_tol == 4);
}

static void test_texcoord_basics()
{
	TexView v{200, 120, 256, 256, 0, 1024, 0};   // yaw 0: cos 1.0, sin 0
	// clamp_wp: [16, 2^24], not wall_w's [16, 32767].
	CHECK(clamp_wp(0.0f) == 16.0f && clamp_wp(40000.0f) == 40000.0f && clamp_wp(1e9f) == 16777216.0f && clamp_wp(16.0f) == 16.0f);
	// A wall in the plane x = 1000 (v = (1024, 0, -1024), o = (1000, 0, 0)): w = 1000 at every column, in the
	// shifted evaluation too; wall_vertex's w at delta 0 is wall_w.
	const WallSurf wl{1000, 0, 0, 0, 1024, -1024};
	for (int sx = 0; sx <= 400; sx += 40)
	{
		CHECK(wall_vertex(v, wl, sx, 100).w == 1000.0);
		CHECK(wall_vertex(v, wl, sx - 0.5, 100 - 0.5).w == 1000.0);
	}
	{
		std::mt19937_64 rng(0xC3D005u);
		int bad = 0;
		for (int i = 0; i < 20000; ++i)
		{
			auto uni = [&](int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(rng); };
			TexView tv{200, 120, uni(160, 320), uni(160, 320), uni(-60, 60), 1024, 0};
			const WallSurf o{uni(-20000, 20000), uni(-20000, 20000), uni(-2000, 2000), uni(-4000, 4000), uni(-4000, 4000), -1024};
			const int sx = uni(0, 400);
			const double w = wall_vertex(tv, o, sx, 50).w;
			const double ref = wall_w(200, tv.wx, o.ox, o.oy, o.vi, o.vj, sx);
			if (std::fabs(w - ref) > 1e-3 * ref + 0.05)
				++bad;
		}
		CHECK(bad == 0);
	}
	// A floor at o.z == 0 is a degenerate skip, never unsupported / finite garbage.
	const Point2 tri[3] = {{10, 200}, {300, 200}, {10, 230}};
	TexAttr at[3];
	CHECK(floor_attrs(v, FloorSurf{5, 6, 0, 0}, tri, 3, -0.5, at) == TexResult::Degenerate);
	TexAttrD d;
	CHECK(!floor_vertex(v, FloorSurf{5, 6, 0, 0}, 10, 10, d));
	// Per-polygon floor(min): every texcoord is >= 0 and < its spread + 1, w within [16, 2^24].
	const FloorSurf fs{40000, 33000, -512, 0};
	CHECK(floor_attrs(v, fs, tri, 3, -0.5, at) == TexResult::Ok);
	double mins = 1e30, mint = 1e30;
	for (int i = 0; i < 3; ++i)
	{
		CHECK(at[i].w >= 16.0f && at[i].w <= 16777216.0f);
		mins = std::min<double>(mins, at[i].s);
		mint = std::min<double>(mint, at[i].t);
	}
	CHECK(mins >= 0.0 && mins < 1.0 && mint >= 0.0 && mint < 1.0);
	// ... and it is an integer shift: the fractional parts equal the raw ones.
	{
		TexAttrD raw[3];
		for (int i = 0; i < 3; ++i)
			CHECK(floor_vertex(v, fs, tri[i].x - 0.5, tri[i].y - 0.5, raw[i]));
		for (int i = 0; i < 3; ++i)
		{
			const double fs_ = std::floor(std::min({raw[0].s, raw[1].s, raw[2].s}));
			CHECK(std::fabs((raw[i].s - fs_) - at[i].s) < 1e-3);
		}
	}
	// The sign guard: a floor below the eye (o.z < 0) with a vertex on the horizon row (sy' = 0, or +1/2 after
	// the shift): its u/w keeps the polygon's sign, so w stays positive and finite and u/w has the sign of the others.
	{
		const int yh = v.hh + v.dtp;
		const FloorSurf below{1000, 2000, -300, 1}, above{1000, 2000, 300, 1};
		TexAttrD on_h, next, other;
		CHECK(floor_vertex(v, below, 100, yh, on_h) && floor_vertex(v, below, 100, yh + 1, next));
		CHECK(floor_vertex(v, below, 100, yh + 0.5, other));   // ey = yh + 1/2: sy' = -1/2 exactly
		CHECK(std::isfinite(on_h.s) && std::isfinite(on_h.w) && on_h.w > 0 && on_h.w == other.w && on_h.s == other.s);
		TexAttrD up;
		CHECK(floor_vertex(v, above, 100, yh - 0.5, up));      // sy' = +1/2 exactly for a ceiling
		TexAttrD on_h2;
		CHECK(floor_vertex(v, above, 100, yh + 0.5, on_h2));   // past the horizon: the guard puts sy' at +1/2
		CHECK(on_h2.w == up.w && on_h2.s == up.s && on_h2.t == up.t);
		// the w of a vertex at the guard is the largest the polygon sees: |o.z| * wy / (1/2)
		CHECK(std::fabs(on_h.w - 2.0 * 300 * 256) < 1e-9);
	}
	// Scale: 2x and 4x halve and quarter the texcoords' rate (same polygon, same raw u up to the origin term).
	{
		TexAttrD a0, a1, a2;
		const FloorSurf f0{0, 0, -512, 0}, f1{0, 0, -512, 1}, f2{0, 0, -512, 2};
		CHECK(floor_vertex(v, f0, 123, 200, a0) && floor_vertex(v, f1, 123, 200, a1) && floor_vertex(v, f2, 123, 200, a2));
		CHECK(std::fabs(a0.s - 2 * a1.s) < 1e-12 && std::fabs(a0.t - 4 * a2.t) < 1e-12 && a0.w == a1.w && a0.w == a2.w);
	}
	// Non-finite input is reported, not drawn: a wall with a zero denominator row stays finite (SW's guard).
	const Point2 q[4] = {{10, 10}, {20, 10}, {20, 20}, {10, 20}};
	TexAttr aq[4];
	CHECK(wall_attrs(v, WallSurf{1000, 0, 0, 0, 0, -1024}, q, 4, -0.5, aq) == TexResult::Ok);
}

static void test_m2_core()
{
	test_config_m2();
	test_tex_supported();
	test_tex_convert();
	test_morton_and_tile_index();
	test_tiling_tex3ds();
	test_cache_keys_generations();
	test_cache_lru();
	test_cache_slot_array_full();
	test_cache_promotion();
	test_cache_eviction_rules();
	test_cache_linear_full();
	test_cache_stats();
	test_config_m2_front();
	test_texcoord_basics();
	test_texcoords_vs_sw();
}

int main()
{
	test_config();
	test_gate_inputs();
	test_tracker();
	test_retire_wait();
	test_config_m0();
	test_classify();
	test_diff();
	test_bmp();
	test_gx_queue_readback();
	test_gx_queue_m1();
	test_stats();
	test_calibrate();
	test_calibrate_m1();
	test_wall_w();
	test_fan_and_key();
	test_colour_and_cmdbuf();
	test_config_m1();
	test_acceptance_and_coverage();
	test_acceptance_and_coverage_horizontal();
	test_tie_pixels();
	test_ring();
	test_m2_core();
	std::printf("%d checks, %d failures\n", s_checks, s_fail);
	return s_fail ? 1 : 0;
}
