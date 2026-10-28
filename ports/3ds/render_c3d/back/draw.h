/*
	Aleph One for the Nintendo 3DS, citro3d back end: the C3D view frame (docs/3ds/CITRO3D_DESIGN.md,
	section 7, M1 specification, "Back end"). A view frame draws into the unlinked view target and reads
	it back keeping alpha; the front end (front/C3dRasterizer, M1 piece 3) feeds it polygons. Plain
	header: no <3ds.h>, no <citro3d.h>, no engine headers. Main thread only.

	Call order of one view frame:
	  view_begin(w, h)                      Ok -> a frame is open; Skipped / Discarded -> nothing is open
	  view_polygon(...) | view_discard(...) any number of times
	  view_end(...)                         only after view_begin() returned Ok; always closes the frame

	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; either version 3 of the License, or
	(at your option) any later version.
*/
#ifndef A1_C3D_BACK_DRAW_H
#define A1_C3D_BACK_DRAW_H

#include "../core/geometry.h"

#include <cstddef>
#include <cstdint>

namespace a1_c3d {
namespace back {

// Nothing was drawn and nothing is open: the check is "skipped (down|gate|size)". Down also covers a
// release pending and citro3d off for the run (a failed ring or BufInfo check); Size also covers no memory
// for the view target and staging (view_ensure).
enum class SkipReason : uint8_t { Down, Gate, Size, kCount };
// The frame was started and is given up: no more draws, C3D_FrameEnd normally, no readback. The check
// is "discarded (unsupported|ring|cmdbuf|gx|begin)" and not compared.
enum class DiscardReason : uint8_t { Unsupported, Ring, Cmdbuf, Gx, Begin, kCount };

const char* skip_name(SkipReason r);
const char* discard_name(DiscardReason r);

struct ViewBegin
{
	enum class Kind : uint8_t { Ok, Skipped, Discarded };
	Kind kind;
	SkipReason skip;        // Skipped
	DiscardReason discard;  // Discarded (frame_begin or C3D_FrameDrawOn failed, or the GX budget refused the clear:
	                        // the frame is already closed)
};

// Starts one C3D view frame for a w x h view (the harness's replay of one recording): the gates and the
// size, the target and staging out of frame, the sentinel fill, frame_begin, the clear (before the first
// draw), C3D_FrameDrawOn, the state save, the world program, attributes, buffers and GPU state.
ViewBegin view_begin(int w, int h);

// Appends one accepted polygon (3..16 vertices, already through core's accept_polygon, positions in view
// pixels, w clamped, colour set) to the current ring half, fans it, and extends the pending draw when
// `key` equals the previous polygon's, else issues the pending draw and starts a new one. False once the
// frame is discarded (ring full, command-buffer reserve, or an earlier view_discard); the polygon is then
// not drawn.
bool view_polygon(const Vertex* v, int n, const DrawKey& key);

// Gives the frame up (first reason wins): the pending draw is dropped, nothing more is drawn. view_end()
// still closes the frame.
void view_discard(DiscardReason reason);

struct ViewEnd
{
	bool ok;                // the readback is in out_argb
	DiscardReason discard;  // !ok: why (the frame is closed either way)
};

// Issues the pending draw, restores program, AttrInfo and BufInfo, flushes exactly the ring bytes written,
// then reads the target back (keeping alpha: ARGB with the alpha byte of the target) into out_argb
// (w x h, `pitch` pixels per row) and counts the pixels still holding the frame's sentinel into
// *sentinel_count (if non-null). A discarded frame is ended without a readback (ok false).
ViewEnd view_end(uint32_t* out_argb, int pitch, unsigned* sentinel_count);

// After C3D_Fini (release_and_fini): frees the world program and the rings. Idempotent.
void draw_free();

// For the "[c3d] cmdbuf ..." line (context.cpp): the counters of this file as text.
int draw_stats_text(char* buf, size_t size);

} // namespace back
} // namespace a1_c3d

#endif
