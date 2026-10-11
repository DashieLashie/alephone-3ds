// devkitARM 3dsx.ld: __exidx_start can sit below .ARM.exidx -> every throw terminates.
#include <3ds.h>
#include <cstdio>
#include <exception>
#include <cstdlib>

extern "C" const char __exidx_start[], __exidx_end[];
extern "C" const short extab_pad;   // pad.s
static FILE* g_log;
static void out(const char* s) { printf("%s\n", s); if (g_log) { fprintf(g_log, "%s\n", s); fflush(g_log); } }

static void wait_start() {
	while (aptMainLoop()) { hidScanInput(); if (hidKeysDown() & KEY_START) break; gspWaitForVBlank(); }
}

static void on_terminate() { out("RESULT: FAILED (std::terminate called; the throw was not caught)"); wait_start(); gfxExit(); exit(1); }

int main() {
	gfxInitDefault();
	consoleInit(GFX_TOP, nullptr);
	g_log = fopen("sdmc:/3ds/exidx-repro.log", "w");
	std::set_terminate(on_terminate);
	char line[96];
	snprintf(line, sizeof line, "__exidx_start = %p (%s)", (void*)__exidx_start,
	         ((unsigned)__exidx_start & 3) ? "NOT 4-aligned" : "4-aligned");
	out(line);
	(void) *(volatile const short*)&extab_pad;
	try { throw 42; }
	catch (int v) { snprintf(line, sizeof line, "RESULT: passed (caught %d)", v); out(line); }
	wait_start();
	gfxExit();
	return 0;
}
