/*
	Aleph One for the Nintendo 3DS: the bottom screen in game (docs/3ds/STAGE7.md).

	With Graphics > Bottom Screen set to HUD + Map, render_screen() hands each
	game frame's bottom screen to bottom_frame():

	- the HUD buffer (640x160, drawn by the engine as always) averaged 2x2
	  to 320x80 at the top;
	- under it, 320x160, the overhead map, rendered at the bottom screen's
	  own resolution by the engine's map renderer into a separate surface
	  (the main map, opened with the map key, is untouched). It has its own
	  zoom, more zoomed out than the main map's default: while the main map
	  is closed, the map zoom keys (D-Pad Up/Down by default) zoom it
	  (bottom_map_zoom(), from RenderOther/screen_shared.h).

	The whole screen is also where the finger drags to look (Touch Screen
	Aiming, input_3ds.cpp); taps do nothing, so a drag never zooms.

	The picture is composed in a 320x240 XRGB8888 surface and presented only
	when something on it changed (present_bottom(), video_3ds.cpp). With
	Classic, menus and dialogs the console legend owns the screen
	(bottom_game(false), input_3ds.cpp).

	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; either version 3 of the License, or
	(at your option) any later version.
*/
#include "a1_3ds_platform.h"

#include "cseries.h"
#include "map.h"
#include "overhead_map.h"
#include "player.h"
#include "render.h"
#include "screen.h"
#include "shell.h"   // screen_mode_data
#include "screen_drawing.h"
#include "sdl_fonts.h"
#include "ViewControl.h"

#include <SDL2/SDL.h>

extern SDL_Surface* HUD_Buffer;   // RenderOther/screen.cpp
extern struct view_data* world_view;
extern font_info* GetInterfaceFont(short font_index);   // RenderOther/screen_drawing.cpp

namespace a1_3ds {

namespace {

constexpr int kW = 320, kH = 240;
// The HUD halved, at the top.
constexpr int kHudH = 80;
// The map under it, redrawn every kMapEvery-th frame (10 Hz at 30 fps):
// rendering it costs about 6 ms in Azahar at 320x188 (STAGE7.md).
constexpr int kMapY = kHudH, kMapH = kH - kHudH;
constexpr int kMapEvery = 3;
// The map's own zoom (overhead_map_data::scale: each step doubles; the main
// map's default is 3). The zoom keys step it between 1 and kMapScaleMax.
constexpr short kMapScaleDefault = 2, kMapScaleMax = 3;
// The renderer writes the level name at the top of the game map (baseline
// 25 pixels down in Monaco 18 by default, RenderOther/overhead_map.cpp). The
// map's frame starts this many rows above the surface, so the name falls
// outside it and the 160 rows are all map.
constexpr int kMapNameSkip = 32;

SDL_Surface* s_surface = nullptr;      // the bottom screen, composed
SDL_Surface* s_map = nullptr;          // the map renderer's target
bool s_full = true;                    // redraw everything
bool s_map_dirty = false;              // redraw the map at the next frame (zoom changed)
short s_map_scale = kMapScaleDefault;
int s_map_frame = 0;
bool s_last_map_allowed = true;

SDL_Surface* new_surface(int w, int h)
{
	return SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_RGB888);
}

uint32_t rgb(Uint8 r, Uint8 g, Uint8 b)
{
	return SDL_MapRGB(s_surface->format, r, g, b);
}

void fill(int x, int y, int w, int h, uint32_t pixel)
{
	SDL_Rect r = {x, y, w, h};
	SDL_FillRect(s_surface, &r, pixel);
}

void centred_text(const char* text, int x, int y, int w, int h, uint32_t pixel)
{
	const font_info* font = GetInterfaceFont(_interface_font);
	if (!font)
		return;
	const int tw = text_width(text, font, styleNormal);
	const int base = y + (h + font->get_ascent() - font->get_descent()) / 2;
	draw_text(s_surface, text, x + (w - tw) / 2, base, pixel, font, styleNormal);
}

void draw_hud()
{
	if (HUD_Buffer && HUD_Buffer->w == 640 && HUD_Buffer->h == 480 && HUD_Buffer->format->BytesPerPixel == 4 &&
	    get_screen_mode()->hud)
	{
		const Uint8* src = static_cast<const Uint8*>(HUD_Buffer->pixels) + 320 * HUD_Buffer->pitch;
		half_blit(src, HUD_Buffer->pitch, kW, kHudH, s_surface->pixels, s_surface->pitch);
	}
	else
		fill(0, 0, kW, kHudH, rgb(0, 0, 0));
}

void draw_map(bool allowed)
{
	if (!allowed)
	{
		fill(0, kMapY, kW, kMapH, rgb(0, 0, 0));
		centred_text("Map unavailable", 0, kMapY, kW, kMapH, rgb(120, 120, 120));
		return;
	}
	if (!s_map)
		s_map = new_surface(kW, kMapH);
	if (!s_map)
		return;
	SDL_FillRect(s_map, nullptr, SDL_MapRGB(s_map->format, 0, 0, 0));
	overhead_map_data d;
	d.mode = _rendering_game_map;
	d.scale = s_map_scale;
	d.origin.x = world_view->origin.x;
	d.origin.y = world_view->origin.y;
	d.origin_polygon_index = world_view->origin_polygon_index;
	d.width = kW;
	d.height = kMapH;
	d.half_width = kW / 2;
	d.half_height = kMapH / 2 + kMapNameSkip;   // the player stays in the middle
	d.top = -kMapNameSkip;
	d.left = 0;
	d.draw_everything = false;
	_set_port_to_custom(s_map);
	_render_overhead_map(&d);
	_restore_port();
	SDL_Rect dst = {0, kMapY, kW, kMapH};
	SDL_BlitSurface(s_map, nullptr, s_surface, &dst);
}

// A framed "Saving..." box, centred on `s` (or on the rows y0..y0+h0 of it).
void draw_saving_box(SDL_Surface* s, int y0, int h0)
{
	const font_info* font = GetInterfaceFont(_interface_font);
	if (!s || !font)
		return;
	const int bw = s->w * 9 / 20, bh = s->h * 3 / 20;   // 180x36 on 400x240, 288x72 on 640x480
	SDL_Rect frame = {(s->w - bw) / 2, y0 + (h0 - bh) / 2, bw, bh};
	SDL_FillRect(s, &frame, SDL_MapRGB(s->format, 0, 150, 0));
	SDL_Rect inner = {frame.x + 2, frame.y + 2, bw - 4, bh - 4};
	SDL_FillRect(s, &inner, SDL_MapRGB(s->format, 0, 0, 0));
	const char* text = "Saving...";
	const int tw = text_width(text, font, styleNormal);
	const int base = inner.y + (inner.h + font->get_ascent() - font->get_descent()) / 2;
	draw_text(s, text, inner.x + (inner.w - tw) / 2, base, SDL_MapRGB(s->format, 230, 230, 230), font,
	          styleNormal);
}

} // namespace

