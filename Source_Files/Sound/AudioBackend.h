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

	This license is contained in the file "COPYING",
	which is included with this source code; it is available online at
	http://www.gnu.org/licenses/gpl.html
*/

#ifndef __AUDIO_BACKEND_H
#define __AUDIO_BACKEND_H

#include <SDL2/SDL.h>
#include <cstdint>
#include <memory>

// Audio output device used by OpenALManager. OpenAL Soft mixes into a
// loopback device and never opens hardware itself; the backend owns the real
// output and pulls mixed samples by calling the callback from its own thread.
// Ports replace CreateDefault() to drive a different output.
class AudioBackend {
public:
	struct Spec {
		int rate = 0;
		SDL_AudioFormat format = 0; // AUDIO_S16SYS, AUDIO_F32SYS, ...
		int channels = 0;
		uint16_t samples = 0; // frames per callback
	};

	// Fills len bytes of stream with mixed audio
	typedef void (*PullCallback)(void* userdata, uint8_t* stream, int len);

	virtual ~AudioBackend() = default;

	// Opens the device paused. On success obtained holds the format actually
	// used, which may differ from desired.
	virtual bool Open(const Spec& desired, Spec& obtained, PullCallback callback, void* userdata) = 0;
	virtual void Close() = 0;
	virtual void Pause(bool paused) = 0;
	virtual bool IsStopped() const = 0;

	// Excludes the pull callback while held
	virtual void Lock() = 0;
	virtual void Unlock() = 0;

	static std::unique_ptr<AudioBackend> CreateDefault();
};

// SDL's legacy single-device API (device id 1)
class SDLAudioBackend : public AudioBackend {
public:
	bool Open(const Spec& desired, Spec& obtained, PullCallback callback, void* userdata) override {
		SDL_AudioSpec sdl_desired = {};
		sdl_desired.freq = desired.rate;
		sdl_desired.format = desired.format;
		sdl_desired.channels = desired.channels;
		sdl_desired.samples = desired.samples;
		sdl_desired.callback = callback;
		sdl_desired.userdata = userdata;

		SDL_AudioSpec sdl_obtained = {};
		if (SDL_OpenAudio(&sdl_desired, &sdl_obtained) < 0) return false;

		obtained.rate = sdl_obtained.freq;
		obtained.format = sdl_obtained.format;
		obtained.channels = sdl_obtained.channels;
		obtained.samples = sdl_obtained.samples;
		return true;
	}

	void Close() override { SDL_CloseAudio(); }
	void Pause(bool paused) override { SDL_PauseAudio(paused); }
	bool IsStopped() const override { return SDL_GetAudioStatus() == SDL_AUDIO_STOPPED; }
	void Lock() override { SDL_LockAudio(); }
	void Unlock() override { SDL_UnlockAudio(); }
};

inline std::unique_ptr<AudioBackend> AudioBackend::CreateDefault() { return std::make_unique<SDLAudioBackend>(); }

#endif
