/*
	Aleph One for the Nintendo 3DS, citro3d back end: the fade curves' classification (core/fade_fit.h; design 7, M6b
	specification).

	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; either version 3 of the License, or
	(at your option) any later version.
*/
#include "fade_fit.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace a1_c3d {

namespace {

constexpr int kWindow = 2;   // candidates within 2 of the end-point estimate (the critic's replica: enough)

// --- part 1 ---

unsigned affine_error(const uint8_t* f, unsigned a8, unsigned b8, unsigned stop)
{
	unsigned worst = 0;
	for (unsigned i = 0; i < kFadeEntries; ++i)
	{
		const unsigned m = fade_affine_gpu(i, a8, b8);
		const unsigned d = m > f[i] ? m - f[i] : f[i] - m;
		if (d > worst)
		{
			worst = d;
			if (worst > stop)
				break;   // already worse than the best so far
		}
	}
	return worst;
}

bool fit_affine8(const uint8_t* f, unsigned tol, FadeChannel& out)
{
	const int b0 = f[0], a0 = static_cast<int>(f[255]) - static_cast<int>(f[0]);
	unsigned best = 256;
	int best_a = 255, best_b = 0;
	for (int a = a0 - kWindow; a <= a0 + kWindow; ++a)
	{
		if (a < 0 || a > 255)
			continue;   // a negative slope or one above 1: no blend form
		for (int b = b0 - kWindow; b <= b0 + kWindow; ++b)
		{
			if (b < 0 || b > 255)
				continue;
			const unsigned e = affine_error(f, static_cast<unsigned>(a), static_cast<unsigned>(b), best);
			if (e < best)
			{
				best = e;
				best_a = a;
				best_b = b;
			}
		}
	}
	out = FadeChannel();
	out.a8 = static_cast<uint8_t>(best_a);
	out.b8 = static_cast<uint8_t>(best_b);
	out.err = static_cast<uint8_t>(best > 255 ? 255 : best);
	return best <= tol;
}

// --- part 2 ---

// SW's dodge stores a negative intermediate into a uint16 (fades.cpp dodge_color_table): from the entry where it goes
// negative the curve jumps from about 0 to about 255 and then falls (2 - t - g) until the int32 overflow's half (a
// rise). The clamped form (user decision) is 0 there. Returns the wrapped entries' range [*from, *to); from =
// kFadeNoWrap: none.
void find_wrap(const uint8_t* f, unsigned* from, unsigned* to)
{
	*from = *to = kFadeNoWrap;
	for (int j = 1; j < kFadeEntries; ++j)
		if (f[j - 1] <= 8 && f[j] >= 248)
		{
			int e = j + 1;
			while (e < kFadeEntries && f[e] <= f[e - 1] + 1)
				++e;
			*from = static_cast<unsigned>(j);
			*to = static_cast<unsigned>(e);
			return;
		}
}

// Entries whose fader input g(i) is above `trust` are not judged (the overflow pass: SW's curve is not the intended
// form there).
// The trusted pass judges at least this many entries (a real fader input below half brightness covers 127 or more:
// g = A i + B with A <= 1 and the liquids' B = 0), so a candidate with almost nothing judged cannot pass vacuously.
constexpr unsigned kMinTrusted = 100;

// `g`: fade_affine_gpu(i, c.a8, c.b8) for every i (the candidate's table, made once for its many K and sigma).
unsigned fold_error(const uint8_t* f, const uint8_t* g, const FadeChannel& c, unsigned stop, unsigned trust)
{
	unsigned worst = 0, judged = 0;
	for (unsigned i = 0; i < kFadeEntries; ++i)
	{
		if (g[i] > trust)
			continue;
		++judged;
		unsigned m = g[i];
		const unsigned h1 = fade_term(g[i], c.s1, c.k1);
		if (h1 < m)
			m = h1;
		const unsigned h2 = fade_term(g[i], c.s2, c.k2);
		if (h2 > m)
			m = h2;
		const unsigned d = m > f[i] ? m - f[i] : f[i] - m;
		if (d > worst)
		{
			worst = d;
			if (worst > stop)
				break;
		}
	}
	return trust < 255 && judged < kMinTrusted && worst <= stop ? 256 : worst;
}

// A part 2 curve is piecewise linear with slopes of at most 1: a few jumps at most (dodge's wrap, the int32 overflow's
// half). SW's static has a jump almost everywhere: no fold search for it (the search is the costly part of a fit).
bool too_rough_for_fold(const uint8_t* f)
{
	unsigned jumps = 0;
	for (int i = 1; i < kFadeEntries; ++i)
		jumps += std::abs(static_cast<int>(f[i]) - static_cast<int>(f[i - 1])) > 4;
	return jumps > 16;
}

// The largest count in a window of three bins (centres 1..n-2), skipping centres within 2 of `skip`.
unsigned top_cluster(const uint16_t* h, int n, int* centre, int skip = -100)
{
	unsigned best = 0;
	*centre = -1;
	for (int c = 1; c < n - 1; ++c)
	{
		if (std::abs(c - skip) <= 2)
			continue;
		const unsigned s = static_cast<unsigned>(h[c - 1]) + h[c] + h[c + 1];
		if (s > best)
		{
			best = s;
			*centre = c;
		}
	}
	return best;
}

// How well slope a8 explains the curve: the entries on one line of slope +a (the g region: f - g constant) plus those on
// the two largest lines of slope -a (the folds: f + g constant). Clamped entries (0, 255) carry no slope. `b_out`: the
// g region's B.
unsigned slope_score(const uint8_t* f, const uint8_t* run, int a8, int* b_out, unsigned trust)
{
	uint16_t e[511] = {}, p[511] = {};
	const int flat = 2 * 255 / a8 + 1;   // a run of equal values longer than slope a8 makes is a cap: no vote
	for (int i = 0; i < kFadeEntries; ++i)
	{
		if (f[i] == 0 || f[i] == 255 || run[i] > flat)
			continue;
		const int g = static_cast<int>(fade_div255(static_cast<unsigned>(i * a8 + 127)));
		if (g > static_cast<int>(trust))
			break;
		++e[f[i] - g + 255];
		++p[f[i] + g];
	}
	int ec = 0, pc = 0, pc2 = 0;
	const unsigned se = top_cluster(e, 511, &ec);
	const unsigned sp = top_cluster(p, 511, &pc);
	const unsigned sp2 = top_cluster(p, 511, &pc2, pc);
	*b_out = se >= 8 ? ec - 255 : 0;   // a g region of a few entries at least (a fold spreads 2 entries a bin)
	return se + sp + sp2;
}

int median(int* v, int n)
{
	std::nth_element(v, v + n / 2, v + n);
	return v[n / 2];
}

// A pass's stage 3 form (shared by the channels, pass_ok): Any, Sub (K <= 255: SUBTRACT) or Add (K >= 255: ADD with a
// one-minus operand). K = 255 suits both.
enum class Form { Any, Sub, Add };

bool form_ok(unsigned k, Form f)
{
	return f == Form::Any || (f == Form::Sub ? k <= 255 : k >= 255);
}

struct Cand { int a, b; };

// The candidates g = (a, b): K and sigma estimated from the regions below and above g, then the combinations
// verified (stopping at the first entry off by more than tol). True as soon as one is within tol (in `out`);
// `best_err` keeps the smallest error seen (approximate once over tol: for the log).
bool fit_fold_cands(const uint8_t* f, unsigned tol, FadeChannel& out, Form f1, Form f2, unsigned trust, const Cand* cands,
                    int nc, unsigned& best_err)
{
	const int itol = static_cast<int>(tol);
	for (int ci = 0; ci < nc; ++ci)
	{
		const Cand& cd = cands[ci];
		uint8_t gt[kFadeEntries];
		for (int i = 0; i < kFadeEntries; ++i)
			gt[i] = static_cast<uint8_t>(fade_affine_gpu(static_cast<unsigned>(i), static_cast<unsigned>(cd.a),
			                                             static_cast<unsigned>(cd.b)));
		// The regions against this g: below it (the MIN term), above it (the MAX term); K estimated per sigma.
		int e1[2][kFadeEntries], e2[2][kFadeEntries];
		int n1 = 0, n2 = 0, zeros = 0;
		for (int i = 0; i < kFadeEntries; ++i)
		{
			const int g = gt[i];
			if (g > static_cast<int>(trust))
				continue;
			const int d = f[i] - g;
			if (f[i] == 0)
			{
				zeros += d < -itol;
				continue;
			}
			if (f[i] == 255)
				continue;
			if (d < -itol)
			{
				e1[0][n1] = f[i];
				e1[1][n1++] = f[i] + g;
			}
			else if (d > itol)
			{
				e2[0][n2] = f[i];
				e2[1][n2++] = f[i] + g;
			}
		}
		int k1c[2][4], nk1[2] = {0, 0}, k2c[2][4], nk2[2] = {0, 0};
		for (int s = 0; s < 2; ++s)
		{
			if (n1)
			{
				int tmp[kFadeEntries];
				std::copy(e1[s], e1[s] + n1, tmp);
				const int m = median(tmp, n1);
				for (int dk = -1; dk <= 1; ++dk)
					k1c[s][nk1[s]++] = std::min(510, std::max(0, m + dk));
			}
			else if (zeros)
				k1c[s][nk1[s]++] = 0;   // the term is clamped to 0 wherever it bites
			else if (s == 0)
				k1c[s][nk1[s]++] = 255;   // no MIN term
			if (n2)
			{
				int tmp[kFadeEntries];
				std::copy(e2[s], e2[s] + n2, tmp);
				const int m = median(tmp, n2);
				for (int dk = -1; dk <= 1; ++dk)
					k2c[s][nk2[s]++] = std::min(510, std::max(0, m + dk));
			}
			else if (s == 0)
				k2c[s][nk2[s]++] = 0;   // no MAX term
		}
		for (int s1 = 0; s1 < 2; ++s1)
			for (int s2 = 0; s2 < 2; ++s2)
				for (int i1 = 0; i1 < nk1[s1]; ++i1)
					for (int i2 = 0; i2 < nk2[s2]; ++i2)
					{
						if (!form_ok(static_cast<unsigned>(k1c[s1][i1]), f1) ||
						    !form_ok(static_cast<unsigned>(k2c[s2][i2]), f2))
							continue;
						FadeChannel c;
						c.a8 = static_cast<uint8_t>(cd.a);
						c.b8 = static_cast<uint8_t>(cd.b);
						c.s1 = static_cast<uint8_t>(s1);
						c.k1 = static_cast<uint16_t>(k1c[s1][i1]);
						c.s2 = static_cast<uint8_t>(s2);
						c.k2 = static_cast<uint16_t>(k2c[s2][i2]);
						const unsigned e = fold_error(f, gt, c, std::min(best_err, tol), trust);
						if (e < best_err)
							best_err = e;
						if (e <= tol)
						{
							c.err = static_cast<uint8_t>(e);
							out = c;
							return true;
						}
					}
	}
	return false;
}

// One channel's curve (8 bits, wraps normalised) fitted to part 2's family, its MIN and MAX terms' K restricted to
// the pass forms f1, f2, judged on the entries whose fader input is at most `trust` (255: all). The identity g (no
// liquid) and `hint` (the last fit's g: the liquid stays through a fade) are tried before the slope search, the costly
// part. True when within tol.
bool fit_fold8(const uint8_t* f, unsigned tol, FadeChannel& out, Form f1, Form f2, unsigned trust,
               const FadeChannel* hint, const FadeChannel* hint_g, bool quick)
{
	if (trust == 255 && fit_affine8(f, tol, out))
		return true;   // an affine channel: identity terms (K 255 and 0 suit every form but Add for the MAX term)
	unsigned best_err = 256;
	Cand cands[12];
	int nc = 0;
	auto add = [&](int a, int b) {
		if (a < 1 || a > 255 || b < 0 || b > 255)
			return;
		for (int i = 0; i < nc; ++i)
			if (cands[i].a == a && cands[i].b == b)
				return;
		cands[nc++] = Cand{a, b};
	};
	add(255, 0);
	if (hint)
		add(hint->a8, hint->b8);
	if (hint_g)
		add(hint_g->a8, hint_g->b8);
	if (fit_fold_cands(f, tol, out, f1, f2, trust, cands, nc, best_err))
		return true;
	if (quick)
	{
		out = FadeChannel();
		out.err = static_cast<uint8_t>(best_err > 255 ? 255 : best_err);
		return false;
	}
	const int hints = nc;
	// The slope: a coarse scan (step 4, plus 255), then +-3 around the best. run[i]: the length of the run of equal
	// values entry i is in.
	uint8_t run[kFadeEntries];
	for (int i = 0; i < kFadeEntries;)
	{
		int j = i;
		while (j < kFadeEntries && f[j] == f[i])
			++j;
		for (int k = i; k < j; ++k)
			run[k] = static_cast<uint8_t>(std::min(255, j - i));
		i = j;
	}
	int best_a = 255, best_b = 0;
	unsigned best_s = 0;
	auto try_a = [&](int a) {
		int b = 0;
		const unsigned s = slope_score(f, run, a, &b, trust);
		if (s > best_s)
		{
			best_s = s;
			best_a = a;
			best_b = b;
		}
	};
	for (int a = 16; a < 256; a += 4)
		try_a(a);
	try_a(255);
	const int coarse = best_a;
	for (int a = coarse - 3; a <= coarse + 3; ++a)
		if (a >= 16 && a <= 255 && a != coarse)
			try_a(a);
	// The score's clusters are 3 bins wide, so its peak is known to about +-2: those slopes are all verified.
	for (int da = 0; da <= 4; ++da)
	{
		const int a = best_a + (da & 1 ? (da + 1) / 2 : -(da / 2));
		add(a, best_b);
		add(a, 0);
	}
	if (fit_fold_cands(f, tol, out, f1, f2, trust, cands + hints, nc - hints, best_err))
		return true;
	out = FadeChannel();
	out.err = static_cast<uint8_t>(best_err > 255 ? 255 : best_err);
	return false;
}

// A pass's stage 3 is shared by the three channels: ADD(1 - g, K - 1) as soon as one channel has K > 1, and then every
// channel needs K >= 1 (round 3, minor 6).
bool pass_ok(const FadeChannel* c, bool min_pass)
{
	bool add = false;
	for (int ch = 0; ch < 3; ++ch)
		add = add || (min_pass ? c[ch].k1 : c[ch].k2) > 255;
	if (!add)
		return true;
	for (int ch = 0; ch < 3; ++ch)
		if ((min_pass ? c[ch].k1 : c[ch].k2) < 255)
			return false;
	return true;
}

// --- part 3 ---

// Static (design 7, M6b "Part 3", rounds 3-4): per channel a base line g (the identity, else the lower supporting line
// of the curve, which part 1 must be able to draw), residuals r = curve - g as signed values (a negative one is SW's
// wrap above 65 535 and counts +65 536), one mask 2^k - 1 (k >= 8) for the three channels, every residual in
// [0, mask] (a fitted line's residuals +- its error), spread over it (mean in [mask/4, 3 mask/4], maximum >= mask/2)
// and rough (the mean step between neighbouring residuals >= mask/8: random offsets, not a smooth curve). A channel's
// mask is read from its mean residual (about mask/2 for SW's uniform offsets), far steadier than the maximum.
struct StaticChannel
{
	int r[kFadeEntries];
	long long sum = 0;
	int slack = 0;      // the base's error: one 8-bit step for the identity (a liquid at slope 1 sits at 256 i - 1)
	bool line = false;  // a fitted line: its error also grows with the mask
};

void residuals(const uint16_t* c, double alpha, double beta, int slack, bool line, StaticChannel& s)
{
	s.sum = 0;
	s.slack = slack;
	s.line = line;
	for (int i = 0; i < kFadeEntries; ++i)
	{
		int r = static_cast<int>(c[i]) - static_cast<int>(alpha + beta * i);
		if (r < -slack)
			r += 65536;   // SW's uint16 wrap-around
		s.r[i] = r;
		s.sum += r;
	}
}

// The lower supporting line of a curve with non-negative random offsets: a line through the minima of 16 windows of
// 16 entries (with `unwrap`, entries more than half the range below their window's maximum count +65 536 first: SW's
// wrap, ambiguous for masks of half the range and more, so both are tried), lowered by its most negative residual
// (round 4, major 1).
void support_line(const uint16_t* c, bool unwrap, double* alpha, double* beta)
{
	double sx = 0, sy = 0, sxx = 0, sxy = 0;
	for (int w = 0; w < 16; ++w)
	{
		int hi = 0;
		for (int i = 16 * w; i < 16 * w + 16; ++i)
			hi = std::max(hi, static_cast<int>(c[i]));
		int best = INT_MAX, bi = 16 * w;
		for (int i = 16 * w; i < 16 * w + 16; ++i)
		{
			int v = c[i];
			if (unwrap && hi - v > 32768)
				v += 65536;
			if (v < best)
			{
				best = v;
				bi = i;
			}
		}
		sx += bi;
		sy += best;
		sxx += static_cast<double>(bi) * bi;
		sxy += static_cast<double>(bi) * best;
	}
	const double den = 16 * sxx - sx * sx;
	const double b = den != 0 ? (16 * sxy - sx * sy) / den : 0.0;
	const double a = (sy - b * sx) / 16;
	int low = 0;
	for (int i = 0; i < kFadeEntries; ++i)
	{
		int r = static_cast<int>(c[i]) - static_cast<int>(a + b * i);
		if (r < -32768)
			r += 65536;
		if (r < 32768)
			low = std::min(low, r);
	}
	*alpha = a + low;
	*beta = b;
}

bool static_shape_ok(const StaticChannel& s, int mask)
{
	long long rough = 0;
	int mx = INT_MIN;
	const int slack = s.slack + (s.line ? mask / 16 : 0);
	for (int i = 0; i < kFadeEntries; ++i)
	{
		const int r = s.r[i];
		if (r < -slack || r > mask + slack)
			return false;
		mx = std::max(mx, r);
		if (i)
			rough += std::abs(r - s.r[i - 1]);
	}
	const long long mean = s.sum / kFadeEntries;
	return mean >= mask / 4 && mean <= 3LL * mask / 4 && mx >= mask / 2 && rough / (kFadeEntries - 1) >= mask / 8;
}

// The mask 2^k - 1 (k = 8..16) nearest (by ratio) to twice the mean residual.
int mask_from_mean(double mean)
{
	const double want = std::max(1.0, 2 * mean);
	int best = 255;
	double best_d = 1e9;
	for (int k = 8; k <= 16; ++k)
	{
		const int m = (1 << k) - 1;
		const double d = std::abs(std::log(want / m));
		if (d < best_d)
		{
			best_d = d;
			best = m;
		}
	}
	return best;
}

bool fit_static(const uint16_t* const* c, unsigned tol, FadeFit& fit)
{
	constexpr int kLineSlack = 128;
	// Per channel three bases: the identity (no liquid) and the supporting line without and with the unwrap (over a
	// liquid, drawable by part 1, not the identity). Of those whose residuals look like SW's offsets, the one with the
	// smallest residuals is the base (over a liquid the identity's residuals wrap, mean about half the range); the
	// identity unless a line is clearly lower.
	StaticChannel base[3][3];   // [ch][0] identity, [1] line, [2] line with the unwrap
	FadeChannel line_ch[3][3];
	for (int ch = 0; ch < 3; ++ch)
	{
		residuals(c[ch], 0.0, 256.0, 256, false, base[ch][0]);
		line_ch[ch][0] = FadeChannel();
		for (int u = 1; u <= 2; ++u)
		{
			double a = 0, b = 0;
			support_line(c[ch], u == 2, &a, &b);
			residuals(c[ch], a, b, kLineSlack, true, base[ch][u]);
			uint16_t line[kFadeEntries];
			for (int i = 0; i < kFadeEntries; ++i)
				line[i] = static_cast<uint16_t>(std::min(65535.0, std::max(0.0, a + b * i)));
			const bool drawable = fade_fit_affine_channel(line, tol, line_ch[ch][u]);
			const bool near_identity = std::abs(b - 256.0) < 8.0 && std::abs(a) < 1024.0;
			if (!drawable || near_identity)
				base[ch][u].sum = -1;   // not a candidate
		}
	}
	int pick[3];
	for (int ch = 0; ch < 3; ++ch)
	{
		pick[ch] = -1;
		bool ok[3];
		for (int o = 0; o < 3; ++o)
			ok[o] = base[ch][o].sum >= 0 &&
			        static_shape_ok(base[ch][o], mask_from_mean(base[ch][o].sum / double(kFadeEntries)));
		for (int o = 1; o <= 2; ++o)
			if (ok[o] && (pick[ch] < 0 || base[ch][o].sum < base[ch][pick[ch]].sum))
				pick[ch] = o;
		if (ok[0] && (pick[ch] < 0 || base[ch][0].sum <= base[ch][pick[ch]].sum * 9 / 8 + 256 * kFadeEntries))
			pick[ch] = 0;
		if (pick[ch] < 0)
			return false;
	}
	// SW's one mask: the channels' masks within a factor of 2 of each other (a base's error at a large mask can move
	// one of them a step); the static takes the largest.
	int mask = 0, lo = 65535;
	for (int ch = 0; ch < 3; ++ch)
	{
		const int m = mask_from_mean(base[ch][pick[ch]].sum / double(kFadeEntries));
		mask = std::max(mask, m);
		lo = std::min(lo, m);
		fit.ch[ch] = line_ch[ch][pick[ch]];
	}
	if (mask > 2 * lo + 1)
		return false;
	fit.mask = static_cast<uint16_t>(mask);
	fit.err = 0;
	return true;
}

} // namespace

