/*
	Aleph One for the Nintendo 3DS, citro3d back end: the RasterizerClass wrappers of the M0
	harness (docs/3ds/CITRO3D_DESIGN.md, section 7, M0 specification). Engine headers only: no
	<3ds.h>, no <citro3d.h>.

	  FilterRasterizer  forwards the primitives of one class (or all of them) to an inner
	                    rasterizer, as copies, optionally with the fullbright edit
	  Recorder          copies every call of one traversal into a fixed-capacity event list and
	                    replays it later, any number of times
	  NullRasterizer    counts what a rasteriser would draw (null mode, defines R) and draws nothing

	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; either version 3 of the License, or
	(at your option) any later version.
*/
#ifndef A1_C3D_FRONT_FILTER_H
#define A1_C3D_FRONT_FILTER_H

#include "cseries.h"
#include "render.h"
#include "Rasterizer.h"
#include "scottish_textures.h"

#include "../core/classify.h"
#include "../core/stats.h"

#include <cstdint>
#include <vector>

namespace a1_c3d {

// The engine's transfer mode onto the classes' Transfer (core/classify.h).
Transfer to_transfer(int16 engine_transfer_mode);

// Forwards the primitives of class `cls` (kFrame: all of them) to `inner`. Every forwarded
// primitive is a local copy, so neither the fullbright edit nor the inner rasterizer's in-place
// clip edits reach the caller's (or the recording's) primitive. SetView, Begin, End and
// SetForeground are always forwarded; the foreground flag (reset by Begin) classifies sprites and
// weapons.
class FilterRasterizer : public RasterizerClass
{
public:
	// fullbright: every forwarded _textured primitive without _SHADELESS_BIT gets
	// ambient_shade = FIXED_ONE (the top shading table at any depth).
	// c3d_only (C3D checks only, on both sides): a wall whose texture is transparent (SW skips texel 0,
	// flat C3D would draw it solid; M1 cannot, section 7 "What C3D draws in M1") is dropped and counted.
	FilterRasterizer(RasterizerClass* inner, int cls, bool fullbright, bool c3d_only = false)
		: inner_(inner), cls_(cls), fullbright_(fullbright), c3d_only_(c3d_only) {}

	unsigned excluded() const { return excluded_; }

	void SetView(view_data& View) override { inner_->SetView(View); }
	void Begin() override { foreground_ = false; inner_->Begin(); }
	void End() override { inner_->End(); }
	void SetForeground() override { foreground_ = true; inner_->SetForeground(); }
	void SetForegroundView(bool HorizReflect) override { inner_->SetForegroundView(HorizReflect); }

	void texture_horizontal_polygon(polygon_definition& p) override;
	void texture_vertical_polygon(polygon_definition& p) override;
	void texture_rectangle(rectangle_definition& r) override;

private:
	bool passes(int cls) const { return cls_ == kFrame || cls_ == cls; }

	RasterizerClass* inner_;
	int cls_;
	bool fullbright_;
	bool c3d_only_;
	unsigned excluded_ = 0;
	bool foreground_ = false;
};

// One traversal, recorded. Storage: kCapacity events, and as many polygons and rectangles, all
// reserved at the first clear() and reused (no allocation per frame). On overflow recording stops
// (overflowed()) while forwarding goes on.
class Recorder : public RasterizerClass
{
public:
	static constexpr size_t kCapacity = 8192;

	// Where every call is forwarded to (after the copy was taken); nullptr = record only.
	RasterizerClass* forward = nullptr;

	// Forgets the recording (allocates the storage on the first call).
	void clear();
	bool overflowed() const { return overflowed_; }
	size_t events() const { return events_.size(); }

	// Re-emits every event, in order, to `target`; each primitive is handed over as a temporary
	// copy of the stored one.
	void replay(RasterizerClass* target) const;

	void SetView(view_data& View) override;
	void Begin() override;
	void End() override;
	void SetForeground() override;
	void SetForegroundView(bool HorizReflect) override;
	void texture_horizontal_polygon(polygon_definition& p) override;
	void texture_vertical_polygon(polygon_definition& p) override;
	void texture_rectangle(rectangle_definition& r) override;

private:
	enum class Kind : uint8_t { SetView, Begin, End, SetForeground, SetForegroundView, Vertical, Horizontal, Rect };
	struct Event
	{
		Kind kind;
		bool flag;          // SetForegroundView's argument
		uint32_t index;     // into polys_ / rects_
		view_data* view;    // SetView
	};

	bool push(Kind kind, bool flag = false, view_data* view = nullptr);
	void push_poly(Kind kind, const polygon_definition& p);

	std::vector<Event> events_;
	std::vector<polygon_definition> polys_;
	std::vector<rectangle_definition> rects_;
	bool overflowed_ = false;
};

// What one frame's traversal holds (design 7, "Null mode").
struct NullCounts
{
	unsigned polygons = 0, rectangles = 0;
	unsigned vertices = 0;       // V = sum of vertex_count + 4 per rectangle
	unsigned indices = 0;        // I = sum of 3(n-2) + 6 per rectangle
	unsigned state_changes = 0;  // consecutive primitives whose key differs; draws = changes + 1
	unsigned pairs = 0;          // distinct (texture, shading_tables); 1024 when the table was full
	bool pair_table_full = false;
};

class NullRasterizer : public RasterizerClass
{
public:
	const NullCounts& counts() const { return counts_; }

	void Begin() override;
	void SetForeground() override { foreground_ = true; }

	void texture_horizontal_polygon(polygon_definition& p) override { polygon(Entry::Horizontal, p); }
	void texture_vertical_polygon(polygon_definition& p) override { polygon(Entry::Vertical, p); }
	void texture_rectangle(rectangle_definition& r) override;

private:
	struct Key
	{
		int cls;
		const void* texture;
		const void* shading_tables;
		int16 transfer_mode, transfer_data;
		uint16 shadeless;
		bool operator==(const Key& o) const
		{
			return cls == o.cls && texture == o.texture && shading_tables == o.shading_tables &&
			       transfer_mode == o.transfer_mode && transfer_data == o.transfer_data && shadeless == o.shadeless;
		}
	};

	void polygon(Entry e, const polygon_definition& p);
	void note(const Key& key);

	NullCounts counts_;
	PairTable pairs_;
	Key prev_{};
	bool have_prev_ = false;
	bool foreground_ = false;
};

} // namespace a1_c3d

#endif
