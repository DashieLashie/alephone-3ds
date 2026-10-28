/*
	Aleph One for the Nintendo 3DS, citro3d back end: image comparison (see diff.h).

	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; either version 3 of the License, or
	(at your option) any later version.
*/
#include "diff.h"

#include <algorithm>
#include <cstdio>
#include <vector>

namespace a1_c3d {

namespace {

inline bool covered(uint32_t px, Coverage mode, uint32_t clear)
{
	return mode == Coverage::TopByte ? (px >> 24) != 0 : px != clear;
}

inline unsigned channel_delta(uint32_t a, uint32_t b)
{
	unsigned d = 0;
	for (int shift = 0; shift <= 16; shift += 8)
	{
		const int ca = static_cast<int>((a >> shift) & 0xFF);
		const int cb = static_cast<int>((b >> shift) & 0xFF);
		d = std::max(d, static_cast<unsigned>(ca > cb ? ca - cb : cb - ca));
	}
	return d;
}

} // namespace

DiffResult diff_images(const Image32& a, const Image32& b, Coverage mode, uint32_t clear, int tol,
                       const uint8_t* tie_mask)
{
	DiffResult r;
	const int w = std::min(a.width, b.width);
	const int h = std::min(a.height, b.height);
	if (w <= 0 || h <= 0)
		return r;

	std::vector<uint8_t> cov(static_cast<size_t>(w) * static_cast<size_t>(h));   // A's coverage
	for (int y = 0; y < h; ++y)
		for (int x = 0; x < w; ++x)
			cov[static_cast<size_t>(y) * w + x] =
				covered(a.px[static_cast<size_t>(y) * a.pitch + x], mode, clear) ? 1 : 0;

	for (int y = 0; y < h; ++y)
	{
		const uint32_t* pa = a.px + static_cast<size_t>(y) * a.pitch;
		const uint32_t* pb = b.px + static_cast<size_t>(y) * b.pitch;
		for (int x = 0; x < w; ++x)
		{
			const bool ca = cov[static_cast<size_t>(y) * w + x] != 0;
			const bool cb = covered(pb[x], mode, clear);
			r.covered_a += ca;
			r.covered_b += cb;
			if (pa[x] != pb[x])
				++r.differ;

			bool edge = false;
			for (int dy = -1; dy <= 1 && !edge; ++dy)
			{
				const int ny = y + dy;
				if (ny < 0 || ny >= h)
					continue;
				for (int dx = -1; dx <= 1; ++dx)
				{
					const int nx = x + dx;
					if ((dx == 0 && dy == 0) || nx < 0 || nx >= w)
						continue;
					if ((cov[static_cast<size_t>(ny) * w + nx] != 0) != ca)
					{
						edge = true;
						break;
					}
				}
			}

			if (tie_mask && tie_mask[static_cast<size_t>(y) * w + x])
			{
				edge = true;
				++r.ties;
				r.tie_mismatch += ca != cb;
			}

			r.band += edge;
			if (ca != cb)
			{
				if (edge)
					++r.cov_mismatch_edge;
				else
					++r.cov_mismatch_nonedge;
			}
			else if (ca && !edge)
			{
				const unsigned d = channel_delta(pa[x], pb[x]);
				++r.common;
				r.sum_delta += d;
				r.max_delta = std::max(r.max_delta, d);
				if (static_cast<long>(d) <= tol)
					++r.within_tol;
			}
		}
	}
	return r;
}

std::string diff_line(unsigned frame, const char* cls, const DiffResult& r, int tol)
{
	char buf[320];
	std::snprintf(buf, sizeof buf,
	              "[c3d] DIFF frame=%u class=%s covered=%u/%u cov_mismatch=%u/%u common=%u within_tol=%.2f%% "
	              "(tol %d) mean=%.2f max=%u differ=%u band=%u",
	              frame, cls, r.covered_a, r.covered_b, r.cov_mismatch_edge, r.cov_mismatch_nonedge, r.common,
	              r.within_pct(), tol, r.mean(), r.max_delta, r.differ, r.band);
	return std::string(buf);
}

std::string diff_line_c3d(unsigned frame, const char* cls, const DiffResult& r, int tol, unsigned excluded,
                          unsigned sentinel)
{
	char tail[128];
	std::snprintf(tail, sizeof tail, " b=c3d excluded=%u sentinel=%u ties=%u tie_mismatch=%u", excluded, sentinel,
	              r.ties, r.tie_mismatch);
	return diff_line(frame, cls, r, tol) + tail;
}

} // namespace a1_c3d