const char* fade_kind_name(FadeKind k)
{
	switch (k)
	{
	case FadeKind::Affine:
		return "affine";
	case FadeKind::Fold:
		return "fold";
	case FadeKind::Static:
		return "static";
	case FadeKind::None:
		break;
	}
	return "no";
}

bool fade_fit_affine_channel(const uint16_t* curve, unsigned tol, FadeChannel& out)
{
	uint8_t f[kFadeEntries];
	for (int i = 0; i < kFadeEntries; ++i)
		f[i] = static_cast<uint8_t>(curve[i] >> 8);
	return fit_affine8(f, tol, out);
}

void fade_reference(const uint16_t* curve, const FadeFit& fit, int ch, uint8_t* out)
{
	const FadeChannel& c = fit.ch[ch];
	for (unsigned i = 0; i < kFadeEntries; ++i)
	{
		if (fit.kind == FadeKind::Fold && fit.trust[ch] < 255 && fade_affine_gpu(i, c.a8, c.b8) > fit.trust[ch])
			out[i] = static_cast<uint8_t>(fade_fold_gpu(i, c));
		else if (fit.kind == FadeKind::Fold && i >= fit.wrap_from[ch] && i < fit.wrap_to[ch])
			out[i] = 0;
		else
			out[i] = static_cast<uint8_t>(curve[i] >> 8);
	}
}

