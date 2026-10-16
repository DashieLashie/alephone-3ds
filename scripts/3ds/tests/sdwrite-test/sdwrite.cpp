// SD card write speed on the console (docs/3ds/NEXT-SPRINT.md B1: a save,
// 2.98 MB, took 69 s in "save: file written" on a New 3DS, 0.13 s in Azahar).
// Each variant writes a file under sdmc:/3ds/sdwrite-test/ and prints KB/s;
// output goes to the top screen and, with 3dslink -s, to the host.
#include <3ds.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <sys/stat.h>
#include <malloc.h>
#include <initializer_list>

static double now_ms() { return svcGetSystemTick() / (double)SYSCLOCK_ARM11 * 1000.0; }

static void report(const char* what, size_t bytes, double ms)
{
	printf("%-34s %7zu KB %8.0f ms %8.0f KB/s\n", what, bytes / 1024, ms, bytes / 1024.0 / (ms / 1000.0));
}

// The engine's pattern: fopen "wb+", a small header write, then one fwrite per
// tag (sizes as a save: ~220 KB of game data, then the 2.76 MB preview), with a
// seek before each write (wad.cpp write_to_file()).
static void engine_like(const char* path, const char* mode, bool prealloc, size_t total, const char* label)
{
	char* buf = (char*)malloc(total);
	memset(buf, 0x5a, total);
	double t = now_ms();
	FILE* f = fopen(path, mode);
	if (!f) { printf("%s: fopen failed\n", label); free(buf); return; }
	if (prealloc && ftruncate(fileno(f), total) != 0) printf("%s: ftruncate failed\n", label);
	size_t first = total / 13, off = 0;
	size_t pieces[] = {128, first, total - first - 128 - 512, 512};
	for (size_t p : pieces) { fseek(f, off, SEEK_SET); fwrite(buf + off, 1, p, f); off += p; }
	fclose(f);
	report(label, total, now_ms() - t);
	free(buf);
}

// Fixed-size chunks, the file growing with each one.
static void chunks(const char* path, size_t chunk, size_t total, bool prealloc, const char* label)
{
	char* buf = (char*)malloc(chunk);
	memset(buf, 0xa5, chunk);
	double t = now_ms();
	FILE* f = fopen(path, "wb");
	if (!f) { printf("%s: fopen failed\n", label); free(buf); return; }
	if (prealloc) ftruncate(fileno(f), total);
	for (size_t off = 0; off < total; off += chunk) fwrite(buf, 1, chunk, f);
	fclose(f);
	report(label, total, now_ms() - t);
	free(buf);
}

// FS directly: one FSFILE_Write of the whole buffer (linear memory), with and
// without FSFILE_SetSize first.
static void fs_direct(const char* name, size_t total, bool prealloc, const char* label)
{
	u8* buf = (u8*)linearAlloc(total);
	memset(buf, 0x3c, total);
	FS_Archive sd;
	FSUSER_OpenArchive(&sd, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, ""));
	double t = now_ms();
	Handle h;
	Result rc = FSUSER_OpenFile(&h, sd, fsMakePath(PATH_ASCII, name), FS_OPEN_WRITE | FS_OPEN_CREATE, 0);
	if (R_FAILED(rc)) { printf("%s: open failed %08lx\n", label, rc); linearFree(buf); FSUSER_CloseArchive(sd); return; }
	FSFILE_SetSize(h, prealloc ? total : 0);
	u32 written = 0;
	rc = FSFILE_Write(h, &written, 0, buf, total, 0);
	FSFILE_Close(h);
	report(label, total, now_ms() - t);
	if (R_FAILED(rc) || written != total) printf("  write rc %08lx, %lu bytes\n", rc, written);
	linearFree(buf);
	FSUSER_CloseArchive(sd);
}

int main()
{
	gfxInitDefault();
	consoleInit(GFX_TOP, nullptr);
	if (R_SUCCEEDED(socInit((u32*)memalign(0x1000, 0x100000), 0x100000)) && link3dsStdio() < 0)
		socExit();
	mkdir("sdmc:/3ds/sdwrite-test", 0777);
	const char* d = "sdmc:/3ds/sdwrite-test/";
	const size_t save = 2980 * 1024, small = 512 * 1024;
	char p[64];
	printf("sdwrite-test: SD write speed\n");
	#define P(n) (snprintf(p, sizeof p, "%s%s", d, n), p)
	unlink(P("a")); engine_like(P("a"), "wb+", false, small, "engine-like wb+, 512 KB");
	unlink(P("b")); engine_like(P("b"), "wb+", true, small, "engine-like wb+, prealloc, 512 KB");
	unlink(P("c")); chunks(P("c"), 8192, small, false, "8 KB fwrites, growing, 512 KB");
	unlink(P("d")); chunks(P("d"), 8192, small, true, "8 KB fwrites, prealloc, 512 KB");
	unlink(P("e")); fs_direct("/3ds/sdwrite-test/e", small, false, "FSFILE_Write once, 512 KB");
	unlink(P("f")); fs_direct("/3ds/sdwrite-test/f", small, true, "FSFILE_Write once, prealloc, 512 KB");
	unlink(P("g")); engine_like(P("g"), "wb+", true, save, "engine-like wb+, prealloc, 2.98 MB");
	unlink(P("h")); fs_direct("/3ds/sdwrite-test/h", save, true, "FSFILE_Write once, prealloc, 2.98MB");
	unlink(P("i")); engine_like(P("i"), "wb+", false, save, "engine-like wb+, 2.98 MB (as a save)");
	for (const char* n : {"a","b","c","d","e","f","g","h","i"}) unlink(P(n));
	rmdir("sdmc:/3ds/sdwrite-test");
	printf("sdwrite-test: done (START quits)\n");
	while (aptMainLoop()) { hidScanInput(); if (hidKeysDown() & KEY_START) break; gspWaitForVBlank(); }
	socExit();
	gfxExit();
	return 0;
}
