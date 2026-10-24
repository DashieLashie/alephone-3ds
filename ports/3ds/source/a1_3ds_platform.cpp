/*
	Aleph One for the Nintendo 3DS: paths, boot log, boot diagnostics and the
	early error screen. See ports/3ds/include/a1_3ds_platform.h and
	docs/3ds/STAGE4.md.

	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; either version 3 of the License, or
	(at your option) any later version.
*/
#include "a1_3ds_platform.h"

#include <3ds.h>
#include <SDL2/SDL.h>

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <signal.h>
#include <fcntl.h>
#include <malloc.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/iosupport.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <exception>
#include <stdexcept>
#include <fstream>
#include <vector>

// Main-thread stack. libctru's initSystem() carves it from the start of the
// heap (sp = heap start + __stacksize__) and its weak default is 32 KiB, which
// Aleph One overflows during initialize_application(): SP walks below
// 0x08000000 into unmapped memory and the next return jumps to 0. The used
// size is measured and logged at each boot milestone (log_memory()).
//
// Linear heap (STAGE5-PREREQS.md item 4). libctru's default gives it 32 MiB
// whenever more than 56 MiB is available; only the framebuffers (2100 KiB)
// and SDL's audio ring (12 KiB) use it, also during gameplay on hardware.
// 8 MiB keeps ~4x that and gives the rest to malloc, up to the kernel's heap
// area (below). A GPU renderer (citro3d) would need more: revisit then.
extern "C" {
u32 __stacksize__ = 1024 * 1024;
u32 __ctru_linear_heap_size = 8 * 1024 * 1024;
extern u32 __ctru_heap;   // heap base = bottom of the main stack
extern u32 __ctru_heap_size;
extern u32 __ctru_linear_heap;
extern char* fake_heap_start;
extern char* fake_heap_end;
}

// Replaces libctru's weak __system_allocateHeaps() (libctru 2.7.0
// allocateHeaps.c, same steps). With only __ctru_linear_heap_size set,
// libctru gives the heap everything else: ~108 MiB with New 3DS extended
// memory, past the end of the heap area (OS_HEAP_AREA_BEGIN..END, 96 MiB).
// The console's kernel refuses that and libctru stops in svcBreak before
// main() (Luma dump: PC svcBreak, LR __system_allocateHeaps); Azahar does
// not enforce the area. So the heap is capped at the area and whatever
// does not fit goes to the linear heap (PATCHES.md 25).
extern "C" void __system_allocateHeaps(void)
{
	Handle reslimit = 0;
	if (R_FAILED(svcGetResourceLimit(&reslimit, CUR_PROCESS_HANDLE)))
		svcBreak(USERBREAK_PANIC);
	ResourceLimitType type = RESLIMIT_COMMIT;
	s64 max_commit = 0, current_commit = 0;
	svcGetResourceLimitLimitValues(&max_commit, reslimit, &type, 1);
	svcGetResourceLimitCurrentValues(&current_commit, reslimit, &type, 1);
	svcCloseHandle(reslimit);
	const u32 remaining = static_cast<u32>(max_commit - current_commit) & ~0xFFFu;
	if (remaining <= __ctru_linear_heap_size)
		svcBreak(USERBREAK_PANIC);

	const u32 heap_area = OS_HEAP_AREA_END - OS_HEAP_AREA_BEGIN;
	__ctru_heap_size = std::min(remaining - __ctru_linear_heap_size, heap_area);
	__ctru_linear_heap_size = remaining - __ctru_heap_size;

	const MemPerm rw = static_cast<MemPerm>(MEMPERM_READ | MEMPERM_WRITE);
	if (R_FAILED(svcControlMemory(&__ctru_heap, OS_HEAP_AREA_BEGIN, 0, __ctru_heap_size,
	                              MEMOP_ALLOC, rw)))
		svcBreak(USERBREAK_PANIC);
	if (R_FAILED(svcControlMemory(&__ctru_linear_heap, 0, 0, __ctru_linear_heap_size,
	                              MEMOP_ALLOC_LINEAR, rw)))
		svcBreak(USERBREAK_PANIC);

	mappableInit(OS_MAP_AREA_BEGIN, OS_MAP_AREA_END);
	fake_heap_start = reinterpret_cast<char*>(__ctru_heap);
	fake_heap_end = fake_heap_start + __ctru_heap_size;
}

#ifndef A1_3DS_USER_DIR
#define A1_3DS_USER_DIR "sdmc:/3ds/alephone"
#endif
#ifndef A1_3DS_DEFAULT_SCENARIO
#define A1_3DS_DEFAULT_SCENARIO "Marathon2"
#endif

