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
14-15 with the stage 5 prerequisites, 16-22 in stage 5 (controls, `STAGE5.md`).

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
| 19 | `Source_Files/Input/joystick_sdl.cpp` | C-Stick response curve and turn acceleration | linear stick response (**by design**) |
| 20 | `Source_Files/Misc/preferences.cpp` | 3DS default bindings, always run, "Touch Screen Aiming" label | upstream controller defaults (**by design**) |
| 21 | `Source_Files/Misc/sdl_widgets.cpp`, `sdl_widgets.h` | 3DS button names; text fields use the system keyboard | cursor positioning by click in text fields |
| 22 | `Source_Files/Misc/interface.cpp` | HOME > Close skips quit screens, fades, scenario reload | none |

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
    speed to 2x over 15 ticks.
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
