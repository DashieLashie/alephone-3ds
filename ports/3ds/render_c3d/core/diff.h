/*
	Aleph One for the Nintendo 3DS, citro3d back end: the image comparison of the diff
	harness (docs/3ds/CITRO3D_DESIGN.md, 8.1 and section 7, M0 specification, "Diff frame").
	Plain C++, host-testable.

	Coverage of a pixel: TopByte: the alpha byte is not 0; NotClear: the pixel is not the
	clear value. All (M4: the frame and play checks, design 7 "M4 specification", "Diff checks"): no coverage
	and no band; the common pixels are those where A or B differs from the clear in RGB, the others count as
	`empty`; the neighbourhood minimum runs over A's 8 in-image neighbours (when only one side drew the pixel,
	over A's neighbours that are not empty). An edge pixel has, inside the image, an 8-neighbour whose coverage in A is
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

enum class Coverage { TopByte, NotClear, All };

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
	unsigned ties = 0;          // tie-mask pixels (M1 piece 5), inside the image
	unsigned tie_mismatch = 0;  // tie-mask pixels whose coverage differs between A and B
	// Neighbourhood metric (M2 colour gate): over the same common pixels, the delta is the minimum of the
	// channel delta to A's pixel and to each of its 8 in-image neighbours that are covered in A.
	unsigned within_nb = 0;
	uint64_t sum_nb = 0;
	unsigned max_nb = 0;
	unsigned band = 0;     // A's edge-band pixels: those with an 8-neighbour whose coverage in A differs
	unsigned empty = 0;    // Coverage::All: pixels where both A and B hold the clear's RGB (not compared)

	double mean() const { return common ? static_cast<double>(sum_delta) / common : 0.0; }
	double mean_nb() const { return common ? static_cast<double>(sum_nb) / common : 0.0; }
	double within_nb_pct() const { return common ? 100.0 * within_nb / common : 100.0; }
	double within_pct() const { return common ? 100.0 * within_tol / common : 100.0; }
};

// a and b must have the same width and height (the smaller of the two is used otherwise).
// tie_mask (optional, nullptr = M0 behaviour): one byte per pixel, row stride = the common width, non-zero =
// an exact-tie pixel (core/geometry tie_pixels). A tie pixel counts as band (edge) for classification, so its
// coverage mismatch is an edge mismatch; `ties` and `tie_mismatch` count the mask pixels.
DiffResult diff_images(const Image32& a, const Image32& b, Coverage mode, uint32_t clear, int tol,
                       const uint8_t* tie_mask = nullptr);

// M5 (design 7, M5 specification "Diff checks"): Coverage::All over the pixels whose `exclude` byte is 0 (row stride =
// the common width); excluded pixels are not compared, not used as neighbours and counted in *excluded (if
// non-null). The play check of a frame with static (the noise differs per pixel by design).
DiffResult diff_images_excluding(const Image32& a, const Image32& b, uint32_t clear, int tol, const uint8_t* exclude,
                                 unsigned* excluded);

// M5 "static": A = SW's static draws, B = C3D's, C = SW's with every drop forced to 0 (SW's coverage), all over a
// clear 0 (a written pixel is != 0: SW's static value is never 0, its top byte is). written_b counts B's pixels inside
// C; B's pixels outside C are `out`, split by C's edge band (a pixel with an 8-neighbour of the other C coverage).
struct StaticResult
{
	unsigned cover = 0;                 // |C|
	unsigned written_a = 0;             // |A| (inside C by construction; A outside C counted in a_out)
	unsigned written_b = 0;             // |B inside C|
	unsigned out_edge = 0, out_nonedge = 0;
	unsigned a_out = 0;                 // A outside C (never, by construction: a check of the harness)
};
StaticResult diff_static(const Image32& a, const Image32& b, const Image32& c);
// "[c3d] DIFF frame=F class=static cover=C written=A/B density=DA/DB out=E/N a_out=X b=c3d excluded=X sentinel=S".
std::string static_line(unsigned frame, const StaticResult& r, unsigned excluded, unsigned sentinel);

// "[c3d] DIFF frame=F class=C covered=A/B cov_mismatch=E/N common=K within_tol=P% (tol T) mean=M max=X
// differ=D band=N", no newline.
std::string diff_line(unsigned frame, const char* cls, const DiffResult& r, int tol);

// The same with M1's C3D fields appended: " b=c3d excluded=X sentinel=S ties=T tie_mismatch=M within_nb=P% mean_nb=M max_nb=X empty=E" (X: primitives the C3D side
// dropped, section 7 M1 "What C3D draws"; S: readback pixels still holding the staging sentinel).
std::string diff_line_c3d(unsigned frame, const char* cls, const DiffResult& r, int tol, unsigned excluded,
                          unsigned sentinel);

} // namespace a1_c3d

#endif
