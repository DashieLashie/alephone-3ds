# Aleph One 3DS port: stage 5 prerequisites

Recorded 2026-10-11 on branch `3ds-port`, on top of stage 4 (`527e67c`).
Each section records the decision, the commands run and the verification.
Stage 5 itself has not started.

## 0. Triage

**Stage 5 scope** (decided 2026-10-11): **controls only**: map the buttons,
circle pad and touch screen and navigate the menus. Reaching a level is a
later stage. Out of scope here (stage work, not gaps): 400x240 menu/HUD
layouts, an Ogg (Tremor) decoder, `Plugins::enumerate()` boot time, the
network buttons on the main menu, hyperlinks, the Lua io/os stubs.

**Hardware available:** a New 3DS / New 2DS. No Old 3DS, so every Old 3DS
figure below is from Azahar's Old 3DS mode and stays unverified.

**Order** (accepted as proposed): 8 → 10 → 2 → 3 → 6 → 4 (investigation)
→ 1 + 5 (one hardware session) → 4 (decision) → 9 → 7. Sections are
numbered by item, not by order of work.

| # | Item | Planned |
| --- | --- | --- |
| 1 | Hardware run | before stage 5 |
| 2 | Identical image-cache file names | before; verify in Azahar, then on hardware |
| 3 | Audio thread stack | before; instrument, measure on hardware |
| 4 | Linear heap size | investigate now, decide after the hardware numbers |
| 5 | Audio stalls / OpenAL CPU cost | measure only, during item 1 |
| 6 | Early fatal-alert error screen | before; Azahar |
| 7 | Cosmetics (platform name, player name) | before, last |
| 8 | Branch backup | before, first |
| 9 | devkitPro `3dsx.ld` bug report | draft only |
| 10 | Emulator home | before, early |

## 8. Branch backup

**Decision:** a git bundle in `~/Documents/Aleph_3ds-backups/`, local only.
Rejected: a private GitHub repo (needs `git fetch --unshallow` and a login),
a public fork, skipping again (stage 3 declined it). Copying the file to
iCloud or an external drive is up to the user. The bundle is refreshed after
each approved commit.

**Command:**

```sh
mkdir -p ~/Documents/Aleph_3ds-backups
git bundle create ~/Documents/Aleph_3ds-backups/3ds-port-2026-10-11-527e67c.bundle \
    fc4c274..3ds-port
git bundle verify ~/Documents/Aleph_3ds-backups/3ds-port-2026-10-11-527e67c.bundle
```

**Verification:**

```
3ds-port-2026-10-11-527e67c.bundle is okay
The bundle contains this ref:
527e67c4e4e8e8336b8ce9c8666159861ac8013f refs/heads/3ds-port
The bundle requires this ref:
fc4c274359088614b58c91af5daa541504cb50cf
size 1 846 668 bytes
sha256 a12d052f9b8a025d2c26e56874267bde52cf001ba58b4644e8370d06cff39dd4
```

**Refreshed** after each approved commit: `3ds-port-2026-10-11-1dfa858.bundle`
(prerequisites), and `3ds-port-2026-10-11-015e63c.bundle` (stage 5,
2026-10-11): `is okay`, 2 188 211 bytes, sha256
`604a74f03b3b6714d1d93d13a3cf4949558737cd9fd8663b11b6f88ada850bc8`;
restored into a scratch repository (tag fetched at depth 1, then the
bundle), tree `2156fa64…` identical to `HEAD`'s.

**Refreshed 2026-10-12** (`NEXT-SPRINT.md` item 12; the last refresh had
been `d19dae0`, stage 6): `3ds-port-2026-10-12-ad4716c.bundle`, `is okay`,
4 321 877 bytes, sha256
`d5bf2a164cfd03e1eb19e446c03fbddcdf915b0372ab31e3e06d369dbb3d4ae1`.
Restored offline into a scratch repository (tag `release-20260930` fetched at
depth 1 from the local repository, then the bundle): tree `90c74d5e…`
identical to `HEAD`'s. Older bundles are kept.

The bundle is incremental: its prerequisite `fc4c274` is upstream tag
`release-20260930` (the shallow boundary). **Restore**, tested in a scratch
directory: the restored tree matched the working copy (`diff -rq`, tracked
files):

```sh
git clone --depth 1 --branch release-20260930 \
    https://github.com/Aleph-One-Marathon/alephone.git Aleph_3ds
cd Aleph_3ds
git fetch <bundle> 3ds-port:3ds-port && git checkout 3ds-port
```

## 10. Emulator home

**Finding:** 2126.2 is still the latest stable Azahar release (2026-10-08).
The only newer build is `2127.0-alpha1` (pre-release, 2026-10-10), which is
probably what the stage 4 "Update Available" dialog announced.

