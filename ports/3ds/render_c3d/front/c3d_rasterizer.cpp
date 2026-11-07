/*
	Aleph One for the Nintendo 3DS, citro3d back end: the C3D RasterizerClass (see c3d_rasterizer.h).

	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; either version 3 of the License, or
	(at your option) any later version.
*/
#include "c3d_rasterizer.h"

#include "filter.h"
#include "../back/draw.h"
#include "../core/classify.h"
#include "../core/landscape.h"
#include "../core/noise.h"
#include "../core/rect.h"
#include "../core/shade_lut.h"
#include "../core/tex_convert.h"

#include "player.h"
#include "ViewControl.h"

#include <algorithm>
#include <cstdio>

namespace a1_c3d {

void C3dRasterizer::set_target(int w, int h)
{
	w_ = w;
	h_ = h;
	foreground_ = false;
	drawn_ = 0;
	m_noted_ = false;
	tie_mask_.assign(static_cast<size_t>(w > 0 ? w : 0) * static_cast<size_t>(h > 0 ? h : 0), 0);
	for (unsigned& s : skipped_)
		s = 0;
}

void C3dRasterizer::texture_horizontal_polygon(polygon_definition& p)
{
	polygon(Entry::Horizontal, p);
}

// M4 (design 7, M4 specification "Rectangles on the GPU", "texcoord1 of a rectangle", "Front end"): SW's set-up
// (core/rect rect_setup), one quad over the box with SW's fixed-point texel positions at the pixel centres plus
// rect_bias, w = 1, texcoord1 constant (the LUT's column 0 at SW's level L, exact).
void C3dRasterizer::texture_rectangle(rectangle_definition& r)
{
	if (!view_)
	{
		back::view_discard(back::DiscardReason::Unsupported);
		return;
	}
	note_m();
	const int32_t M = static_cast<int32_t>(view_->maximum_depth_intensity);
	int L = 255;
	const bool stat = r.transfer_mode == _static_transfer;
	if (rect_support(r, M, textures_, &L) != RectSupport::Ok || (stat && !lit_))
	{
		++rect_unsupported_;
		static_unsupported_ += stat;
		back::view_discard(back::DiscardReason::Unsupported);
		return;
	}
	const bitmap_definition* tex = r.texture;
	RectBox box;
	if (!rect_setup(RectIn{r.x0, r.y0, r.x1, r.y1, r.clip_left, r.clip_right, r.clip_top, r.clip_bottom, tex->width,
	                       tex->height, r.flip_horizontal},
	                w_, h_, box))
	{
		++rect_skipped_;
		return;
	}
	unsigned S = 0, T = 0;
	rect_tex_size(tex->width, tex->height, true, S, T);   // rect_support checked it
	const bool tinted = r.transfer_mode == _tinted_transfer;
	const bool shadeless = !tinted && (r.flags & _SHADELESS_BIT) != 0;

	back::TexSource ts;
	ts.id = tex;
	ts.bitmap.width = tex->width;
	ts.bitmap.height = tex->height;
	ts.bitmap.rows = const_cast<const uint8_t* const*>(reinterpret_cast<uint8_t* const*>(tex->row_addresses));
	ts.bitmap.rle = true;
	ts.tables = static_cast<const uint32_t*>(r.shading_tables);
	ts.kind = tinted || stat ? TexKind::Mask : (shadeless ? TexKind::ShadelessKeyed : TexKind::NormalKeyed);
	ts.rect = true;

	// texcoord1: the level L in the LUT's column 0 (shade_lut_value(0, j) == j); shadeless: LUT 255; tinted: any
	// defined coordinate (the blend multiplies the source by zero). Without `lit` the second stage is off.
	const bool lit = lit_;
	const float u1 = shadeless ? 255.5f / 256.0f : 0.5f / 256.0f;
	const float v1 = (static_cast<float>(tinted ? 0 : L) + 0.5f) / 256.0f;
	// M5 (design 7, M5 specification "Static on the GPU"): a static rectangle samples the Mask on unit 0 and the
	// noise on unit 1 at the screen position plus this rectangle's offset.
	double nox = 0.0, noy = 0.0;
	if (stat)
		noise_offset(nox, noy);
	const double eps = static_cast<double>(rect_bias_) / 131072.0;
	const DrawKey key{classify(Entry::Rectangle, to_transfer(r.transfer_mode), foreground_), r.texture, r.shading_tables,
	                  r.transfer_mode, r.transfer_data, static_cast<uint16_t>(r.flags & _SHADELESS_BIT)};
	const int xs[4] = {box.cl, box.cr, box.cr, box.cl}, ys[4] = {box.ct, box.ct, box.cb, box.cb};
	Vertex v[4];
	for (int i = 0; i < 4; ++i)
	{
		Vertex& o = v[i];
		o.x = static_cast<float>(xs[i]);
		o.y = static_cast<float>(ys[i]);
		o.w = 1.0f;
		o.u0 = static_cast<float>((rect_s(box, ys[i]) + eps) / S);   // texels along the column (memory row)
		o.v0 = static_cast<float>((rect_t(box, xs[i]) + eps) / T);   // the column (memory row index)
		o.u1 = stat ? static_cast<float>((xs[i] + nox) / kNoiseSide) : (lit ? u1 : 0.0f);
		o.v1 = stat ? static_cast<float>((ys[i] + noy) / kNoiseSide) : (lit ? v1 : 0.0f);
		o.r = o.g = o.b = o.a = 0xFF;
	}
	if (back::view_polygon(v, 4, key, &ts))
	{
		++rects_;
		tinted_rects_ += tinted;
		static_rects_ += stat;
	}
}

void C3dRasterizer::note_m()
{
	if (m_noted_ || !view_)
		return;
	m_noted_ = true;
	const int32_t M = static_cast<int32_t>(view_->maximum_depth_intensity);
	if (M > NATURAL_LIGHT_INTENSITY)
		++flash_checks_;
	if (M > m_max_)
		m_max_ = M;
}

void C3dRasterizer::texture_vertical_polygon(polygon_definition& p)
{
	polygon(Entry::Vertical, p);
}

TexResult C3dRasterizer::attrs(bool horizontal, const polygon_definition& p, const Point2* pts, int n,
                               TexAttr* at) const
{
	TexView tv;
	tv.hw = view_->half_screen_width;
	tv.hh = view_->half_screen_height;
	tv.wx = view_->world_to_screen_x;
	tv.wy = view_->world_to_screen_y;
	tv.dtp = view_->dtanpitch;
	tv.cos_t = horizontal ? cosine_table[view_->yaw] : 0;
	tv.sin_t = horizontal ? sine_table[view_->yaw] : 0;
	const double delta = uv_half_ ? -0.5 : 0.0;
	if (horizontal)
		return floor_attrs(tv, FloorSurf{static_cast<int>(p.origin.x), static_cast<int>(p.origin.y),
		                                 static_cast<int>(p.origin.z), p.flags & _SCALE_BITS},
		                   pts, n, delta, at);
	return wall_attrs(tv, WallSurf{static_cast<int>(p.origin.x), static_cast<int>(p.origin.y),
	                               static_cast<int>(p.origin.z), p.vector.i, p.vector.j, p.vector.k},
	                  pts, n, delta, at);
}

// Walls and floors (design 7, M2 specification), M5's landscapes and static polygons. Acceptance first (what SW
// draws), then the vertex attributes at (X + delta, Y + delta), delta = -1/2 with uv_half. A convex polygon is one
// fan; a non-convex one (rounded vertices can make a thin polygon slightly concave, M2 status) is drawn as SW's own
// spans, one quad per run of equal spans, so its coverage is SW's exactly.
void C3dRasterizer::polygon(Entry e, polygon_definition& p)
{
	const bitmap_definition* tex = p.texture;
	const Transfer xfer = to_transfer(p.transfer_mode);
	// M5 (design 7, M5 specification, table "SW's static" and the _solid note (f)): smear polygons are drawn by no SW
	// mapper in a release build; nothing to draw, nothing to discard.
	if (p.transfer_mode == _solid_transfer)
	{
		++solid_skipped_;
		return;
	}
	const bool landscape = xfer == Transfer::BigLandscaped;
	const bool stat = xfer == Transfer::Static;
	const bool transparent = tex && (tex->flags & _TRANSPARENT_BIT) != 0;
	bool supported = view_ && tex;
	if (supported && landscape)
		supported = landscape_supported(p, textures_);
	else if (supported && stat)
		supported = static_supported(e, p, textures_, lit_);
	else if (supported)
		supported = c3d_supports(e, xfer, tex->width, tex->height, tex->bytes_per_row == NONE,
		                         (p.flags & _SHADELESS_BIT) != 0, transparent, textures_);
	if (!supported)
	{
		land_unsupported_ += landscape;
		static_unsupported_ += stat;
		back::view_discard(back::DiscardReason::Unsupported);
		return;
	}
	const int n = p.vertex_count;
	if (n > MAXIMUM_VERTICES_PER_SCREEN_POLYGON)
	{
		back::view_discard(back::DiscardReason::Unsupported);
		return;
	}
	Point2 pts[MAXIMUM_VERTICES_PER_SCREEN_POLYGON];
	for (int i = 0; i < n; ++i)
		pts[i] = Point2{p.vertices[i].x, p.vertices[i].y};
	// SW's mapper: landscapes go through the horizontal one (scottish_textures.cpp:436-439), static through the
	// vertical one (:221-225), the rest by their entry.
	const bool horizontal = landscape || (!stat && e == Entry::Horizontal);
	const Orient orient = horizontal ? Orient::Horizontal : Orient::Vertical;
	const Accept a = accept_polygon(pts, n, w_, h_, orient);
	if (!accepted(a))
	{
		++skipped_[static_cast<int>(a)];
		return;
	}

	// The polygon's line range (landscape bands and row offsets).
	int ymin = pts[0].y, ymax = pts[0].y;
	for (int i = 1; i < n; ++i)
	{
		ymin = std::min(ymin, pts[i].y);
		ymax = std::max(ymax, pts[i].y);
	}
	LandParams lp;
	bool multi_band = false;
	if (landscape)
	{
		const LandscapeOptions* lo = View_GetLandscapeOptions(p.ShapeDesc);
		LandOpts o;
		o.horiz_exp = lo->HorizExp;
		o.vert_exp = lo->VertExp;
		o.asp_rat_exp = lo->OGL_AspRatExp;
		o.azimuth = lo->Azimuth;
		o.vert_repeat = lo->VertRepeat;
		const LandView lv{view_->half_screen_height, view_->dtanpitch, view_->landscape_yaw, view_->half_cone,
		                  view_->standard_screen_width};
		if (!landscape_setup(lv, o, tex->width, tex->height, tex->bytes_per_row == NONE, lp))
		{
			++land_unsupported_;
			back::view_discard(back::DiscardReason::Unsupported);
			return;
		}
		// Several pieces (VertRepeat bands, or the two texture parts of a bitmap over 512 rows) go through SW's spans.
		multi_band = landscape_piece(lp, ymin) != landscape_piece(lp, ymax - 1);
	}
	const double delta = uv_half_ ? -0.5 : 0.0;
	// A static polygon samples its bitmap only when it is transparent (the Mask: SW's check_transparent); otherwise
	// it draws its coverage with w = 1.
	const bool masked = stat && transparent;
	auto attrs_of = [&](const Point2* q, int count, int piece, double row_off, TexAttr* out) -> TexResult {
		if (landscape)
			return landscape_attrs(lp, q, count, delta, piece >> 1, piece & 1, row_off, out);
		if (stat && !masked)
		{
			for (int i = 0; i < count; ++i)
				out[i] = TexAttr{1.0f, 0.0f, 0.0f};
			return TexResult::Ok;
		}
		return attrs(horizontal, p, q, count, out);
	};

	const int piece0 = landscape ? landscape_piece(lp, ymin) : 0;
	const double row_off0 = landscape && !multi_band ? landscape_row_offset(lp, ymin, ymax, piece0 >> 1, piece0 & 1) : 0.0;
	TexAttr at[MAXIMUM_VERTICES_PER_SCREEN_POLYGON];
	const TexResult tr = multi_band ? TexResult::Ok : attrs_of(pts, n, piece0, row_off0, at);
	if (tr == TexResult::Degenerate)
	{
		++skipped_[static_cast<int>(Accept::Degenerate)];
		return;
	}
	if (tr != TexResult::Ok)
	{
		// SW draws this polygon, so the check is discarded (not skipped); logged apart from real unsupported
		// primitives (a float precision limit, not a missing feature).
		if (nonfinite_++ == 0)
			printf("[c3d] front: texcoords non-finite or spread over 2^20 repeats: view frame discarded "
			       "(unsupported); first of its kind\n");
		back::view_discard(back::DiscardReason::Unsupported);
		return;
	}

	// texcoord1 (M3 "texcoord1"): the shade LUT's coordinates from M (the weapon flash), the ambient and the
	// vertex's perspective weight; constant for shadeless and negative-ambient polygons. M5: landscapes the LUT's
	// column 0 at SW's level of depth 0, ambient FIXED_ONE (or the shadeless constant); static the noise at the
	// screen position plus this primitive's offset.
	const bool shadeless = (p.flags & _SHADELESS_BIT) != 0;
	const bool lit = textures_ && lit_;
	const int32_t M = static_cast<int32_t>(view_->maximum_depth_intensity);
	note_m();
	ShadePoly sp;
	float land_u1 = 0.0f, land_v1 = 0.0f;
	double nox = 0.0, noy = 0.0;
	if (landscape && lit)
	{
		const int L = shadeless ? 255 : sw_shade_index(M, 0, FIXED_ONE);
		land_u1 = shadeless ? 255.5f / 256.0f : 0.5f / 256.0f;
		land_v1 = (static_cast<float>(L < 0 ? 0 : L) + 0.5f) / 256.0f;
	}
	else if (stat)
		noise_offset(nox, noy);
	else if (lit)
	{
		sp = shade_polygon(M, static_cast<int32_t>(p.ambient_shade), shadeless);
		if (sp.kind == ShadeKind::FlashOver || sp.kind == ShadeKind::Excluded)
		{
			// FlashOver: SW's SHRT_MAX depth clamp would leave s > 0 (never in Marathon 2). Excluded: the filter
			// drops these on both sides first (FilterRasterizer::ambient_wraps), so reaching here is a bug.
			if (sp.kind == ShadeKind::FlashOver && flash_over_++ == 0)
				printf("[c3d] front: maximum_depth_intensity %ld above %ld: view frame discarded (unsupported); first of "
				       "its kind\n", static_cast<long>(M), static_cast<long>(kFlashMax));
			if (sp.kind == ShadeKind::Excluded && excluded_reached_++ == 0)
				printf("[c3d] front: ambient %ld wraps SW's index but reached the C3D side (filter mismatch): view frame "
				       "discarded (unsupported); first of its kind\n", static_cast<long>(p.ambient_shade));
			back::view_discard(back::DiscardReason::Unsupported);
			return;
		}
	}

	// The texture (M2 "View frame with textures"): the bitmap's rows, the 32-bit tables (the harness runs at
	// bit_depth 32 only, frame.cpp), keyed texel 0 only for a _TRANSPARENT_BIT bitmap on a wall (the floor
	// mapper always writes, M2 "Transparent texels"); infravision's tint table through the shadeless kinds (M3).
	// M5: a landscape's padded texture (normal / shadeless); a transparent static wall's Mask; other statics none.
	back::TexSource ts;
	const bool with_tex = textures_ && (!stat || masked);
	if (with_tex)
	{
		ts.id = tex;
		ts.bitmap.width = tex->width;
		ts.bitmap.height = tex->height;
		ts.bitmap.rows = const_cast<const uint8_t* const*>(reinterpret_cast<uint8_t* const*>(tex->row_addresses));
		ts.bitmap.rle = tex->bytes_per_row == NONE;
		ts.tables = static_cast<const uint32_t*>(p.shading_tables);
		const bool keyed = !horizontal && transparent;
		if (landscape)
		{
			ts.kind = shadeless ? TexKind::Shadeless : TexKind::Normal;
			ts.landscape = true;
		}
		else if (masked)
			ts.kind = TexKind::Mask;
		else
			ts.kind = shadeless ? (keyed ? TexKind::ShadelessKeyed : TexKind::Shadeless)
			                    : (keyed ? TexKind::NormalKeyed : TexKind::Normal);
	}
	const back::TexSource* tsp = with_tex ? &ts : nullptr;
	const Rgb col = textures_ ? Rgb{0xFF, 0xFF, 0xFF} : shape_colour(static_cast<uint16_t>(p.ShapeDesc));
	const Transfer key_xfer = landscape || stat ? xfer : Transfer::Textured;
	DrawKey key{classify(e, key_xfer, foreground_), p.texture, p.shading_tables,
	            p.transfer_mode, p.transfer_data, static_cast<uint16_t>(p.flags & _SHADELESS_BIT)};
	// M5: a landscape's texture part is part of the draw (one bitmap, two textures).
	auto set_part = [&](int piece) {
		ts.land_part = static_cast<uint8_t>(piece & 1);
		key.part = static_cast<uint8_t>(piece & 1);
	};
	if (landscape)
		set_part(piece0);
	auto make = [&](const Point2* q, const TexAttr* qa, int count, Vertex* out) {
		for (int i = 0; i < count; ++i)
		{
			Vertex& o = out[i];
			o.x = static_cast<float>(q[i].x);
			o.y = static_cast<float>(q[i].y);
			o.w = qa[i].w;
			o.u0 = qa[i].s;
			o.v0 = qa[i].t;
			if (stat)
			{
				// The noise texel under the pixel centre X + 1/2: (X + ox + 1/2) / 256 at the centre, so the vertex
				// carries (X + ox) / 256 (no uv_half shift: the noise has no SW position to match).
				o.u1 = static_cast<float>((q[i].x + nox) / kNoiseSide);
				o.v1 = static_cast<float>((q[i].y + noy) / kNoiseSide);
			}
			else if (landscape)
			{
				o.u1 = land_u1;
				o.v1 = land_v1;
			}
			else
			{
				o.u1 = lit ? shade_u(sp, qa[i].w) : 0.0f;
				o.v1 = lit ? sp.v : 0.0f;
			}
			o.r = col.r;
			o.g = col.g;
			o.b = col.b;
			o.a = 0xFF;
		}
	};
	auto count_drawn = [&]() {
		++drawn_;
		if (landscape)
			++land_polys_;
		else if (stat)
			++static_polys_;
		else
			count_lit(lit, shadeless, p.ambient_shade < 0);
	};

	if (multi_band || !convex_cw(pts, n))
	{
		Span spans[1024];
		const int ns = sw_spans(pts, n, orient, spans, 1024);
		if (ns <= 0)
		{
			// Unreachable after accept_polygon (the same walks); a check that cannot be drawn faithfully.
			back::view_discard(back::DiscardReason::Unsupported);
			return;
		}
		// Consecutive lines with the same [a0, a1) are one rectangle (still exact: every edge on a pixel boundary);
		// M5: a landscape's run also stays inside one VertRepeat band.
		Span rects[1024];
		int band_of[1024];
		int nr = 0;
		for (int k = 0; k < ns; ++k)
		{
			if (spans[k].a1 <= spans[k].a0)
				continue;
			const int band = landscape ? landscape_piece(lp, spans[k].line) : 0;
			if (nr && rects[nr - 1].a0 == spans[k].a0 && rects[nr - 1].a1 == spans[k].a1 &&
			    rects[nr - 1].line + span_len_[nr - 1] == spans[k].line && band_of[nr - 1] == band)
				++span_len_[nr - 1];
			else
			{
				band_of[nr] = band;
				rects[nr] = spans[k];
				span_len_[nr++] = 1;
			}
		}
		if (span_polygons_++ < 8)
		{
			printf("[c3d] front: %s %s polygon drawn as %d spans in %d quads (first 8 logged):",
			       multi_band ? "multi-band" : "non-convex", landscape ? "landscape" : (horizontal ? "floor" : "wall"), ns,
			       nr);
			for (int i = 0; i < n; ++i)
				printf(" (%d,%d)", pts[i].x, pts[i].y);
			printf("\n");
		}
		if (nr > span_quads_max_)
			span_quads_max_ = nr;
		// The whole polygon or nothing: a ring that cannot take every quad is the ring discard, counted apart.
		if (!back::view_ring_room(4u * static_cast<unsigned>(nr), 6u * static_cast<unsigned>(nr)))
		{
			++span_ring_refused_;
			back::view_discard(back::DiscardReason::Ring);
			return;
		}
		land_spans_ += landscape;
		int off_band = 0x7FFFFFFF;
		double off = 0.0;
		for (int k = 0; k < nr; ++k)
		{
			const Span& rc = rects[k];
			const int l0 = rc.line, l1 = rc.line + span_len_[k];
			// [a0, a1) x [l0, l1) (or its transpose): every edge on a pixel boundary, no tie.
			Point2 q[4];
			if (horizontal)
			{
				q[0] = Point2{rc.a0, l0};
				q[1] = Point2{rc.a1, l0};
				q[2] = Point2{rc.a1, l1};
				q[3] = Point2{rc.a0, l1};
			}
			else
			{
				q[0] = Point2{l0, rc.a0};
				q[1] = Point2{l1, rc.a0};
				q[2] = Point2{l1, rc.a1};
				q[3] = Point2{l0, rc.a1};
			}
			if (landscape && band_of[k] != off_band)
			{
				off_band = band_of[k];
				off = landscape_row_offset(lp, ymin, ymax, off_band >> 1, off_band & 1);
				set_part(off_band);
			}
			TexAttr qa[4];
			const TexResult qr = attrs_of(q, 4, band_of[k], landscape ? off : row_off0, qa);
			if (qr != TexResult::Ok)
			{
				// A quad corner lies on a pixel boundary, possibly just outside the polygon (near the horizon its
				// texcoords can blow up): counted as the fan path's non-finite case, not as unsupported (minor 2).
				if (nonfinite_++ == 0)
					printf("[c3d] front: span texcoords non-finite or spread over 2^20 repeats: view frame discarded "
					       "(unsupported); first of its kind\n");
				back::view_discard(back::DiscardReason::Unsupported);
				return;
			}
			Vertex v[4];
			make(q, qa, 4, v);
			if (!back::view_polygon(v, 4, key, tsp))
				return;
			++span_quads_;
		}
		count_drawn();
		return;   // no tie pixels: span edges lie on pixel boundaries, and the spans are SW's own
	}

	Vertex v[MAXIMUM_VERTICES_PER_SCREEN_POLYGON];
	make(pts, at, n, v);
	if (back::view_polygon(v, n, key, tsp))
	{
		count_drawn();
		tie_pixels(pts, n, w_, h_,
		           [](void* ctx, int k, int m) {
			           auto* self = static_cast<C3dRasterizer*>(ctx);
			           self->tie_mask_[static_cast<size_t>(m) * self->w_ + k] = 1;
		           },
		           this);
	}
}

// M5: this static primitive's noise offset (design 7, M5 specification "Static on the GPU": a front-end xorshift32,
// never texture_random_seed(), which SW's replays restore).
void C3dRasterizer::noise_offset(double& ox, double& oy)
{
	uint32_t x = noise_state_;
	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	noise_state_ = x;
	ox = static_cast<double>(x & (kNoiseSide - 1));
	oy = static_cast<double>((x >> 8) & (kNoiseSide - 1));
}

} // namespace a1_c3d
