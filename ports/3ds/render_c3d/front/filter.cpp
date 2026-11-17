/*
	Aleph One for the Nintendo 3DS, citro3d back end: the rasterizer wrappers of the M0 harness
	(see filter.h; docs/3ds/CITRO3D_DESIGN.md, section 7, M0 specification).

	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; either version 3 of the License, or
	(at your option) any later version.
*/
#include "filter.h"

#include "../core/shade_lut.h"

#include "../core/geometry.h"
#include "../core/landscape.h"
#include "../core/tex_convert.h"

#include "ViewControl.h"

#include <algorithm>
#include <cstdio>
#include <climits>

namespace a1_c3d {

Transfer to_transfer(int16 engine_transfer_mode)
{
	switch (engine_transfer_mode)
	{
	case _textured_transfer: return Transfer::Textured;
	case _big_landscaped_transfer: return Transfer::BigLandscaped;
	case _static_transfer: return Transfer::Static;
	case _tinted_transfer: return Transfer::Tinted;
	default: return Transfer::Other;
	}
}

RectSupport rect_support(const rectangle_definition& r, int32_t M, bool textures, int* level)
{
	const bool stat = r.transfer_mode == _static_transfer;
	const bool tinted = r.transfer_mode == _tinted_transfer;
	if (stat && !textures)
		return RectSupport::Static;
	if ((!tinted && !stat && r.transfer_mode != _textured_transfer) || !textures)
		return RectSupport::Other;
	const bitmap_definition* t = r.texture;
	unsigned S = 0, T = 0;
	if (!t || !(t->flags & _COLUMN_ORDER_BIT) || !rect_tex_size(t->width, t->height, t->bytes_per_row == NONE, S, T))
		return stat ? RectSupport::Static : RectSupport::Bitmap;
	if (stat)
	{
		// M5: the Mask on unit 0 and the noise on unit 1; SW reads table 0 for nothing (scottish_textures.cpp:752-755).
		if (level)
			*level = 255;
		return RectSupport::Ok;
	}
	int L = 255;
	if (tinted)
	{
		if ((r.transfer_data & 0xFF) >= 32)
			return RectSupport::Other;
	}
	else if (!(r.flags & _SHADELESS_BIT))
	{
		L = sw_shade_index(M, static_cast<int16_t>(std::min<int32_t>(r.depth, SHRT_MAX)), static_cast<int32_t>(r.ambient_shade));
		if (L < 0)
			return RectSupport::Wrap;
	}
	if (level)
		*level = L;
	return RectSupport::Ok;
}

bool landscape_params(const polygon_definition& p, const view_data& view, LandParams& out)
{
	const bitmap_definition* t = p.texture;
	if (!t)
		return false;
	const LandscapeOptions* lo = View_GetLandscapeOptions(p.ShapeDesc);
	LandOpts o;
	o.horiz_exp = lo->HorizExp;
	o.vert_exp = lo->VertExp;
	o.asp_rat_exp = lo->OGL_AspRatExp;
	o.azimuth = lo->Azimuth;
	o.vert_repeat = lo->VertRepeat;
	const LandView lv{view.half_screen_height, view.dtanpitch, view.landscape_yaw, view.half_cone,
	                  view.standard_screen_width};
	return landscape_setup(lv, o, t->width, t->height, t->bytes_per_row == NONE, out);
}

bool landscape_supported(const polygon_definition& p, const view_data* view, bool textures)
{
	const bitmap_definition* t = p.texture;
	if (!textures || !t)
		return false;
	LandParams lp;
	unsigned S = 0;
	return view ? landscape_params(p, *view, lp)
	            : landscape_tex_size(t->width, t->height, t->bytes_per_row == NONE, S, nullptr, nullptr);
}

bool static_supported(Entry e, const polygon_definition& p, bool textures, bool lit)
{
	const bitmap_definition* t = p.texture;
	if (!textures || !lit || !t)
		return false;
	if (!(t->flags & _TRANSPARENT_BIT))
		return true;   // the coverage only (w = 1): the bitmap is never sampled
	// A transparent bitmap is sampled (SW's check_transparent): the wall mapper's texcoords and M2's square bitmaps.
	return e == Entry::Vertical && tex_supported(t->width, t->height, t->bytes_per_row == NONE);
}

// --- FilterRasterizer ---

bool FilterRasterizer::c3d_excluded(Entry e, const polygon_definition& p) const
{
	if (!c3d_only_ || !p.texture)
		return false;
	// M5: the landscapes and statics C3D cannot draw are dropped on both sides, as M2's unsupported walls. (Static
	// needs lit: the diff configs of M5 are lit.)
	if (p.transfer_mode == _big_landscaped_transfer)
	{
		const bool no = !landscape_supported(p, view_, textures_);
		static bool logged = false;
		if (no && !logged)
		{
			logged = true;
			const bitmap_definition* t = p.texture;
			printf("[c3d] front: landscape bitmap %d x %d (bytes_per_row %d, flags 0x%x) not drawn by C3D: excluded on both "
			       "sides (first of its kind)\n", t->width, t->height, t->bytes_per_row, static_cast<unsigned>(t->flags));
		}
		return no;
	}
	if (p.transfer_mode == _static_transfer)
		return !static_supported(e, p, textures_, true);
	if (p.transfer_mode != _textured_transfer)
		return false;
	const bitmap_definition* t = p.texture;
	return !c3d_supports(e, Transfer::Textured, t->width, t->height, t->bytes_per_row == NONE,
	                     (p.flags & _SHADELESS_BIT) != 0, (t->flags & _TRANSPARENT_BIT) != 0, textures_);
}

// M3 "texcoord1": SW's short index wraps for this ambient (negative index: SW reads before the tables; or
// a + (s >> 1) can pass 2^23 at some pixel), so SW's output is undefined. The ambient SW will see: the
// fullbright rewrite makes it FIXED_ONE, and shadeless polygons ignore it (scottish_textures.cpp:984, 1085).
bool FilterRasterizer::ambient_wraps(const polygon_definition& p) const
{
	if (!c3d_only_ || !textures_ || fullbright_ || p.transfer_mode != _textured_transfer || (p.flags & _SHADELESS_BIT))
		return false;
	return shade_ambient_wraps(static_cast<int32_t>(p.ambient_shade));
}

void FilterRasterizer::texture_horizontal_polygon(polygon_definition& p)
{
	const int cls = classify(Entry::Horizontal, to_transfer(p.transfer_mode), foreground_);
	if (!passes(cls))
		return;
	if (frame_excluded(cls))
	{
		++excluded_;
		return;
	}
	if (c3d_excluded(Entry::Horizontal, p))
	{
		++excluded_;
		return;
	}
	if (ambient_wraps(p))
	{
		++excluded_;
		++ambient_wrap_;
		return;
	}
	polygon_definition copy = p;
	if (fullbright_ && copy.transfer_mode == _textured_transfer && !(copy.flags & _SHADELESS_BIT))
		copy.ambient_shade = FIXED_ONE;
	if (static_cover_ && copy.transfer_mode == _static_transfer)
		copy.transfer_data = 0;
	inner_->texture_horizontal_polygon(copy);
}

void FilterRasterizer::texture_vertical_polygon(polygon_definition& p)
{
	const int cls = classify(Entry::Vertical, to_transfer(p.transfer_mode), foreground_);
	if (!passes(cls))
		return;
	if (frame_excluded(cls))
	{
		++excluded_;
		return;
	}
	if (c3d_excluded(Entry::Vertical, p))
	{
		++excluded_;
		return;
	}
	if (ambient_wraps(p))
	{
		++excluded_;
		++ambient_wrap_;
		return;
	}
	polygon_definition copy = p;
	if (fullbright_ && copy.transfer_mode == _textured_transfer && !(copy.flags & _SHADELESS_BIT))
		copy.ambient_shade = FIXED_ONE;
	if (static_cover_ && copy.transfer_mode == _static_transfer)
		copy.transfer_data = 0;
	inner_->texture_vertical_polygon(copy);
}

void FilterRasterizer::texture_rectangle(rectangle_definition& r)
{
	const int cls = classify(Entry::Rectangle, to_transfer(r.transfer_mode), foreground_);
	if (!passes(cls))
		return;
	if (frame_excluded(cls))
	{
		++excluded_;
		return;
	}
	rectangle_definition copy = r;
	if (fullbright_ && copy.transfer_mode == _textured_transfer && !(copy.flags & _SHADELESS_BIT))
		copy.ambient_shade = FIXED_ONE;
	if (static_cover_ && copy.transfer_mode == _static_transfer)
		copy.transfer_data = 0;
	// M4: what C3D cannot draw is dropped on both sides of a C3D check (after the fullbright rewrite: the ambient
	// SW will see). M5: static rectangles are a C3D class (`static`; excluded from `frame` above).
	if (c3d_only_)
	{
		const int32_t M = view_ ? static_cast<int32_t>(view_->maximum_depth_intensity) : 0;
		const RectSupport rs = rect_support(copy, M, textures_);
		if (rs != RectSupport::Ok)
		{
			++excluded_;
			ambient_wrap_ += rs == RectSupport::Wrap;
			return;
		}
	}
	inner_->texture_rectangle(copy);
}

// --- Recorder ---

void Recorder::clear()
{
	if (events_.capacity() < kCapacity)
	{
		// 8192 x (12 + 108 + 120) B = about 1.9 MiB on the ARM build.
		events_.reserve(kCapacity);
		polys_.reserve(kCapacity);
		rects_.reserve(kCapacity);
	}
	events_.clear();
	polys_.clear();
	rects_.clear();
	overflowed_ = false;
}

bool Recorder::push(Kind kind, bool flag, view_data* view)
{
	if (overflowed_)
		return false;
	if (events_.size() >= kCapacity)
	{
		overflowed_ = true;
		return false;
	}
	events_.push_back(Event{kind, flag, 0, view});
	return true;
}

void Recorder::push_poly(Kind kind, const polygon_definition& p)
{
	if (!push(kind))
		return;
	if (polys_.size() >= kCapacity)   // cannot happen (events are capped at the same size); stay safe
	{
		events_.pop_back();
		overflowed_ = true;
		return;
	}
	events_.back().index = static_cast<uint32_t>(polys_.size());
	polys_.push_back(p);
}

void Recorder::SetView(view_data& View)
{
	push(Kind::SetView, false, &View);
	if (forward)
		forward->SetView(View);
}

void Recorder::Begin()
{
	push(Kind::Begin);
	if (forward)
		forward->Begin();
}

void Recorder::End()
{
	push(Kind::End);
	if (forward)
		forward->End();
}

void Recorder::SetForeground()
{
	push(Kind::SetForeground);
	if (forward)
		forward->SetForeground();
}

void Recorder::SetForegroundView(bool HorizReflect)
{
	push(Kind::SetForegroundView, HorizReflect);
	if (forward)
		forward->SetForegroundView(HorizReflect);
}

void Recorder::texture_horizontal_polygon(polygon_definition& p)
{
	push_poly(Kind::Horizontal, p);   // the copy first: the forward below may edit p
	if (forward)
		forward->texture_horizontal_polygon(p);
}

void Recorder::texture_vertical_polygon(polygon_definition& p)
{
	push_poly(Kind::Vertical, p);
	if (forward)
		forward->texture_vertical_polygon(p);
}

void Recorder::texture_rectangle(rectangle_definition& r)
{
	if (push(Kind::Rect))
	{
		if (rects_.size() >= kCapacity)
		{
			events_.pop_back();
			overflowed_ = true;
		}
		else
		{
			events_.back().index = static_cast<uint32_t>(rects_.size());
			rects_.push_back(r);
		}
	}
	if (forward)
		forward->texture_rectangle(r);
}

void Recorder::replay(RasterizerClass* target) const
{
	for (const Event& e : events_)
	{
		switch (e.kind)
		{
		case Kind::SetView:
			target->SetView(*e.view);
			break;
		case Kind::Begin:
			target->Begin();
			break;
		case Kind::End:
			target->End();
			break;
		case Kind::SetForeground:
			target->SetForeground();
			break;
		case Kind::SetForegroundView:
			target->SetForegroundView(e.flag);
			break;
		case Kind::Vertical:
		{
			polygon_definition tmp = polys_[e.index];
			target->texture_vertical_polygon(tmp);
			break;
		}
		case Kind::Horizontal:
		{
			polygon_definition tmp = polys_[e.index];
			target->texture_horizontal_polygon(tmp);
			break;
		}
		case Kind::Rect:
		{
			rectangle_definition tmp = rects_[e.index];
			target->texture_rectangle(tmp);
			break;
		}
		}
	}
}

// --- NullRasterizer ---

void NullRasterizer::Begin()
{
	counts_ = NullCounts();
	pairs_.begin_frame();
	have_prev_ = false;
	foreground_ = false;
}

void NullRasterizer::note(const Key& key)
{
	if (have_prev_ && !(key == prev_))
		++counts_.state_changes;
	prev_ = key;
	have_prev_ = true;
	pairs_.insert(reinterpret_cast<uintptr_t>(key.texture), reinterpret_cast<uintptr_t>(key.shading_tables));
	counts_.pairs = pairs_.count();
	counts_.pair_table_full = pairs_.full();
}

void NullRasterizer::polygon(Entry e, const polygon_definition& p)
{
	++counts_.polygons;
	const int n = p.vertex_count;
	counts_.vertices += n > 0 ? n : 0;
	counts_.indices += n >= 3 ? 3 * (n - 2) : 0;
	note(Key{classify(e, to_transfer(p.transfer_mode), foreground_), p.texture, p.shading_tables,
	         p.transfer_mode, p.transfer_data, static_cast<uint16>(p.flags & _SHADELESS_BIT)});
}

void NullRasterizer::texture_rectangle(rectangle_definition& r)
{
	++counts_.rectangles;
	counts_.vertices += 4;
	counts_.indices += 6;
	note(Key{classify(Entry::Rectangle, to_transfer(r.transfer_mode), foreground_), r.texture, r.shading_tables,
	         r.transfer_mode, r.transfer_data, static_cast<uint16>(r.flags & _SHADELESS_BIT)});
}

} // namespace a1_c3d