**Decision:** move the portable install to `~/Applications/Azahar-2126.2/`
and stay on 2126.2, so measurements stay comparable with stage 4. The /tmp
copy and the original zip were deleted afterwards (re-downloadable:
`azahar-macos-arm64-2126.2.zip`, 99 740 365 bytes, from the
`azahar-emu/azahar` GitHub releases). Rejected: also trying the alpha; a
git-ignored copy inside the repo (289 MB).

**Commands:**

```sh
ditto <old scratchpad>/azahar-macos-arm64-2126.2 ~/Applications/Azahar-2126.2
diff -rq <old> ~/Applications/Azahar-2126.2        # no output: identical
sed -i '' "s#<old>#$HOME/Applications/Azahar-2126.2#g" \
    ~/Applications/Azahar-2126.2/user/config/qt-config.ini
```

**Gotcha:** `qt-config.ini` stores absolute paths (`nand_directory`,
`sdmc_directory`, `Paths\recentFiles`) pointing at the old /tmp folder.
`use_custom_storage=false`, so portable mode probably ignores them, but they
were rewritten so nothing can fall back to the old location (0 `/private/tmp`
references left). `check_for_update_on_start=false` was carried over.
`codesign -v` reports "bundle format is ambiguous" for a Qt framework in
both the old and the new copy, so the release itself does that, not the copy.

**Script:** `scripts/3ds/run-azahar.sh` now also looks for
`~/Applications/Azahar-*/Azahar.app` (the last by name) after
`/Applications/Azahar.app` and `~/Applications/Azahar.app`, so `AZAHAR=` is
no longer needed. The memory note for this project was updated.

**Verification** (`scripts/3ds/run-azahar.sh --timeout 60`, `AZAHAR` unset):

```
run-azahar: SD card /Users/dashielashie/Applications/Azahar-2126.2/user/sdmc
run-azahar: starting /Users/dashielashie/Applications/Azahar-2126.2/Azahar.app/Contents/MacOS/azahar
[3ds] C++ exceptions: OK
[3ds] +32225.6 ms: TIME TO MAIN MENU (from main(); includes the intro screens)
```

The new SD card's `log.txt` was written at 03:23; the old one kept its 00:30
time stamp. The settings were unchanged after the run.

## 2. Identical image-cache file names

**Problem** (`STAGE3.md` risk 3): devkitARM's `std::random_device` is
mt19937 with the default seed, so `boost::uuids::random_generator` gives
every cached image the same name and `a1fs::unique_path` repeats its names
in every run. Only save-game previews (`QuickSave.cpp`) use the image cache,
so a boot to the menu never showed it.

**Decision:** fix at the two call sites under `A1_PLATFORM_3DS` with an
entropy source from the port (`PATCHES.md` 14). Rejected: replacing
`std::random_device` itself (it would mean wrapping libstdc++ internals:
`random.o` has no entropy hook; newlib's `getentropy`/`arc4random` need a
`_getentropy_r` that nothing provides).

**Change:**

- `a1_3ds::random_bytes()` / `a1_3ds::Urng` (`ports/3ds/include/a1_3ds_platform.h`,
  `ports/3ds/source/a1_3ds_platform.cpp`): `psInit()` once (thread-safe
  static), then `PS_GenerateRandomBytes`. Fallback when `psInit` or a later
  call fails: splitmix64 over `svcGetSystemTick()`, `osGetTime()` and an
  atomic counter (the counter makes values distinct within a run, the clock
  across runs; not cryptographic). Logged once at boot:
  `[3ds] entropy: ps:ps (PS_GenerateRandomBytes)` or
  `[3ds] entropy: psInit failed (0x...); using the tick/clock fallback`.
- `WadImageCache.cpp`: `basic_random_generator<a1_3ds::Urng>`;
  `a1_filesystem.h`: `unique_path` seeds from `a1_3ds::Urng`.
- Debug flag `sdmc:/3ds/alephone/debug-entropy` (`ports/3ds/source/selftest.cpp`,
  a separate TU because it needs engine headers and must not see `<3ds.h>`):
  at the main menu, logs three `unique_path("%%%%%%%%")` names, caches three
  8x8 images under test descriptors through `WadImageCache::cache_image`
  (resized to 4x4, so they are stored), logs the new file names, then
  `remove_image`s them.

Build: 0 warnings.

**Verification (Azahar 2126.2, New 3DS mode)**, two runs each, with
`debug-entropy` present:

