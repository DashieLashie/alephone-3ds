/*
	Aleph One for the Nintendo 3DS, citro3d back end: the image comparison of the diff
	harness (docs/3ds/CITRO3D_DESIGN.md, 8.1 and section 7, M0 specification, "Diff frame").
	Plain C++, host-testable.

	Coverage of a pixel: TopByte: the alpha byte is not 0; NotClear: the pixel is not the
	clear value. An edge pixel has, inside the image, an 8-neighbour whose coverage in A is
	different. Over the common pixels (covered in both, not edge) the delta of a pixel is the
	largest |a-b| over its R, G and B channels.

	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; either version 3 of the License, or
	(at your option) any later version.
*/
#ifndef A1_C3D_CORE_DIFF_H
#define A1_C3D_CORE_DIFF_H

#include <cstdint>
#include <string>

namespace a1_c3d {

enum class Coverage { TopByte, NotClear };

struct Image32
{
	const uint32_t* px;
	int width, height;
	int pitch;   // in pixels
};

struct DiffResult
{
	unsigned covered_a = 0, covered_b = 0;
	unsigned cov_mismatch_edge = 0, cov_mismatch_nonedge = 0;
	unsigned common = 0, within_tol = 0;
	uint64_t sum_delta = 0;
	unsigned max_delta = 0;
	unsigned differ = 0;   // pixels anywhere whose 32 bits differ

	double mean() const { return common ? static_cast<double>(sum_delta) / common : 0.0; }
	double within_pct() const { return common ? 100.0 * within_tol / common : 100.0; }
};

// a and b must have the same width and height (the smaller of the two is used otherwise).
DiffResult diff_images(const Image32& a, const Image32& b, Coverage mode, uint32_t clear, int tol);

// "[c3d] DIFF frame=F class=C covered=A/B cov_mismatch=E/N common=K within_tol=P% (tol T) mean=M max=X
// differ=D", no newline.
std::string diff_line(unsigned frame, const char* cls, const DiffResult& r, int tol);

} // namespace a1_c3d

#endif
