# Aleph One 3DS port: source patches

Every change to upstream source made for the 3DS build, with its rationale.
Changes to engine sources (`Source_Files/`) are guarded by
`#ifdef A1_PLATFORM_3DS` (defined only by `ports/3ds/CMakeLists.txt`), so
non-3DS builds preprocess as before. Checked in Docker with the
`aleph-host-build` and `aleph-host-build-nonet` flags and `config.h`
(`g++ -E -P`, `HEAD` vs working tree): `vbl.cpp` and `csalerts_sdl.cpp`
differ only in `__LINE__` values inside logging/assert macros (6 and 12
lines, shifted by the added lines); with those normalised the output is
identical, and both files pass `-fsyntax-only` with 0 diagnostics. Earlier stages' feature switches
(`A1_NO_NETWORK`, `A1_FS_STD`, `AudioBackend`) are recorded in `STAGE2.md`
and `STAGE3-PREREQS.md`, not repeated here.

Recorded 2026-10-10 on branch `3ds-port`, stage 3 (first `.3dsx` link);
entries 5-13 were added in stage 4 (boot to the main menu, `STAGE4.md`),
14-15 with the stage 5 prerequisites, 16-22 in stage 5 (controls, `STAGE5.md`),
23-25 in stage 5's hardware session (default focus, linear heap), 26-33 in
stage 6 (audio, `STAGE6.md`), 34-35 in the performance stage (`PERF.md`),
36-40 in stage 7 (the bottom screen, `STAGE7.md`), 41-42 and an update to 19
in the sprint after it (`NEXT-SPRINT.md`), 49 in stage 10 (`CITRO3D_DESIGN.md`; 45-48 are
reserved there for the engine hooks of M0, M2, M6 and M8).

## Summary

