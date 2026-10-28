/*
	Aleph One for the Nintendo 3DS, citro3d back end: the C3D RasterizerClass (docs/3ds/CITRO3D_DESIGN.md,
	section 7, M1 specification, "What C3D draws in M1" and "Polygon acceptance"). Engine headers and the
	plain headers of back/ and core/ only: no <3ds.h>, no <citro3d.h>.

	Receives the traversal from the harness (through FilterRasterizer), accepts walls with an opaque
	texture exactly as the software mapper would draw them, builds their vertices and hands them to
	back/draw.cpp (view_polygon). Any other primitive discards the view frame (view_discard(Unsupported)).

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
	// Skipped polygons (SW draws nothing for them) by Accept kind (index = Accept; Draw is 0).
	unsigned skipped(Accept a) const { return skipped_[static_cast<int>(a)]; }

private:
	view_data* view_ = nullptr;
	int w_ = 0, h_ = 0;
	bool foreground_ = false;
	unsigned drawn_ = 0;
	std::vector<uint8_t> tie_mask_;
	unsigned skipped_[5] = {0, 0, 0, 0, 0};
};

} // namespace a1_c3d

#endif
