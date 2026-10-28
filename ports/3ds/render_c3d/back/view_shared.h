/*
	Aleph One for the Nintendo 3DS, citro3d back end: the view target, staging and readback shared
	by the calibration (back/view.cpp) and the draw path (back/draw.cpp)
	(docs/3ds/CITRO3D_DESIGN.md, section 7, M1 specification, "Back end"). Back-internal: it names a
	citro3d type, so only back/ includes it. Main thread only.

	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; either version 3 of the License, or
	(at your option) any later version.
*/
#ifndef A1_C3D_BACK_VIEW_SHARED_H
#define A1_C3D_BACK_VIEW_SHARED_H

#include <citro3d.h>

#include <cstdint>

namespace a1_c3d {
namespace back {

// A view size the readback supports: positive, multiples of 8, at least `min_side`, at most 1024 per
// side and 262144 pixels (design 1.5).
bool view_size_supported(int w, int h, int min_side);

// The unlinked RGBA8 target and the linear staging for a w x h view, made (or, for another size,
// deleted and made again) OUT of frame: C3D_RenderTargetDelete svcBreaks inside one. False: out of
// memory (logged once; the size is not retried).
bool view_ensure(int w, int h);
// The target of the last successful view_ensure(); owned by citro3d's context.
C3D_RenderTarget* view_target();
// Fills the staging of the current size with `rgba` (0xRRGGBBAA) and flushes it (out of frame, before
// frame_begin()), so a pixel the transfer never wrote is recognisable and no dirty cache line is
// written back over the GPU's output.
void view_fill_staging(uint32_t rgba);
// gx_op: clears the target to `rgba` (0xRRGGBBAA, C3D_RenderTargetClear, renderqueue.h:72). False: the
// GX budget refused. Valid only right after frame_begin(), before the first draw of the frame
// (C3D_FrameBufClear is a GX_MemoryFill without a split, so a clear after draws would run before them).
bool view_clear_target(uint32_t rgba);

enum class Readback { Ok, GxRefused, WaitFailed };
// The readback sequence of M0, shared: gx_split(C3D_FrameSplit(GX_CMDLIST_FLUSH)) and the in-frame
// gx_op(C3D_SyncDisplayTransfer), frame_end(0, false, View), the empty frame_begin()/frame_end()
// retirement pair, GSPGPU_InvalidateDataCache, and the conversion of the w x h staging into the
// converted copy: rgba_to_argb_keep when `keep_alpha`, else the forcing rgba_to_argb. The frame is
// closed on return whatever the result (GxRefused: closed, nothing read; WaitFailed: the retirement
// frame could not begin). The caller must have made sure of the command-buffer reserve and flushed
// the data the draws read.
Readback view_readback(bool keep_alpha);
// The converted copy of the last Ok view_readback(): w x h, pitch w; valid until the next call or
// view_free().
const uint32_t* view_argb();
int view_width();
int view_height();

} // namespace back
} // namespace a1_c3d

#endif