| # | File | Kind | Functionality lost on 3DS |
| --- | --- | --- | --- |
| 1 | `Source_Files/Misc/vbl.cpp` (3 sites) | type fix | none |
| 2 | `Source_Files/CSeries/csalerts_sdl.cpp` | **stub** | hyperlinks do not open a browser |
| 3 | OpenAL Soft 1.25.2 `common/altypes.hpp` (third-party, applied by `scripts/3ds/build-openal.sh`) | type fix | none |
| 4 | `ports/3ds/source/sysbase_hooks.c` (new port file, no upstream edit) | missing platform hooks | pthread-key destructors are not run (**stub**) |
| 5 | `Source_Files/CSeries/cspaths_sdl.cpp` | 3DS path branch | none (`$ALEPHONE_DATA` list separator is `;`) |
| 6 | `Source_Files/main.cpp` | boot hooks | none |
| 7 | `Source_Files/shell.cpp` (7 sites) | boot milestones (log only) | none |
| 8 | `Source_Files/Misc/interface.cpp` | time-to-menu hook (log only) | none |
| 9 | `Source_Files/Misc/preferences.cpp` | prefs clamp + hidden dialog items | screen size, windowed mode, renderer and SDL driver are fixed (**by design**) |
| 10 | `Source_Files/RenderOther/screen.cpp` | one screen mode | none |
| 11 | `Source_Files/CSeries/csalerts_sdl.cpp` | early alerts | pre-window warnings go to the log only |
| 12 | `ports/3ds/source/exidx_fix.c` (new port file) | **toolchain bug** (devkitARM `3dsx.ld`) | none |
| 13 | `ports/3ds/source/a1_3ds_platform.cpp`: `__stacksize__` (new port file) | main stack size | none |
| 14 | `Source_Files/Files/WadImageCache.cpp`, `Source_Files/CSeries/a1_filesystem.h` | entropy source | none |
| 15 | `Source_Files/Misc/alephversion.h` | platform name | none |
| 16 | `Source_Files/shell.cpp` | input init; HOME > Close quits without a dialog | none |
| 17 | `Source_Files/main.cpp` | "main loop ended" milestone (log only) | none |
| 18 | `Source_Files/Input/mouse_sdl.cpp` | touch screen mode follows mouse aiming | none |
| 19 | `Source_Files/Input/joystick_sdl.cpp` | C-Stick response curve, turn acceleration, slower pitch | linear stick response (**by design**) |
| 20 | `Source_Files/Misc/preferences.cpp` | 3DS default bindings, always run, "Touch Screen Aiming" label | upstream controller defaults (**by design**) |
| 21 | `Source_Files/Misc/sdl_widgets.cpp`, `sdl_widgets.h` | 3DS button names; text fields use the system keyboard | cursor positioning by click in text fields |
| 22 | `Source_Files/Misc/interface.cpp` | HOME > Close skips quit screens, fades, scenario reload | none |
| 23 | `Source_Files/Misc/interface.cpp` | main menu starts with BEGIN NEW GAME highlighted | none |
| 24 | `Source_Files/Misc/sdl_dialogs.cpp` | dialogs focus their first widget when they set no default | none |
| 25 | `ports/3ds/source/a1_3ds_platform.cpp`: `__ctru_linear_heap_size`, `__system_allocateHeaps()` (port file) | linear heap at least 8 MiB, malloc the rest up to the 96 MiB heap area | none |
| 26 | `Source_Files/Sound/AudioBackend.h` | the 3DS AudioBackend | none |
| 27 | `Source_Files/Sound/Decoder.cpp` | Ogg Vorbis / MP3 decoders | Ogg Opus, FLAC (not decoded on 3DS) |
| 28 | `Source_Files/Sound/SoundManager.h`, `SoundManager.cpp` | 3DS audio parameters | HRTF, 3D sounds, surround channel layouts (**by design**) |
| 29 | `Source_Files/Misc/preferences.cpp` | sound dialog hides Channels, 3D Sounds, HRTF | as 28 |
| 30 | `Source_Files/Sound/AudioPlayer.cpp`, `OpenALManager.h` | buffers filled per mixer pass | none |
| 31 | `Source_Files/Sound/MusicPlayer.cpp`, `MusicPlayer.h` | endless recursion on an empty segment | none |
| 32 | `Source_Files/Misc/interface.cpp`, `Source_Files/XML/XML_LevelScript.cpp` | three music busy-waits sleep; the fade waits are bounded | none |
| 33 | `Source_Files/shell.cpp` | in-game frame pacing sleeps instead of spinning | none |
| 34 | `RenderOther/screen.cpp`, `Misc/interface.cpp`, `GameWorld/marathon2.cpp`, `shell.cpp` | frame profiler call sites (`A1_PROFILE` builds only) | none |
| 35 | `Source_Files/RenderOther/screen.cpp` | direct present into the framebuffer | none |
| 36 | `Source_Files/RenderOther/screen.cpp` | terminal rectangles and fonts fitted to the screen | none (networked games keep the scenario's layout) |
| 37 | `Source_Files/RenderOther/screen.cpp`, `screen_shared.h` | 400x240 in-game screen, HUD on the bottom screen | none |
| 38 | (removed) | touch button action flags; removed with the buttons, `vbl.cpp` is back to its stage 3 state | none |
| 39 | `Source_Files/Misc/sdl_dialogs.cpp` | in-game dialogs at 640x480 | none |
| 40 | `Source_Files/Misc/preferences.cpp`, `preferences.h` | Bottom Screen preference | none |
| 41 | `shell.cpp`, `Misc/interface.cpp`, `Files/game_wad.cpp`, `GameWorld/marathon2.cpp`, `XML/QuickSave.cpp`, `RenderMain/shapes.cpp`, `Misc/preferences.cpp`, `Sound/SoundManager.cpp` | timed log marks in boot, level loads, saves and `finish_game()` (log only) | none |
| 42 | `Source_Files/RenderMain/shapes.cpp` | a collection is read with one read and parsed from memory | none |
| 43 | `Source_Files/Sound/SoundFile.cpp` | a sound header, and a sound's samples, read with one read each and parsed from memory | none |
| 44 | `Source_Files/RenderOther/screen_shared.h` | the map zoom keys zoom the bottom screen's map while the main map is closed | none |
| 49 | `ports/3ds/source/video_3ds.cpp`, `a1_3ds_platform.cpp`, `profile_3ds.cpp`, `include/a1_3ds_profile.h`, `CMakeLists.txt`, `render_c3d/` (new port files, no upstream edit; `A1_RENDER_C3D=ON` builds only) | stage 10 P1: citro3d owns the top screen, GPU present | none (the CPU present stays as `debug-c3d` `present cpu` and in the default build) |

The complete list of stubs and lost functionality, including items not tied
to a source patch (libsndfile codecs, Lua/newlib behaviour), is the table in
`STAGE3.md`, "Stubs and lost functionality".

Only these files differ from upstream for the 3DS build. The first full
build (`cmake --build build-3ds -- -k`) failed in exactly one engine
translation unit (`vbl.cpp`, patch 1), and the first link failed on exactly
one symbol (`execlp`, patch 2). All 187 engine TUs compile with 0 warnings
at the default warning level (the build, like automake's, passes no
`-Wall`). Item 4 links fine without it; the review found it by auditing the
link map for libsysbase stubs.

## 1. `Misc/vbl.cpp`: `int` where the API takes `int32`

**Error** (3 sites):

```
vbl.cpp:577:36: error: cannot bind non-const lvalue reference of type 'int32&' {aka 'long int&'} to a value of type 'int'
  577 |                 FilmFile.GetLength(file_length);
vbl.cpp:1074:30: error: call of overloaded 'ValueToStreamBE(uint8*&, int)' is ambiguous
 1074 |                 ValueToStream(S, static_cast<int>(ObjPtr->extension_type));
vbl.cpp:1090:30: error: no matching function for call to 'StreamToValueBE(uint8*&, int&)'
 1090 |                 StreamToValue(S, extension_type);
```

**Cause:** `int32` is SDL's `Sint32`, i.e. `int32_t` (`CSeries/cstypes.h`), and on arm-none-eabi
newlib `int32_t` is `long`, not `int`. `OpenedFile::GetLength(int32&)` and
the `Packing.h` overloads (`int16`, `uint16`, `int32`, `uint32` only) then
have no exact match for a plain `int`. On every upstream platform `int32_t`
is `int`, so the code compiled there. OpenAL Soft (patch 3) and libsndfile
(`alac.c`, see `STAGE3.md`) hit the same newlib ABI difference.

**Change:** declare the local as `int32` (and cast to `int32`) on the 3DS:

```diff
+#ifdef A1_PLATFORM_3DS // int32 is long on newlib ARM; GetLength takes int32&
+		int32 file_length;
+#else
 		int file_length;
+#endif
 		FilmFile.GetLength(file_length);
```

```diff
+#ifdef A1_PLATFORM_3DS // no int overload when int32 is long; same 4 bytes
+		ValueToStream(S, static_cast<int32>(ObjPtr->extension_type));
+#else
 		ValueToStream(S, static_cast<int>(ObjPtr->extension_type));
+#endif
```

```diff
+#ifdef A1_PLATFORM_3DS // no int& overload when int32 is long; same 4 bytes
+		int32 extension_type;
+#else
 		int extension_type;
+#endif
 		StreamToValue(S, extension_type);
```

**Why this is behaviour-preserving:** `int` and `long` are both 32-bit on
the 3DS, so the film length check and the on-disk recording extension header
(`SIZEOF_recording_extension_header` = 8: `uint32 length` + 4-byte (`int32`)
type) keep the same values and byte layout. The review compiled the patched
`vbl.cpp` to `-O2` assembly and compared it with the same file built with
`int32_t` forced to `int`: identical after normalisation. On other platforms
the preprocessor output is unchanged apart from `__LINE__` values.

**Upstreaming note:** an unguarded `int32` would be identical on every
platform upstream supports (where `int32 == int`) and is arguably the correct
type; the guard is here only because this stage's rule is to fence all 3DS
changes.

## 2. `CSeries/csalerts_sdl.cpp`: `system_launch_url_in_browser` stubbed (STUB)

**Error** (link):

```
ld: Source_Files/CSeries/csalerts_sdl.cpp:128:(.text._Z28system_launch_url_in_browserPKc+0x58): undefined reference to `execlp'
```

**Cause:** the non-Windows branch launches a browser with
`fork()` + `execlp("xdg-open" / "sensible-browser")` + `wait()`. The 3DS has
no processes; newlib provides no `execlp`. (`fork` and `wait` do link,
against newlib/libsysbase stubs that only fail, so without the `execlp` call
this branch would already do nothing at run time.)

**Change:** a 3DS branch that does nothing:

```diff
 #if defined(__WIN32__)
 	ShellExecuteW(NULL, L"open", utf8_to_wide(url).c_str(), NULL, NULL, SW_SHOWNORMAL);
+#elif defined(A1_PLATFORM_3DS)
+	// Stub: the 3DS cannot fork/exec a browser (newlib has no execlp).
+	// launch_url_in_browser() has already printed the URL to stderr.
+	(void) url;
 #else
 	pid_t pid = fork();
```

**Functionality lost:** `launch_url_in_browser()` (same file, just below)
still prints `System launch url: <url>` to stderr, but no browser opens.
Callers:

- `Misc/sdl_widgets.cpp:406`, `w_hyperlink::prochandler` (clickable links,
  e.g. in the About dialog, `Misc/interface.cpp:2264,2270`): no browser
  opens, but the handler still runs `toggle_fullscreen(false)` first, so a
  click leaves fullscreen (screen mode change) and redraws the dialog.
  Unchanged upstream behaviour around the stub; a 3DS guard there is a
  possible follow-up.
- `Misc/preferences.cpp:672`, the metaserver account "settings" link: not
  reachable, because the INTERNET button is hidden with `A1_NO_NETWORK`
  (`STAGE2.md`).

**Possible follow-up:** open the URL in the 3DS Internet Browser through the
APT system-applet interface in libctru. Not attempted here: it needs
hardware testing, and the stage goal is linking.

## 3. OpenAL Soft 1.25.2 (third-party): `al::make_strict` for `int`

Not an engine change. `scripts/3ds/build-openal.sh` applies
`scripts/3ds/patches/openal-soft-1.25.2/0001-altypes-make_strict-for-int-when-int32_t-is-long.patch`
to the downloaded tarball.

**Error** (every OpenAL TU):

```
common/altypes.hpp:114:7: error: no type named 'type' in 'struct al::make_strict<int>'
common/altypes.hpp:508:39: error: 'sys_uint' does not name a type
```

**Cause:** the same `int32_t == long` difference. `al::make_strict<>` has
specializations only for the `int*_t`/`uint*_t` types, so `int` and
`unsigned` have none on newlib ARM.

**Change:** add `make_strict<int>`/`make_strict<unsigned>` specializations
(mapping to `i32`/`u32`) constrained with a C++20 `requires` clause, so they
exist only when `int`/`unsigned` differ from `int32_t`/`uint32_t` but have
the same size. On every platform upstream supports they are inactive.
Upstream master still had the issue on 2026-10-10.

## 4. `ports/3ds/source/sysbase_hooks.c`: libsysbase hooks libctru lacks

A new port file (linked into `alephone` by `ports/3ds/CMakeLists.txt` and
into the OpenAL link test by `scripts/3ds/build-openal.sh`); no upstream
source is edited.

**Problem 1, pthread keys:** devkitARM's libsysbase implements
`pthread_key_create`/`pthread_setspecific`/`pthread_getspecific`/
`pthread_key_delete` by calling weak hooks `__syscall_tls_create/set/get/delete`
and returns ENOSYS (88) when they are absent. libctru 2.7.0 provides the
`__syscall_thread_*` hooks but none of the TLS ones: in the stage 3 ELF all
four were weak-undefined. Consequences, none visible at build time:

- OpenAL Soft: `al::Context` has a `tss<ALenum> mLastThreadError`
  (`alc/context.hpp:96`); `tss_create` → `pthread_key_create` fails, the
  constructor throws `std::runtime_error` (`common/althreads.h:114`), and
  `alcCreateContext` returns NULL. `OpenALManager::OpenDevice` would then
  fail and the game would have **no sound at all**.
- libstdc++: `__eh_globals_init` gets no key, so all threads share one
  exception-handling state.

**Problem 2, `sched_yield`:** libsysbase's `sched_yield` is a weak stub that
only sets ENOSYS, so `std::this_thread::yield()` (engine `yield()`,
`CSeries/csmisc_sdl.cpp:73`, and OpenAL's `StopEventThrd`) did nothing.

**Change:** strong definitions of the four hooks over a `__thread` table of
64 slots (keys carry a generation tag so a re-created key reads NULL in
every thread, as POSIX requires), and `sched_yield()` as
`svcSleepThread(1)`: a minimal sleep that forces a reschedule, the same
yield SDL2's N3DS port uses (`src/thread/n3ds/SDL_syssem.c`). A 0 ns sleep
is not used because its Horizon semantics are undocumented.
After the change `pthread_key_create` branches to `__syscall_tls_create`
(disassembly) and `sched_yield` resolves to the shim.

**Limitations:** key destructors are accepted but never run (libctru has no
thread-exit hook), so per-thread values are abandoned when a thread ends:
libstdc++'s 12-byte per-thread exception globals (and any exception still
held in them) leak once per exiting thread that used exceptions. Running
destructors would need a `__syscall_thread_create` trampoline and would
still miss threads SDL creates with `threadCreate` directly;
at most 64 keys exist at once (`EAGAIN` after that). Unit-tested on the host
(per-thread values, deleted keys rejected, re-created keys read NULL,
64-key limit, double delete rejected; an independent review repeated this
under ThreadSanitizer with 8 threads racing create/delete) and checked in
the linked ELF (TLS is set up by libctru for the main thread and for every
`threadCreate` thread, including SDL's and libsysbase's; the atomics compile
to `ldrex`/`strex` with `dmb`); not yet run on a 3DS.

## Stage 4 engine hooks (5-11)

All guarded by `#ifdef A1_PLATFORM_3DS`; the 3DS side lives in
`ports/3ds/source/a1_3ds_platform.cpp` behind `ports/3ds/include/a1_3ds_platform.h`
(no `<3ds.h>` in engine TUs). **Non-3DS check** (2026-10-11): each of the 7
files was preprocessed with its own automake compile line from the
`aleph-host-build` and `aleph-host-build-nonet` volumes, `HEAD` vs working
tree, from the same path: `shell.cpp`, `cspaths_sdl.cpp` and
`interface.cpp` are byte-identical; `main.cpp`, `csalerts_sdl.cpp`,
`preferences.cpp` and `screen.cpp` differ only in `__LINE__` values inside
logging macros (4/12/20/14 lines).

5. **`cspaths_sdl.cpp`**: a `#elif defined(A1_PLATFORM_3DS)` branch of
   `get_data_path()` before the Linux one (which would derive everything
   from the unset `$HOME`, `AUDIT.md` f). User paths under
   `sdmc:/3ds/alephone`, `kPathDefaultData` = the scenario folder,
   `kPathBundleData` = `SDL_GetBasePath()` (`romfs:/`). The bundle path is
   the hook macOS uses for its app bundle, so `shell.cpp` puts the romfs
   first in `data_search_path` without changes. `get_path_list_separator()`
   returns `;`, since `:` would split `sdmc:/`.
6. **`main.cpp`**: `a1_3ds::boot_begin(argc, argv)` first (it may replace
   an empty argv, see `STAGE4.md`, and returns `false` after the error
   screen), a milestone after `initialize_application()`, and the error
   screen in both `catch` blocks before `shutdown_application()`.
7. **`shell.cpp`**: `a1_3ds::boot_mark()` after `SDL_Init`, the scenario
   chooser, the base MML, fonts + plugins, preferences, sound and screen
   initialisation. Logging only.
8. **`interface.cpp`**: `a1_3ds::main_menu_displayed()` at the end of
   `display_main_menu()` (it reports once).
9. **`preferences.cpp`**: `validate_graphics_preferences()` pins
   `400x240`, `auto_resolution=false`, `fullscreen=true`,
   `high_dpi=false`, software renderer, SDL driver "None" (= SDL's
   `software` renderer) and a valid bit depth, whatever a prefs file says.
   The graphics dialog no longer shows Rendering System, Screen Size,
   Windowed Mode or the "built without OpenGL" note; the software options
   dialog no longer shows Acceleration (Default/None/Direct3D/OpenGL). The
   hidden widgets are still created (as the existing non-macOS High DPI
   toggle is), so the accept code reads unchanged values. RENDERING OPTIONS
   opens the software options directly: `rendering_options_dialog_demux`
   looks the renderer up among the dialog's widgets and would find none.
10. **`screen.cpp`**: `Screen::Initialize` replaces the mode list with the
    single 400x240 mode. The desktop is smaller than 640x480, so the list
    would otherwise start with the 1600x900 fallback (the auto-resolution
    size, and a choice in the hidden Screen Size popup).
11. **`csalerts_sdl.cpp`**: before the main window exists, `alert_user()`
    calls `SDL_ShowSimpleMessageBox`, which SDL's N3DS driver does not
    implement; a fatal alert then exited with a black screen. On 3DS fatal
    alerts use the error screen and other alerts go to stderr (the log).

## 12. `ports/3ds/source/exidx_fix.c`: C++ exceptions were broken (toolchain bug)

**Symptom** (Azahar, first runs of the engine): the game stopped silently
right after `SDL_Init`. With a terminate handler installed, it reported that
`std::terminate()` was called, and then that even a rethrow inside the
handler's own `try { … } catch (...)` was not caught.

**Cause:** devkitARM r68's `arm-none-eabi/lib/3dsx.ld` sets the unwind
table bounds outside the output section:

```ld
.ARM.extab : { *(.ARM.extab* .gnu.linkonce.armextab.*) } : rodata
__exidx_start = .;
.ARM.exidx : { *(.ARM.exidx* .gnu.linkonce.armexidx.*) } : rodata
__exidx_end = .;
```

`.ARM.exidx` is 4-byte aligned, but Aleph One's `.ARM.extab` ends on a
2-byte boundary (`0x00742aa8 + 0x1fe96 = 0x0076293e`), so `__exidx_start`
was `0x0076293e` while the table starts at `0x00762940`. libgcc's
`get_eit_entry()` binary-searches 8-byte entries from `__exidx_start`; every
lookup reads misaligned garbage, so **every `throw` ends in
`std::terminate()`**. Aleph One throws routinely while starting (MML, prefs,
file probing). The stage 0 probe passed only because its `.ARM.extab`
happened to end aligned. GNU ld's default ARM scripts define the symbols
inside the section, after alignment.

**Change:** a strong `__gnu_Unwind_Find_exidx()` (libgcc calls it when it
is defined; the weak reference is meant for dynamic loaders) returning
`__exidx_start` rounded up to 4 and the matching entry count. Correct
whether or not the symbol happens to be aligned, and nothing in devkitPro is
edited. `boot_begin()` throws and catches once and logs
`C++ exceptions: OK`, so a regression shows up in the first lines of
`log.txt`.

**Upstream:** worth reporting to devkitPro (`devkitarm-crtls`); the fix is
to move both assignments inside `.ARM.exidx { … }`.

## 13. Main-thread stack: `u32 __stacksize__ = 1 MiB`

libctru's `initSystem()` carves the main stack from the start of the heap
(`sp = heap start + __stacksize__`), and its weak default is 32 KiB. Aleph
One overflowed it inside `initialize_application()`: SP went to
`0x07FF7740`, below the heap at `0x08000000`, into unmapped memory, and the
next return jumped to address 0 (Azahar: "Undefined Instruction", PC 0).
`a1_3ds_platform.cpp` defines `__stacksize__ = 1024 * 1024`. The stack is
painted at boot and the deepest use is logged at every milestone: **68 KiB
at the main menu**, so 1 MiB leaves room for gameplay (Lua, rendering), which
has not been measured yet. The 1 MiB comes out of the heap and is shown
separately in the memory log.

## 14. Random names: `WadImageCache.cpp`, `a1_filesystem.h`

Added 2026-10-11 (`STAGE5-PREREQS.md` item 2; the problem was found in
`STAGE3.md`, risk 3).

**Problem:** devkitARM's `std::random_device` has no entropy source: it is
mt19937 with the default seed 5489, so it returns the same numbers in every
run and every thread. Two engine sites depend on it:

- `WadImageCache::image_to_new_name` constructs a
  `boost::uuids::random_generator` (ChaCha20 seeded from `random_device` in
  Boost 1.92) per call, so **every cached image got the same name**,
  `fb135fdb-2482-4d31-aa8c-351caf595b56`. Each new save-game preview
  overwrote the previous one's file, while `Cache.ini` mapped every
  descriptor to it.
- `a1fs::unique_path` (used by `FileSpecifier::SetTempName`) seeds one
  `thread_local` mt19937 per thread from `random_device`, so temporary names
  repeat from run to run (`4a69d98a de42ae2d bcbe0dce` every time).

**Change:** the port provides `a1_3ds::random_bytes()` and a
UniformRandomBitGenerator `a1_3ds::Urng` (`a1_3ds_platform.h`). Bytes come
from the ps:ps service (`PS_GenerateRandomBytes`, after `psInit()`), or, if
that fails, from a splitmix64 mix of `svcGetSystemTick()`, `osGetTime()` and
an atomic counter. The boot log says which (`[3ds] entropy: ...`).

```diff
+#ifdef A1_PLATFORM_3DS // std::random_device has no entropy: same UUID every time
+	a1_3ds::Urng urng;
+	boost::uuids::basic_random_generator<a1_3ds::Urng> gen(urng);
+#else
 	boost::uuids::random_generator gen;
+#endif
```

```diff
+#ifdef A1_PLATFORM_3DS // std::random_device gives the same seed in every run
+	static thread_local std::mt19937 rng{a1_3ds::Urng{}()};
+#else
 	static thread_local std::mt19937 rng{std::random_device{}()};
+#endif
```

plus `#include "a1_3ds_platform.h"` under the same guard in both files.
`basic_random_generator` still sets the UUID version (4) and variant bits.

**Verification:** a `debug-entropy` flag file (`ports/3ds/source/selftest.cpp`)
puts three images through the real `WadImageCache` at the main menu, logs
their names and three `unique_path` names, and removes them again. Results
are in `STAGE5-PREREQS.md` item 2: Azahar with ps:ps, with the fallback
forced, and a control build without this patch (collision reproduced).
Hardware: see item 1 there.

**Non-3DS check** (2026-10-11, `aleph-host-build` and
`aleph-host-build-nonet`, automake compile line, `g++ -E -P`, `HEAD` vs
working tree from the same path): `FileHandler.cpp` (includes
`a1_filesystem.h`, calls `unique_path`) is byte-identical;
`WadImageCache.cpp` differs only in two `__LINE__` values inside `logError`
(323→332, 384→393). Both pass `-fsyntax-only` with 0 diagnostics.

## 15. `Misc/alephversion.h`: platform name

Added 2026-10-11 (`STAGE5-PREREQS.md` item 7; `STAGE3.md` row 6). The
platform chain ended in `"Unknown"` for the 3DS, so the version string read
`Aleph One Unknown 2026-09-30 1.11.1`. A 3DS branch before the fallback:

```diff
+#elif defined (A1_PLATFORM_3DS)
+#define A1_DISPLAY_PLATFORM "Nintendo 3DS"
+#define A1_UPDATE_PLATFORM "source"
 #else
```

`A1_UPDATE_PLATFORM` stays `"source"` like Linux and the BSDs (the update
check is network code, compiled out with `A1_NO_NETWORK`). Displayed in the
version string (log, About) and `$appPlatform$` in MML strings. Azahar log:
`Aleph One Nintendo 3DS 2026-09-30 1.11.1`.

**Non-3DS check** (`scripts/3ds/host-baseline/ppcheck.sh`, both volumes):
`shell.cpp`, `Misc/interface.cpp` and `CSeries/csstrings.cpp` are
byte-identical; 0 diagnostics.

**Checking future patches:** `scripts/3ds/host-baseline/ppcheck.sh
"<changed files>" "<TUs>"` automates the method above: automake compile
line, `g++ -E -P` with `HEAD`'s and the working tree's files from the same
path, `cmp`, `-fsyntax-only`, in `aleph-host-build` and
`aleph-host-build-nonet`.

## Stage 5 input hooks (16-22)

Added 2026-10-11 (`STAGE5.md`). All guarded by `#ifdef A1_PLATFORM_3DS`;
the 3DS side is `ports/3ds/source/input_3ds.cpp` behind
`a1_3ds_platform.h`. Port-only, no engine edit: the touch screen event
filter, the APT hook, the bottom-screen legend, and `-Wl,--wrap` on
`SDL_StartTextInput` (SDL's N3DS driver opens a blocking keyboard as soon as
a text field gains focus) and on six libctru HID functions (test scripts,
keyboard debounce, stick priming).

**Non-3DS check** (2026-10-11, `ppcheck.sh`, `aleph-host-build` and
`aleph-host-build-nonet`): `shell.cpp`, `Misc/interface.cpp`,
`Input/mouse_sdl.cpp`, `Input/joystick_sdl.cpp`, `Misc/sdl_widgets.cpp`
and `Misc/sdl_dialogs.cpp` (for `sdl_widgets.h`) are byte-identical;
`main.cpp` and `Misc/preferences.cpp` differ only in `__LINE__` values
inside logging macros (4 and 20 lines, inspected). 0 diagnostics.

16. **`shell.cpp`**: `a1_3ds::input_init()` after `initialize_joystick()`.
    In `process_event()`, `SDL_QUIT` during a game when the system asked the
    application to close (`a1_3ds::exit_requested()`) goes straight to
    `set_game_state(_quit_game)` instead of the "cancel the game in
    progress?" dialog.
17. **`main.cpp`**: `boot_mark("main loop ended")` after `main_event_loop()`,
    to time exits.
18. **`Input/mouse_sdl.cpp`**: `enter_mouse()` / `exit_mouse()` (mouse
    aiming on/off, i.e. gameplay input active or not) call
    `a1_3ds::set_touch_look(true/false)`: drag-to-look in a game, a pointer
    elsewhere.
19. **`Input/joystick_sdl.cpp`**: in `process_joystick_axes()`, the aim
    axes' response is `x²` of the deflection past the dead zone (rescaled so
    90% is full) instead of linear, and holding full yaw deflection ramps the
    speed to 2x over 15 ticks. *Added in the sprint after stage 7:* the
    pitch axis is scaled by `kPitchScale` (0.5) before the Controller
    Advanced vertical sensitivity. Marathon's pitch range is ±30 degrees,
    which full deflection crossed in about 0.12 s; the player kept ending
    up looking at the sky (`STAGE5.md` open item 2). A constant rather than
    a new default, so existing prefs files (which store 1.0) get it too.
