// Host tests for render_c3d/core: the debug-c3d parser (piece 5 keys) and core/gate.h
// (docs/3ds/CITRO3D_DESIGN.md 5.6.1, 8.4).
#include "config.h"
#include "gate.h"

#include <cstdio>
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

int main()
{
	test_config();
	test_gate_inputs();
	test_tracker();
	test_retire_wait();
	std::printf("%d checks, %d failures\n", s_checks, s_fail);
	return s_fail ? 1 : 0;
}
