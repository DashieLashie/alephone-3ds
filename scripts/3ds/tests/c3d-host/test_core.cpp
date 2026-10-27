// Host tests for render_c3d/core: the debug-c3d parser (P1, piece 5, B-a and M0 keys), core/gate.h
// (docs/3ds/CITRO3D_DESIGN.md 5.6.1, 8.4), and the M0 pieces (7, "M0 specification"): classify.h
// (class mapping, class list), diff.* (the 8.1 metric and its log line), bmp.* (32-bit BMP),
// gx_queue.h (2.4's worst case with the readback's +1), stats.* (nearest-rank percentile, the
// bounded sample array, the pair table) and calibrate.* (the readback calibration's check).
#include "bmp.h"
#include "calibrate.h"
#include "classify.h"
#include "config.h"
#include "diff.h"
#include "gate.h"
#include "gx_queue.h"
#include "stats.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

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
	l.common = 80; l.within_tol = 60; l.sum_delta = 120; l.max_delta = 9; l.differ = 17;
	CHECK(diff_line(12, "walls", l, 4) ==
	      "[c3d] DIFF frame=12 class=walls covered=100/98 cov_mismatch=4/1 common=80 within_tol=75.00% (tol 4) "
	      "mean=1.50 max=9 differ=17");
	CHECK(diff_line(0, "frame", DiffResult(), 0) ==
	      "[c3d] DIFF frame=0 class=frame covered=0/0 cov_mismatch=0/0 common=0 within_tol=100.00% (tol 0) "
	      "mean=0.00 max=0 differ=0");
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
	for (int i = 0; i < GxQueue::kBudget - 2; ++i)
		CHECK(q.take(false));
	CHECK(q.take(true));   // the readback transfer, splitting the command list
	CHECK(!q.take(false) && q.refused() == 1);
	CHECK(q.end_frame(0) == GxQueue::kBudget + 1);
	CHECK(q.last_total() == 25 && q.high_water() == 25 && q.high_water() < 32);

	// A later, smaller frame does not lower the high-water mark.
	q.begin_frame();
	CHECK(q.take(true));
	CHECK(q.end_frame(0) == 3 && q.last_total() == 3 && q.high_water() == 25);
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
			const CalResult r = cal_check(img.data(), w, h, w, k);
			CHECK(r.orientation == cs.o);
			CHECK(r.observed_k == 17 && r.current_frame_ok == (cs.o != Orientation::Unknown));   // unknown: corners mismatch
			CHECK(r.corner[0] == cs.tl && r.corner[1] == cs.tr && r.corner[2] == cs.bl && r.corner[3] == cs.br);
			CHECK(r.centre == cal_clear_argb(k));
			CHECK(r.channels_ok == (cs.o != Orientation::Unknown));
			if (cs.o == Orientation::Ok)
				CHECK(cal_line(k, r) ==
				      "[c3d] readback_cal k=17 orientation ok current frame ok mismatched 0 (sentinel 0) "
				      "channels ok corners 0xFFFF0000 0xFF00FF00 0xFF0000FF 0xFFFFFFFF");
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
		return im;
	};
	{
		std::vector<uint32_t> im = perfect(17);
		CalResult p = cal_check(im.data(), w, h, w, 17);
		CHECK(p.current_frame_ok && p.mismatched == 0 && p.sentinel == 0 && p.orientation == Orientation::Ok);
		CHECK(cal_line(17, p) ==
		      "[c3d] readback_cal k=17 orientation ok current frame ok mismatched 0 (sentinel 0) channels ok "
		      "corners 0xFFFF0000 0xFF00FF00 0xFF0000FF 0xFFFFFFFF");

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
	      "[c3d] readback_cal k=17 orientation ok current frame NO (centre k=16) mismatched 94976 (sentinel 0) "
	      "channels ok corners "
	      "0xFFFF0000 0xFF00FF00 0xFF0000FF 0xFFFFFFFF");
	img[(h / 2) * w + w / 2] = 0xFF123456u;
	r = cal_check(img.data(), w, h, w, 17);
	CHECK(!r.current_frame_ok && r.observed_k == -1);
	CHECK(cal_line(17, r) ==
	      "[c3d] readback_cal k=17 orientation ok current frame NO (centre 0xFF123456) mismatched 94976 "
	      "(sentinel 0) channels ok corners "
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
		}
	r = cal_check(wide.data(), w, h, 512, 3);
	CHECK(r.orientation == Orientation::Ok && r.current_frame_ok && r.channels_ok && r.mismatched == 0);
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
	test_stats();
	test_calibrate();
	std::printf("%d checks, %d failures\n", s_checks, s_fail);
	return s_fail ? 1 : 0;
}