20. **`Misc/preferences.cpp`**: a 3DS `default_key_bindings` table (one
    controller code per action; keyboard and mouse codes as upstream),
    `_inputmod_interchange_run_walk` added to the default modifiers, and the
    "Mouse Aiming" toggle labelled "Touch Screen Aiming".
21. **`Misc/sdl_widgets.cpp`**: the controller key names are the 3DS's
    ("ZR", "C-Stick Up", "SELECT", ...). `w_text_entry::click()` opens the
    system keyboard (`a1_3ds::text_input()`) with the field's text and
    label, and filters the result like typed text; it no longer positions a
    cursor. **`sdl_widgets.h`**: `w_static_text::get_text()` (for the
    label).
22. **`Misc/interface.cpp`**: when the system asked to close,
    `display_quit_screens()` skips the quit screens, and `finish_game()`
    skips its two `full_fade()`s and `load_environment_from_preferences()`
    (3.8 s in Azahar). HOME > Close from a game: 4.6 s → 0.2 s.

## Default focus (23-24)

Added 2026-10-11 (`STAGE5.md`, "Hardware session", decision 2). Upstream
expects a mouse to find things and the keyboard as a shortcut: hover gives
the highlight, so starting with nothing focused costs nothing on a desktop.
On the 3DS the D-Pad is the only way to move focus, and A did nothing until
it had moved once, at the main menu and in every dialog without a chosen
default.

