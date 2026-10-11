# Draft bug report for devkitPro (not filed)

Drafted 2026-10-11 (`STAGE5-PREREQS.md` item 9). Suggested place:
<https://github.com/devkitPro/devkitarm-crtls/issues> (the repository that
ships `3dsx.ld`). The user files it; nothing has been posted.

Checked before drafting: `3dsx.ld` on `devkitarm-crtls` `master` is
byte-identical to the installed one, and its last `.ARM.exidx` change
(`5d07c23`, 2023-11-17, "fix name of .ARM.exidx section") does not touch the
symbols. A GitHub search of the devkitPro organisation for `exidx` /
`__exidx_start` found only buildscripts#4 (2015, "Catching exceptions not
supported"), which had a different cause (3dsxtool dropped the unwind
sections) and was fixed in r45.

---

**Title:** 3dsx.ld: `__exidx_start` is set outside `.ARM.exidx`, so it can point 2 bytes before the table and every C++ throw calls `std::terminate()`

**Environment:** devkitARM r68 (gcc 16.1.0, binutils 2.46.0), devkitarm-crtls
1.2.6, libctru 2.7.0, macOS arm64 host. Reproduced in Azahar 2126.2. The
same defect stopped a large C++ program (an Aleph One port) at its first
`throw`.

**Problem.** `3dsx.ld` assigns the unwind table bounds between output
sections:

```ld
	.ARM.extab : { *(.ARM.extab* .gnu.linkonce.armextab.*) } : rodata
	__exidx_start = .;
	.ARM.exidx : { *(.ARM.exidx* .gnu.linkonce.armexidx.*) } : rodata
	__exidx_end = .;
```

`.ARM.extab` entries are halfword-granular, so the section can end on a
2-byte boundary. `__exidx_start` then takes that address, but `.ARM.exidx`
is 4-byte aligned and starts 2 bytes later. libgcc's ARM unwinder
(`__gnu_Unwind_Find_exidx`, the static case) binary-searches 8-byte entries
from `__exidx_start`, so every lookup reads misaligned data, no handler is
found, and every `throw` ends in `std::terminate()`. Whether a program is
affected depends only on the total size of its `.ARM.extab`, so the bug
appears and disappears as code changes.

**Reproducer.** A 2-byte `.ARM.extab` input linked last forces the
condition deterministically:

`pad.s`:

```asm
	.section .ARM.extab.pad,"a",%progbits
	.balign 2
	.global extab_pad
extab_pad:
	.short 0
```

`repro.cpp`:

```cpp
#include <3ds.h>
#include <cstdio>
#include <cstdlib>
#include <exception>

extern "C" const char __exidx_start[];
extern "C" const short extab_pad;   // pad.s; referenced so --gc-sections keeps it

static void wait_start() {
	while (aptMainLoop()) { hidScanInput(); if (hidKeysDown() & KEY_START) break; gspWaitForVBlank(); }
}
static void on_terminate() { printf("FAILED: std::terminate\n"); wait_start(); gfxExit(); exit(1); }

int main() {
	gfxInitDefault();
	consoleInit(GFX_TOP, nullptr);
	std::set_terminate(on_terminate);
	printf("__exidx_start = %p\n", (void*)__exidx_start);
	(void) *(volatile const short*)&extab_pad;
	try { throw 42; }
	catch (int v) { printf("passed: caught %d\n", v); }
	wait_start();
	gfxExit();
	return 0;
}
```

```sh
F="-march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft"
arm-none-eabi-g++ $F -O2 -I$DEVKITPRO/libctru/include -c repro.cpp
arm-none-eabi-as -march=armv6k pad.s -o pad.o
arm-none-eabi-g++ $F -specs=3dsx.specs -L$DEVKITPRO/libctru/lib \
    repro.o -lstdc++ -lctru -lgcc pad.o -o repro.elf   # pad.o last
3dsxtool repro.elf repro.3dsx
```

**Result with the stock `3dsx.ld`:**

```
$ arm-none-eabi-readelf -SW repro.elf | grep ARM.ex
  [ 6] .ARM.extab   PROGBITS   001219e8 0229e8 0000d2 00  A  0 0 4
  [ 8] .ARM.exidx   ARM_EXIDX  00121abc 022abc 0001b8 00 AL  1 0 4
$ arm-none-eabi-nm repro.elf | grep __exidx_start
00121aba R __exidx_start          <- 2 bytes before .ARM.exidx
```

Running it prints `__exidx_start = 0x121aba` and then `FAILED: std::terminate`.

**Expected:** `__exidx_start` equal to the start of `.ARM.exidx`, and
`passed: caught 42`.

**Fix:** define both symbols inside the output section, after its
alignment, as GNU ld's default ARM scripts do:

```diff
--- a/3dsx.ld
+++ b/3dsx.ld
@@ -62,9 +62,12 @@
 	} : rodata
 
 	.ARM.extab : { *(.ARM.extab* .gnu.linkonce.armextab.*) } : rodata
-	__exidx_start = .;
-	.ARM.exidx : { *(.ARM.exidx* .gnu.linkonce.armexidx.*) } : rodata
-	__exidx_end = .;
+	.ARM.exidx :
+	{
+		__exidx_start = .;
+		*(.ARM.exidx* .gnu.linkonce.armexidx.*)
+		__exidx_end = .;
+	} : rodata
 
 	/* =========== DATA section =========== */
 
```

With this script (selected with `-B <dir>`, since the spec file links
`-T 3dsx.ld%s`), the same objects give `__exidx_start = 0x00121abc`
(= the start of `.ARM.exidx`), and the program prints `passed: caught 42`.

**Other scripts.** In devkitARM r68's `arm-none-eabi/lib/`, `ds_arm7.ld`
already defines both symbols inside `.ARM.exidx`. Eight others assign them
outside, like `3dsx.ld`: `dldi.ld`, `ds_arm7_iwram.ld`, `ds_arm7_vram.ld`,
`ds_arm9.ld`, `gba_cart.ld`, `gba_mb.ld`, `gp32.ld` and `gp32_gpsdk.ld`.
Where an output section ends on a 2-byte boundary before `.ARM.exidx`, they
can hit the same problem (only `3dsx.ld` was tested). `ds_arm9.ld` also
names the output section `ARM.exidx` (no leading dot), the slip that
`5d07c23` fixed in `3dsx.ld`.

**Workaround for affected programs:** define a strong
`__gnu_Unwind_Find_exidx(_Unwind_Ptr, int*)` that returns `__exidx_start`
rounded up to 4 and the entry count `(__exidx_end - aligned_start) / 8`.

---

Local copy of the reproducer: `scripts/3ds/tests/exidx-repro/`
(`build.sh`; `FIXED=1 build.sh` applies `3dsx.ld.patch`), verified in Azahar
2126.2 on 2026-10-11. **Not run on hardware.**
