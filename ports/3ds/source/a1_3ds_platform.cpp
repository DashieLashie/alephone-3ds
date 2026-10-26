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
#ifdef A1_RENDER_C3D
#include "a1_c3d_port.h"
#endif

#include <3ds.h>
#include <SDL2/SDL.h>

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
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

// libctru's exit path (system/ctru_init.c, services/apt.c): what __libctru_exit() calls
// last (the loader's return address, or aptExitProcess = APT_CloseApplication after
// aptExit()'s close branch). Not in a header. Read by d7 and the fallback's exit
// (docs/3ds/CITRO3D_DESIGN.md 5.6.3 step 3a).
extern "C" void (*__system_retAddr)(void);

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

// --- Fatal errors on any thread (docs/3ds/CITRO3D_DESIGN.md 5.6.3) ------------------
//
// A fatal error on a thread other than main is handed to the main thread: the
// requester copies the message into static buffers, publishes it in s_fatal_from
// (compare-exchange: the first request wins) and blocks; main serves it at its next
// top-level service point (fatal_service_point(): present_screen(), present_bottom(),
// __wrap_hidScanInput on main), draws the error screen and ends the process
// (fatal_exit()). If main does not acknowledge in 5 s the requester draws the
// screen itself (the fallback) and ends the process through aptExit() and
// APT_CloseApplication, without __appExit's teardown and without freeing the heap
// (step 3a, behind a 5 s watchdog; the bare svcExitProcess() it replaced made the
// system restart the console).
//
// Invariant: no fatal error may be raised from inside tee_write() or newlib's
// malloc. The raw log writes and the static buffers below assume that the
// requesting thread holds neither lock. No entry point is reachable from either
// today; keep it so.
std::atomic<u32> s_fatal_from{0};         // kernel thread id of the request, 0 = none
std::atomic<bool> s_fatal_ready{false};   // the title/body buffers are filled
std::atomic<bool> s_fatal_acked{false};   // main took the request, or the requester took it back
std::atomic<bool> s_audio_parked{false};  // the SDL audio thread failed (or lost): see fatal_exit()
std::atomic<bool> s_fatal_active{false};  // main is in a fatal path of its own, or serving one
std::atomic<u32> s_audio_tid{0};          // the SDL audio thread (first timed_callback call)
std::atomic<bool> s_fatal_fallback{false};   // the requester runs the fallback (step 3a): apt_hook stays quiet
std::atomic<const char*> s_exit_step{"fallback start"};   // the fallback's current step, named by the watchdog
std::atomic<int> s_hook_depth{0};         // inside apt_hook / the SDL event filter: no serving
Handle s_fatal_never = 0;                 // an event that is never signalled
void* s_main_tls = nullptr;               // the main thread's TLS block
std::atomic<bool> s_fatal_main_took{false};  // main acknowledged the request (it did not get reclaimed)
char s_fatal_title[128];
char s_fatal_body[1024];
// vhalt() on main: the message, kept for __wrap_abort (upstream's vhalt() ends in abort(),
// and its own system_alert_user() only logs on the 3DS, so no screen is shown before that).
std::atomic<bool> s_vhalt_pending{false};
char s_vhalt_message[1024];

bool on_main_thread()
{
	return !s_main_tls || getThreadLocalStorage() == s_main_tls;
}

// A log line straight to the log fd: no stdio, no heap, and no waiting for the tee
// lock (the thread that holds it may be the main thread we are waiting for). The
// lock is only tried, for up to 200 ms: two writes at once on the SD file race on
// its offset (the reason for s_tee_lock) and one line is lost; if the lock stays
// taken the line is written without it.
void log_raw(const char* text)
{
	if (s_log_fd < 0)
		return;
	bool locked = false;
	for (int i = 0; i < 200 && !(locked = LightLock_TryLock(&s_tee_lock) == 0); ++i)
		svcSleepThread(1000000);
	write(s_log_fd, text, strlen(text));
	if (locked)
		LightLock_Unlock(&s_tee_lock);
}

// A log line from the error screen: through the tee, or raw in the fallback.
void log_line(bool raw, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
void log_line(bool raw, const char* fmt, ...)
{
	char buf[384];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof buf, fmt, ap);
	va_end(ap);
	if (raw)
		log_raw(buf);
	else
	{
		fputs(buf, stdout);
		fflush(stdout);
	}
}

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
	if (s_audio_parked)
		return;   // SDL_Quit would wait for the parked audio thread (5.6.3 step 6)
	printf("[3ds] +%.1f ms: exit without shutdown_application (a dialog's exit): SDL_Quit\n",
	       ms_since_boot());
	fflush(stdout);
	SDL_Quit();
}