namespace a1_3ds {

// source/selftest.cpp (engine headers; kept apart from <3ds.h>)
void entropy_selftest();
void audio_load_selftest();
void decoder_selftest();
// source/audio_3ds.cpp
void audio_probe_log();
// source/input_3ds.cpp
void input_script_start();

namespace {

const char kScenarioFile[] = "scenario.txt";
const char kLogFile[] = "log.txt";
const char kPrevLogFile[] = "log.prev.txt";
// If this file exists in the user dir, the screens are saved as BMPs at the
// error screen and after the main menu appears (screen-*.bmp): a way to see
// what is displayed when nobody can look (emulator in CI, remote hardware).
const char kDebugScreensFlag[] = "debug-screens";
// If this file exists, entropy_selftest() runs once the main menu is up: it
// puts three images through the engine's WadImageCache and logs their file
// names and three a1fs::unique_path() names (STAGE5-PREREQS.md item 2).
const char kDebugEntropyFlag[] = "debug-entropy";
// If this file exists, the audio statistics (audio_3ds.cpp) are also
// logged every 5 s once the main menu is up, not only at boot milestones
// and at exit, and shown on the bottom screen (input_3ds.cpp).
const char kDebugAudioFlag[] = "debug-audio";
// If this file exists, decoder_selftest() runs once the main menu is up:
// every file in audio-test/ and the scenario's Music.ogg is decoded twice
// (with a rewind between) and checked (docs/3ds/STAGE6.md).
const char kDebugDecodersFlag[] = "debug-decoders";

const char kScenarioTemplate[] =
	"# Aleph One 3DS: scenario folder.\n"
	"#\n"
	"# The first line that is not empty and does not start with '#' names the\n"
	"# folder holding the scenario's Map, Shapes, Sounds, Images, ... files.\n"
	"# A relative name is looked up in " A1_3DS_USER_DIR "/.\n"
	"# Without such a line, " A1_3DS_USER_DIR "/" A1_3DS_DEFAULT_SCENARIO " is used.\n"
	"#\n"
	"# Examples:\n"
	"#   Marathon2\n"
	"#   Marathon Infinity\n"
	"#   sdmc:/games/Marathon\n";

u64 s_boot_tick = 0;
bool s_boot_begun = false;
u32 s_main_tid = 0;   // the main thread's kernel thread id, for error_screen's d6 line

// --- main stack high-water mark ------------------------------------------------

const u8 kStackPaint = 0xA5;
u8* s_stack_bottom = nullptr;
u8* s_stack_top = nullptr;

// Fills the unused part of the main stack (below the current SP) with a
// pattern; stack_used() later finds the deepest byte that was overwritten.
__attribute__((noinline)) void stack_paint()
{
	u8* bottom = reinterpret_cast<u8*>(__ctru_heap);
	u8* top = bottom + __stacksize__;
	volatile u8 marker = 0;
	u8* sp = const_cast<u8*>(&marker);
	if (sp <= bottom || sp > top)
		return;   // not the layout initSystem() sets up; do not touch
	s_stack_bottom = bottom;
	s_stack_top = top;
	for (u8* p = bottom; p < sp - 512; ++p)
		*p = kStackPaint;
}

size_t stack_used()
{
	const u8* p = s_stack_bottom;
	while (p < s_stack_top && *p == kStackPaint)
		++p;
	return s_stack_top - p;
}

// --- stdout/stderr tee: log file + 3dslink socket ---------------------------

int s_log_fd = -1;
int s_link_fd = -1;
u32* s_soc_buffer = nullptr;
const u32 kSocBufferSize = 0x100000;
devoptab_t s_tee_devoptab;
// Serialises tee_write(): threads other than the main one print too (the
// debug-audio and debug-screens threads). Unlocked, two writes raced on the
// SD file's offset: one overwrote the other's line and the offset still
// advanced twice, leaving never-written gaps (stale card data) in log.txt.
LightLock s_tee_lock;
// How long one write may wait for room in the socket's send buffer. A blocking
// send() on a stalled Wi-Fi link held s_tee_lock for ~100 s on the console, and
// every thread that logged waited behind it (NEXT-SPRINT.md backlog item 15).
// libctru has no SO_SNDTIMEO (sys/socket.h), so: MSG_DONTWAIT, and poll() for
// POLLOUT while the buffer is full.
const int kLinkStallMs = 500;

// Host went away or stalled: stop mirroring; the SD log goes on.
void drop_link(const char* why)
{
	close(s_link_fd);
	s_link_fd = -1;
	if (s_log_fd >= 0)
	{
		char line[96];
		const int n = snprintf(line, sizeof line, "[3ds] 3dslink mirror stopped (%s); the SD log goes on\n", why);
		write(s_log_fd, line, n);
	}
}

ssize_t tee_write(struct _reent*, void*, const char* ptr, size_t len)
{
	LightLock_Lock(&s_tee_lock);
	if (s_log_fd >= 0)
		write(s_log_fd, ptr, len);
	if (s_link_fd >= 0)
	{
		size_t sent = 0;
		int waited_ms = 0;
		while (sent < len)
		{
			ssize_t n = send(s_link_fd, ptr + sent, len - sent, MSG_DONTWAIT);
			if (n > 0)
			{
				sent += n;
				continue;
			}
			if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) && waited_ms < kLinkStallMs)
			{
				const int step = std::min(100, kLinkStallMs - waited_ms);
				pollfd p = {s_link_fd, POLLOUT, 0};
				poll(&p, 1, step);
				waited_ms += step;
				continue;
			}
			drop_link(n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) ? "stalled" : "host went away");
			break;
		}
	}
	LightLock_Unlock(&s_tee_lock);
	return len;
}

void install_tee()
{
	LightLock_Init(&s_tee_lock);
	memset(&s_tee_devoptab, 0, sizeof(s_tee_devoptab));
	s_tee_devoptab.name = "a1log";
	s_tee_devoptab.write_r = tee_write;
	devoptab_list[STD_OUT] = &s_tee_devoptab;
	devoptab_list[STD_ERR] = &s_tee_devoptab;
	// Line-buffer stdout so a crash loses at most one line; stderr stays
	// unbuffered.
	setvbuf(stdout, nullptr, _IOLBF, BUFSIZ);
	setvbuf(stderr, nullptr, _IONBF, 0);
}