// --- Used by input_3ds.cpp ---------------------------------------------------

void bottom_force_redraw()
{
	s_full = true;
}

// --- Public ----------------------------------------------------------------------

// A save blocks the main thread for seconds (the SD card), and nothing draws
// meanwhile: say so on both screens, before it starts. The top screen's box
// goes on the engine's own screen surface, which the next rendered frame
// replaces; the bottom one on the map area, and the next frame redraws it.
void show_saving()
{
	if (SDL_Surface* top = MainScreenSurface())
	{
		draw_saving_box(top, 0, top->h);
		MainScreenUpdateRect(0, 0, 0, 0);
	}
	if (bottom_is_game() && s_surface)
	{
		draw_saving_box(s_surface, kMapY, kMapH);
		present_bottom(s_surface->pixels, s_surface->pitch);
		s_full = true;
	}
}

int bottom_map_zoom(int step)
{
	if (!bottom_is_game() || !world_view || world_view->overhead_map_active)
		return -1;
	const int scale = s_map_scale + step;
	if (scale < OVERHEAD_MAP_MINIMUM_SCALE || scale > kMapScaleMax)
		return 0;
	s_map_scale = static_cast<short>(scale);
	s_map_dirty = true;
	return 1;
}

void bottom_frame(BottomMode mode, bool hud_changed)
{
	if (mode == BottomClassic)
	{
		bottom_game(false);
		return;
	}
	bottom_game(true);
	if (!bottom_is_game())
		return;   // no console yet (input_init() not run)
	if (!s_surface)
	{
		s_surface = new_surface(kW, kH);
		if (!s_surface)
			return;
		s_full = true;
	}
	bool changed = false;
	if (s_full || hud_changed)
	{
		draw_hud();
		changed = true;
	}
	// The player moves, and the map marks what is seen: redraw it
	// regularly, not only on changes.
	const bool allowed = View_MapActive();
	const bool tick = ++s_map_frame >= kMapEvery;
	if (tick)
		s_map_frame = 0;
	if ((allowed && tick) || s_full || s_map_dirty || allowed != s_last_map_allowed)
	{
		draw_map(allowed);
		changed = true;
	}
	s_last_map_allowed = allowed;
	s_full = s_map_dirty = false;
	if (changed)
		present_bottom(s_surface->pixels, s_surface->pitch);
}

} // namespace a1_3ds