// The one end of every fatal path on the main thread (5.6.3 step 5). The variant
// is picked now, from s_audio_parked, not from who made the request: a parked SDL
// audio thread sits in the kernel inside SDL's mixer lock, so SDL_Quit() (it locks
// the device and joins that thread) and any OpenAL call would deadlock here.
[[noreturn]] void fatal_exit()
{
	if (s_audio_parked)
	{
		printf("[3ds] +%.1f ms: fatal exit, audio thread parked: gfxExit, no SDL_Quit\n", ms_since_boot());
		fflush(stdout);
		if (SDL_WasInit(SDL_INIT_VIDEO))
			gfxExit();   // SDL's reference (SDL_Quit would release it)
	}
	else if (SDL_WasInit(0))
	{
		// Stops the SDL audio thread and reaches gfxExit through SDL's video
		// driver, like the console-proven dialog exit (STAGE5.md).
		SDL_Quit();
	}
	close_outputs();
	_Exit(1);   // __appExit runs on main; no atexit handlers, no static destructors
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
void dump_screen(int screen, const char* name, bool raw_log = false)
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
		log_line(raw_log, "[3ds] saved %s (%s screen, format %lu)\n", path.c_str(),
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
	error_screen("Aleph One stopped", what);   // off the main thread: never returns
	fatal_exit();
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
	s_main_tls = getThreadLocalStorage();
	if (R_FAILED(svcCreateEvent(&s_fatal_never, RESET_ONESHOT)))
		s_fatal_never = 0;
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
	// d7 (docs/3ds/CITRO3D_DESIGN.md 5.6.3 step 3a): which aptExit() branch the launcher's run
	// flags select, and what libctru will call last.
	{
		const u32 flags = envGetSystemRunFlags();
		printf("[3ds] env: homebrew %d, run flags 0x%lX (aptworkaround %d, aptreinit %d, aptchainload %d), "
		       "return address %s\n",
		       envIsHomebrew() ? 1 : 0, static_cast<unsigned long>(flags),
		       (flags & RUNFLAG_APTWORKAROUND) ? 1 : 0, (flags & RUNFLAG_APTREINIT) ? 1 : 0,
		       (flags & RUNFLAG_APTCHAINLOAD) ? 1 : 0, __system_retAddr ? "set" : "null");
	}
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

// The screen itself, on the calling thread: the body of error_screen() without the
// hand-off to the main thread. raw_log is the fallback's (5.6.3 step 3): every log
// line goes straight to the log fd, never through stdio or the tee lock.
void error_screen_here(const std::string& title, const std::string& body, bool raw_log);
}

// Shows the error on the bottom screen. Off the main thread this hands the whole
// fatal path to the main thread and never returns (5.6.3): the callers all end the
// process afterwards anyway. On the main thread it draws, waits for A/START and
// returns; s_fatal_active makes the service points acknowledge a request that
// arrives meanwhile without nesting a second screen.
void error_screen(const std::string& title, const std::string& body)
{
	if (!on_main_thread())
		fatal_from_other_thread(title.c_str(), body.c_str());
	s_fatal_active = true;
	error_screen_here(title, body, false);
	// With the SDL audio thread parked the callers' exit(1) would run the atexit handlers
	// (SoundManager::Shutdown -> SDL_CloseAudio) and block on the parked thread's mixer lock.
	if (s_audio_parked)
		fatal_exit();
}

namespace {
void error_screen_here(const std::string& title, const std::string& body, bool raw_log)
{
	if (raw_log)
	{
		log_raw("[3ds] ERROR: ");
		log_raw(title.c_str());
		log_raw("\n");
		log_raw(body.c_str());
		log_raw("\n");
	}
	else
	{
		fprintf(stderr, "[3ds] ERROR: %s\n%s\n", title.c_str(), body.c_str());
		fflush(stdout);
		fflush(stderr);
	}

	// The port's own bottom-screen console output stops here: a HOME round trip
	// during the loop below must not redraw the legend over the error.
	console_error_screen_begin();

#ifdef A1_RENDER_C3D
	// citro3d owns the top screen once the GPU present is up: give it back before the
	// console or SDL draw there (docs/3ds/CITRO3D_DESIGN.md, 1.3, 5.1). On main (every
	// error screen since 5.6.3) it releases directly. Off main this is 5.6.3's fallback
	// (raw_log), with main blocked, perhaps on the tee lock or between C3D_FrameBegin and
	// C3D_FrameEnd: a1_c3d_release_top()'s hand-off would printf and wait for nothing, so
	// only ask whether citro3d is up. If it is: no consoleInit (it swaps the top screen
	// too), nothing drawn, no wait (5.6.2 a, 5.6.3 step 3); the frozen frame is the design,
	// and this thread must never call citro3d.
	if (raw_log ? a1_c3d_owns_top() : !a1_c3d_release_top())
	{
		log_line(raw_log, "[3ds] error screen: citro3d still up (fallback off main): logged only\n");
		return;
	}
#endif

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
	log_line(raw_log, "[3ds] error screen d6: thread %s (id %lu); stdout device after consoleInit: %s\n",
	         tid == s_main_tid ? "main" : "other", static_cast<unsigned long>(tid),
	         out == console_stdout_device() ? "console" : out == &s_tee_devoptab ? "log" : "other");

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
		log_line(raw_log, "[3ds] error screen: hidInit failed (0x%08lX)\n", static_cast<unsigned long>(hid_rc));

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
		else
		{
			if (frames >= kNoHidFrames)
				break;
			fatal_service_point();   // no hidScanInput here: acknowledge another thread's request
		}
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
			dump_screen(0, "screen-error-top.bmp", raw_log);   // logs "saved" lines
			dump_screen(1, "screen-error-bottom.bmp", raw_log);
		}
	}

	if (have_hid)
		hidExit();
	if (own_gfx)
		gfxExit();
}