| Build | Run | Entropy | Image cache names | Result |
| --- | --- | --- | --- | --- |
| Control (engine files from `HEAD`, temporary) | 1 | (not used) | `fb135fdb-2482-4d31-aa8c-351caf595b56` only | **1 of 3, collided** |
| | 2 | | `fb135fdb-2482-4d31-aa8c-351caf595b56` only | **1 of 3, collided** |
| Fix | 1 | ps:ps | `1f6c7760-b132-4886-…`, `36dad8a3-9ef8-4857-…`, `9fc41b71-1619-4036-…` | 3 of 3 |
| | 2 | ps:ps | `3a3317e2-5cf0-4cbc-…`, `4f500e4e-dbb2-4a36-…`, `dd57b34b-6772-468a-…` | 3 of 3 |
| Fix, `psInit` forced to fail (temporary) | 1 | fallback | `1447791b-1e72-495a-…`, `53d75ef7-c8b7-4df2-…`, `5cca2f2b-b4bb-443a-…` | 3 of 3 |
| | 2 | fallback | `07d9c562-ed9c-4098-…`, `644590db-f2c1-4c8e-…`, `ddc973bc-5915-4922-…` | 3 of 3 |

`unique_path`: control `4a69d98a de42ae2d bcbe0dce` in both runs; fix
`7a1b82f4 459918c9 7a613bf1` / `aa6fa805 3181a7c2 9117b8b8`; fallback
`9690ff7f ece00763 1c127e2c` / `429eb0ad d081eeca 6304fa31`. The control
reproduces the UUID `STAGE3.md` predicted on the host. All fixed UUIDs are
version 4 with the RFC variant. After each run the image cache held 0 test
files. The temporary builds were reverted and the real build rebuilt;
`debug-entropy` was removed from the emulated SD card.

**Non-3DS check:** see `PATCHES.md` 14 (`FileHandler.cpp` identical,
`WadImageCache.cpp` differs only in two `__LINE__` values, both volumes).
Method: in a throwaway `aleph-host-baseline` container with the volume
mounted, `make -n -o Makefile -o Makefile.in -W <file>.cpp <file>.o` gives
automake's compile line; `-c` → `-E -P` (dependency flags and `-o` removed),
run with the `HEAD` and working-tree versions copied to the same path, then
`cmp`; the volume's files are restored afterwards.

**Hardware (2026-10-11, `STAGE5.md` "Hardware session"):** `psInit()`
succeeds under the Homebrew Launcher: `[3ds] entropy: ps:ps
(PS_GenerateRandomBytes)` in every run. `debug-entropy`: `unique_path`
`a10fa4a2 856c9aad 3a595470`, image cache `08762a0b-…`, `7924f7ec-…`,
`fa5f60c9-…`, 3 of 3 distinct. The Boost probe's expected FAIL
(`fb135fdb-…` twice) reproduces on hardware too.

## 3. Audio thread stack

**Facts** (SDL 2.32.10 source, `build/3ds-deps/`): the engine opens audio
with the legacy `SDL_OpenAudio()` (`Sound/AudioBackend.h:71`); its callback
runs OpenAL's `alcRenderSamplesSOFT` on SDL's audio thread.
`SDL_audio.c:1556` creates that thread with `SDL_CreateThreadInternal(…, 0, …)`,
and `src/thread/n3ds/SDL_systhread.c` turns a 0 size into
`N3DS_THREAD_STACK_SIZE_DEFAULT` = 80 KiB. **`SDL_HINT_THREAD_STACK_SIZE`
cannot change it**: only the public `SDL_CreateThread()` reads the hint.
Raising the size would mean patching SDL (`scripts/3ds/build-sdl2.sh`). The
same file puts the thread on core 1 (the system core) only if
`APT_SetAppCpuTimeLimit(30)` succeeds; otherwise the thread runs on core 0
(`cpu = -1` = the process's default core).

**Measurement tool** (port-only, no engine change): `ports/3ds/source/audio_probe.cpp`,
linked with `-Wl,--wrap=SDL_OpenAudio`. `__wrap_SDL_OpenAudio` swaps in a
trampoline callback. On its first call (on the audio thread) the trampoline
records the core and priority and paints the unused stack: the bounds come
from libctru 2.7.0's `threadCreate()` layout (`source/thread.c`:
`[Thread_tag][stack]`, `stacktop` = the struct's last field), using the
top 80 KiB under `stacktop`, which always lies inside the allocation. It
times every callback against the buffer's play time and counts callbacks
that start more than 1.5 buffers after the previous one. `log_memory()`
reports it at every boot milestone and at exit. With the `debug-audio` flag
file, the game also plays the first 8 sounds that load, all at once, at the
main menu (`selftest.cpp`, main thread), and reports 1 s later and then
every 5 s.

**Finding: audio never ran in Azahar.** The first probe run logged
`audio: SDL_OpenAudio failed: DSP init failed: dspfirm.cdc missing!`.
libctru's `ndspInit()` loads `sdmc:/3ds/dspfirm.cdc` (or an `hb:ndsp`
handle). On a console the file is dumped with DSP1. The stage 4 Azahar runs
were therefore silent, and "sound initialized" passed with no device (the
engine continues without sound). Azahar's default DSP emulation is HLE
(`audio_emulation=0`, log: `Audio_Emulation: HLE`), which does not execute
the firmware: a 4 KiB placeholder (zeros, `DSP1` and the size at 0x100) makes
`ndspInit()` succeed. `run-azahar.sh` now creates it when missing. The
placeholder contains no Nintendo code and must never go onto a real SD card.

