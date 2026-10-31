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

namespace a1_c3d {

void C3dRasterizer::set_target(int w, int h)
{
	w_ = w;
	h_ = h;
	foreground_ = false;
	drawn_ = 0;
	tie_mask_.assign(static_cast<size_t>(w > 0 ? w : 0) * static_cast<size_t>(h > 0 ? h : 0), 0);
	for (unsigned& s : skipped_)
		s = 0;
}

void C3dRasterizer::texture_horizontal_polygon(polygon_definition& p)
{
	polygon(Entry::Horizontal, p);
}

void C3dRasterizer::texture_rectangle(rectangle_definition&)
{
	back::view_discard(back::DiscardReason::Unsupported);
}

void C3dRasterizer::texture_vertical_polygon(polygon_definition& p)
{
	polygon(Entry::Vertical, p);
}

// Walls and floors (design 7, M2 specification). Acceptance first (what SW draws), then the vertex
// attributes at (X + delta, Y + delta), delta = -1/2 with uv_half.
void C3dRasterizer::polygon(Entry e, polygon_definition& p)
{
	const bitmap_definition* tex = p.texture;
	if (!view_ || !tex ||
	    !c3d_supports(e, to_transfer(p.transfer_mode), tex->width, tex->height, tex->bytes_per_row == NONE,
	                  (p.flags & _SHADELESS_BIT) != 0, (tex->flags & _TRANSPARENT_BIT) != 0, textures_))
	{
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
	const bool horizontal = e == Entry::Horizontal;
	const Accept a = accept_polygon(pts, n, w_, h_, horizontal ? Orient::Horizontal : Orient::Vertical);
	if (!accepted(a))
	{
		++skipped_[static_cast<int>(a)];
		return;
	}

	TexView tv;
	tv.hw = view_->half_screen_width;
	tv.hh = view_->half_screen_height;
	tv.wx = view_->world_to_screen_x;
	tv.wy = view_->world_to_screen_y;
	tv.dtp = view_->dtanpitch;
	tv.cos_t = horizontal ? cosine_table[view_->yaw] : 0;
	tv.sin_t = horizontal ? sine_table[view_->yaw] : 0;
	const double delta = uv_half_ ? -0.5 : 0.0;
	TexAttr at[MAXIMUM_VERTICES_PER_SCREEN_POLYGON];
	TexResult tr;
	if (horizontal)
		tr = floor_attrs(tv, FloorSurf{static_cast<int>(p.origin.x), static_cast<int>(p.origin.y),
		                               static_cast<int>(p.origin.z), p.flags & _SCALE_BITS},
		                 pts, n, delta, at);
	else
		tr = wall_attrs(tv, WallSurf{static_cast<int>(p.origin.x), static_cast<int>(p.origin.y),
		                             static_cast<int>(p.origin.z), p.vector.i, p.vector.j, p.vector.k},
		                pts, n, delta, at);
	if (tr == TexResult::Degenerate)
	{
		++skipped_[static_cast<int>(Accept::Degenerate)];
		return;
	}
	if (tr != TexResult::Ok)
	{
		back::view_discard(back::DiscardReason::Unsupported);
		return;
	}

	const Rgb col = shape_colour(static_cast<uint16_t>(p.ShapeDesc));
	Vertex v[MAXIMUM_VERTICES_PER_SCREEN_POLYGON];
	for (int i = 0; i < n; ++i)
	{
		Vertex& o = v[i];
		o.x = static_cast<float>(pts[i].x);
		o.y = static_cast<float>(pts[i].y);
		o.w = at[i].w;
		o.u0 = at[i].s;
		o.v0 = at[i].t;
		o.u1 = o.v1 = 0.0f;
		o.r = col.r;
		o.g = col.g;
		o.b = col.b;
		o.a = 0xFF;
	}
	const DrawKey key{classify(e, Transfer::Textured, foreground_), p.texture, p.shading_tables,
	                  p.transfer_mode, p.transfer_data, static_cast<uint16_t>(p.flags & _SHADELESS_BIT)};
	if (back::view_polygon(v, n, key))
	{
		++drawn_;
		tie_pixels(pts, n, w_, h_,
		           [](void* ctx, int k, int m) {
			           auto* self = static_cast<C3dRasterizer*>(ctx);
			           self->tie_mask_[static_cast<size_t>(m) * self->w_ + k] = 1;
		           },
		           this);
	}
}

} // namespace a1_c3d