// Blocks the calling thread for good, in an untimed kernel wait. Never a sleep
// loop: __libctru_exit frees the heap before svcExitProcess, and a thread that
// wakes with its stack in the heap faults (STAGE5.md, the gspEventThreadMain fault).
[[noreturn]] void block_forever()
{
	for (;;)
	{
		if (s_fatal_never)
			svcWaitSynchronization(s_fatal_never, -1);
		else
			svcSleepThread(1000000000LL);   // no event (svcCreateEvent failed at boot)
	}
}

void copy_text(char* dst, size_t size, const char* src)
{
	size_t i = 0;
	for (; i + 1 < size && src && src[i]; ++i)
		dst[i] = src[i];
	dst[i] = 0;
}

// 5.6.3 step 3a: the fallback's exit watchdog. If a step of the exit hangs (aptExit joins
// the APT event thread, gfxExit waits for a VBlank), the process still ends the way the
// bare svcExitProcess() did. A small stack: no printf here, the line is built by hand.
void fallback_watchdog(void*)
{
	svcSleepThread(5000000000LL);
	char line[160] = "[3ds] fallback exit: watchdog fired (stuck in ";
	strncat(line, s_exit_step.load(), 60);
	strcat(line, "); svcExitProcess\n");
	log_raw(line);
	svcExitProcess();
}

// Logs the step raw and publishes it for the watchdog.
void fallback_step(const char* step, const char* text)
{
	s_exit_step.store(step);
	log_raw(text);
}
} // namespace

