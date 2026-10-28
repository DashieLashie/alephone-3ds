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

void C3dRasterizer::texture_horizontal_polygon(polygon_definition&)
{
	back::view_discard(back::DiscardReason::Unsupported);
}

void C3dRasterizer::texture_rectangle(rectangle_definition&)
{
	back::view_discard(back::DiscardReason::Unsupported);
}

void C3dRasterizer::texture_vertical_polygon(polygon_definition& p)
{
	const bool transparent = p.texture && (p.texture->flags & _TRANSPARENT_BIT);
	if (!view_ || !c3d_supports(Entry::Vertical, to_transfer(p.transfer_mode), transparent))
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
	const Accept a = accept_polygon(pts, n, w_, h_);
	if (!accepted(a))
	{
		++skipped_[static_cast<int>(a)];
		return;
	}

	const int hw = view_->half_screen_width, wx = view_->world_to_screen_x;
	const Rgb col = shape_colour(static_cast<uint16_t>(p.ShapeDesc));
	Vertex v[MAXIMUM_VERTICES_PER_SCREEN_POLYGON];
	for (int i = 0; i < n; ++i)
	{
		Vertex& o = v[i];
		o.x = static_cast<float>(pts[i].x);
		o.y = static_cast<float>(pts[i].y);
		o.w = wall_w_clamped(hw, wx, p.origin.x, p.origin.y, p.vector.i, p.vector.j, pts[i].x);
		o.u0 = o.v0 = o.u1 = o.v1 = 0.0f;
		o.r = col.r;
		o.g = col.g;
		o.b = col.b;
		o.a = 0xFF;
	}
	const DrawKey key{classify(Entry::Vertical, Transfer::Textured, foreground_), p.texture, p.shading_tables,
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
