/*
	Aleph One for the Nintendo 3DS: the bottom screen in game (docs/3ds/STAGE7.md).

	With Graphics > Bottom Screen set to HUD or Map, render_screen() hands each
	game frame's bottom screen to bottom_frame():

	- HUD: the HUD buffer (640x160, drawn by the engine as always) averaged
	  2x2 to 320x80 at the top; below it the area the finger drags to look.
	- Map: the overhead map, rendered at the bottom screen's own resolution
	  by the engine's map renderer into a separate surface (the main map,
	  opened with the map key, is untouched).
	- Both: a strip of touch buttons (map, action, next weapon) at the
	  bottom. input_3ds.cpp hit-tests touches with bottom_button_at(); a
	  press becomes the button's action flag for the next input tick
	  (take_touch_action_flags(), OR-ed in by Misc/vbl.cpp parse_keymap()),
	  so it acts as if the bound key had been pressed.

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
// The button strip: three buttons in the bottom 52 rows.
constexpr int kStripY = 188;
constexpr int kButtonY = kStripY + 2, kButtonH = kH - kButtonY - 4;
constexpr int kMargin = 4, kGap = 4;
constexpr int kButtonCount = 3;
constexpr int kButtonW = (kW - 2 * kMargin - (kButtonCount - 1) * kGap) / kButtonCount;
// HUD mode: the HUD halved, at the top.
constexpr int kHudH = 80;
// Map mode: the map above the strip, redrawn every kMapEvery-th frame
// (10 Hz at 30 fps): rendering it costs about 6 ms in Azahar (STAGE7.md).
constexpr int kMapH = kStripY;
constexpr int kMapEvery = 3;

struct Button
{
	const char* label;
	uint32_t flag;
};
const Button kButtons[kButtonCount] = {
	{"MAP", _toggle_map},
	{"ACTION", _action_trigger_state},
	{"NEXT WEAPON", _cycle_weapons_forward},
};

SDL_Surface* s_surface = nullptr;      // the bottom screen, composed
SDL_Surface* s_map = nullptr;          // map mode: the map renderer's target
bool s_full = true;                    // redraw everything
bool s_buttons_dirty = true;
bool s_down[kButtonCount] = {};
uint32_t s_pending = 0;                // action flags for the next input tick
int s_last_mode = -1;
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

void draw_buttons()
{
	fill(0, kStripY, kW, kH - kStripY, rgb(0, 0, 0));
	for (int i = 0; i < kButtonCount; ++i)
	{
		const int x = kMargin + i * (kButtonW + kGap);
		const uint32_t edge = s_down[i] ? rgb(120, 255, 120) : rgb(40, 140, 40);
		fill(x, kButtonY, kButtonW, kButtonH, edge);
		fill(x + 2, kButtonY + 2, kButtonW - 4, kButtonH - 4, s_down[i] ? rgb(30, 110, 30) : rgb(10, 36, 10));
		centred_text(kButtons[i].label, x, kButtonY, kButtonW, kButtonH,
		             s_down[i] ? rgb(255, 255, 255) : rgb(120, 255, 120));
	}
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

void draw_look_area()
{
	fill(0, kHudH, kW, kStripY - kHudH, rgb(0, 0, 0));
	centred_text("drag here to look (Touch Screen Aiming)", 0, kHudH, kW, kStripY - kHudH, rgb(60, 60, 60));
}

void draw_map(bool allowed)
{
	if (!allowed)
	{
		fill(0, 0, kW, kMapH, rgb(0, 0, 0));
		centred_text("Map unavailable", 0, 0, kW, kMapH, rgb(120, 120, 120));
		return;
	}
	if (!s_map)
		s_map = new_surface(kW, kMapH);
	if (!s_map)
		return;
	SDL_FillRect(s_map, nullptr, SDL_MapRGB(s_map->format, 0, 0, 0));
	overhead_map_data d;
	d.mode = _rendering_game_map;
	d.scale = world_view->overhead_map_scale;
	d.origin.x = world_view->origin.x;
	d.origin.y = world_view->origin.y;
	d.origin_polygon_index = world_view->origin_polygon_index;
	d.width = kW;
	d.height = kMapH;
	d.half_width = kW / 2;
	d.half_height = kMapH / 2;
	d.top = d.left = 0;
	d.draw_everything = false;
	_set_port_to_custom(s_map);
	_render_overhead_map(&d);
	_restore_port();
	SDL_Rect dst = {0, 0, kW, kMapH};
	SDL_BlitSurface(s_map, nullptr, s_surface, &dst);
}

} // namespace

// --- Used by input_3ds.cpp ---------------------------------------------------

int bottom_button_at(float x, float y)
{
	if (y < kStripY)
		return -1;
	for (int i = 0; i < kButtonCount; ++i)
	{
		const int bx = kMargin + i * (kButtonW + kGap);
		if (x >= bx - kGap / 2 && x < bx + kButtonW + kGap / 2)
			return i;
	}
	return -1;
}

void bottom_button_set(int button, bool down)
{
	if (button < 0 || button >= kButtonCount)
		return;
	if (down)
		s_pending |= kButtons[button].flag;
	s_down[button] = down;
	s_buttons_dirty = true;
}

void bottom_force_redraw()
{
	s_full = true;
}

// --- Public ----------------------------------------------------------------------

uint32_t take_touch_action_flags()
{
	const uint32_t flags = s_pending;
	s_pending = 0;
	return flags;
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
	if (mode != s_last_mode)
	{
		s_last_mode = mode;
		s_full = true;
	}

	bool changed = false;
	if (mode == BottomHud)
	{
		if (s_full || hud_changed)
		{
			draw_hud();
			changed = true;
		}
		if (s_full)
			draw_look_area();
	}
	else
	{
		// The player moves, and the map marks what is seen: redraw it
		// regularly, not only on changes.
		const bool allowed = View_MapActive();
		const bool tick = ++s_map_frame >= kMapEvery;
		if (tick)
			s_map_frame = 0;
		if ((allowed && tick) || s_full || allowed != s_last_map_allowed)
		{
			draw_map(allowed);
			changed = true;
		}
		s_last_map_allowed = allowed;
	}
	if (s_full || s_buttons_dirty)
	{
		draw_buttons();
		changed = true;
	}
	s_full = s_buttons_dirty = false;
	if (changed)
		present_bottom(s_surface->pixels, s_surface->pitch);
}

} // namespace a1_3ds
