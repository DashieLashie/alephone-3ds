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

	Aleph One for the Nintendo 3DS: Ogg Vorbis and MP3 decoders
	(docs/3ds/STAGE6.md).

	The 3DS libsndfile has no codecs (no float libvorbis or mpg123 build for
	it), so Sound/Decoder.cpp asks open_decoder() after SndfileDecoder fails:
	- Ogg Vorbis through Tremor (libvorbisidec, integer-only),
	- MP3 (MPEG audio layer I-III) through libmpg123.
	Both produce native-endian signed 16-bit PCM (AL_FORMAT_*16: no float
	conversion in OpenAL, half the memory traffic of SndfileDecoder's float).

	Streaming: music is decoded by MusicPlayer on SDL's audio thread in the
	requests AudioPlayer::FillBuffers makes (at most 8 KiB of PCM per OpenAL
	buffer, 4 buffers per source), so a track is never decoded into memory
	as a whole. The *compressed* file is read into memory when it is small
	(kPreloadMax, within a total of kPreloadBudget): the audio thread then
	never waits on the SD card, whose reads take milliseconds and queue
	behind the main thread's level loading. Larger files are read through
	SDL_RWops on demand, as libsndfile does.

	Replacement sounds (Decoder::Get, MML <sound>) are decoded whole on the
	main thread, as upstream does for every format.
*/

#include "a1_3ds_platform.h"

#include "Decoder.h"
#include "FileHandler.h"

#include <SDL2/SDL.h>
#include <mpg123.h>
#include <tremor/ivorbisfile.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <stdio.h>
#include <vector>

namespace a1_3ds {

namespace {

const size_t kPreloadMax = 4u << 20;      // per file
const size_t kPreloadBudget = 12u << 20;  // all open decoders together
std::atomic<size_t> s_preloaded{0};

// --- statistics -------------------------------------------------------------

std::mutex s_info_mutex;
DecodeStats s_info;                        // last_* fields only
std::atomic<uint64_t> s_decode_us{0};
std::atomic<uint64_t> s_decoded_us{0};   // audio decoded, in microseconds
std::atomic<uint32_t> s_max_call_us{0};

class DecodeTimer
{
public:
	DecodeTimer() : t0(SDL_GetPerformanceCounter()) {}
	void finish(int32 bytes, int bytes_per_frame, float rate)
	{
		const uint64_t us = (SDL_GetPerformanceCounter() - t0) * 1000000 / SDL_GetPerformanceFrequency();
		s_decode_us.fetch_add(us, std::memory_order_relaxed);
		if (us > s_max_call_us.load(std::memory_order_relaxed))
			s_max_call_us.store(static_cast<uint32_t>(us), std::memory_order_relaxed);
		if (bytes > 0 && bytes_per_frame > 0 && rate > 0)
			s_decoded_us.fetch_add(static_cast<uint64_t>(bytes / bytes_per_frame * 1e6 / rate),
			                       std::memory_order_relaxed);
	}
private:
	uint64_t t0;
};

std::string file_name(const FileSpecifier& file)
{
	const std::string path = file.GetPath();
	const size_t slash = path.find_last_of('/');
	return slash == std::string::npos ? path : path.substr(slash + 1);
}

// --- compressed data source: memory or SDL_RWops ------------------------------

class Source
{
public:
	~Source()
	{
		if (rw)
			SDL_RWclose(rw);
		if (!data.empty())
			s_preloaded.fetch_sub(data.size());
	}

	// Takes the file's RWops; reads it into memory if it is small enough.
	static std::unique_ptr<Source> open(FileSpecifier& file)
	{
		OpenedFile opened;
		if (!file.Open(opened))
			return nullptr;
		auto src = std::unique_ptr<Source>(new Source);
		src->rw = opened.TakeRWops();
		if (!src->rw)
			return nullptr;
		const Sint64 size = SDL_RWsize(src->rw);
		if (size > 0 && static_cast<size_t>(size) <= kPreloadMax)
		{
			const size_t n = static_cast<size_t>(size);
			if (s_preloaded.fetch_add(n) + n <= kPreloadBudget)
			{
				src->data.resize(n);
				if (SDL_RWread(src->rw, src->data.data(), 1, n) == n)
				{
					SDL_RWclose(src->rw);
					src->rw = nullptr;
					return src;
				}
				src->data.clear();
				src->data.shrink_to_fit();
				SDL_RWseek(src->rw, 0, RW_SEEK_SET);
			}
			s_preloaded.fetch_sub(n);
		}
		return src;
	}

	bool in_memory() const { return !rw; }

	size_t read(void* out, size_t len)
	{
		if (rw)
			return SDL_RWread(rw, out, 1, len);
		len = std::min(len, data.size() - pos);
		memcpy(out, data.data() + pos, len);
		pos += len;
		return len;
	}

	// Returns the new position, or -1.
	int64_t seek(int64_t offset, int whence)
	{
		if (rw)
			return SDL_RWseek(rw, offset, whence);
		int64_t base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? static_cast<int64_t>(pos)
		                                                          : static_cast<int64_t>(data.size());
		const int64_t to = base + offset;
		if (to < 0 || to > static_cast<int64_t>(data.size()))
			return -1;
		pos = static_cast<size_t>(to);
		return to;
	}

	int64_t tell() { return rw ? SDL_RWtell(rw) : static_cast<int64_t>(pos); }

private:
	Source() = default;
	SDL_RWops* rw = nullptr;
	std::vector<uint8> data;
	size_t pos = 0;
};

void note_open(const FileSpecifier& file, const char* codec, int rate, int channels, bool in_memory)
{
	const std::string name = file_name(file);
	{
		std::lock_guard<std::mutex> lock(s_info_mutex);
		s_info.last_file = name;
		s_info.last_codec = codec;
		s_info.last_rate = rate;
		s_info.last_channels = channels;
		s_info.last_in_memory = in_memory;
	}
	printf("[3ds] audio: %s: %s, %d Hz, %s, %s\n", name.c_str(), codec, rate,
	       channels == 2 ? "stereo" : "mono", in_memory ? "compressed data in memory" : "streamed from the file");
}

// Requests are whole frames; a partial frame at the end of a request would
// make ov_read/mpg123_read return less and look like the end of the track.
int32 whole_frames(int32 max_length, int bytes_per_frame)
{
	return max_length - max_length % bytes_per_frame;
}

// --- Ogg Vorbis (Tremor) ------------------------------------------------------

size_t ov_read_cb(void* ptr, size_t size, size_t nmemb, void* ds)
{
	if (!size)
		return 0;
	return static_cast<Source*>(ds)->read(ptr, size * nmemb) / size;
}

int ov_seek_cb(void* ds, ogg_int64_t offset, int whence)
{
	return static_cast<Source*>(ds)->seek(offset, whence) < 0 ? -1 : 0;
}

int ov_close_cb(void*)
{
	return 0;   // the decoder owns the Source
}

long ov_tell_cb(void* ds)
{
	return static_cast<long>(static_cast<Source*>(ds)->tell());
}

class TremorDecoder final : public Decoder
{
public:
	~TremorDecoder() override { Close(); }

	bool Open(FileSpecifier& File) override
	{
		Close();
		auto src = Source::open(File);
		return src && OpenSource(File, std::move(src));
	}

	bool OpenSource(const FileSpecifier& file, std::unique_ptr<Source> src)
	{
		Close();
		source = std::move(src);
		const ov_callbacks callbacks = {ov_read_cb, ov_seek_cb, ov_close_cb, ov_tell_cb};
		if (ov_open_callbacks(source.get(), &vf, nullptr, 0, callbacks) != 0)
		{
			source.reset();
			return false;
		}
		opened = true;
		const vorbis_info* info = ov_info(&vf, -1);
		if (!info || info->channels < 1 || info->channels > 2 || info->rate <= 0)
		{
			printf("[3ds] audio: %s: Ogg Vorbis with %d channels is not supported (mono or stereo only)\n",
			       file_name(file).c_str(), info ? info->channels : 0);
			Close();
			return false;
		}
		channels = info->channels;
		rate = info->rate;
		// Tremor sets up its decoder on the first ov_read: the first request
		// of a track took 50-60 ms in a game (on SDL's audio thread, against a
		// 31 ms buffer), while requests after a seek back take about 5 ms.
		// Pay for that here, on the thread that opens the file.
		char prime[4096];
		int bitstream = 0;
		if (ov_read(&vf, prime, sizeof prime, &bitstream) > 0)
			ov_raw_seek(&vf, 0);
		at_start = true;
		note_open(file, "Ogg Vorbis (Tremor)", rate, channels, source->in_memory());
		return true;
	}

	int32 Decode(uint8* buffer, int32 max_length) override
	{
		if (!opened)
			return 0;
		DecodeTimer timer;
		const int32 want = whole_frames(max_length, BytesPerFrame());
		int32 filled = 0;
		int errors = 0;
		while (filled < want && !ended)
		{
			int bitstream = 0;
			const long n = ov_read(&vf, reinterpret_cast<char*>(buffer + filled), want - filled, &bitstream);
			at_start = false;
			if (n > 0)
			{
				const vorbis_info* info = ov_info(&vf, bitstream);
				if (info && (info->channels != channels || info->rate != rate))
				{
					// A chained stream switching format: OpenAL buffers have
					// one format, so end the track here.
					ended = true;
					break;
				}
				filled += n;
				errors = 0;
			}
			else if (n == 0 || ++errors > 8)
				ended = true;   // end of stream, or a damaged one
			// n == OV_HOLE (a gap in the data) and other recoverable errors
			// are skipped
		}
		timer.finish(filled, BytesPerFrame(), rate);
		return filled;
	}

	// A seek makes the next ov_read rescan pages (50 ms for the first
	// request after it in Azahar, against 15 ms at most otherwise), and
	// MusicPlayer rewinds every track it starts: skip it while nothing has
	// been decoded yet.
	void Rewind() override
	{
		if (!opened || at_start)
			return;
		ov_raw_seek(&vf, 0);
		ended = false;
		at_start = true;
	}

	void Close() override
	{
		if (opened)
			ov_clear(&vf);
		opened = false;
		ended = false;
		source.reset();
	}

	AudioFormat GetAudioFormat() override { return AudioFormat::_16_bit; }
	bool IsStereo() override { return channels == 2; }
	int BytesPerFrame() override { return 2 * channels; }
	float Rate() override { return static_cast<float>(rate); }
	bool IsLittleEndian() override { return PlatformIsLittleEndian(); }
	int32 Frames() override { return opened ? static_cast<int32>(ov_pcm_total(&vf, -1)) : 0; }

private:
	std::unique_ptr<Source> source;
	OggVorbis_File vf;
	bool opened = false;
	bool ended = false;
	bool at_start = false;   // nothing decoded since the open or the last Rewind
	int channels = 0;
	long rate = 0;
};

// --- MP3 (libmpg123) ------------------------------------------------------------

mpg123_ssize_t mpg_read_cb(void* handle, void* buf, size_t count)
{
	return static_cast<mpg123_ssize_t>(static_cast<Source*>(handle)->read(buf, count));
}

off_t mpg_seek_cb(void* handle, off_t offset, int whence)
{
	return static_cast<off_t>(static_cast<Source*>(handle)->seek(offset, whence));
}

bool mpg123_ready()
{
	static const bool ok = [] {
		const int rc = mpg123_init();   // a no-op in mpg123 >= 1.27, required before
		if (rc != MPG123_OK)
			printf("[3ds] audio: mpg123_init failed: %s\n", mpg123_plain_strerror(rc));
		return rc == MPG123_OK;
	}();
	return ok;
}

class Mpg123Decoder final : public Decoder
{
public:
	~Mpg123Decoder() override { Close(); }

	bool Open(FileSpecifier& File) override
	{
		Close();
		auto src = Source::open(File);
		return src && OpenSource(File, std::move(src));
	}

	bool OpenSource(const FileSpecifier& file, std::unique_ptr<Source> src)
	{
		Close();
		if (!mpg123_ready())
			return false;
		int err = MPG123_OK;
		mh = mpg123_new(nullptr, &err);
		if (!mh)
			return false;
		source = std::move(src);
		mpg123_param(mh, MPG123_ADD_FLAGS, MPG123_QUIET, 0);
		// 16-bit output at the stream's own rate and channel count.
		mpg123_format_none(mh);
		const long* rates = nullptr;
		size_t nrates = 0;
		mpg123_rates(&rates, &nrates);
		for (size_t i = 0; i < nrates; ++i)
			mpg123_format(mh, rates[i], MPG123_MONO | MPG123_STEREO, MPG123_ENC_SIGNED_16);
		long r = 0;
		int ch = 0, enc = 0;
		if (mpg123_replace_reader_handle(mh, mpg_read_cb, mpg_seek_cb, nullptr) != MPG123_OK ||
		    mpg123_open_handle(mh, source.get()) != MPG123_OK ||
		    mpg123_getformat(mh, &r, &ch, &enc) != MPG123_OK ||
		    enc != MPG123_ENC_SIGNED_16 || (ch != 1 && ch != 2) || r <= 0)
		{
			Close();
			return false;
		}
		// Keep that format for the whole stream (one format per OpenAL buffer).
		mpg123_format_none(mh);
		mpg123_format(mh, r, ch, enc);
		rate = r;
		channels = ch;
		opened = true;
		at_start = true;
		note_open(file, "MP3 (mpg123)", rate, channels, source->in_memory());
		return true;
	}

	int32 Decode(uint8* buffer, int32 max_length) override
	{
		if (!opened)
			return 0;
		DecodeTimer timer;
		const int32 want = whole_frames(max_length, BytesPerFrame());
		int32 filled = 0;
		int stalls = 0;
		while (filled < want && !ended)
		{
			size_t done = 0;
			const int rc = mpg123_read(mh, buffer + filled, want - filled, &done);
			at_start = false;
			filled += static_cast<int32>(done);
			if (rc == MPG123_DONE)
				ended = true;
			else if (rc == MPG123_OK || rc == MPG123_NEW_FORMAT)
			{
				if (done == 0 && ++stalls > 8)
					ended = true;   // no progress: give up rather than spin
			}
			else
				ended = true;   // an error (damaged file): end the track
		}
		timer.finish(filled, BytesPerFrame(), rate);
		return filled;
	}

	// mpg123_seek(mh, 0, SEEK_SET) restarts with leftover decoder state: the
	// second pass of a stereo file differed from the first (debug-decoders).
	// Reopening the stream gives exactly the same samples every loop.
	void Rewind() override
	{
		if (!opened || at_start)
			return;
		mpg123_close(mh);
		source->seek(0, SEEK_SET);
		if (mpg123_open_handle(mh, source.get()) != MPG123_OK)
		{
			opened = false;   // Decode() returns 0: the track ends
			return;
		}
		ended = false;
		at_start = true;
	}

	void Close() override
	{
		if (mh)
		{
			mpg123_close(mh);
			mpg123_delete(mh);
			mh = nullptr;
		}
		opened = false;
		ended = false;
		scanned = false;
		source.reset();
	}

	AudioFormat GetAudioFormat() override { return AudioFormat::_16_bit; }
	bool IsStereo() override { return channels == 2; }
	int BytesPerFrame() override { return 2 * channels; }
	float Rate() override { return static_cast<float>(rate); }
	bool IsLittleEndian() override { return PlatformIsLittleEndian(); }

	// Exact only after a scan of the whole file (only replacement sounds,
	// decoded whole on the main thread, ask).
	int32 Frames() override
	{
		if (!opened)
			return 0;
		if (!scanned)
		{
			const off_t pos = mpg123_tell(mh);
			mpg123_scan(mh);
			mpg123_seek(mh, pos, SEEK_SET);
			scanned = true;
			at_start = false;   // after a seek; a Rewind() must reopen
		}
		const off_t n = mpg123_length(mh);
		return n > 0 ? static_cast<int32>(n) : 0;
	}

private:
	std::unique_ptr<Source> source;
	mpg123_handle* mh = nullptr;
	bool opened = false;
	bool ended = false;
	bool at_start = false;
	bool scanned = false;
	int channels = 0;
	long rate = 0;
};

// --- format detection -------------------------------------------------------------

bool has_extension(const std::string& name, const char* ext)
{
	const size_t n = strlen(ext);
	if (name.size() < n)
		return false;
	return SDL_strcasecmp(name.c_str() + name.size() - n, ext) == 0;
}

// MPEG audio: an ID3v2 tag, or a frame header (11 sync bits, a valid
// version, layer, bitrate and sample rate).
bool looks_like_mpeg_audio(const uint8* h, size_t n)
{
	if (n >= 3 && h[0] == 'I' && h[1] == 'D' && h[2] == '3')
		return true;
	if (n < 4 || h[0] != 0xFF || (h[1] & 0xE0) != 0xE0)
		return false;
	const int version = (h[1] >> 3) & 3, layer = (h[1] >> 1) & 3;
	const int bitrate = h[2] >> 4, samplerate = (h[2] >> 2) & 3;
	return version != 1 && layer != 0 && bitrate != 15 && samplerate != 3;
}

} // namespace

Decoder* open_decoder(FileSpecifier& file)
{
	auto src = Source::open(file);
	if (!src)
		return nullptr;
	uint8 head[4] = {};
	const size_t n = src->read(head, sizeof head);
	src->seek(0, SEEK_SET);

	if (n == 4 && memcmp(head, "OggS", 4) == 0)
	{
		auto d = std::make_unique<TremorDecoder>();
		if (d->OpenSource(file, std::move(src)))
			return d.release();
		return nullptr;   // Ogg, but not Vorbis (e.g. Opus): unsupported
	}

	const std::string name = file_name(file);
	if (looks_like_mpeg_audio(head, n) || has_extension(name, ".mp3") || has_extension(name, ".mp2"))
	{
		auto d = std::make_unique<Mpg123Decoder>();
		if (d->OpenSource(file, std::move(src)))
			return d.release();
	}
	return nullptr;
}

DecodeStats decode_stats(bool reset_max)
{
	DecodeStats st;
	{
		std::lock_guard<std::mutex> lock(s_info_mutex);
		st = s_info;
	}
	st.decode_ms = s_decode_us.load() / 1000.0;
	st.audio_s = s_decoded_us.load() / 1e6;
	st.max_call_ms = (reset_max ? s_max_call_us.exchange(0) : s_max_call_us.load()) / 1000.0f;
	return st;
}

} // namespace a1_3ds
