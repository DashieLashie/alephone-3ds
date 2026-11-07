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

// Coverage::All (see diff.h); `exclude` (M5, optional): pixels not compared and not used as neighbours.
DiffResult diff_all(const Image32& a, const Image32& b, uint32_t clear, int tol, int w, int h,
                    const uint8_t* exclude = nullptr, unsigned* excluded = nullptr)
{
	DiffResult r;
	unsigned nex = 0;
	for (int y = 0; y < h; ++y)
	{
		const uint32_t* pa = a.px + static_cast<size_t>(y) * a.pitch;
		const uint32_t* pb = b.px + static_cast<size_t>(y) * b.pitch;
		for (int x = 0; x < w; ++x)
		{
			if (exclude && exclude[static_cast<size_t>(y) * w + x])
			{
				++nex;
				continue;
			}
			const bool ea = ((pa[x] ^ clear) & 0x00FFFFFFu) == 0, eb = ((pb[x] ^ clear) & 0x00FFFFFFu) == 0;
			r.covered_a += !ea;
			r.covered_b += !eb;
			if (pa[x] != pb[x])
				++r.differ;
			if (ea && eb)
			{
				++r.empty;
				continue;
			}
			const unsigned d = channel_delta(pa[x], pb[x]);
			++r.common;
			r.sum_delta += d;
			r.max_delta = std::max(r.max_delta, d);
			if (static_cast<long>(d) <= tol)
				++r.within_tol;
			unsigned dn = d;
			for (int dy = -1; dy <= 1; ++dy)
			{
				const int ny = y + dy;
				if (ny < 0 || ny >= h)
					continue;
				for (int dx = -1; dx <= 1; ++dx)
				{
					const int nx = x + dx;
					if ((dx == 0 && dy == 0) || nx < 0 || nx >= w)
						continue;
					if (exclude && exclude[static_cast<size_t>(ny) * w + nx])
						continue;
					const uint32_t an = a.px[static_cast<size_t>(ny) * a.pitch + nx];
					// One side empty (a pixel only one renderer drew): an empty A-neighbour must not match B's
					// clear and hide the miss (M4 critic round 2, minor 2).
					if (ea != eb && ((an ^ clear) & 0x00FFFFFFu) == 0)
						continue;
					dn = std::min(dn, channel_delta(an, pb[x]));
				}
			}
			r.sum_nb += dn;
			r.max_nb = std::max(r.max_nb, dn);
			if (static_cast<long>(dn) <= tol)
				++r.within_nb;
		}
	}
	if (excluded)
		*excluded = nex;
	return r;
}

} // namespace

DiffResult diff_images_excluding(const Image32& a, const Image32& b, uint32_t clear, int tol, const uint8_t* exclude,
                                 unsigned* excluded)
{
	const int w = std::min(a.width, b.width);
	const int h = std::min(a.height, b.height);
	if (excluded)
		*excluded = 0;
	if (w <= 0 || h <= 0)
		return DiffResult();
	return diff_all(a, b, clear, tol, w, h, exclude, excluded);
}

StaticResult diff_static(const Image32& a, const Image32& b, const Image32& c)
{
	StaticResult r;
	const int w = std::min(std::min(a.width, b.width), c.width);
	const int h = std::min(std::min(a.height, b.height), c.height);
	auto in_c = [&](int x, int y) { return c.px[static_cast<size_t>(y) * c.pitch + x] != 0; };
	for (int y = 0; y < h; ++y)
		for (int x = 0; x < w; ++x)
		{
			const bool cc = in_c(x, y);
			const bool wa = a.px[static_cast<size_t>(y) * a.pitch + x] != 0;
			const bool wb = b.px[static_cast<size_t>(y) * b.pitch + x] != 0;
			r.cover += cc;
			if (cc)
			{
				r.written_a += wa;
				r.written_b += wb;
				continue;
			}
			r.a_out += wa;
			if (!wb)
				continue;
			bool edge = false;
			for (int dy = -1; dy <= 1 && !edge; ++dy)
				for (int dx = -1; dx <= 1; ++dx)
				{
					const int nx = x + dx, ny = y + dy;
					if ((dx || dy) && nx >= 0 && nx < w && ny >= 0 && ny < h && in_c(nx, ny))
					{
						edge = true;
						break;
					}
				}
			if (edge)
				++r.out_edge;
			else
				++r.out_nonedge;
		}
	return r;
}

std::string static_line(unsigned frame, const StaticResult& r, unsigned excluded, unsigned sentinel)
{
	char buf[256];
	const double da = r.cover ? 100.0 * r.written_a / r.cover : 0.0, db = r.cover ? 100.0 * r.written_b / r.cover : 0.0;
	std::snprintf(buf, sizeof buf,
	              "[c3d] DIFF frame=%u class=static cover=%u written=%u/%u density=%.3f/%.3f out=%u/%u a_out=%u b=c3d "
	              "excluded=%u sentinel=%u",
	              frame, r.cover, r.written_a, r.written_b, da, db, r.out_edge, r.out_nonedge, r.a_out, excluded, sentinel);
	return std::string(buf);
}

DiffResult diff_images(const Image32& a, const Image32& b, Coverage mode, uint32_t clear, int tol,
                       const uint8_t* tie_mask)
{
	DiffResult r;
	const int w = std::min(a.width, b.width);
	const int h = std::min(a.height, b.height);
	if (w <= 0 || h <= 0)
		return r;
	if (mode == Coverage::All)
		return diff_all(a, b, clear, tol, w, h);

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
				unsigned dn = d;
				for (int dy = -1; dy <= 1; ++dy)
				{
					const int ny = y + dy;
					if (ny < 0 || ny >= h)
						continue;
					for (int dx = -1; dx <= 1; ++dx)
					{
						const int nx = x + dx;
						if ((dx == 0 && dy == 0) || nx < 0 || nx >= w)
							continue;
						// Always true for a common pixel (covered and not edge, so every in-image neighbour is
						// covered in A); kept as the definition's guard.
						if (cov[static_cast<size_t>(ny) * w + nx])
							dn = std::min(dn, channel_delta(a.px[static_cast<size_t>(ny) * a.pitch + nx], pb[x]));
					}
				}
				r.sum_nb += dn;
				r.max_nb = std::max(r.max_nb, dn);
				if (static_cast<long>(dn) <= tol)
					++r.within_nb;
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
	char tail[208];
	std::snprintf(tail, sizeof tail,
	              " b=c3d excluded=%u sentinel=%u ties=%u tie_mismatch=%u within_nb=%.2f%% mean_nb=%.2f max_nb=%u empty=%u",
	              excluded, sentinel, r.ties, r.tie_mismatch, r.within_nb_pct(), r.mean_nb(), r.max_nb, r.empty);
	return diff_line(frame, cls, r, tol) + tail;
}

} // namespace a1_c3d