23. **`Misc/interface.cpp`**: `display_main_menu()` sets
    `highlighted_main_menu_item` to `iNewGame` instead of `-1`, so the
    menu appears with BEGIN NEW GAME highlighted (also after a game or a
    demo) and A starts a game. D-Pad down now goes to the next item (CONTINUE
    SAVED GAME in Marathon 2).
24. **`Misc/sdl_dialogs.cpp`**: `dialog::start()`, after `layout()`, calls
    `activate_first_widget()` when the dialog chose no widget. Upstream has
    the same call commented out at the top of `start()`. Dialogs that pick a
    default keep it (18 `activate_widget()` call sites; e.g. YES in
    `quit_without_saving()`, unchanged). A focused text field no longer
    opens anything by itself on the 3DS (`SDL_StartTextInput` is wrapped,
    entry 21), which is probably why upstream left the call out.

**Non-3DS check** (2026-10-11, `ppcheck.sh`, `aleph-host-build` and
`aleph-host-build-nonet`): `Misc/interface.cpp` and `Misc/sdl_dialogs.cpp`
byte-identical, 0 diagnostics. 3DS build: 0 warnings.

**Verification** (Azahar 2126.2, New 3DS mode): the 7 input scripts,
updated for the new start focus (`STAGE5.md`, "Hardware session"), all
pass. 01: after the D-Pad round trip BEGIN NEW GAME is highlighted again.
02: Preferences opens with PLAYER focused (one D-Pad down + A opens
GRAPHICS). 03: the Player dialog opens with Name focused and no keyboard;
keyboards open only on the two taps. 04: A on the just-opened Controls
dialog switches AIM → MOVE (the tab bar is focused). 05: two downs reach
Look Up's controller binding; bind, clear, cancel, accept and reset as
before. 06: A alone at the main menu starts a new game; HOME > Close exits
0.19 s after the request. 07: unchanged (HOME/sleep, held touch ignored).

