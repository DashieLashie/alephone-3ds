/*
	Aleph One for the Nintendo 3DS, citro3d back end (docs/3ds/CITRO3D_DESIGN.md, 1.4 "Engine-facing"):
	what the engine calls. Included by RenderMain/render.cpp under
	#if defined(A1_PLATFORM_3DS) && defined(A1_RENDER_C3D) only. No <3ds.h>.

	M0: render_frame(); M2: collections_changed() (hooks 2a, 2b); M6: hook 3's direct_policy(), direct_frame(),
	overlay_begin() and overlay_target() (screen.cpp, screen_shared.h; PATCHES.md 47).

	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; either version 3 of the License, or
	(at your option) any later version.
*/
#ifndef A1_C3D_H
#define A1_C3D_H

class RasterizerClass;
struct view_data;
struct bitmap_definition;
struct SDL_Surface;

namespace a1_c3d {

using DrawFn = void (*)(void* ctx, RasterizerClass* ras);

// One world view. true: frame done (in `dest`, or on the GPU in direct mode).
// false: caller draws it with `sw`; nothing was written to `dest`.
bool render_frame(view_data& view, bitmap_definition* dest, RasterizerClass* sw,
                  DrawFn draw, void* ctx);

// Hooks 2a (end of load_collections) and 2b (end of unload_all_collections), shapes.cpp: the engine's
// collection bitmaps and shading tables may have been freed or reallocated. Bumps the texture cache's
// generation (M2); cheap, main thread only, works with citro3d down.
void collections_changed();

// --- M6, hook 3 (design 7, M6 specification "Hook 3", "Engine-facing API", "Eligibility") ---

// The conditions that keep a frame's world view off the GPU (screen.cpp's helper sets them; 0 = eligible). The order
// is the reason order of the logs (core/direct_state.cpp).
enum DirectBlock : unsigned
{
	kDirectNotNative = 1u << 0,   // not the 400x240 HUD + Map surface (Classic, menus, a Lua HUD)
	kDirectViewRect = 1u << 1,    // the view is scaled, offset or interlaced by the blit
	kDirectAccel = 1u << 2,       // not the software blit path
	kDirectMap = 1u << 3,         // overhead map
	kDirectTerminal = 1u << 4,
	kDirectGamma = 1u << 5,       // !using_default_gamma: fades, flashes, tints, brightness
	kDirectDarken = 1u << 6,      // darken_world_window
	kDirectNetwork = 1u << 7,
	kDirectLuaHud = 1u << 8,
	kDirectMovie = 1u << 9
};

// 3a, every frame just before render_view. With 0 the frame may be direct; also drops an overlay hand-out the last
// frame's present did not consume, and makes the overlay surface exist (out of frame) before a frame is decided direct.
void direct_policy(unsigned blocks);
// 3b, after render_view: this frame's world view is on the GPU (a direct view frame was drawn and not discarded), so the
// overlays go to overlay_begin()'s surface and the world-view blit is skipped.
bool direct_frame();

struct OverlayBand
{
	short y0, y1;   // surface rows [y0, y1)
};

// 3b, on a direct frame only: the 400x240 RGBA8888 surface for the frame's overlays, transparent in the bands (and
// everywhere else); never null when direct_frame() is true.
SDL_Surface* overlay_begin(int n, const OverlayBand* bands);
// The surface handed out for this frame, or nullptr (screen_shared.h maps DisplayText's colours with its format).
const SDL_Surface* overlay_target();

} // namespace a1_c3d

#endif
