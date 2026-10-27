/*
	Aleph One for the Nintendo 3DS, citro3d back end: the rasterizer wrappers of the M0 harness
	(see filter.h; docs/3ds/CITRO3D_DESIGN.md, section 7, M0 specification).

	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; either version 3 of the License, or
	(at your option) any later version.
*/
#include "filter.h"

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

// --- FilterRasterizer ---

void FilterRasterizer::texture_horizontal_polygon(polygon_definition& p)
{
	if (!passes(classify(Entry::Horizontal, to_transfer(p.transfer_mode), foreground_)))
		return;
	polygon_definition copy = p;
	if (fullbright_ && copy.transfer_mode == _textured_transfer && !(copy.flags & _SHADELESS_BIT))
		copy.ambient_shade = FIXED_ONE;
	inner_->texture_horizontal_polygon(copy);
}

void FilterRasterizer::texture_vertical_polygon(polygon_definition& p)
{
	if (!passes(classify(Entry::Vertical, to_transfer(p.transfer_mode), foreground_)))
		return;
	polygon_definition copy = p;
	if (fullbright_ && copy.transfer_mode == _textured_transfer && !(copy.flags & _SHADELESS_BIT))
		copy.ambient_shade = FIXED_ONE;
	inner_->texture_vertical_polygon(copy);
}

void FilterRasterizer::texture_rectangle(rectangle_definition& r)
{
	if (!passes(classify(Entry::Rectangle, to_transfer(r.transfer_mode), foreground_)))
		return;
	rectangle_definition copy = r;
	if (fullbright_ && copy.transfer_mode == _textured_transfer && !(copy.flags & _SHADELESS_BIT))
		copy.ambient_shade = FIXED_ONE;
	inner_->texture_rectangle(copy);
}

// --- Recorder ---

void Recorder::clear()
{
	if (events_.capacity() < kCapacity)
	{
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