## 25. Linear heap: `u32 __ctru_linear_heap_size = 8 MiB`

Added 2026-10-11 (`STAGE5-PREREQS.md` item 4). libctru splits the
application's memory into the malloc heap and the linear heap (memory the
GPU and DSP can address); by default linear gets 32 MiB whenever more than
56 MiB is available. The port uses 2 112 KiB of it (the double-buffered
framebuffers and SDL's audio ring), and the hardware session measured the
same at the menu, in gameplay, in the demo and at exit. Defining the weak
symbol in `a1_3ds_platform.cpp` gives linear 8 MiB (~4x the use) and the
rest to malloc:

| | before | 8 MiB linear |
| --- | ---: | ---: |
| heap, New 3DS (hardware / Azahar) | 85 860 KiB | 110 428 KiB (Azahar) |
| heap, Old 3DS mode (Azahar) | 57 212 KiB | 81 756 KiB |
| malloc free after level 1 is loaded, Old 3DS mode | ~31 MiB | 55.5 MiB |
| linear free after a level | 30 656 of 32 768 KiB | 6 080 of 8 192 KiB |

`nm`: `__ctru_linear_heap_size` is `D` (was libctru's weak `V`). Too small
a value would fail at boot (SDL video or audio init), not mid-game. A GPU
renderer (citro3d: textures, command buffers) needs linear memory: revisit
the size then.

**Hardware fix (2026-10-11, PERF.md hardware session).** The split above
**did not boot on a New 3DS**: the console stopped before `main()` with a
Luma3DS exception screen, no `log.txt` and no 3dslink output. The Luma dump
(`/luma/dumps/arm11/crash_dump_00000000.dmp`) has PC = `svcBreak`, LR =
`__system_allocateHeaps` (libctru 2.7.0 `allocateHeaps.c:66`, the failed
heap `svcControlMemory`), SP = `0x0fffffc0` (the kernel's initial stack)
and `r3` = `0x06B7E000`: libctru asked for a 110 072 KiB heap, which would
end at `0x0EB7E000`. The heap must fit the heap area
`OS_HEAP_AREA_BEGIN..OS_HEAP_AREA_END` (`0x08000000..0x0E000000`, 96 MiB,
`3ds/os.h`); the console's kernel refuses more, Azahar does not check it.
With only `__ctru_linear_heap_size` set, libctru gives the heap everything
else and has no such cap.

The port now also defines `__system_allocateHeaps()` (libctru's is weak):
the same steps as libctru 2.7.0 (resource-limit commit, heap, linear heap,
`mappableInit`, `fake_heap_start/end`), but the heap is
`min(remaining − 8 MiB, 96 MiB)` and the linear heap gets the rest. On
this console (New 3DS, 124 MiB mode through the Homebrew Launcher):

| | 8 MiB linear (did not boot) | now (boot log) |
| --- | ---: | ---: |
| heap | 110 072 KiB (requested) | **98 304 KiB** (1 024 stack + 97 280 malloc) |
| linear heap | 8 192 KiB | **19 960 KiB** (17 860 free at the menu) |

Below 104 MiB of free memory (Old 3DS, or the 64/96 MiB modes) the cap does
not apply and the split is exactly the 8 MiB one above. Seven boots on the
console in the same session, a 30-minute play-through among them.

## Not patched, by design

- **Build-system-level choices** (no source edits): `ports/3ds/config.h`
  leaves `HAVE_PWD_H` and `HAVE_SYSCTLBYNAME` undefined and sets
  `HAVE_SYSCONF`/`LUA_USE_MKSTEMP` after link tests against newlib; see
  `STAGE3.md`.
- **`-Wformat` mismatches:** with `-Wformat`, the 3DS source set gives 17
  warnings at 7 call sites passing `long`/`int32`/`uint32` to
  `%d`/`%u`/`%x`/`%c` (`csalerts_sdl.cpp:326,331`, `wad.cpp:919`,
  `vbl.cpp:456`, `shapes.cpp:1007`, `QuickSave.cpp:669,673`). Harmless on the 3DS: `long` and `int` are both
  32-bit and passed identically under AAPCS32. The review found no other
  site where `int32_t == long` changes which overload is chosen.
- **libsndfile** `src/alac.c` `-Wincompatible-pointer-types` (same
  `int32_t == long` cause) is downgraded to a warning by
  `scripts/3ds/build-libsndfile.sh` (`-Wno-error=incompatible-pointer-types`);
  the library is not patched.

## Stage 6 audio (26-33)

Added 2026-10-11 (`STAGE6.md`). All guarded by `#ifdef A1_PLATFORM_3DS`;
the 3DS side is `ports/3ds/source/audio_3ds.cpp` (backend, instrumentation)
and `ports/3ds/source/decoders_3ds.cpp` (Tremor, mpg123).

**Non-3DS check** (2026-10-11, `ppcheck.sh`, `aleph-host-build` and
`aleph-host-build-nonet`): `shell.cpp`, `Misc/interface.cpp`,
`Sound/Decoder.cpp`, `MusicPlayer.cpp`, `SoundManager.cpp`,
`AudioPlayer.cpp`, `OpenALManager.cpp` (for `AudioBackend.h` and
`OpenALManager.h`), `Music.cpp` (for `MusicPlayer.h`) and
`XML/XML_LevelScript.cpp` are byte-identical; `Misc/preferences.cpp` differs only in 10 `__LINE__`
values inside logging macros (20 lines, inspected). 0 diagnostics.

26. **`Sound/AudioBackend.h`**: `AudioBackend::CreateDefault()` returns
    `a1_3ds::create_audio_backend()`: SDL's legacy device as before, but
    always stereo S16 at the preferences' rate, with callback timing, the
    audio thread's stack probe, DSP underrun counting and the missing-DSP
    state. Replaces stage 5's `--wrap=SDL_OpenAudio` probe.
27. **`Sound/Decoder.cpp`**: both factories try `a1_3ds::open_decoder()`
    after `SndfileDecoder` (the 3DS libsndfile has no codecs): Ogg Vorbis
    through Tremor, MP3 through mpg123, 16-bit output. Lost: Ogg Opus and
    FLAC still have no decoder (rejected cleanly; `debug-decoders`).
28. **`Sound/SoundManager.h`**: `DEFAULT_RATE` is 32728 Hz, the DSP's output
    rate (the DSP then plays SDL's buffers 1:1). **`SoundManager.cpp`**,
    `Parameters::Verify()`: clears `_hrtf_flag` and `_3d_sounds_flag`, sets
    stereo, and puts a rate outside 8000-48000 Hz or a buffer outside
    256-4096 samples back to the default. The rate stays configurable in
    the preferences file (`<sound rate="...">`); a file written before this
    stage says 44100, which still works.
29. **`Misc/preferences.cpp`**: the sound dialog no longer shows Channels,
    3D Sounds or HRTF (Headphones). The widgets are still created (patch 9's
    pattern), so the accept code reads unchanged values.
30. **`Sound/AudioPlayer.cpp`**: `FillBuffers()` fills at most
    1 + (callback length / buffer length) buffers per pass, i.e. what
    playback uses before the next pass plus one. Upstream fills every free
    buffer: a starting track decoded four 8 KiB buffers (~370 ms of
    22 kHz music) inside one callback, 96 ms against a 31 ms budget, and
    the DSP ran dry (Azahar: 1 underrun at every music start; 0 after).
    **`OpenALManager.h`**: `GetCallbackSeconds()` for it.
31. **`Sound/MusicPlayer.cpp`**: `GetNextData()` returns 0 when a segment
    gives no data right after it was (re)started. Upstream then rewinds and
    recurses again, without end, on the audio thread's 80 KiB stack: an
    empty or damaged music file looping crashed the game. **`MusicPlayer.h`**:
    the `segment_just_started` flag.
32. **`Misc/interface.cpp`**: `display_epilogue()`'s 10 ms wait and
    `interface_fade_out()`'s wait for the music fade sleep 1 ms per
    iteration instead of spinning; the fade wait gives up after 2 s (the fade
    takes 0.5 s). **`XML/XML_LevelScript.cpp`**: `ResetLevelScript()` (every
    level load) has the same wait and the same fix. Only the audio thread
    can end those two waits, and upstream spins with no limit; while OpenAL
    is paused `Music::Idle()` does nothing, so they would never end.
    (`AUDIT.md` b listed the first; the other two were found here.)
33. **`shell.cpp`**: in a game, the frame pacing also sleeps 1 ms through
    the last third of the frame, where upstream spins (desktop sleeps
    overshoot; the 3DS kernel's do not). A spinning main thread starves
    every lower-priority thread on its core.


## Performance stage (34-)

Added 2026-10-11 (`PERF.md`).

34. **Frame profiler** (`A1_PROFILE`, a CMake option of the 3DS build, off by
    default; the code is `ports/3ds/source/profile_3ds.cpp`). Guarded by
    `#ifdef A1_PROFILE` (only ever defined with `A1_PLATFORM_3DS`), so a
    normal 3DS build contains none of it either. Timed zones:
    `Misc/interface.cpp` `update_world()` (World) in `idle_game_state()`;
    `GameWorld/marathon2.cpp` `update_interface()` (Hud, nested in World and
    subtracted from it); `RenderOther/screen.cpp` `render_view()` (Render),
    `update_screen()` (Blit), the HUD blit (Hud) and `MainScreenUpdateRects()`
    (Present), plus `frame_done()` at the end of `render_screen()`;
    `shell.cpp` the in-game frame pacing (Idle). `Misc/interface.cpp`
    `begin_game()`: a single-player game starts the level named in
    `debug-perf` (the benchmark). **Non-3DS check** (2026-10-11):
    `shell.cpp` and `interface.cpp` byte-identical; `screen.cpp` and
    `marathon2.cpp` differ only in `__LINE__` values inside logging macros
    (14 and 4 lines, inspected). 0 diagnostics.
35. **`RenderOther/screen.cpp`**, `MainScreenUpdateRects()`: calls
    `a1_3ds::present_screen()` (`ports/3ds/source/video_3ds.cpp`) first,
    which puts the 640x480 screen surface on the top screen itself: 2x2
    averages (what SDL's linear scaling computes at 2:1), centred, rotated
    and converted into the framebuffer in one pass, swapped at VBlank. SDL's
    texture upload, renderer copy and N3DS framebuffer copy are skipped;
    they remain the fallback for any other surface size or format. Measured
    in `PERF.md` (experiment a). **Non-3DS check** (2026-10-11): differs
    only in `__LINE__` values inside logging macros (as 34).

## Stage 7 bottom screen (36-40)

Added 2026-10-12 (`STAGE7.md`). All guarded by `#ifdef A1_PLATFORM_3DS`;
the 3DS side is `ports/3ds/source/bottom_3ds.cpp`, `video_3ds.cpp` and
`input_3ds.cpp`.

36. **`RenderOther/screen.cpp`** `scale_terminal_layout()`, called in
    `change_screen_mode()` before `Term_Buffer` is allocated: the
    `_terminal_*` interface rectangles are fitted to the space the terminal
    has (x and y scaled separately, the header and footer bars at least one
    line tall, the rectangles between them stretched) and the
    `_computer_interface_font` / `_computer_interface_title_font` sizes set
    for about 11 screen pixels (`FontSpecifier::Size` + `Update()`), smaller
    if 57 characters would not fit across. The scenario's (MML) values are
    the reference and are kept when they change. `computer_interface.cpp`
    is unchanged: it wraps and paginates from these when a terminal opens.
    Skipped in networked games (their pages are counted in advance).
37. **`RenderOther/screen.cpp`, `screen_shared.h`**: with Bottom Screen set
    to HUD + Map, the in-game logical screen is 400x240
    (`change_screen_mode()` both overloads); `Screen::window_width/height()`
    follow a 400-wide surface; `seventyfive_percent()` is false for it;
    `Screen::hud()` is false on it (the HUD is on the bottom screen).
    `game_window_is_full_screen()` is `!screen_mode.hud`, so the HUD buffer
    is still drawn (also at the `draw_interface()` call in
    `render_screen()`). The HUD blit in `render_screen()` hands the change
    to the bottom screen instead, and `render_screen()` calls
    `a1_3ds::bottom_frame()`; it also switches the surface size when it no
    longer matches the setting (after a dialog; a Lua HUD gets Classic).
    `MainScreenUpdateRects()` returns the bottom screen to the legend for any
    640x480 present.
38. *(Removed 2026-10-12.)* **`Misc/vbl.cpp`** `parse_keymap()` ORed in
    `a1_3ds::take_touch_action_flags()`, the bottom screen's MAP, ACTION and
    NEXT WEAPON touch buttons. The buttons went when HUD and Map became one
    HUD + Map mode (`STAGE7.md`, "Revision"): the console's buttons already
    do those actions. `vbl.cpp` is back to its stage 3 content.
39. **`Misc/sdl_dialogs.cpp`** `dialog::start()`: a top-level dialog over a
    logical screen narrower than 640 switches to the menu size
    (`change_screen_mode(_screentype_menu)`).
40. **`Misc/preferences.cpp`, `preferences.h`**: `graphics_preferences_data::bottom_screen`
    (`<graphics bottom_screen="0|2">`, default 0 = HUD + Map, 2 = Classic;
    any other value, such as the first version's 1 = Map, becomes 0 in the
    3DS block of `validate_graphics_preferences()`), and its "Bottom Screen"
    selector (HUD + Map, Classic) in the Graphics dialog's Heads-Up Display
    section, which maps its two rows to 0 and 2.

**Non-3DS check** (2026-10-12, `ppcheck.sh`, `aleph-host-build` and
`aleph-host-build-nonet`): `sdl_dialogs.cpp`, `Misc/interface.cpp` and
`shell.cpp` byte-identical; `preferences.cpp` (20 lines), `screen.cpp`
(14 / 12) and `vbl.cpp` (8) differ only in `__LINE__` values inside logging
and assert macros (each removed line has an added twin once numbers are
normalised). 0 diagnostics.

**Revision check** (2026-10-12, HUD + Map; same tools, the working tree
against `4981724`): `screen.cpp` (14 / 12 lines), `preferences.cpp` (20)
and `vbl.cpp` (8) differ only in `__LINE__` values: 0 lines differ once
numbers are normalised. 0 diagnostics.

## The sprint after stage 7 (41-44)

Added 2026-10-12 (`NEXT-SPRINT.md`, items B1, B3, B6, B7); 43-44 and the
last 41 call sites on 2026-10-15, after the first console session. All guarded by
`#ifdef A1_PLATFORM_3DS`. Entry 19 was extended in the same sprint.

41. **Timed log marks** (log only): `a1_3ds::time_mark("what")` writes
    `[3ds] +T ms: what (+D ms)`, D being the time since the previous mark
    (or `boot_mark()`), into the log (`ports/3ds/source/a1_3ds_platform.cpp`).
    Call sites: `shell.cpp` `initialize_application()` between the steps
    after "preferences loaded" (directories, image cache, preferences and
    plugin MML, SDL_ttf, intro music, `initialize_marathon()`, screen
    drawing, dialogs, terminals and shape handler, fades and images, the
    scenario files); `Misc/interface.cpp` the level change (movie, chapter
    screen, `goto_level()`, `start_game()`), a new game (chapter screen,
    `new_game()`, `start_game()`) and `finish_game()` (each step);
    `Files/game_wad.cpp` `goto_level()` (old map left, level script, map
    read, Lua loaded); `GameWorld/marathon2.cpp` `entering_map()`
    (collections, sounds, Lua init); `XML/QuickSave.cpp`
    `create_quick_save()` (metadata, map preview, file written, surplus
    saves); `RenderMain/shapes.cpp` `load_collections()` (unloaded, read and
    decoded, shapes patches, shading tables, software texture extras). The
    files without the port header declare `a1_3ds::time_mark()` themselves.
    *Added 2026-10-15*, where the console was slow and Azahar is not:
    `game_wad.cpp` `save_game_file()` (temporary file opened, game wad
    built and written, map checksum read, preview wad built and written,
    header and directory, file closed, renamed); `Misc/preferences.cpp`
    `load_environment_from_preferences()` (map file, physics, shapes file,
    sounds file, external resources); `Sound/SoundManager.cpp`
    `Initialize()` (sounds file opened); `entering_map()` now marks the
    monster sounds apart from the rest.
42. **`RenderMain/shapes.cpp`** `load_collection()`: for a Marathon 2/∞
    shapes file, the collection's byte range (`length` or `length16` from
    its header) is read with one `SDL_RWread()` into a buffer and parsed
    from an `SDL_RWFromConstMem()` over it; the parsing code is unchanged
    (its seeks are relative to `src_offset`, which becomes 0, or to the
    current position). Upstream parses straight from the file: a few bytes
    per read and a seek for every bitmap row, twice for RLE bitmaps. In
    Azahar a level's collections took 28.0-28.4 s that way and 0.72-0.73 s
    with this (`NEXT-SPRINT.md` B1). If the read comes back short, the file
    is parsed as before. Marathon 1 shapes (resources, already in memory)
    are untouched.
43. **`Sound/SoundFile.cpp`** `SoundHeader::Load(OpenedFile&)` and
    `LoadData(OpenedFile&)`: the header's 64 bytes (a System 7 header is
    50 or 60), and for the data the header plus its samples
    (`data_offset + length`), are read with one `OpenedFile::Read()` and
    parsed from an `io::array_source` stream over them by the unchanged
    `Load(BIStreamBE&)` / `LoadData(BIStreamBE&)`. Upstream wraps the file
    in a boost `stream_buffer`, which reads 4 KB per fill: every header
    (975 of them, all read when the Sounds file is opened at boot and on
    each return to the main menu) cost a 4 KB read, and a sound's samples
    came 4 KB per read, also for sounds first played during the game,
    inside `update_world()`. In Azahar: opening the Sounds file 2.59 →
    0.51 s, a level's sounds 0.96 → 0.50 s (level 0) and 0.34 → 0.18 s
    (level 1). Checked with a temporary CRC log of every header and load:
    the same 975 headers in the same order and the same 107 sounds
    (3.3 MB), byte for byte. If a read comes back short, the stream path
    runs as before. Marathon 1 sounds (resources) are untouched.
44. **`RenderOther/screen_shared.h`** `zoom_overhead_map_in()` / `_out()`
    (the map zoom keys, D-Pad Up/Down by default): first ask
    `a1_3ds::bottom_map_zoom(±1)`, which zooms the bottom screen's map
    (HUD + Map, `STAGE7.md`) when it is shown and the main map is closed,
    between scale 1 and 3, and returns whether it did (the usual success /
    failure click). Otherwise the main map zooms as upstream. Replaces the
    revision's tap-to-zoom: on the console taps and drags to look clashed
    (`NEXT-SPRINT.md` A.3), so a touch on the bottom screen now only looks.

**Non-3DS check** (2026-10-12, `ppcheck.sh`, both volumes; the script now
also counts the differing lines with every number normalised):
`shell.cpp`, `interface.cpp` and `joystick_sdl.cpp` byte-identical;
`game_wad.cpp` (10 lines), `marathon2.cpp` (4), `QuickSave.cpp` (2) and
`shapes.cpp` (2) differ only in numbers: 0 lines with numbers normalised.
0 diagnostics.

**Non-3DS check for 43-44 and the new 41 marks** (2026-10-15, both
volumes): `screen.cpp` (for `screen_shared.h`), `marathon2.cpp` and
`SoundManager.cpp` byte-identical; `game_wad.cpp` (8 lines),
`SoundFile.cpp` (2) and `preferences.cpp` (20) differ only in numbers:
0 lines with numbers normalised. 0 diagnostics.

## Stage 10: citro3d (49)

Added 2026-10-16 on branch `stage10-c3d` (`CITRO3D_DESIGN.md`). **No engine
(`Source_Files/`) file changes in P1**, so there is no `ppcheck.sh` run: the
host build does not compile `ports/3ds/`, and the diffstat against `3ds-port`
lists no `Source_Files/` path. Everything is behind the `A1_RENDER_C3D` CMake
option (OFF by default; `nm` on the OFF `alephone.elf`: no `C3D_`/citro3d
symbols).

49. **P1, the GPU present** (port files only). `ports/3ds/CMakeLists.txt`:
    option `A1_RENDER_C3D` (defines `A1_RENDER_C3D`), the sources
    `render_c3d/{core/config.cpp,back/context.cpp,back/present.cpp}`, the
    shader `render_c3d/shaders/present.v.pica` (picasso via
    `ctr_add_shader_library`, embedded with `dkp_add_embedded_binary_library`),
    `citro3d ctru` linked, `-Wl,--wrap=gfxExit`.
    `source/video_3ds.cpp`: `present_screen()` first calls
    `a1_c3d_present()` (citro3d, lazily initialised at the first present, owns
    the top screen from then on); the unchanged CPU code is now
    `present_screen_cpu()`, used when `a1_c3d_present()` returns false
    (`debug-c3d` `present cpu`, init failed, ownership given up), and
    `a1_c3d_note_false_return()` counts a false return while citro3d is up.
    `source/a1_3ds_platform.cpp` `error_screen()`: `a1_c3d_release_top()`
    first. `include/a1_3ds_profile.h`, `source/profile_3ds.cpp`: the zone
    `c3d_wait` (`C3D_FrameBegin(0)`, A1_RENDER_C3D builds only).
    `scripts/3ds/run-azahar.sh` `--c3d FILE` (installs `debug-c3d`),
    `scripts/3ds/tests/perf-bench.sh` `C3D=FILE`, `scripts/3ds/tests/c3d-pixdiff.py`
    and `scripts/3ds/tests/c3d/p1-*.cfg` (tests).

    **P1 follow-ups (2026-10-17, port files only, no `Source_Files/` change):**
    critic minor 4: `a1_c3d_release_top()` now returns whether the top screen is
    free and is safe from any thread (`render_c3d/back/context.cpp`
    `release_from_any_thread()`, the `gfxExit` wrapper likewise); `error_screen()`
    keeps off the top screen when it is not (`PERF.md` "P1 follow-ups").
