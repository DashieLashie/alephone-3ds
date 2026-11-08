/*
	Aleph One for the Nintendo 3DS, citro3d back end: the fade curves' classification (core/fade_fit.h; design 7, M6b
	specification).

	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; either version 3 of the License, or
	(at your option) any later version.
*/
#include "fade_fit.h"

#include <cstring>

namespace a1_c3d {

namespace {

constexpr int kWindow = 2;   // candidates within 2 of the end-point estimate (the critic's replica: enough)

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

} // namespace

bool fade_fit_affine_channel(const uint16_t* curve, unsigned tol, FadeChannel& out)
{
	uint8_t f[kFadeEntries];
	for (int i = 0; i < kFadeEntries; ++i)
		f[i] = static_cast<uint8_t>(curve[i] >> 8);
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
	out.a8 = static_cast<uint8_t>(best_a);
	out.b8 = static_cast<uint8_t>(best_b);
	out.err = static_cast<uint8_t>(best > 255 ? 255 : best);
	return best <= tol;
}

FadeFit fade_fit(const uint16_t* r, const uint16_t* g, const uint16_t* b, unsigned tol, unsigned parts)
{
	FadeFit fit;
	const uint16_t* c[3] = {r, g, b};
	if (parts & kFadeAffine)
	{
		bool all = true;
		for (int ch = 0; ch < 3; ++ch)
		{
			all = fade_fit_affine_channel(c[ch], tol, fit.ch[ch]) && all;
			if (fit.ch[ch].err > fit.err)
				fit.err = fit.ch[ch].err;
		}
		if (all)
		{
			fit.kind = FadeKind::Affine;
			return fit;
		}
	}
	return fit;
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
	memcpy(m_c[0], r, n);
	memcpy(m_c[1], g, n);
	memcpy(m_c[2], b, n);
	m_tol = tol;
	m_parts = parts;
	m_valid = true;
	m_fit = fade_fit(r, g, b, tol, parts);
	++m_fits;
	return m_fit;
}

} // namespace a1_c3d