// 5.6.3 step 1-3: a fatal error on a thread other than main.
void fatal_from_other_thread(const char* title, const char* body)
{
	u32 tid = 0;
	svcGetThreadId(&tid, CUR_THREAD_HANDLE);
	char line[160];
	// The SDL audio thread first, win or lose: main must never call SDL_Quit/OpenAL
	// afterwards (step 6).
	if (tid && tid == s_audio_tid.load())
		s_audio_parked = true;

	u32 expected = 0;
	if (!s_fatal_from.compare_exchange_strong(expected, tid ? tid : 0xFFFFFFFFu))
	{
		// A loser never draws (a second consoleInit would also swap the top over
		// the first screen) and never exits the process.
		snprintf(line, sizeof line, "[3ds] fatal error on thread %lu while thread %lu's is handled: blocked\n",
		         static_cast<unsigned long>(tid), static_cast<unsigned long>(expected));
		log_raw(line);
		// Main shows another thread's text: this one's may be the real cause.
		log_raw(title ? title : "");
		log_raw("\n");
		log_raw(body ? body : "");
		log_raw("\n");
		block_forever();
	}
	// The winner owns the buffers; main reads them only after s_fatal_ready.
	copy_text(s_fatal_title, sizeof s_fatal_title, title);
	copy_text(s_fatal_body, sizeof s_fatal_body, body);
	s_fatal_ready.store(true, std::memory_order_release);
	snprintf(line, sizeof line, "[3ds] fatal error on thread %lu: handed to the main thread\n",
	         static_cast<unsigned long>(tid));
	log_raw(line);

	for (int ms = 0; ms < 5000 && !s_fatal_acked.load(); ++ms)
		svcSleepThread(1000000);
	// Take the request back if main has not (it can no longer serve it), else it did.
	if (s_fatal_acked.exchange(true))
		block_forever();

	snprintf(line, sizeof line, "[3ds] fatal error on thread %lu: not acknowledged in 5 s; shown on this thread\n",
	         static_cast<unsigned long>(tid));
	log_raw(line);
	// The fallback (main is blocked): this thread draws and ends the process through
	// aptExit() and APT_CloseApplication (step 3a), not __appExit's teardown under a live
	// main thread, and without the heap frees that come before them in __libctru_exit.
	// The bare svcExitProcess() this replaced made the system restart the console
	// (A6, 2026-10-26); whether the missing APT close was the cause is UNVERIFIED (5.6.4 item 5).
	// In the c3d build error_screen_here() logs only and returns at once when
	// a1_c3d_release_top() fails (citro3d still up).
	// std::string and the console use the heap here, unlike step 1's request (static
	// buffers): in the fallback main is blocked on a lock this thread holds or on one
	// of the engine's, never inside newlib's malloc (malloc calls out to nothing), so the
	// heap is usable; the remaining risk is a corrupted heap faulting the fallback,
	// acceptable for a last resort.
	s_fatal_fallback.store(true);   // before the screen: apt_hook (HOME, a close order) stays quiet
	error_screen_here(s_fatal_title, s_fatal_body, true);

	// Step 3a.1: the watchdog starts after the screen (which waits for A/START), so only
	// the exit below is timed. 4 KiB stack, priority 0x18, detached.
	fallback_step("watchdog start", "[3ds] fallback exit: starting the 5 s watchdog\n");
	if (!threadCreate(fallback_watchdog, nullptr, 4 * 1024, 0x18, -2, true))
		log_raw("[3ds] fallback exit: watchdog thread could not be created; going on without it\n");

#ifndef A1_RENDER_C3D
	// Step 3a.2 (software build only): what every clean exit did before its APT close. With
	// citro3d up (the c3d build) this thread must not touch the GPU state (5.6.2 a): gspExit
	// joins the thread that runs citro3d's callbacks and main may be inside a C3D frame, and
	// the only gfxExit reachable there, __wrap_gfxExit, printf's through the tee lock main
	// may hold. So mode 7 (software) vs 6b/10 (GPU) also tells whether gspExit matters.
	fallback_step("gfxExit", "[3ds] fallback exit: gfxExit\n");
	gfxExit();
#endif

	// Step 3a.3: aptExit() once. Its branch depends on the launcher's run flags (d7).
	{
		char line[200];
		const u32 flags = envGetSystemRunFlags();
		snprintf(line, sizeof line,
		         "[3ds] fallback exit: should close %d, run flags 0x%lX (aptworkaround %d, aptreinit %d, "
		         "aptchainload %d), return address %s; aptExit\n",
		         aptShouldClose() ? 1 : 0, static_cast<unsigned long>(flags),
		         (flags & RUNFLAG_APTWORKAROUND) ? 1 : 0, (flags & RUNFLAG_APTREINIT) ? 1 : 0,
		         (flags & RUNFLAG_APTCHAINLOAD) ? 1 : 0, __system_retAddr ? "set" : "null");
		fallback_step("aptExit", line);
	}
	void (*const ret_before)(void) = __system_retAddr;
	aptExit();

	// Step 3a.4: the close branch set __system_retAddr to aptExitProcess (APT_CloseApplication,
	// "kills us"): call it, as __libctru_exit() would, minus the heap frees. Any other branch
	// (reinit, chainload, crippled, or aptExit not the last reference) leaves it alone; the
	// loader's own return address expects a freed heap, so end with svcExitProcess().
	if (__system_retAddr && __system_retAddr != ret_before)
	{
		fallback_step("APT_CloseApplication", "[3ds] fallback exit: APT_CloseApplication via __system_retAddr\n");
		__system_retAddr();
		log_raw("[3ds] fallback exit: APT_CloseApplication returned; svcExitProcess\n");
	}
	else
		log_raw("[3ds] fallback exit: svcExitProcess (aptExit left __system_retAddr unchanged: reinit, chainload, crippled, or not the last APT reference)\n");
	svcExitProcess();
}

