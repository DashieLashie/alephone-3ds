/*
	Copyright (C) 2026 and beyond by the "Aleph One" developers.

	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; either version 3 of the License, or
	(at your option) any later version.

	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	Aleph One for the Nintendo 3DS: debug self-tests that need engine
	headers, kept out of a1_3ds_platform.cpp (which includes <3ds.h>).
*/

#include "a1_3ds_platform.h"

#include "FileHandler.h"
#include "SoundManager.h"
#include "WadImageCache.h"
#include "a1_filesystem.h"

#include <SDL2/SDL.h>

#include <dirent.h>
#include <stdio.h>
#include <string.h>

#include <set>
#include <string>

namespace a1_3ds {

namespace {

std::set<std::string> image_cache_files()
{
	std::set<std::string> names;
	FileSpecifier dir;
	dir.SetToImageCacheDir();
	if (DIR* d = opendir(dir.GetPath()))
	{
		while (struct dirent* e = readdir(d))
			if (e->d_name[0] != '.' && strcmp(e->d_name, "Cache.ini"))
				names.insert(e->d_name);
		closedir(d);
	}
	return names;
}

} // namespace

// Debug flag "debug-entropy" (STAGE5-PREREQS.md item 2). Checks the two
// engine sites that used std::random_device: WadImageCache's file names and
// a1fs::unique_path() (FileSpecifier::SetTempName). Three test images are
// cached under a descriptor no scenario uses, logged and removed again.
void entropy_selftest()
{
	printf("[3ds] debug-entropy: unique_path:");
	for (int i = 0; i < 3; ++i)
		printf(" %s", a1fs::unique_path("%%%%%%%%").string().c_str());
	printf("\n");

	SDL_Surface* src = SDL_CreateRGBSurfaceWithFormat(0, 8, 8, 32, SDL_PIXELFORMAT_RGBA8888);
	if (!src)
	{
		printf("[3ds] debug-entropy: SDL_CreateRGBSurfaceWithFormat failed: %s\n", SDL_GetError());
		return;
	}
	SDL_FillRect(src, nullptr, SDL_MapRGBA(src->format, 0xA5, 0x00, 0x5A, 0xFF));

	const std::set<std::string> before = image_cache_files();
	WadImageDescriptor desc[3];
	for (int i = 0; i < 3; ++i)
	{
		desc[i].file = FileSpecifier(user_dir() + "/debug-entropy");
		desc[i].checksum = 0xA1E7709Eu;
		desc[i].index = static_cast<short>(i);
		desc[i].tag = 0;
		WadImageCache::instance()->cache_image(desc[i], 4, 4, src);   // 8x8 -> 4x4: resized, stored
	}
	SDL_FreeSurface(src);

	const std::set<std::string> after = image_cache_files();
	int added = 0;
	printf("[3ds] debug-entropy: image cache names:");
	for (const std::string& n : after)
		if (!before.count(n))
		{
			printf(" %s", n.c_str());
			++added;
		}
	printf("\n[3ds] debug-entropy: %d of 3 images stored under distinct names%s\n", added,
	       added == 3 ? "" : " (FAIL: names collided)");

	for (WadImageDescriptor& d : desc)
		WadImageCache::instance()->remove_image(d, 4, 4);
	printf("[3ds] debug-entropy: removed; %zu files left in the image cache (was %zu)\n",
	       image_cache_files().size(), before.size());
}

// Debug flag "debug-audio" (STAGE5-PREREQS.md items 3 and 5): with no
// input yet, nothing plays at the main menu (and the music needs an Ogg
// decoder), so the audio probe would only see OpenAL mixing silence. This
// starts the first 8 sounds that load, all at once, as local (non-3D)
// sounds. Main thread only: SoundManager is not thread-safe.
void audio_load_selftest()
{
	SoundManager* sm = SoundManager::instance();
	if (!sm->IsInitialized() || !sm->IsActive())
	{
		printf("[3ds] debug-audio: sound is not active; no test sounds\n");
		return;
	}
	int played = 0;
	printf("[3ds] debug-audio: playing sounds");
	for (short i = 0; i < NUMBER_OF_SOUND_DEFINITIONS && played < 8; ++i)
		if (sm->PlaySound(i, nullptr, NONE))
		{
			printf(" %d", i);
			++played;
		}
	printf(" (%d at once)\n", played);
}

} // namespace a1_3ds