**Results (Azahar 2126.2, New 3DS mode; emulated timing, relative only):**

```
[3ds] audio: SDL_OpenAudio 44100 Hz, 2 channels, format 0x8010, 1024 samples (23.22 ms per buffer)
[3ds] +32200.3 ms: TIME TO MAIN MENU (from main(); includes the intro screens)
[3ds]   audio thread (core 0, priority 0x19): stack 80 KiB, deepest use so far 2 KiB
[3ds]   audio callback, last interval: 374 calls, avg 6.84 ms, max 7.90 ms per 23.22 ms buffer (avg load 29%); 0 late (> 1.5 buffers apart), longest gap 25.83 ms
[3ds] debug-audio: playing sounds 1 2 3 5 7 8 9 10 (8 at once)
[3ds] +33563.0 ms: audio report
[3ds]   audio thread (core 0, priority 0x19): stack 80 KiB, deepest use so far 9 KiB
[3ds]   audio callback, last interval: 63 calls, avg 11.24 ms, max 16.28 ms per 23.22 ms buffer (avg load 48%); 0 late (> 1.5 buffers apart), longest gap 25.84 ms
[3ds] +38689.2 ms: audio report
[3ds]   audio callback, last interval: 216 calls, avg 6.85 ms, max 7.90 ms per 23.22 ms buffer (avg load 30%); 0 late (> 1.5 buffers apart), longest gap 25.80 ms
```

- Stack: 2 KiB while mixing silence, **9 KiB with 8 voices**, of 80 KiB.
- Azahar puts the thread on **core 0**, so `APT_SetAppCpuTimeLimit(30)` was
  refused there. Its priority 0x19 is above the main thread's 0x30.
- Mixing silence costs ~30% of the buffer time in Azahar. Item 5 measures
  the real cost.
- Time to menu is unchanged with audio working (32.2 s vs 32.1-32.2 s
  without).

**Decision:** keep SDL's 80 KiB (measured 9 KiB peak, ~9x headroom) instead of
patching SDL. Revisit if the hardware peak is much higher, and when an Ogg
decoder lands: Tremor uses `alloca` heavily, and music decoding may run on
this thread.

**Hardware (2026-10-11):** the audio thread runs on **core 1**
(`APT_SetAppCpuTimeLimit(30)` succeeds there; Azahar refused it), priority
0x19. Stack peak **9 KiB of 80 KiB** with the 8-sound burst and in
gameplay, as in Azahar. **Decision confirmed:** keep SDL's 80 KiB.

## 6. Early fatal-alert error screen

**Trigger:** a scenario folder that exists and is not empty but has no
Map/Shapes. That passes the platform's own check in `boot_begin()` and
fails in the engine. Steps (emulated SD card, undone afterwards):

```sh
U=~/Applications/Azahar-2126.2/user/sdmc/3ds/alephone
mkdir -p "$U/Broken" && echo test > "$U/Broken/readme.txt"
echo Broken >> "$U/scenario.txt"             # last non-comment line wins: the first one
scripts/3ds/run-azahar.sh --no-data --screens --timeout 45
```

**Result (Azahar):** at 1.3 s (after "fonts and plugins", long before the
main window) the engine raises `alert_user(fatalError)` ("Please be sure the
files 'Map', 'Shapes', 'Images' and 'Sounds' are correctly installed…",
`csalerts_sdl.cpp:262`). `MainScreenVisible()` is false, so `PATCHES.md`
11 routes it to `a1_3ds::error_screen`. The bottom screen showed the
message and "Press START or A to exit".

**Defect found and fixed (port only):** the top screen stayed **black**.
`error_screen()` left the top screen to SDL whenever SDL video was
initialised, but between `SDL_Init` (0.4 s) and the main window (~17 s)
there is no game frame to keep. It now also takes the top screen when no
window has keyboard focus (`SDL_GetKeyboardFocus() == NULL`, the test
`log_video()` uses), and shows the "See the bottom screen" pointer there.
SDL's `N3DS_VideoInit` has already called `gfxInit()`, so the console can
draw on both screens. Every caller exits afterwards, so the top screen's
format change is harmless.

| Run | Top screen | Bottom screen |
| --- | --- | --- |
| Before the fix | black | "Error" + message + "Press START or A to exit." |
| After the fix | "Error / See the bottom screen." | same |
| Regression: missing folder (`scenario.txt` → `Nonexistent`, before `SDL_Init`) | pointer, as in stage 4 | "Aleph One cannot start" |