FadeFit fade_fit(const uint16_t* r, const uint16_t* g, const uint16_t* b, unsigned tol, unsigned parts,
                 const FadeFit* hint, const FadeFit* hint_g)
{
	FadeFit fit;
	const uint16_t* c[3] = {r, g, b};
	bool all = true;
	for (int ch = 0; ch < 3; ++ch)
	{
		all = fade_fit_affine_channel(c[ch], tol, fit.ch[ch]) && all;
		fit.err = std::max(fit.err, static_cast<unsigned>(fit.ch[ch].err));
	}
	if (all && (parts & kFadeAffine))
	{
		fit.kind = FadeKind::Affine;
		return fit;
	}
	const FadeFit none = fit;   // a rejected fit reports part 1's error
	if (parts & kFadeFold)
	{
		FadeFit f2;
		uint8_t f[3][kFadeEntries];
		for (int ch = 0; ch < 3; ++ch)
		{
			for (int i = 0; i < kFadeEntries; ++i)
				f[ch][i] = static_cast<uint8_t>(c[ch][i] >> 8);
			unsigned from = 0, to = 0;
			find_wrap(f[ch], &from, &to);
			f2.wrap_from[ch] = static_cast<uint16_t>(from);
			f2.wrap_to[ch] = static_cast<uint16_t>(to);
			for (unsigned i = from; i < to; ++i)
				f[ch][i] = 0;
		}
		// Unconstrained first; when the channels' forms clash (one wants a cap, K < 1, where another needs K > 1 in the
		// same pass: an equivalent form may exist), each pair of pass forms in turn.
		const Form forms[5][2] = {{Form::Any, Form::Any}, {Form::Sub, Form::Sub}, {Form::Add, Form::Sub},
		                          {Form::Sub, Form::Add}, {Form::Add, Form::Add}};
		bool rough = false;
		for (int ch = 0; ch < 3; ++ch)
			rough = rough || too_rough_for_fold(f[ch]);
		// Two rounds: SW's curve alone on every channel through every form pair (exact fits win), then with the
		// trusted-entries pass (the intended form where SW's int32 overflow distorts dodge and burn: user decision
		// 2026-11-08). The first round is skipped when the hint needed the second (the overflow stays through a fade).
		bool hint_trusted = false;
		if (hint && hint->kind == FadeKind::Fold)
			for (int ch = 0; ch < 3; ++ch)
				hint_trusted = hint_trusted || hint->trust[ch] != 255;
		// Each round has a quick phase first (only the identity g and the hints' g: the liquid's, from the last affine
		// fit, or the last fold's), then the thorough one (the slope search).
		for (int step = hint_trusted ? 2 : 0; step < 4 && !rough; ++step)
		{
			const int round = step / 2;
			const bool quick = step % 2 == 0;
			if (!quick && (parts & kFadeQuickOnly))
				continue;
			// The unconstrained fit of each channel, reused by a later form pair it already suits.
			FadeChannel any_fit[3];
			uint16_t any_trust[3] = {0, 0, 0};
			bool any_ok[3] = {false, false, false};
			for (const auto& fm : forms)
			{
				bool ok = true;
				f2.err = 0;
				for (int ch = 0; ch < 3 && ok; ++ch)
				{
					if (fm[0] != Form::Any && any_ok[ch] && form_ok(any_fit[ch].k1, fm[0]) &&
					    form_ok(any_fit[ch].k2, fm[1]))
					{
						f2.ch[ch] = any_fit[ch];
						f2.trust[ch] = any_trust[ch];
					}
					else
					{
						const FadeChannel* h = hint && hint->kind == FadeKind::Fold ? &hint->ch[ch] : nullptr;
						const FadeChannel* hg = hint_g && hint_g->kind == FadeKind::Affine ? &hint_g->ch[ch] : nullptr;
						// Round 1: a channel the hint fitted on the trusted entries is tried that way first.
						const unsigned first = round == 1 && h && hint->trust[ch] != 255 ? kFadeOverflowTrust : 255;
						f2.trust[ch] = static_cast<uint16_t>(first);
						ok = fit_fold8(f[ch], tol, f2.ch[ch], fm[0], fm[1], first, h, hg, quick);
						if (!ok && round == 1)
						{
							const unsigned second = first == 255 ? kFadeOverflowTrust : 255;
							f2.trust[ch] = static_cast<uint16_t>(second);
							ok = fit_fold8(f[ch], tol, f2.ch[ch], fm[0], fm[1], second, h, hg, quick);
						}
						if (fm[0] == Form::Any)
						{
							any_fit[ch] = f2.ch[ch];
							any_trust[ch] = f2.trust[ch];
							any_ok[ch] = ok;
						}
					}
					f2.err = std::max(f2.err, static_cast<unsigned>(f2.ch[ch].err));
				}
				if (ok && pass_ok(f2.ch, true) && pass_ok(f2.ch, false))
				{
					f2.kind = FadeKind::Fold;
					return f2;
				}
				if (!ok && fm[0] == Form::Any)
					break;   // a channel fits no form at all in this round
			}
		}
	}
	if (parts & kFadeStatic)
	{
		FadeFit f3;
		if (fit_static(c, tol, f3))
		{
			f3.kind = FadeKind::Static;
			return f3;
		}
	}
	return none;
}