void open_log(const std::string& dir)
{
	const std::string log = dir + "/" + kLogFile;
	const std::string prev = dir + "/" + kPrevLogFile;
	// Keep the previous run's log: the run after a crash would otherwise
	// overwrite the evidence. (sdmc rename() fails if the target exists.)
	unlink(prev.c_str());
	rename(log.c_str(), prev.c_str());
	s_log_fd = open(log.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
}

// Connect to the 3dslink host (3dslink -s) when the netloader started us.
// link3dsStdio() would dup2() the socket over stdout/stderr and the SD log
// would stay empty, so connect without redirecting and let tee_write() feed
// both.
void open_3dslink()
{
	if (__3dslink_host.s_addr == 0)
		return;
	s_soc_buffer = static_cast<u32*>(memalign(0x1000, kSocBufferSize));
	if (!s_soc_buffer)
		return;
	if (R_FAILED(socInit(s_soc_buffer, kSocBufferSize)))
	{
		free(s_soc_buffer);
		s_soc_buffer = nullptr;
		return;
	}
	s_link_fd = link3dsConnectToHost(false, false);
}

std::atomic<bool> s_exiting{false};
double ms_since_boot();

void close_outputs()
{
	s_exiting = true;
	if (s_boot_begun)
	{
		printf("[3ds] +%.1f ms: exit\n", ms_since_boot());
		audio_probe_log();
	}
	fflush(stdout);
	fflush(stderr);
	if (s_link_fd >= 0)
	{
		close(s_link_fd);
		s_link_fd = -1;
	}
	if (s_soc_buffer)
	{
		socExit();
		free(s_soc_buffer);
		s_soc_buffer = nullptr;
	}
	if (s_log_fd >= 0)
	{
		close(s_log_fd);
		s_log_fd = -1;
	}
}

// Upstream ends the process with a bare exit(0) when the system asks to close
// while a modal dialog is open (sdl_dialogs.cpp: SDL_QUIT), skipping
// shutdown_application(). Without SDL_Quit() the n3ds video driver never
// releases GSP: libctru frees the heap while its GSP event thread (whose stack
// is in that heap) is alive, and the thread faults (HOME > Close in
// Preferences: Luma dump, PC syncArbitrateAddress, LR gspEventThreadMain).
// Runs before close_outputs (atexit is last in, first out). A normal
// shutdown has already run SDL_Quit(), so SDL_WasInit(0) is 0 and this is a no-op.
void quit_sdl_at_exit()
{
	if (SDL_WasInit(0) == 0)
		return;
	printf("[3ds] +%.1f ms: exit without shutdown_application (a dialog's exit): SDL_Quit\n",
	       ms_since_boot());
	fflush(stdout);
	SDL_Quit();
}

// --- helpers ---------------------------------------------------------------

double ms_since_boot()
{
	return (svcGetSystemTick() - s_boot_tick) / CPU_TICKS_PER_MSEC;
}

bool is_dir(const std::string& path)
{
	struct stat st;
	return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool exists(const std::string& path)
{
	struct stat st;
	return stat(path.c_str(), &st) == 0;
}

// mkdir that treats an existing directory as success.
bool ensure_dir(const std::string& path)
{
	if (mkdir(path.c_str(), 0777) == 0 || errno == EEXIST)
		return is_dir(path);
	return false;
}

std::string trim(const std::string& s)
{
	const char* ws = " \t\r\n";
	size_t b = s.find_first_not_of(ws);
	if (b == std::string::npos)
		return "";
	size_t e = s.find_last_not_of(ws);
	return s.substr(b, e - b + 1);
}

// Absolute: has a device prefix ("sdmc:/x") or starts with '/' (sdmc's
// current directory is the card root). Everything else is relative to the
// user directory.
std::string resolve_scenario_path(std::string p)
{
	while (p.size() > 1 && p.back() == '/')
		p.pop_back();
	if (p.find(':') != std::string::npos)
		return p;
	if (!p.empty() && p[0] == '/')
		return "sdmc:" + p;
	return user_dir() + "/" + p;
}

void write_scenario_template()
{
	const std::string path = user_dir() + "/" + kScenarioFile;
	if (exists(path))
		return;
	if (FILE* f = fopen(path.c_str(), "w"))
	{
		fputs(kScenarioTemplate, f);
		fclose(f);
	}
}

unsigned long kib(unsigned long long bytes)
{
	return static_cast<unsigned long>(bytes / 1024);
}

void log_memory(const char* when)
{
	static bool linear_pool_ready = false;
	if (!linear_pool_ready)
	{
		// libctru sets the linear allocator's pool up on the first
		// allocation; until then linearSpaceFree() reports 0.
		linearFree(linearAlloc(1));
		linear_pool_ready = true;
	}
	struct mallinfo mi = mallinfo();
	// initSystem() takes the main stack from the start of the heap; malloc
	// gets the rest through sbrk(). newlib's arena is what it has taken so
	// far, so free = (not yet taken) + (free chunks inside the arena).
	const u32 heap = envGetHeapSize();
	const u32 malloc_heap = heap > __stacksize__ ? heap - __stacksize__ : 0;
	const unsigned long long heap_free =
		(malloc_heap > mi.arena ? malloc_heap - mi.arena : 0) + mi.fordblks;
	printf("[3ds] memory %s:\n"
	       "[3ds]   heap %lu KiB (%lu KiB main stack + %lu KiB malloc): malloc in use %lu KiB, "
	       "free %lu KiB (arena %lu KiB, %lu KiB free inside it)\n"
	       "[3ds]   linear heap %lu KiB: free %lu KiB\n",
	       when,
	       kib(heap), kib(__stacksize__), kib(malloc_heap), kib(mi.uordblks), kib(heap_free),
	       kib(mi.arena), kib(mi.fordblks),
	       kib(envGetLinearHeapSize()), kib(linearSpaceFree()));
	// libctru maps both heaps from the APPLICATION region at startup, so
	// little is normally left there. osGetMemRegionFree() is size - used in
	// u32 and wraps if a kernel (Azahar) reports more used than the size.
	const u32 region_size = osGetMemRegionSize(MEMREGION_APPLICATION);
	const u32 region_used = osGetMemRegionUsed(MEMREGION_APPLICATION);
	if (region_used <= region_size)
		printf("[3ds]   APPLICATION region %lu KiB: used %lu KiB, free %lu KiB (osGetMemRegionFree)\n",
		       kib(region_size), kib(region_used), kib(osGetMemRegionFree(MEMREGION_APPLICATION)));
	else
		printf("[3ds]   APPLICATION region %lu KiB: used %lu KiB (more than its size: emulator?)\n",
		       kib(region_size), kib(region_used));
	if (s_stack_bottom)
		printf("[3ds]   main stack %lu KiB (part of the heap): deepest use so far %lu KiB\n",
		       kib(__stacksize__), kib(stack_used()));
	audio_probe_log();
}

// --- screen capture (debug-screens) --------------------------------------------

bool debug_screens_enabled()
{
	static const bool enabled = exists(user_dir() + "/" + kDebugScreensFlag);
	return enabled;
}

void put_le(std::vector<unsigned char>& v, size_t at, u32 value, int bytes)
{
	for (int i = 0; i < bytes; ++i)
		v[at + i] = (value >> (8 * i)) & 0xff;
}

// Saves the framebuffer the display is showing (GSPGPU capture info, as
// screenshot tools do) as a 24-bit BMP. 3DS framebuffers are stored rotated:
// each memory row is one screen column, bottom pixel first.
void dump_screen(int screen, const char* name)
{
	GSPGPU_CaptureInfo info;
	if (R_FAILED(GSPGPU_ImportDisplayCaptureInfo(&info)))
		return;
	const GSPGPU_CaptureInfoEntry& e = info.screencapture[screen];
	const u8* fb = reinterpret_cast<const u8*>(e.framebuf0_vaddr);
	const int width = screen == 0 ? 400 : 320;   // screen columns = memory rows
	const int height = 240;
	const u32 format = e.format & 7;
	const int bpp = format == GSP_RGBA8_OES ? 4 : format == GSP_BGR8_OES ? 3 : 2;
	if (!fb || e.framebuf_widthbytesize < static_cast<u32>(height * bpp))
		return;

	const size_t row = width * 3;   // multiple of 4 for 400 and 320
	std::vector<unsigned char> bmp(54 + row * height, 0);
	bmp[0] = 'B';
	bmp[1] = 'M';
	put_le(bmp, 2, bmp.size(), 4);
	put_le(bmp, 10, 54, 4);
	put_le(bmp, 14, 40, 4);
	put_le(bmp, 18, width, 4);
	put_le(bmp, 22, height, 4);    // positive: bottom-up rows
	put_le(bmp, 26, 1, 2);
	put_le(bmp, 28, 24, 2);
	for (int x = 0; x < width; ++x)
	{
		const u8* col = fb + x * e.framebuf_widthbytesize;
		for (int i = 0; i < height; ++i)   // i = 0 is the bottom row
		{
			const u8* px = col + i * bpp;
			u8 r, g, b;
			if (format == GSP_RGBA8_OES)        { r = px[3]; g = px[2]; b = px[1]; }
			else if (format == GSP_BGR8_OES)    { r = px[2]; g = px[1]; b = px[0]; }
			else
			{
				const u16 v = px[0] | (px[1] << 8);
				if (format == GSP_RGB565_OES)       { r = (v >> 11) << 3; g = ((v >> 5) & 63) << 2; b = (v & 31) << 3; }
				else if (format == GSP_RGB5_A1_OES) { r = (v >> 11) << 3; g = ((v >> 6) & 31) << 3; b = ((v >> 1) & 31) << 3; }
				else                                { r = (v >> 12) << 4; g = ((v >> 8) & 15) << 4; b = ((v >> 4) & 15) << 4; }
			}
			unsigned char* out = &bmp[54 + i * row + x * 3];
			out[0] = b;
			out[1] = g;
			out[2] = r;
		}
	}
	const std::string path = user_dir() + "/" + name;
	if (FILE* f = fopen(path.c_str(), "wb"))
	{
		fwrite(bmp.data(), 1, bmp.size(), f);
		fclose(f);
		printf("[3ds] saved %s (%s screen, format %lu)\n", path.c_str(),
		       screen == 0 ? "top" : "bottom", static_cast<unsigned long>(format));
	}
}

// Captures the screens a few seconds after the main menu appears (it fades
// in), from a low-priority thread so the game loop does not wait for it.
void menu_capture_thread(void*)
{
	svcSleepThread(3000000000LL);
	dump_screen(0, "screen-menu-3s-top.bmp");
	svcSleepThread(5000000000LL);
	dump_screen(0, "screen-menu-8s-top.bmp");
	dump_screen(1, "screen-menu-8s-bottom.bmp");
}

void log_video()
{
	const char* driver = SDL_GetCurrentVideoDriver();
	printf("[3ds] video: SDL driver %s\n", driver ? driver : "(none)");
	SDL_Window* window = SDL_GetKeyboardFocus();
	if (!window)
	{
		printf("[3ds] video: no focused window\n");
		return;
	}
	int ww = 0, wh = 0;
	SDL_GetWindowSize(window, &ww, &wh);
	printf("[3ds] video: window %dx%d, %s, flags 0x%08lx\n", ww, wh,
	       SDL_GetPixelFormatName(SDL_GetWindowPixelFormat(window)),
	       static_cast<unsigned long>(SDL_GetWindowFlags(window)));
	if (SDL_Renderer* renderer = SDL_GetRenderer(window))
	{
		SDL_RendererInfo info;
		int ow = 0, oh = 0, lw = 0, lh = 0;
		SDL_GetRendererInfo(renderer, &info);
		SDL_GetRendererOutputSize(renderer, &ow, &oh);
		SDL_RenderGetLogicalSize(renderer, &lw, &lh);
		printf("[3ds] video: renderer \"%s\", output %dx%d, logical %dx%d\n",
		       info.name, ow, oh, lw, lh);
	}
}

// Greedy word wrap to `width` columns. Lines are kept one column short of the
// console width: a full-width line followed by '\n' would print a blank line.
std::vector<std::string> wrap(const std::string& text, size_t width)
{
	std::vector<std::string> lines;
	size_t start = 0;
	while (start <= text.size())
	{
		size_t nl = text.find('\n', start);
		std::string para = text.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
		// keep a paragraph's indentation (used for paths)
		size_t i = std::min(para.find_first_not_of(' '), para.size());
		const std::string indent(std::min(i, width / 2), ' ');
		std::string line;   // without the indent
		const size_t room = width - indent.size();
		while (i < para.size())
		{
			size_t sp = para.find(' ', i);
			std::string word = para.substr(i, sp == std::string::npos ? std::string::npos : sp - i);
			i = (sp == std::string::npos) ? para.size() : sp + 1;
			while (word.size() > room)
			{
				if (!line.empty())
				{
					lines.push_back(indent + line);
					line.clear();
				}
				lines.push_back(indent + word.substr(0, room));
				word.erase(0, room);
			}
			if (line.empty())
				line = word;
			else if (line.size() + 1 + word.size() <= room)
				line += " " + word;
			else
			{
				lines.push_back(indent + line);
				line = word;
			}
		}
		lines.push_back(line.empty() ? line : indent + line);
		if (nl == std::string::npos)
			break;
		start = nl + 1;
	}
	return lines;
}

// std::terminate() and abort() end the process without flushing stdio or
// telling anyone: the 3DS just returns to the Homebrew Launcher (an emulator
// to its game list). Say why, on screen and in the log, first.
[[noreturn]] void die_with_message(const std::string& what)
{
	fflush(stdout);
	error_screen("Aleph One stopped", what);
	close_outputs();
	_Exit(1);
}

void on_terminate()
{
	// Rethrowing to read the message can itself end in terminate() (e.g. if
	// unwinding is broken); never recurse.
	static bool entered = false;
	if (entered)
		die_with_message("std::terminate() was called again while reporting an uncaught "
		                 "exception: C++ exception unwinding is failing.");
	entered = true;
	fprintf(stderr, "[3ds] std::terminate() called\n");

	std::string what = "std::terminate() was called";
	if (std::exception_ptr ep = std::current_exception())
	{
		try
		{
			std::rethrow_exception(ep);
		}
		catch (const std::exception& e)
		{
			what += " with an uncaught exception:\n" + std::string(e.what());
		}
		catch (...)
		{
			what += " with an uncaught non-standard exception";
		}
	}
	else
		what += " (no active exception; e.g. a joinable std::thread was destroyed)";
	die_with_message(what);
}

void on_abort(int)
{
	die_with_message("abort() was called (failed assertion, halt() or vhalt()); "
	                 "the lines before this in the log may say why.");
}

// The wrapped lines, each ended by '\n' (error_screen draws them with
// console_write_on()).
std::string wrapped_text(const std::string& text, size_t width)
{
	std::string out;
	for (const auto& line : wrap(text, width))
		out += line + "\n";
	return out;
}

} // namespace

// Used by input_3ds.cpp (script timing and the "screens" command).
double elapsed_ms()
{
	return ms_since_boot();
}

void save_screens(const std::string& tag)
{
	dump_screen(0, ("screen-" + tag + "-top.bmp").c_str());
	dump_screen(1, ("screen-" + tag + "-bottom.bmp").c_str());
}

// --- entropy -------------------------------------------------------------------

namespace {

// splitmix64's output function: a bijection on 64 bits that spreads every
// input bit over the whole output.
u64 mix64(u64 z)
{
	z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
	z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
	return z ^ (z >> 31);
}

struct EntropySource
{
	bool ps;
	Result rc;
};

const EntropySource& entropy_source()
{
	static const EntropySource src = [] {
		const Result rc = psInit();
		return EntropySource{R_SUCCEEDED(rc), rc};
	}();
	return src;
}

std::atomic<u32> s_entropy_counter{0};
std::atomic<bool> s_ps_failure_logged{false};

// Distinct within a run (the counter), and across runs through osGetTime()
// (milliseconds) and the tick at the moment of the call.
void fallback_bytes(void* out, size_t len)
{
	u8* p = static_cast<u8*>(out);
	while (len)
	{
		const u64 n = s_entropy_counter.fetch_add(1, std::memory_order_relaxed);
		const u64 v = mix64(svcGetSystemTick() ^ mix64(osGetTime() + n * 0x9E3779B97F4A7C15ull));
		const size_t k = std::min(len, sizeof v);
		memcpy(p, &v, k);
		p += k;
		len -= k;
	}
}

} // namespace

void random_bytes(void* out, size_t len)
{
	if (entropy_source().ps)
	{
		const Result rc = PS_GenerateRandomBytes(out, len);
		if (R_SUCCEEDED(rc))
			return;
		if (!s_ps_failure_logged.exchange(true))
			printf("[3ds] entropy: PS_GenerateRandomBytes failed (0x%08lX); using the tick/clock fallback\n",
			       static_cast<unsigned long>(rc));
	}
	fallback_bytes(out, len);
}

// --- player name -------------------------------------------------------------

namespace {

// Config block 0x000A0000 (cited in libctru's uds.h:30, udsNodeInfo): the user name,
// u16 username[10] (UTF-16, NUL-terminated when shorter), then flags; 0x1C bytes
// (3dbrew "Config Savegame"). Read with CFGU_GetConfigInfoBlk2 (cfgu.h:112).
constexpr u32 kUserBlockId = 0x000A0000;
constexpr u32 kUserBlockSize = 0x1C;
constexpr size_t kUserNameUnits = 10;

// The name units (up to the first NUL, at most kUserNameUnits) as UTF-8; empty
// with `why` set if there is no usable name.
std::string user_name_to_utf8(const u16* block_units, size_t count, std::string& why)
{
	u16 name[kUserNameUnits + 1] = {};
	size_t n = 0;
	while (n < count && n < kUserNameUnits && block_units[n])
	{
		name[n] = block_units[n];
		++n;
	}
	if (n == 0)
	{
		why = "empty";
		return "";
	}
	for (size_t i = 0; i < n;)   // valid UTF-16, no control characters
	{
		uint32_t cp = 0;
		const ssize_t used = decode_utf16(&cp, name + i);   // util/utf.h
		if (used <= 0 || i + used > n)
		{
			why = "not valid UTF-16";
			return "";
		}
		if (cp < 0x20 || (cp >= 0x7F && cp < 0xA0))
		{
			why = "contains a control character";
			return "";
		}
		i += used;
	}
	uint8_t utf8[4 * kUserNameUnits + 1] = {};   // at most 3 bytes per unit, 4 per surrogate pair
	const ssize_t len = utf16_to_utf8(utf8, name, sizeof utf8 - 1);   // util/utf.h:106
	if (len <= 0)
	{
		why = "not valid UTF-16";
		return "";
	}
	std::string s(reinterpret_cast<const char*>(utf8), static_cast<size_t>(len));
	s = trim(s);
	if (s.empty())
		why = "blank";
	return s;
}

// debug-player-name: hex code units instead of the block. False if the file is absent.
bool fake_user_block(std::vector<u16>& units)
{
	std::ifstream in(user_dir() + "/debug-player-name");
	if (!in)
		return false;
	std::string word;
	while (in >> word && units.size() < kUserBlockSize / 2)
		units.push_back(static_cast<u16>(strtoul(word.c_str(), nullptr, 16)));
	return true;
}

} // namespace

const std::string& console_user_name()
{
	static const std::string name = [] {
		std::vector<u16> units;
		const char* from = "debug-player-name";
		if (!fake_user_block(units))
		{
			from = "the console profile";
			// cfgu is reference counted (libctru): a session of our own, closed again here.
			const Result init = cfguInit();
			if (R_FAILED(init))
			{
				printf("[3ds] player name: cfguInit failed (0x%08lX); keeping the default\n",
				       static_cast<unsigned long>(init));
				return std::string();
			}
			units.assign(kUserBlockSize / 2, 0);
			const Result rc = CFGU_GetConfigInfoBlk2(kUserBlockSize, kUserBlockId, units.data());
			cfguExit();
			if (R_FAILED(rc))
			{
				printf("[3ds] player name: CFGU_GetConfigInfoBlk2(0x%lX, 0x%08lX) failed (0x%08lX); "
				       "keeping the default\n", static_cast<unsigned long>(kUserBlockSize),
				       static_cast<unsigned long>(kUserBlockId), static_cast<unsigned long>(rc));
				return std::string();
			}
		}
		std::string why;
		std::string s = user_name_to_utf8(units.data(), units.size(), why);
		if (s.empty())
			printf("[3ds] player name from %s: %s; keeping the default\n", from, why.c_str());
		else
			printf("[3ds] player name from %s: \"%s\"\n", from, s.c_str());
		return s;
	}();
	return name;
}

// --- paths -------------------------------------------------------------------

const std::string& user_dir()
{
	static const std::string dir = A1_3DS_USER_DIR;
	return dir;
}

namespace {
struct ScenarioChoice
{
	std::string path;
	std::string source;
};

const ScenarioChoice& scenario_choice()
{
	static const ScenarioChoice choice = [] {
		ScenarioChoice c{user_dir() + "/" + A1_3DS_DEFAULT_SCENARIO, "default"};
		std::ifstream in(user_dir() + "/" + kScenarioFile);
		std::string line;
		int lineno = 0;
		while (std::getline(in, line))
		{
			++lineno;
			line = trim(line);
			if (line.empty() || line[0] == '#')
				continue;
			c.path = resolve_scenario_path(line);
			c.source = std::string(kScenarioFile) + " line " + std::to_string(lineno);
			break;
		}
		return c;
	}();
	return choice;
}
} // namespace

const std::string& scenario_dir()
{
	return scenario_choice().path;
}

const std::string& scenario_dir_source()
{
	return scenario_choice().source;
}

const std::string& resource_dir()
{
	static const std::string dir = [] {
		std::string d = "romfs:/";
		if (char* base = SDL_GetBasePath())
		{
			d = base;
			SDL_free(base);
		}
		return d;
	}();
	return dir;
}

// --- boot ------------------------------------------------------------------

bool boot_begin(int& argc, char**& argv)
{
	const bool no_argv = argc < 1 || !argv || !argv[0];
	if (no_argv)
	{
		static char program_name[] = "alephone";
		static char* default_argv[] = {program_name, nullptr};
		argc = 1;
		argv = default_argv;
	}

	s_boot_tick = svcGetSystemTick();
	s_boot_begun = true;
	svcGetThreadId(&s_main_tid, CUR_THREAD_HANDLE);
	stack_paint();

	// sdmc:/3ds normally exists (the Homebrew Launcher lives there), but a
	// fresh card or an emulator may not have it.
	ensure_dir("sdmc:/3ds");
	const bool have_user_dir = ensure_dir(user_dir());
	if (have_user_dir)
		open_log(user_dir());
	open_3dslink();
	install_tee();
	atexit(close_outputs);
	atexit(quit_sdl_at_exit);
	std::set_terminate(on_terminate);
	signal(SIGABRT, on_abort);
	try
	{
		// The engine relies on exceptions (MML, prefs, files); if unwinding
		// is broken this ends in on_terminate() with a clear message.
		throw std::runtime_error("self-test");
	}
	catch (const std::exception&)
	{
		printf("[3ds] C++ exceptions: OK\n");
	}
	if (entropy_source().ps)
		printf("[3ds] entropy: ps:ps (PS_GenerateRandomBytes)\n");
	else
		printf("[3ds] entropy: psInit failed (0x%08lX); using the tick/clock fallback\n",
		       static_cast<unsigned long>(entropy_source().rc));

	bool is_new3ds = false;
	APT_CheckNew3DS(&is_new3ds);
	printf("[3ds] Aleph One 3DS boot (%s 3DS)\n", is_new3ds ? "New" : "Old");
	if (s_link_fd >= 0)
		printf("[3ds] launched via 3dslink from %s; output mirrored to the host\n",
		       inet_ntoa(__3dslink_host));
	else if (__3dslink_host.s_addr != 0)
		printf("[3ds] launched via 3dslink from %s, but connecting back failed "
		       "(start 3dslink with -s to receive output)\n", inet_ntoa(__3dslink_host));
	if (no_argv)
		printf("[3ds] started without argv (emulator?); using argv[0] = \"%s\"\n", argv[0]);
	else
		for (int i = 0; i < argc; ++i)
			printf("[3ds] argv[%d] = \"%s\"\n", i, argv[i] ? argv[i] : "(null)");
	printf("[3ds] user dir: %s%s\n", user_dir().c_str(),
	       s_log_fd >= 0 ? "" : " (log.txt could not be opened)");
	printf("[3ds] resources: %s\n", resource_dir().c_str());
	printf("[3ds] scenario: %s (%s)\n", scenario_dir().c_str(), scenario_dir_source().c_str());
	log_memory("at boot");

	if (!have_user_dir)
	{
		error_screen("Cannot create the data folder",
		             "Could not create\n  " + user_dir() + "\n"
		             "Check that an SD card is inserted and not write-protected.");
		return false;
	}
	write_scenario_template();

	const std::string theme = resource_dir() + "Plugins/Default_Theme/Plugin.xml";
	if (!exists(theme))
		printf("[3ds] WARNING: %s not found; the romfs is missing or incomplete\n", theme.c_str());

	const std::string& scen = scenario_dir();
	std::string problem;
	if (!exists(scen))
		problem = "Scenario folder not found:";
	else if (!is_dir(scen))
		problem = "This is a file, not a folder:";
	else
	{
		DIR* d = opendir(scen.c_str());
		bool empty = true;
		if (d)
		{
			while (struct dirent* e = readdir(d))
			{
				if (strcmp(e->d_name, ".") && strcmp(e->d_name, ".."))
				{
					empty = false;
					break;
				}
			}
			closedir(d);
		}
		if (empty)
			problem = "Scenario folder is empty:";
	}
	if (!problem.empty())
	{
		error_screen("Aleph One cannot start",
		             problem + "\n  " + scen + "\n(chosen by: " + scenario_dir_source() + ")\n\n"
		             "Copy a scenario's files (Map, Shapes, Sounds, Images, ...), for example "
		             "Marathon 2's, into that folder, or write the path of another scenario "
		             "folder in\n  " + user_dir() + "/" + kScenarioFile + "\n\n"
		             "Log: " + user_dir() + "/" + kLogFile);
		return false;
	}

	boot_mark("platform ready");
	return true;
}

namespace {
double s_last_mark_ms = 0;
}

void boot_mark(const char* what)
{
	if (!s_boot_begun)
		return;
	s_last_mark_ms = ms_since_boot();
	printf("[3ds] +%.1f ms: %s\n", s_last_mark_ms, what);
	log_memory(what);
}

void time_mark(const char* what)
{
	if (!s_boot_begun)
		return;
	const double now = ms_since_boot();
	printf("[3ds] +%.1f ms: %s (+%.1f ms)\n", now, what, now - s_last_mark_ms);
	s_last_mark_ms = now;
}

namespace {
void audio_report_thread(void*)
{
	svcSleepThread(1000000000LL);   // the first report covers the test sounds
	for (;;)
	{
		if (s_exiting)
			return;
		printf("[3ds] +%.1f ms: audio report\n", ms_since_boot());
		audio_probe_log();
		svcSleepThread(5000000000LL);
		if (s_exiting)
			return;
	}
}
} // namespace

void main_menu_displayed()
{
	static bool reported = false;
	if (reported || !s_boot_begun)
		return;
	reported = true;
	printf("[3ds] +%.1f ms: TIME TO MAIN MENU (from main(); includes the intro screens)\n",
	       ms_since_boot());
	log_memory("at main menu");
	log_video();
	if (debug_screens_enabled())
		threadCreate(menu_capture_thread, nullptr, 32 * 1024, 0x3f, -2, true);
	if (exists(user_dir() + "/" + kDebugEntropyFlag))
		entropy_selftest();
	if (exists(user_dir() + "/" + kDebugDecodersFlag))
		decoder_selftest();
	if (exists(user_dir() + "/" + kDebugAudioFlag))
	{
		audio_load_selftest();
		threadCreate(audio_report_thread, nullptr, 16 * 1024, 0x3f, -2, true);
	}
	input_script_start();
}

// --- error screen ------------------------------------------------------------

// input_3ds.cpp (docs/3ds/CITRO3D_DESIGN.md 5.6.2 a). The error screen is drawn
// through the console device, never through stdout: after boot stdout is the log
// tee, and consoleInit() reroutes stdout only on its first call.
PrintConsole* console_init_keep_log(gfxScreen_t screen, PrintConsole* console);
void console_write_on(PrintConsole* console, const char* text);
const devoptab_t* console_stdout_device();
void console_error_screen_begin();

namespace {
// Set by error_screen_here() once the text is drawn: "a screen was drawn"
// (5.6.3: a later fatal path shows exactly one screen).
std::atomic<bool> s_error_shown{false};

// The screen itself, on the calling thread. Piece 2 (5.6.3) puts the hand-off
// to the main thread in error_screen() and calls this for the fallback.
void error_screen_here(const std::string& title, const std::string& body);
}

void error_screen(const std::string& title, const std::string& body)
{
	error_screen_here(title, body);
}

namespace {
void error_screen_here(const std::string& title, const std::string& body)
{
	fprintf(stderr, "[3ds] ERROR: %s\n%s\n", title.c_str(), body.c_str());
	fflush(stdout);
	fflush(stderr);

	// Merge note (piece 4, stage10-c3d): there, the `#ifdef A1_RENDER_C3D` release
	// call goes right here (a1_c3d_release_top()). If it returns false (citro3d
	// still up) this function logs only: no console_init_keep_log, no wait loop,
	// it returns (CITRO3D_DESIGN.md 5.6.2 a, 5.6.3 step 3).

	// The port's own bottom-screen console output stops here: a HOME round trip
	// during the loop below must not redraw the legend over the error.
	console_error_screen_begin();

	// Before SDL video is up nobody owns the screens yet. After it, SDL owns
	// the top screen and the bottom one is unused. Keep the game's last frame
	// on top if a window has shown one; until then (SDL_Init to the main
	// window, e.g. a missing Map) the top would stay black, so use it too.
	// Every caller exits afterwards, so taking the screen over is harmless.
	const bool own_gfx = !SDL_WasInit(SDL_INIT_VIDEO);
	const bool own_top = own_gfx || !SDL_GetKeyboardFocus();
	if (own_gfx)
		gfxInitDefault();

	// The consoles are initialised first, then d6 is logged, then they are drawn
	// on. The first consoleInit() of the process (an error before input_init())
	// makes libctru's console device, which console_init_keep_log() captures;
	// stdout is not moved.
	static PrintConsole top, bottom;
	if (own_top)
		console_init_keep_log(GFX_TOP, &top);
	console_init_keep_log(GFX_BOTTOM, &bottom);

	// d6: where the error screen runs, and that stdout is not the console.
	u32 tid = 0;
	svcGetThreadId(&tid, CUR_THREAD_HANDLE);
	const devoptab_t* out = devoptab_list[STD_OUT];
	printf("[3ds] error screen d6: thread %s (id %lu); stdout device after consoleInit: %s\n",
	       tid == s_main_tid ? "main" : "other", static_cast<unsigned long>(tid),
	       out == console_stdout_device() ? "console" : out == &s_tee_devoptab ? "log" : "other");
	fflush(stdout);

	if (own_top)
	{
		console_write_on(&top, ("\x1b[14;2H\x1b[31;1m" + title + "\x1b[0m").c_str());
		console_write_on(&top, "\x1b[16;2HSee the bottom screen.");
	}
	console_write_on(&bottom, ("\x1b[31;1m" + wrapped_text(title, 39) + "\x1b[0m\n" +
	                           wrapped_text(body, 39) +
	                           "\n\x1b[32mPress START or A to exit.\x1b[0m").c_str());
	s_error_shown = true;

	// The error screen holds its own HID reference (libctru's is counted):
	// hidScanInput() stays valid whatever SDL_Quit released before this
	// (docs/3ds/CITRO3D_DESIGN.md 5.6.2 b, NEXT-SPRINT.md item 14).
	const Result hid_rc = hidInit();
	if (R_FAILED(hid_rc))
		printf("[3ds] error screen: hidInit failed (0x%08lX)\n", static_cast<unsigned long>(hid_rc));

	// Without a HID reference hidScanInput() may read unmapped shared memory (the
	// old dump 3 fault): then no input is polled and the screen stays up 10 s.
	const bool have_hid = R_SUCCEEDED(hid_rc);
	const int kNoHidFrames = 600;   // 10 s of VBlanks
	int frames = 0;
	while (aptMainLoop())
	{
		if (have_hid)
		{
			hidScanInput();
			if (hidKeysDown() & (KEY_START | KEY_A))
				break;
		}
		else if (frames >= kNoHidFrames)
			break;
		gfxFlushBuffers();
		if (own_gfx)
			gfxSwapBuffers();
		else
		{
			gfxScreenSwapBuffers(GFX_BOTTOM, false);
			if (own_top)
				gfxScreenSwapBuffers(GFX_TOP, false);
		}
		gspWaitForVBlank();
		if (++frames == 10 && debug_screens_enabled())
		{
			dump_screen(0, "screen-error-top.bmp");   // logs "saved" lines
			dump_screen(1, "screen-error-bottom.bmp");
		}
	}

	if (have_hid)
		hidExit();
	if (own_gfx)
		gfxExit();
}
}

} // namespace a1_3ds
