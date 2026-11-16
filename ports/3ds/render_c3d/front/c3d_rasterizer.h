/*
	Aleph One for the Nintendo 3DS, citro3d back end: the C3D RasterizerClass (docs/3ds/CITRO3D_DESIGN.md,
	section 7, M1 specification, "What C3D draws in M1" and "Polygon acceptance"; M2 specification,
	"What C3D draws in M2", "Texcoords"). Engine headers and the plain headers of back/ and core/ only: no
	<3ds.h>, no <citro3d.h>.

	Receives the traversal from the harness (through FilterRasterizer), accepts the walls and floors
	c3d_supports() allows exactly as the software mapper would draw them (core/geometry accept_polygon,
	vertical or horizontal), builds their vertices (position, perspective weight, texcoords) and hands them
	to back/draw.cpp (view_polygon). M4: rectangles (sprites, weapons, tinted) as one quad each (core/rect).
	Any other primitive discards the view frame (view_discard(Unsupported)).

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

#include <climits>
#include <cstdint>
#include <vector>

#include "../core/geometry.h"
#include "../core/stereo.h"

namespace a1_c3d {

class C3dRasterizer : public RasterizerClass
{
public:
	// The size of the target the polygons are accepted against (the view_begin size); resets the counters.
	void set_target(int w, int h);
	// debug-c3d `textures` (the support predicate's transparent-wall rule), `uv_half` (the attribute
	// shift of -1/2 pixel) and `lit` (texcoord1 for the shade LUT, M3; with textures only). Persist across
	// set_target.
	void set_options(bool textures, bool uv_half, bool lit, unsigned rect_bias = 1)
	{
		textures_ = textures;
		uv_half_ = uv_half;
		lit_ = lit;
		rect_bias_ = rect_bias;
	}

	// M9 (design 7, "M9 specification", "What w a vertex carries on a stereo frame"): on a stereo frame the vertices carry
	// the w of core/stereo.h stereo_w (rectangles their depth, the weapon `weapon_w`, landscapes kWPerspMax, unmasked static
	// polygons their surface's depth); on any other frame nothing changes (w = 1 where it was 1). Persists across set_target;
	// the frame sets it every frame, after set_target.
	// M9 fix (design 7, "M9 fix: the edge strip"): edge_px > 0 continues every primitive the engine clipped at the view's
	// left or right edge by that many pixels on stereo frames (core/stereo.h stereo_edge_strip), so a shifted eye has
	// content at its edges; 0 = M9's behaviour.
	void set_stereo(bool on, float weapon_w, int edge_px = 0)
	{
		stereo_ = on;
		weapon_w_ = weapon_w;
		edge_ = on && edge_px > 0 ? edge_px : 0;
	}
	// Edge strips drawn / not drawn (attributes not finite past the edge, or no ring room). Not reset by set_target.
	unsigned edge_strips() const { return edge_strips_; }
	unsigned edge_skipped() const { return edge_skipped_; }
	// Vertices issued on stereo frames with w < 16 (kWMin: the smallest a wall or floor carries): must stay 0, it catches any
	// path that still sends w = 1 (the landscape span path included). Not reset by set_target.
	unsigned w_low() const { return w_low_; }
	unsigned stereo_vertices() const { return stereo_vertices_; }

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

	// M4 counters since the start (design 7, M4 specification "Front end"): rectangles drawn, of them tinted;
	// rectangles SW draws nothing for (rect_setup false); rectangles C3D cannot draw (the view frame is discarded).
	unsigned rects() const { return rects_; }
	unsigned tinted_rects() const { return tinted_rects_; }
	unsigned rect_skipped() const { return rect_skipped_; }
	unsigned rect_unsupported() const { return rect_unsupported_; }

	// M5 counters since the start (design 7, M5 specification "Front end"): landscape polygons drawn, of them through
	// SW's spans (VertRepeat bands, or not convex); landscapes C3D cannot draw; static polygons and rectangles drawn;
	// statics C3D cannot draw; _solid polygons skipped (SW draws nothing for them).
	unsigned land_polys() const { return land_polys_; }
	unsigned land_spans() const { return land_spans_; }
	int land_row_min() const { return land_row_min_ == INT_MAX ? -1 : land_row_min_; }
	int land_row_max() const { return land_row_max_; }
	unsigned land_unsupported() const { return land_unsupported_; }
	unsigned static_polys() const { return static_polys_; }
	unsigned static_rects() const { return static_rects_; }
	unsigned static_unsupported() const { return static_unsupported_; }
	unsigned solid_skipped() const { return solid_skipped_; }

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
	TexView tex_view(bool horizontal) const;
	// M9: the w of an unmasked static polygon on a stereo frame: its surface's depth at each point (attrs()), else the mean
	// of the finite depths, else kWPerspMax (a floor at eye height lies on the horizon).
	void static_w(bool horizontal, const polygon_definition& p, const Point2* pts, int n, TexAttr* out) const;
	void note_w(float w)
	{
		if (stereo_)
		{
			++stereo_vertices_;
			w_low_ += w < kWMin;
		}
	}
	bool stereo_ = false;
	float weapon_w_ = 1024.0f;
	unsigned w_low_ = 0, stereo_vertices_ = 0;
	int edge_ = 0;
	unsigned edge_strips_ = 0, edge_skipped_ = 0;

	bool foreground_ = false;
	bool textures_ = false;
	bool uv_half_ = true;
	bool lit_ = true;
	unsigned rect_bias_ = 1;   // 1/131072 texel
	unsigned rects_ = 0, tinted_rects_ = 0, rect_skipped_ = 0, rect_unsupported_ = 0;
	int land_row_min_ = INT_MAX, land_row_max_ = -1;   // SW rows of the landscapes drawn (M5)
	unsigned land_polys_ = 0, land_spans_ = 0, land_unsupported_ = 0, static_polys_ = 0, static_rects_ = 0;
	unsigned static_unsupported_ = 0, solid_skipped_ = 0;
	uint32_t noise_state_ = 0x9E3779B9u;   // M5: the static noise offsets' xorshift32 (never 0)
	void noise_offset(double& ox, double& oy);
	void note_m();
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