// 5.6.3 step 4: main thread, top-level service points only.
void fatal_service_point()
{
	if (!on_main_thread())
		return;
	fatal_test_frame_lock();   // debug-fatal mode 7: a lock main takes at every service point
	if (s_fatal_from.load(std::memory_order_relaxed) == 0)
		return;
	// Not inside libctru's APT hook loop or SDL's event filter (their locks).
	if (s_hook_depth.load() != 0 || !s_fatal_ready.load(std::memory_order_acquire))
		return;
	if (s_fatal_acked.exchange(true))
	{
		// Acknowledged by main earlier (it is in its own fatal path): carry on. Otherwise
		// the requester took the request back and is drawing the fallback screen: main must
		// not run on and draw over it (present_bottom redraws the HUD), it waits for the
		// requester's svcExitProcess.
		if (!s_fatal_main_took.load())
		{
			log_raw("[3ds] main thread: the request was taken back by its thread (fallback screen); blocking\n");
			block_forever();
		}
		return;
	}
	s_fatal_main_took = true;
	// Main is in a fatal path of its own (or serving): acknowledge, do not nest. Its
	// screen shows its own text, so the request's goes to the log.
	if (s_fatal_active.exchange(true))
	{
		log_raw("[3ds] main thread: in its own fatal path; the request's text, not shown:\n");
		log_raw(s_fatal_title);
		log_raw("\n");
		log_raw(s_fatal_body);
		log_raw("\n");
		return;
	}
	char line[160];
	snprintf(line, sizeof line, "[3ds] +%.1f ms: fatal error from thread %lu, shown by the main thread\n",
	         ms_since_boot(), static_cast<unsigned long>(s_fatal_from.load()));
	fputs(line, stdout);
	fflush(stdout);
	error_screen(s_fatal_title, s_fatal_body);
	fatal_exit();
}

void fatal_guard_enter()
{
	++s_hook_depth;
}

void fatal_guard_leave()
{
	--s_hook_depth;
}

bool fatal_audio_parked()
{
	return s_audio_parked;
}

void fatal_note_audio_thread()
{
	u32 tid = 0;
	svcGetThreadId(&tid, CUR_THREAD_HANDLE);
	s_audio_tid = tid;
}

bool is_abort_handler(void (*handler)(int))
{
	return handler == on_abort;
}

// 5.6.3 step 7: the engine's vhalt(), at its top (PATCHES.md 54), before
// stop_recording(), logFatal and shutdown_application() can run on the wrong thread.
void vhalt_hook(const char* message)
{
	if (!on_main_thread())
		fatal_from_other_thread("Aleph One stopped", message);   // never returns
	if (s_audio_parked)
	{
		// Upstream's shutdown_application() would deadlock on the audio lock.
		s_fatal_active = true;
		error_screen("Aleph One stopped", message);
		fatal_exit();
	}
	// Upstream's vhalt() goes on as before (SDL_Quit, a log line, abort()); __wrap_abort then
	// shows this message on the error screen.
	copy_text(s_vhalt_message, sizeof s_vhalt_message, message);
	s_vhalt_pending = true;
	s_fatal_active = true;
}

bool fatal_fallback_active()
{
	return s_fatal_fallback.load();
}

void fatal_log_raw(const char* text)
{
	log_raw(text);
}

unsigned long main_thread_id()
{
	return s_main_tid;
}

} // namespace a1_3ds

// 5.6.3 step 7: abort() on any thread. newlib's signal handlers are per _reent and
// a thread's is zeroed, so on a second thread signal(SIGABRT, on_abort) does not
// apply and abort() would _exit() under a live main thread.
extern "C" void __real_abort(void) __attribute__((noreturn));
extern "C" void __wrap_abort(void)
{
	if (!a1_3ds::on_main_thread())
	{
		u32 tid = 0;
		svcGetThreadId(&tid, CUR_THREAD_HANDLE);
		char what[200];   // the thread blocks inside the call below: the buffer stays valid
		snprintf(what, sizeof what, "abort() was called on thread %lu (failed assertion, halt() or "
		         "vhalt()); the lines before this in the log may say why.", static_cast<unsigned long>(tid));
		a1_3ds::fatal_from_other_thread("Aleph One stopped", what);
	}
	if (a1_3ds::s_error_shown)
		a1_3ds::fatal_exit();   // a screen was shown already: no second one (debug-fatal mode 9)
	if (a1_3ds::s_vhalt_pending.exchange(false))   // once: an abort() inside the screen goes on below
	{
		// vhalt() on main ends in abort(), and its own alert shows nothing on the 3DS (the
		// 3DS screen is in alert_user(), which vhalt() does not call): show vhalt()'s message,
		// not on_abort's generic one.
		a1_3ds::error_screen("Aleph One stopped", a1_3ds::s_vhalt_message);
		a1_3ds::fatal_exit();
	}
	__real_abort();   // a bare abort() or halt(): on_abort -> die_with_message
}
