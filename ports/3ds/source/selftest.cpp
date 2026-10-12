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

#include "Decoder.h"
#include "FileHandler.h"
#include "SoundManager.h"
#include "WadImageCache.h"
#include "a1_filesystem.h"

#include <SDL2/SDL.h>

#include <dirent.h>
#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <memory>
#include <set>
#include <string>
#include <vector>

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

namespace {

// Decodes a whole stream the way MusicPlayer does (8 KiB requests until a
// short read). Returns the bytes decoded and an FNV-1a hash of them.
size_t decode_all(StreamDecoder& d, uint32& hash, double& ms, float& max_ms)
{
	std::vector<uint8> buf(8192);
	size_t total = 0;
	hash = 2166136261u;
	max_ms = 0;
	const Uint64 t0 = SDL_GetPerformanceCounter();
	for (int calls = 0; calls < 1000000; ++calls)
	{
		const Uint64 c0 = SDL_GetPerformanceCounter();
		const int32 n = d.Decode(buf.data(), static_cast<int32>(buf.size()));
		max_ms = std::max(max_ms, static_cast<float>((SDL_GetPerformanceCounter() - c0) * 1000.0 / SDL_GetPerformanceFrequency()));
		for (int32 i = 0; i < n; ++i)
			hash = (hash ^ buf[i]) * 16777619u;
		total += n;
		if (n < static_cast<int32>(buf.size()))
			break;
	}
	ms = (SDL_GetPerformanceCounter() - t0) * 1000.0 / SDL_GetPerformanceFrequency();
	return total;
}

} // namespace

// Debug flag "debug-decoders" (docs/3ds/STAGE6.md): every file in
// <user dir>/audio-test, and the scenario's Music.ogg, goes through the
// engine's decoder factories (Sound/Decoder.cpp). Each one is decoded to the
// end, rewound and decoded again (a looping track), and checked against
// Decoder::Frames() (what replacement sounds rely on). Main thread.
void decoder_selftest()
{
	std::vector<std::string> paths;
	const std::string dir = user_dir() + "/audio-test";
	if (DIR* d = opendir(dir.c_str()))
	{
		while (struct dirent* e = readdir(d))
			if (e->d_name[0] != '.')
				paths.push_back(dir + "/" + e->d_name);
		closedir(d);
	}
	std::sort(paths.begin(), paths.end());
	paths.push_back(scenario_dir() + "/Music.ogg");
	printf("[3ds] debug-decoders: %zu files\n", paths.size());
	int failures = 0;
	for (const std::string& path : paths)
	{
		const char* name = strrchr(path.c_str(), '/') + 1;
		FileSpecifier file(path.c_str());
		std::unique_ptr<StreamDecoder> d = StreamDecoder::Get(file);
		if (!d)
		{
			printf("[3ds] debug-decoders: %s: not decodable\n", name);
			continue;
		}
		uint32 h1 = 0, h2 = 0;
		double ms1 = 0, ms2 = 0;
		float max1 = 0, max2 = 0;
		const size_t n1 = decode_all(*d, h1, ms1, max1);
		d->Rewind();
		const size_t n2 = decode_all(*d, h2, ms2, max2);
		const double seconds = d->BytesPerFrame() ? n1 / static_cast<double>(d->BytesPerFrame()) / d->Rate() : 0;
		std::unique_ptr<Decoder> whole(Decoder::Get(file));
		const long frames = whole ? whole->Frames() : -1;
		const bool frames_ok = whole && static_cast<size_t>(frames) * whole->BytesPerFrame() == n1;
		const bool ok = n1 > 0 && n1 == n2 && h1 == h2 && frames_ok;
		failures += !ok;
		printf("[3ds] debug-decoders: %s: %s, %.0f Hz %s, %.2f s; pass 1 %zu bytes %08lX in %.0f ms "
		       "(%.1f%% of real time, longest 8 KiB request %.1f ms); after Rewind %zu bytes %08lX "
		       "(longest request %.1f ms); Frames() %ld (%s) -> %s\n",
		       name, d->GetAudioFormat() == AudioFormat::_16_bit ? "16-bit" : "float", d->Rate(),
		       d->IsStereo() ? "stereo" : "mono", seconds, n1, static_cast<unsigned long>(h1), ms1,
		       seconds > 0 ? ms1 / (10.0 * seconds) : 0.0, max1, n2, static_cast<unsigned long>(h2), max2, frames,
		       frames_ok ? "matches" : "MISMATCH", ok ? "PASS" : "FAIL");
	}
	printf("[3ds] debug-decoders: done, %d failure(s)\n", failures);
}

} // namespace a1_3ds