const FadeFit& FadeCache::get(const uint16_t* r, const uint16_t* g, const uint16_t* b, unsigned tol, unsigned parts,
                              bool* changed)
{
	const size_t n = sizeof m_c[0];
	const bool same = m_valid && m_tol == tol && m_parts == parts && !memcmp(m_c[0], r, n) &&
	                  !memcmp(m_c[1], g, n) && !memcmp(m_c[2], b, n);
	if (changed)
		*changed = !same;
	if (same)
		return m_fit;
	// The hints: the last fold fit, and the last affine one (a liquid's g, the base of a fold over it). After a fit that
	// found nothing, the quick phase only (invalidate(), at the identity, lifts it).
	const bool quick_only = m_valid && m_fit.kind == FadeKind::None;
	memcpy(m_c[0], r, n);
	memcpy(m_c[1], g, n);
	memcpy(m_c[2], b, n);
	m_tol = tol;
	m_parts = parts;
	m_valid = true;
	m_fit = fade_fit(r, g, b, tol, parts | (quick_only ? kFadeQuickOnly : 0u), &m_last_fold, &m_last_affine);
	if (m_fit.kind == FadeKind::Fold)
		m_last_fold = m_fit;
	else if (m_fit.kind == FadeKind::Affine)
		m_last_affine = m_fit;
	++m_fits;
	return m_fit;
}

} // namespace a1_c3d