Not covered: an error after the main window exists (the top screen keeps
the game's frame). Nothing triggers one safely before input exists.
**Hardware (2026-10-11):** the missing-scenario screen ("Aleph One cannot
start / Scenario folder not found: sdmc:/3ds/alephone/Marathon2") shows on
the bottom screen and **A exits** cleanly.

## 4. Linear heap size

**How libctru 2.7.0 sizes the heaps** (source: `libctru/source/system/allocateHeaps.c`
from the `v2.7.0` tag; installed package `libctru-2.7.0-1`).
`__system_allocateHeaps()` reads the process's commit limit
(`svcGetResourceLimitLimitValues(RESLIMIT_COMMIT)`) minus what is already
committed = `remaining`. Unless the program defines `__ctru_heap_size` or
`__ctru_linear_heap_size` (both weak, default 0), it splits `remaining` in
half, caps the application heap at 24 MiB (`HEAP_SPLIT_SIZE_CAP`) by moving
the rest to linear, and then caps linear at 32 MiB (`LINEAR_HEAP_SIZE_CAP`),
giving the rest back to the application heap. **So for any
`remaining` > 56 MiB: linear = 32 MiB, heap = `remaining` − 32 MiB.** If only
`__ctru_linear_heap_size` is set, the heap gets `remaining` minus that. A
sum larger than `remaining` is a `svcBreak(USERBREAK_PANIC)`.

**Checked in the binaries:**

```
$ arm-none-eabi-nm -A /opt/devkitpro/libctru/lib/libctru.a | grep -E '__ctru_(linear_)?heap_size|__system_allocateHeaps'
libctru.a:allocateHeaps.o:00000000 V __ctru_heap_size
libctru.a:allocateHeaps.o:00000000 V __ctru_linear_heap_size
libctru.a:allocateHeaps.o:00000000 W __system_allocateHeaps
$ arm-none-eabi-nm build-3ds/alephone.elf | grep …          # still the weak defaults: nothing overrides them
0091a8c0 V __ctru_heap_size
0091a8bc V __ctru_linear_heap_size
00566058 W __system_allocateHeaps
$ arm-none-eabi-objdump -d build-3ds/alephone.elf   # in __system_allocateHeaps
  5661a8:  cmp   r3, #25165824   @ 0x1800000       24 MiB heap cap
  5661bc:  cmp   r1, #33554432   @ 0x2000000       32 MiB linear cap
  5661c0:  movhi r1, #33554432
```

This matches stage 4: New 3DS mode `remaining` = 117.9 MiB → 85.9 + 32;
Old 3DS mode 89.2 MiB → 57.2 + 32.

**What uses linear memory** (2 100 KiB at the main menu, `STAGE4.md`):
SDL's `N3DS_VideoInit` calls `gfxInit(GSP_RGBA8_OES, GSP_RGBA8_OES, false)`,
so libctru allocates the framebuffers with `linearAlloc`, double-buffered:
the top screen at 3D width 800x240x4 B x 2 = 1 500 KiB, the bottom
320x240x4 B x 2 = 600 KiB. SDL's audio ring is `linearAlloc(mixlen * 3)` =
3 x 4 KiB (`SDL_n3dsaudio.c:136`). Total 2 112 KiB, which matches the
measurement. Nothing in the engine, OpenAL, libsndfile or `ndsp` calls
`linearAlloc` (grep of the sources), and the software renderer draws into
malloc'd SDL surfaces, so **gameplay should not add linear use**. A future
citro3d (GPU) renderer would change that.

**Proposal:** define `u32 __ctru_linear_heap_size = 8 << 20;` (8 MiB: ~4x
the measured 2.1 MiB) in `a1_3ds_platform.cpp` next to `__stacksize__`. The
application heap then gets `remaining` − 8 MiB: **+24 MiB for malloc** (Old
3DS mode in Azahar: 57 → 81 MiB). Alternative: 16 MiB (+16 MiB). Both
would be checked by the boot log (`linear heap … free`), and a shortage
would show at boot (SDL video/audio init fails), not in the middle of a
game.

**Hardware (2026-10-11):** linear use is 2 112 KiB at boot, at the menu,
in gameplay, in the demo and at exit (30 656 of 32 768 KiB free in every
memory line); malloc peaked at about 25 MiB with a level loaded.

**Decision (2026-10-11): 8 MiB**, as proposed (`PATCHES.md` 25).
`u32 __ctru_linear_heap_size = 8 * 1024 * 1024;` in `a1_3ds_platform.cpp`.
Build: 0 warnings; `nm` shows it as `D` (overrides the weak default).
Azahar, test 06 (a level played) in both modes:

| Mode | Heap | Linear free after the level | malloc free after the level |
| --- | ---: | ---: | ---: |
| New 3DS | 110 428 KiB (was 85 860) | 6 080 of 8 192 KiB | 84 239 KiB |
| Old 3DS | 81 756 KiB (was 57 212) | 6 080 of 8 192 KiB | 55 549 KiB |

(Old 3DS mode needs both `is_new_3ds=false` and `is_new_3ds\default=false`
in Azahar's `qt-config.ini`; with only the first it still boots as a New
3DS.) Hardware check of the new split: next console session.

## 1. Hardware run

### 1a. Dependency tests in Azahar first (2026-10-11)

None of the stage 3 test programs had run on any 3DS target; the stage 0
probe had run only in Azahar. All four were run in Azahar 2126.2 before the
hardware session (`scripts/3ds/run-azahar.sh --no-data --timeout 25 <file>.3dsx`;
each writes `sdmc:/3ds/<name>.log`):

| Program | Log | Azahar |
| --- | --- | --- |
| `build/3ds-probe/aleph-probe.3dsx` (stage 0) | `probe.log` | **RESULT: all passed** (8 PASS) |
| `build/3ds-deps/openal-1.25.2-linktest/openal-loopback-test.3dsx` | `openal-test.log` | **RESULT: all passed** (0 failures, 18 PASS) |
| `build/3ds-deps/libsndfile-1.2.2/test/sndfile-vio-test.3dsx` | `sndfile-test.log` (new) | **RESULT: all passed** (0 failures, 25 PASS) |
| `build/3ds-deps/boost-verify/boost-probe.3dsx` | `boost-probe.log` | 7 of 8 PASS; the 1 FAIL is expected (below) |

Two test harness fixes were needed:

- **sndfile test:** printed only to the console, which cannot be read from
  Azahar or over 3dslink. `scripts/3ds/tests/sndfile-vio-test.c` now also
  writes `sdmc:/3ds/sndfile-test.log` (a `printf` tee under `__3DS__`).
  Rebuilt with the link line recorded in
  `build/3ds-deps/logs/libsndfile-verify.log`: 0 warnings, 541 664 bytes.
- **Boost probe:** crashed after its first check. Azahar logged
  `unmapped Read32 @ 0x07FF6C70 at PC 0x00000000`: the main stack ran below
  the heap at `0x08000000`. That is stage 4's bug 1 (libctru's 32 KiB
  default stack), in the test program. `verify-boost-headers.sh` now gives
  the probe `__stacksize__ = 1 MiB` like the game. Re-run: lockfree,
  property_tree XML/INI, any_range, iostreams, algorithm/tokenizer,
  unordered_map/small_vector/error_code PASS. `FAIL uuid random_generator
  (fresh generators differ): fb135fdb-… vs fb135fdb-…` is item 2's root
  cause, which the probe was written to detect. The game no longer depends
  on it (`PATCHES.md` 14). `std::random_device` itself stays deterministic.

The game links SDL2main, whose `N3DS_Init()` calls `osSetSpeedupEnable(true)`
(`nm`: `osSetSpeedupEnable`, `SDL_main` in the ELF), so a New 3DS runs it at
804 MHz with the L2 cache.

### 1b. Hardware session (done 2026-10-11 in stage 5)

Run in stage 5's hardware session: Phase 0, A and B as planned below,
results in `STAGE5.md` "Hardware session". All four dependency tests
pass on a New 3DS (Boost: the 1 expected FAIL).

Planned:

**Decision** (2026-10-11): stage 5 is controls only, and nothing in it depends
on the hardware numbers. The first console session therefore moves into
stage 5, where a person has to press the buttons anyway. Deferred with it:
the item 4 decision (on a New 3DS malloc has ~72 MiB free at the menu; the
gain matters most on an Old 3DS, which is not available) and item 5
(measure-only; the dialog-stall test needs controls to open a dialog).

**Ready for that session** (no further code needed):

- Phase 0, ftpd: check `sdmc:/3ds/dspfirm.cdc` (if missing, run DSP1 once:
  without it `SDL_OpenAudio` fails and the game is silent) and whether
  `sdmc:/3ds/alephone/` exists.
- Phase A, 3dslink, no scenario data on the card: the four test programs
  in 1a (`$DEVKITPRO/tools/bin/3dslink <file>.3dsx`, each shows `RESULT:`
  and exits on START), then the game
  (`cmake --build build-3ds --target run-3dslink`): expect the "Aleph One
  cannot start" screen, `C++ exceptions: OK`, `launched via 3dslink from …`,
  `argv[0]`, and **`[3ds] entropy: …`** (item 2: is ps:ps allowed under the
  Homebrew Launcher?).
- Phase B: upload `Marathon2/` and the flag files `debug-audio`,
  `debug-entropy` and `debug-screens`, then run the game again and stay
  at the menu for ~1 min. Expect `TIME TO MAIN MENU`, memory lines (`remaining`
  = heap + linear, for item 4), the audio probe (core, stack, callback
  load, late callbacks; items 3 and 5; listen during the 8-sound burst),
  `debug-entropy: 3 of 3`. Exit with HOME → Close (the `exit` line
  carries the final audio figures). Fetch `log.txt`, `screen-*.bmp` and the
  test logs through ftpd.

## 9. devkitPro bug report (`3dsx.ld`)

**Upstream status** (2026-10-11): unreported and unfixed. The installed
`3dsx.ld` is byte-identical to `devkitarm-crtls` `master`. The last
`.ARM.exidx` change there (`5d07c23`, 2023) renamed the section and left the
symbols outside. A GitHub search of the devkitPro organisation for `exidx` /
`__exidx_start` found only buildscripts#4 (2015), which had a different
cause and was fixed in r45. `devkitarm-crtls` accepts issues.

**Reproducer** (`scripts/3ds/tests/exidx-repro/`): `repro.cpp` throws and
catches an `int`; `pad.s` is a 2-byte `.ARM.extab` input linked last
(referenced, so `--gc-sections` keeps it), which forces the misalignment
deterministically. `FIXED=1 build.sh` links the same objects with
`3dsx.ld.patch` applied (selected with `-B`, since the spec file uses
`-T 3dsx.ld%s`). Nothing in devkitPro is modified.

| Build | `.ARM.exidx` | `__exidx_start` | Azahar 2126.2 |
| --- | --- | --- | --- |
| stock `3dsx.ld` | `0x00121abc` | `0x00121aba` | `RESULT: FAILED (std::terminate called; …)` |
| patched (symbols inside the section) | `0x00121abc` | `0x00121abc` | `RESULT: passed (caught 42)` |

Survey of devkitARM r68's other linker scripts: `ds_arm7.ld` already
defines the symbols inside the section; eight others share the pattern
(not tested). `ds_arm9.ld` also has the output section named `ARM.exidx`
without the dot.

**Draft:** `docs/3ds/devkitpro-3dsx-ld-report.md` (title, environment,
reproducer inline, expected/actual, patch, workaround). **Not filed**; the
user files it.

## 7. Cosmetics

**Decision:** fix `A1_DISPLAY_PLATFORM` now. Defer the player name: reading
the console profile name needs CFGU (`CFGU_GetConfigInfoBlk2`, block
0x000A0000, UTF-16), and only hardware can verify it, so it moves to stage 5
together with the hardware session. "Bob User" stays the default until then.
**2026-10-11:** deferred again by the user to the next console session.

**Platform name** (`PATCHES.md` 15): `alephversion.h` gets an
`A1_PLATFORM_3DS` branch: `A1_DISPLAY_PLATFORM "Nintendo 3DS"`,
`A1_UPDATE_PLATFORM "source"`. Build: 0 warnings. Azahar log:
`Aleph One Nintendo 3DS 2026-09-30 1.11.1` (was `Unknown`). Non-3DS check:
3 TUs byte-identical in both volumes.

**New tool:** `scripts/3ds/host-baseline/ppcheck.sh` (with
`ppcheck-in-container.sh`) runs the non-3DS preprocessor check in one
command. Re-running item 2's check with it gave the same result as the
manual run.

**For stage 5 (found along the way):** SDL 2.32.10 has a built-in game
controller mapping named "Nintendo 3DS" (`strings` on the ELF: a/b/x/y,
d-pad, L/R, ZL/ZR, circle pad and C-stick as axes, start, back), so
`SDL_GameController` should work without a custom mapping.

## 5. Audio stalls and OpenAL CPU cost

**Deferred** with the hardware session (1b). The measurement is ready: the
audio probe (item 3) logs each interval's average/maximum callback time
against the 23.22 ms buffer, the number of late callbacks (> 1.5 buffers
apart), and the longest gap. `debug-audio` adds a burst of 8 sounds and a
report every 5 s. The dialog-stall question (do the `yield()` busy-waits
starve the mixer?) needs controls to open a dialog, so it belongs after
stage 5's input work. The thread's core decides the answer:

- **core 0** (Azahar): the audio thread (priority 0x19) preempts the main
  thread (0x30), so busy-waits cannot starve it, but mixing takes CPU from
  the game;
- **core 1** (if `APT_SetAppCpuTimeLimit(30)` succeeds on hardware): the
  app gets at most 30% of that core. In Azahar mixing silence already costs
  ~30% of the buffer time, and 8 voices peaked at 70%, so this case could
  stutter.

Azahar figures are emulated timing and only relative.

**Hardware (2026-10-11, New 3DS, audio thread on core 1):**

| Situation | Callback avg / max per 23.22 ms buffer | Late (> 1.5 buffers) |
| --- | --- | --- |
| Main menu, silence | 6.1 ms (26%) / 7.5 ms | 0 |
| 8-sound burst | 8.0 ms (34%) / 12.5 ms | 0 |
| Demo and gameplay | 9-11 ms (40-48%); highest 5 s average 14.7 ms; highest single 26.2 ms | 0 |
| **START dialog open ~100 s** (the dialog-stall test) | 6.1 ms (26%) / 7.5 ms | **0** |
| HOME, keyboard applet, lid | paused by design (gaps of 1.0-175.8 s) | counted, expected |
| Game cancelled (START > YES), a game start/end | | **1 gap of 3.9 s, 1 of 2.8 s** |

**Answers:** the `yield()` busy-waits of dialogs do not starve the mixer
(core 1 runs it whatever the main thread does). Mixing costs a quarter of
the buffer time at rest and under half in gameplay; one callback in about
45 minutes took longer than a buffer, without a late callback (SDL queues 3
buffers). The 30% core-1 limit was not a problem. The only stalls are the
two gaps at game/menu transitions, during fades: recorded, not fixed.

## Summary

| # | Item | Status |
| --- | --- | --- |
| 1 | Hardware run | **Done** 2026-10-11 in stage 5's hardware session (`STAGE5.md`): the four dependency tests pass on a New 3DS (Boost: the expected FAIL), the game boots to the menu in 41.7-49.4 s. Azahar runs in 1a. Test harness fixes: sndfile log file, Boost probe stack |
| 2 | Identical image-cache names | **Fixed** (`PATCHES.md` 14): ps:ps entropy with a tick/clock fallback. Verified in Azahar (ps:ps, forced fallback, and a control build reproducing the bug); non-3DS check passed. **Hardware: ps:ps works under the Homebrew Launcher**; 3 of 3 distinct names |
| 3 | Audio thread stack | **Measured, kept at 80 KiB**: 9 KiB peak with 8 voices in Azahar and on hardware (core 1 on hardware). The SDL hint cannot change it. Port-only probe `audio_probe.cpp`. **Found:** audio never ran in Azahar (no `dspfirm.cdc`); `run-azahar.sh` now stages an HLE placeholder |
| 4 | Linear heap size | **Decided: 8 MiB** (`PATCHES.md` 25). Hardware: 2.1 MiB used everywhere, gameplay included. +24 MiB for malloc (Old 3DS mode 57 → 81 MiB); Azahar both modes OK; hardware check next session |
| 5 | Audio stalls / CPU | **Measured on hardware**: 26% at rest, 40-48% in gameplay, 0 late callbacks; dialogs do not starve the mixer. Two 2.8-3.9 s gaps at game/menu transitions (recorded) |
| 6 | Early fatal-alert screen | **Verified** in Azahar (scenario without Map/Shapes). **Fixed:** the top screen was black before the main window; it now shows the pointer. Regression of the pre-SDL path OK. **Hardware:** screen shown, A exits |
| 7 | Cosmetics | **Platform name fixed** (`PATCHES.md` 15, "Nintendo 3DS"). Player name from CFGU **deferred** to the next console session (user, 2026-10-11) |
| 8 | Branch backup | **Done**, refreshed after each commit; latest `~/Documents/Aleph_3ds-backups/3ds-port-2026-10-11-015e63c.bundle` (stage 5), restore tested |
| 9 | devkitPro report | **Drafted, not filed**: `docs/3ds/devkitpro-3dsx-ld-report.md`; reproducer `scripts/3ds/tests/exidx-repro/` fails with the stock script and passes with the fix (Azahar). Unreported upstream; 8 more devkitARM scripts share the pattern |
| 10 | Emulator home | **Done**: `~/Applications/Azahar-2126.2` (still the latest stable); config paths rewritten; `run-azahar.sh` finds it without `AZAHAR=`; /tmp copy removed |

**Files changed:** `Source_Files/Files/WadImageCache.cpp`,
`Source_Files/CSeries/a1_filesystem.h`, `Source_Files/Misc/alephversion.h`
(fenced; `PATCHES.md` 14-15); `ports/3ds/include/a1_3ds_platform.h`,
`ports/3ds/source/a1_3ds_platform.cpp`, new `ports/3ds/source/selftest.cpp`
and `ports/3ds/source/audio_probe.cpp`, `ports/3ds/CMakeLists.txt`;
`scripts/3ds/run-azahar.sh`, `scripts/3ds/verify-boost-headers.sh`,
`scripts/3ds/tests/sndfile-vio-test.c`, new `scripts/3ds/tests/exidx-repro/`
and `scripts/3ds/host-baseline/ppcheck{,-in-container}.sh`; docs. Build: 0
warnings.

**Debug flag files** in `sdmc:/3ds/alephone/` (new in this round):
`debug-entropy` (item 2 self-test), `debug-audio` (sound burst + audio
reports every 5 s); `debug-screens` as before.
