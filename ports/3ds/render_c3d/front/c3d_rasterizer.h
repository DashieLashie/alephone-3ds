/*
	Aleph One for the Nintendo 3DS, citro3d back end: the C3D RasterizerClass (docs/3ds/CITRO3D_DESIGN.md,
	section 7, M1 specification, "What C3D draws in M1" and "Polygon acceptance"; M2 specification,
	"What C3D draws in M2", "Texcoords"). Engine headers and the plain headers of back/ and core/ only: no
	<3ds.h>, no <citro3d.h>.

	Receives the traversal from the harness (through FilterRasterizer), accepts the walls and floors
	c3d_supports() allows exactly as the software mapper would draw them (core/geometry accept_polygon,
	vertical or horizontal), builds their vertices (position, perspective weight, texcoords) and hands them
	to back/draw.cpp (view_polygon). Any other primitive discards the view frame (view_discard(Unsupported)).

	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; either version 3 of the License, or
	(at your option) any later version.
*/
#ifndef A1_C3D_FRONT_RASTERIZER_H
#define A1_C3D_FRONT_RASTERIZER_H

#include "cseries.h"
#include "render.h"
#include "Rasterizer.h"
#include "scottish_textures.h"

#include <cstdint>
#include <vector>

#include "../core/geometry.h"

namespace a1_c3d {

class C3dRasterizer : public RasterizerClass
{
public:
	// The size of the target the polygons are accepted against (the view_begin size); resets the counters.
	void set_target(int w, int h);
	// debug-c3d `textures` (the support predicate's transparent-wall rule), `uv_half` (the attribute
	// shift of -1/2 pixel) and `lit` (texcoord1 for the shade LUT, M3; with textures only). Persist across
	// set_target.
	void set_options(bool textures, bool uv_half, bool lit)
	{
		textures_ = textures;
		uv_half_ = uv_half;
		lit_ = lit;
	}

	void SetView(view_data& View) override { view_ = &View; }
	void Begin() override { foreground_ = false; }
	void SetForeground() override { foreground_ = true; }

	void texture_horizontal_polygon(polygon_definition&) override;
	void texture_vertical_polygon(polygon_definition& p) override;
	void texture_rectangle(rectangle_definition&) override;

	// The tie mask (one byte per pixel of the target, row stride w): the tie pixels (core/geometry tie_pixels)
	// of every polygon handed to view_polygon since set_target. Cleared by set_target; allocated once per size.
	const uint8_t* tie_mask() const { return tie_mask_.data(); }

	unsigned drawn() const { return drawn_; }
	// Polygons drawn as SW's spans (not convex: design 7, M2 status, the non-convex sliver) and their quads,
	// since the start (not reset by set_target).
	unsigned span_polygons() const { return span_polygons_; }
	unsigned span_quads() const { return span_quads_; }
	// The most quads one polygon needed, and the polygons refused because the ring could not take them all.
	int span_quads_max() const { return span_quads_max_; }
	unsigned span_ring_refused() const { return span_ring_refused_; }
	// Polygons whose texcoords came out non-finite or too spread (TexResult::NonFinite) since the start.
	unsigned nonfinite() const { return nonfinite_; }
	// Skipped polygons (SW draws nothing for them) by Accept kind (index = Accept; Draw is 0). A floor at
	// eye height (o.z == 0) counts as Degenerate.
	unsigned skipped(Accept a) const { return skipped_[static_cast<int>(a)]; }

	// M3 counters since the start (not reset by set_target; design 7, M3 "Counters"): polygons drawn with depth
	// and ambient shading, shadeless (infravision), with a negative ambient; polygons refused because M is above
	// SW's SHRT_MAX equivalence (discard); targets (checks) whose M exceeded NATURAL_LIGHT_INTENSITY (the weapon
	// flash), and the largest M seen.
	unsigned lit_polys() const { return lit_polys_; }
	unsigned shadeless_polys() const { return shadeless_polys_; }
	unsigned neg_ambient_polys() const { return neg_ambient_polys_; }
	unsigned flash_over() const { return flash_over_; }
	unsigned flash_checks() const { return flash_checks_; }
	long m_max() const { return m_max_; }

private:
	view_data* view_ = nullptr;
	int w_ = 0, h_ = 0;
	void polygon(Entry e, polygon_definition& p);
	void count_lit(bool lit, bool shadeless, bool negative)
	{
		if (!lit)
			return;
		if (shadeless)
			++shadeless_polys_;
		else
		{
			++lit_polys_;
			neg_ambient_polys_ += negative;
		}
	}
	// The attributes of n points of the polygon (wall or floor row of 2.2) into `at`.
	TexResult attrs(bool horizontal, const polygon_definition& p, const Point2* pts, int n, TexAttr* at) const;

	bool foreground_ = false;
	bool textures_ = false;
	bool uv_half_ = true;
	bool lit_ = true;
	bool m_noted_ = false;   // this target's M was looked at (reset by set_target)
	unsigned lit_polys_ = 0, shadeless_polys_ = 0, neg_ambient_polys_ = 0, flash_over_ = 0, flash_checks_ = 0;
	unsigned excluded_reached_ = 0;   // an Excluded ambient past the filter (a bug; logged once)
	long m_max_ = 0;
	unsigned drawn_ = 0;
	unsigned nonfinite_ = 0;   // not reset by set_target
	unsigned span_polygons_ = 0, span_quads_ = 0;   // not reset by set_target
	int span_quads_max_ = 0;
	unsigned span_ring_refused_ = 0;
	int span_len_[1024];   // lines per merged span rectangle (scratch of polygon())
	std::vector<uint8_t> tie_mask_;
	unsigned skipped_[5] = {0, 0, 0, 0, 0};
};

} // namespace a1_c3d

#endif
