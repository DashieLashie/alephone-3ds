# Aleph One 3DS port: stage 3 — first linked `.3dsx`

Recorded 2026-10-10 on branch `3ds-port`, on top of the stage 3
prerequisites (`6c728f2`). Goal: link a 3DS `.3dsx` of Aleph One without
restructuring upstream sources. All 3DS build glue is in `ports/3ds/`, the
dependency scripts are in `scripts/3ds/`, and source patches are logged in
[`PATCHES.md`](PATCHES.md).

## Summary

| Step | Result |
| --- | --- |
| `ports/3ds/CMakeLists.txt` + `config.h` | **OK.** Sources derived from `Source_Files/**/Makefile.am` at configure time: 187, **identical** to the stage 2 automake no-network/std::filesystem object list |
| Boost | **Headers only**, 1.92.0. `A1_FS_STD` (ENVIRONMENT.md: std::filesystem works), so no b2 build and no Boost libraries |
| OpenAL Soft | **Built**, 1.25.2 static, Null + Loopback only. One small upstream type patch. `A1_AUDIO_NULL` fallback not needed |
| libsndfile | **Built**, 1.2.2 static, no external codecs. `A1_NO_SNDFILE` fallback not needed. **No Ogg/MP3/FLAC/Opus decoding** (see stubs) |
| Compile | 187/187 TUs, **0 warnings**. 1 TU needed a fix (`vbl.cpp`, `int32_t` is `long` on newlib) |
| Link | 1 undefined symbol (`execlp`), fixed with a stub |
| libsysbase gaps | libctru provides no pthread-key hooks (OpenAL's `alcCreateContext` would always fail → **no sound**) and `sched_yield` is an ENOSYS stub. Fixed by `ports/3ds/source/sysbase_hooks.c` (`PATCHES.md` 4). Found by the review, not by the linker |
| **DoD** | `cmake --build build-3ds` → **`build-3ds/alephone.3dsx`** (7,545,216 bytes, `3DSX` magic, SMDH embedded) |
| Runtime | **Not run.** No emulator is installed on this host and the hardware probe is still deferred (`STAGE3-PREREQS.md` item 5) |

### Reproducing

```sh
# once: dependencies into $DEVKITPRO/portlibs/3ds (SDL2/SDL2_ttf from stage 0)
scripts/3ds/install-boost-headers.sh
scripts/3ds/build-openal.sh
scripts/3ds/build-libsndfile.sh

# engine
cmake -S ports/3ds -B build-3ds        # toolchain defaults to $DEVKITPRO/cmake/3DS.cmake
cmake --build build-3ds                # -> build-3ds/alephone.3dsx

# optional checks
scripts/3ds/verify-boost-headers.sh
docker run --rm -v aleph-host-build-fsstd:/b:ro aleph-host-baseline \
    sh -c 'cd /b/src/Source_Files && find . -name "*.o" | sort' > /tmp/automake-objs.txt
scripts/3ds/check-sources.sh /tmp/automake-objs.txt build-3ds/aleph-sources.txt
```

(The object list comes from the stage 2 `--disable-network
--enable-std-filesystem` build volume; see `STAGE2.md`.)

Verified from an empty `build-3ds/` on the final tree (2026-10-10, with
`sysbase_hooks.c`): configure 0 warnings, build exit 0 with 0
compiler/linker warnings, 188 compiled units (187 engine + the shim).

```
$ arm-none-eabi-size build-3ds/alephone.elf
   text    data     bss     dec     hex filename
6741806  681328 1076788 8499922  81b2d2 build-3ds/alephone.elf
$ head -c 4 build-3ds/alephone.3dsx
3DSX
SMDH: title "Aleph One", description "Marathon engine",
      author "Aleph One developers / 3DS port by DashieLashie", icon ports/3ds/res/icon.png
```

`alephone.elf` is 182 MB on disk because of `-g` debug info (Release is
devkitPro's `-g -O2 -DNDEBUG`); the `.3dsx` contains no debug info.

Final link line (`build-3ds/CMakeFiles/alephone.dir/link.txt`, objects and
paths shortened):

```
arm-none-eabi-g++ <arch flags> -g -O2 -DNDEBUG -L$DEVKITPRO/libctru/lib -L$DEVKITPRO/portlibs/3ds/lib
  -specs=3dsx.specs shell.o shell_misc.o shell_options.o main.o sysbase_hooks.o -o alephone.elf
  -Wl,--start-group <13 engine archives> -Wl,--end-group
  libopenal.a libsndfile.a libSDL2_ttf.a libSDL2main.a libSDL2.a libctru.a libfreetype.a libbz2.a libpng16.a
  libctru.a libfreetype.a libbz2.a libpng16.a libm.a libz.a -lctru -lm
```

That is the relative order of
`arm-none-eabi-pkg-config --static --libs sdl2 SDL2_ttf freetype2 libpng zlib`
(`-lSDL2_ttf -lSDL2main -lSDL2 -lctru -lm -lfreetype -lbz2 -lpng16 -lm -lm -lctru -lz`);
CMake repeats `ctru freetype bz2 png16` and merges the `m` entries into one
`libm.a` before `libz.a`, which still satisfies every dependency. `main`
comes from `libSDL2main.a(SDL_n3ds_main.o)`.

## Stubs and lost functionality

Everything that does not behave as on desktop, from the link-map audit of
`alephone.elf` and the preprocessor differences of the 3DS target. **Rows
1-3 are the stubs introduced by this stage**; rows 4-8 are unchanged
upstream code paths or toolchain defaults that behave differently on the
3DS.

| # | What | Where | Effect on 3DS |
| --- | --- | --- | --- |
| 1 | `system_launch_url_in_browser()` is a no-op | `CSeries/csalerts_sdl.cpp` (`PATCHES.md` 2) | `w_hyperlink` widgets do nothing when clicked; the URL is still printed to stderr |
| 2 | Ogg Vorbis / Opus / FLAC / MP3 decoding absent (libsndfile built without external codecs) | `scripts/3ds/build-libsndfile.sh` | **Marathon 2 `Music.ogg` (intro/menu music) is silent**, as is any Ogg/MP3/FLAC/Opus music or replacement sound from MML, Lua or plugins. `SndfileDecoder::Open` fails cleanly. WAV/AIFF and 21 other formats work; `Sounds.sndA` effects use Aleph One's own parser and are unaffected |
| 3 | pthread-key destructors never run | `ports/3ds/source/sysbase_hooks.c` | Per-thread values are abandoned at thread exit (libstdc++'s 12-byte per-thread exception globals; OpenAL registers no destructor). A small leak per exited thread; no thread is created repeatedly in single-player |
| 4 | Default player name | `Misc/preferences.cpp:194` (`#if defined(unix) …`, not defined by arm-none-eabi) | `get_name_from_system()` returns `"Bob User"` instead of the `getlogin()` name. A follow-up could read the console profile name through CFGU |
| 5 | Lua `io`/`os` functions without a backing implementation | `Lua/liolib.c`, `Lua/loslib.c` | `io.tmpfile` fails (`/tmp` does not exist on the SD card); `io.popen` raises "'popen' not supported" (`LUA_USE_POPEN` is off without `LUA_USE_POSIX`, as on Linux; newlib has no `popen`). The `os` library is only loaded with `--insecure-lua`; there `os.tmpname` fails (`/tmp`), `os.clock` returns -0.01 (`times()` is ENOSYS, `CLOCKS_PER_SEC` is 100), `os.getenv` always returns nil (empty environment) and `os.execute` reports no shell (newlib `system` is ENOSYS). All fail cleanly |
| 6 | Platform name | `Misc/alephversion.h:43` (`#elif defined (linux)`) | `A1_DISPLAY_PLATFORM` is `"Unknown"`: shown in the banner, `--version` and `$appPlatform$`/`$appLongVersion$` strings |
| 7 | Data-directory environment overrides | `shell.cpp:282,420` (`ALEPHONE_DEFAULT_DATA`, `ALEPHONE_DATA`) | The 3DS has no environment, so `getenv` returns NULL and the overrides cannot be used |
| 8 | `-DNDEBUG` | devkitPro Release flags | Standard `assert()` is compiled out (automake's default `-g -O2` keeps it; 86 engine objects reference `__assert_fail` on Linux). Aleph One's own `assert` (`CSeries/csalerts.h`) depends on `DEBUG`, not `NDEBUG`, and is unaffected |

Not introduced here, but in effect in this build (listed so nothing is
silent):

- `A1_NO_NETWORK` stubs and behaviour changes: `STAGE2.md`.
- `FILM_EXPORT` undefined: `Videos/Movie.cpp` compiles upstream's own no-op
  `Movie` methods, the same as any automake build without the film-export
  libraries.
- `Misc/thread_priority_sdl_posix.cpp`: `_POSIX_PRIORITY_SCHEDULING` is not
  defined by newlib, so `BoostThreadPriority()` compiles to `return true`
  without changing anything (upstream code path, no edit).
- ENOSYS stubs in the link: `times` (libsysbase), `system` (newlib libc),
  `kill`/`getpid` (libsysbase; reached only through newlib `raise`/`abort`
  and `tmpnam`/`mktemp` name generation), and `sched_yield`, now overridden.
  `isatty` always returns 0. The pthread-key functions forward to weak
  `__syscall_tls_*` hooks, now provided by `sysbase_hooks.c`.
  `fork`/`wait`/`execlp` are no longer referenced (the `csalerts_sdl.cpp`
  branch is not compiled on 3DS); `popen` does not exist in newlib. Everything
  else linked from newlib is a real implementation or a devoptab-backed file
  call.

## Known runtime risks (for the first hardware/emulator run)

1. **Paths.** `PKGDATADIR` is a provisional `sdmc:/3ds/alephone`
   (`A1_3DS_PKGDATADIR`), and user paths still derive from `$HOME`
   (`AUDIT.md` f). Data, prefs and saves will not be found where intended
   until the 3DS `get_data_path()` work is done.
2. **Screen.** The renderer enforces a 640x480 minimum and the menus are
   640x480 (`AUDIT.md` e); the top screen is 400x240.
3. **Deterministic `std::random_device`.** devkitARM's libstdc++ has no entropy
   source (mt19937, default seed). `boost::uuids::random_generator` therefore
   yields the same UUID every time it is constructed, and
   `WadImageCache::image_to_new_name` (`Files/WadImageCache.cpp:121`) gives
   every cached image the same file name. Not patched in this stage; a
   follow-up should seed from libctru (`PS_GenerateRandomBytes`) under
   `A1_PLATFORM_3DS`. `a1fs::unique_path` (temporary save names) also seeds
   from `std::random_device`: its generator is one `thread_local` mt19937
   per thread, and on the 3DS every thread and every run starts from the
   same seed, so names differ only within one thread's run. All current
   `SetTempName` callers run on the main thread, so in-run collisions do
   not occur.
4. **Memory/CPU.** `.text` is 6.7 MB and `.bss` 1.0 MB, of which OpenAL's
   statically initialised resampler tables are ~872 KB. OpenAL mixes with
   the plain C mixer (no NEON on ARM11), with upstream STL hardening on.
   Old 3DS headroom is untested.
5. **Threads.** At run time: main thread, SDL2's N3DS audio thread (runs
   OpenAL mixing and music decoding), and one OpenAL event thread per
   context (libctru thread, priority 0x3F, core 0). Horizon schedules by
   strict priority: `sched_yield()` (now `svcSleepThread(1)`) gives up the
   core only briefly, so the dialog and intro loops
   (`Misc/sdl_dialogs.cpp:2262`, `RenderOther/images.cpp:968`, which spin on
   `yield()`) can starve lower-priority threads on the same core, and
   OpenAL's `StopEventThrd` spin-waits on `yield()`. Watch for stalled audio
   while dialogs are open.
6. **Not run anywhere.** The TLS hooks were unit-tested on the host (per-thread
   values, deleted keys invalid, re-created keys read NULL, 64-key limit);
   none of the 3DS binaries has been run.

## Files added in this stage

| Path | Purpose |
| --- | --- |
| `ports/3ds/CMakeLists.txt` | 3DS build |
| `ports/3ds/config.h` | hand-maintained `config.h` for newlib/devkitARM |
| `ports/3ds/source/sysbase_hooks.c` | libsysbase hooks libctru lacks: pthread keys, `sched_yield` |
| `ports/3ds/cmake/AutomakeSources.cmake`, `A1AutomakeConditions.cmake`, `PrintSources.cmake` | Makefile.am parser and the 3DS `AM_CONDITIONAL` values |
| `scripts/3ds/install-boost-headers.sh`, `verify-boost-headers.sh` | Boost headers + cross-compile/link check |
| `scripts/3ds/build-openal.sh`, `scripts/3ds/patches/openal-soft-1.25.2/*.patch` | OpenAL Soft |
| `scripts/3ds/build-libsndfile.sh` | libsndfile |
| `scripts/3ds/check-sources.sh` | derived source list vs an automake object list |
| `scripts/3ds/tests/*` | dependency link tests (OpenAL loopback, sndfile virtual I/O); the Boost probe is generated by `verify-boost-headers.sh` |
| `docs/3ds/STAGE3.md`, `docs/3ds/PATCHES.md` | this document, patch log |

Engine source changes: `Misc/vbl.cpp`, `CSeries/csalerts_sdl.cpp` (both
`#ifdef A1_PLATFORM_3DS`, see `PATCHES.md`).

The sections below are the detailed records for the build glue and each
dependency.

---

## CMake build glue (`ports/3ds/`)

```sh
cmake -S ports/3ds -B build-3ds        # from the repo root; toolchain defaults to $DEVKITPRO/cmake/3DS.cmake
cmake --build build-3ds                # -> build-3ds/alephone.elf -> build-3ds/alephone.3dsx
```

### Files

| File | Purpose |
| --- | --- |
| `ports/3ds/CMakeLists.txt` | Toolchain default, flags, dependencies, engine targets, SMDH/3DSX packaging |
| `ports/3ds/config.h` | Hand-maintained `config.h` (stage 2 no-network + std::filesystem config, adapted to newlib) |
| `ports/3ds/cmake/AutomakeSources.cmake` | Parser that derives source lists from `Source_Files/**/Makefile.am` |
| `ports/3ds/cmake/A1AutomakeConditions.cmake` | AM_CONDITIONAL values for the 3DS (all OFF) |
| `ports/3ds/cmake/PrintSources.cmake` | Runs the parser in script mode (`cmake -DOUTPUT=f -P ...`) |
| `scripts/3ds/check-sources.sh` | Diffs the derived source list against an automake object list |

`build-3ds/` is listed in `.git/info/exclude` (local only).

### Sources: derived from automake, not listed by hand

At configure time `AutomakeSources.cmake` reads `Source_Files/Makefile.am` and follows `SUBDIRS`. It joins `\` continuations, strips comments, skips recipe lines and evaluates `if`/`else`/`endif`. It records `VAR = ...` and `VAR += ...` and expands `$(VAR)`. For every `noinst_LIBRARIES` entry and for the `alephone` program it takes `<canon>_SOURCES` (keeping `.c`/`.cpp`) and `AM_CPPFLAGS` (`-I` and `-D`). If it meets a conditional with no value or syntax it does not know, it stops with an error rather than guessing. Every `Makefile.am` it reads is added to `CMAKE_CONFIGURE_DEPENDS`.

Conditionals: `MAKE_NETWORK`, `MAKE_OPENGL`, `STEAM`, `BUILD_ZZIP_RWOPS`, `MAKE_WINDOWS`, `BUILD_ALEPHONE_TESTS`, `STANDALONE_HUB` and `INSTALL_MIME` are all OFF.

Each automake library becomes a CMake static library with the same name (`libcseries.a`, ...) and its own `AM_CPPFLAGS` include order. For example, Videos gets `-D__STDC_CONSTANT_MACROS`. `alephone` links them inside `-Wl,--start-group ... --end-group`, which does the same job as automake listing each library twice in `alephone_LDADD`.

| Directory | Library | Sources |
| --- | --- | --- |
| Source_Files | alephone (program) | 4 |
| CSeries | libcseries.a | 10 |
| Files | libfiles.a | 15 |
| Videos | libvideos.a | 1 |
| GameWorld | libgameworld.a | 22 |
| Input | libinput.a | 2 |
| Lua | liba1lua.a | 42 |
| Misc | libmisc.a | 21 |
| ModelView | libmodelview.a | 5 |
| Network | libnetwork.a | 2 (`network_games.cpp`, `network_dummy.cpp`) |
| RenderMain | librendermain.a | 21 |
| RenderOther | librenderother.a | 26 |
| Sound | libsound.a | 11 |
| XML | libxml.a | 5 |
| **Total** | | **187** |

**Ground-truth check** against the object list of the stage 2 Linux build with the same feature set (no network, no OpenGL, std::filesystem):

```
$ scripts/3ds/check-sources.sh objs-aleph-host-build-fsstd.txt build-3ds/aleph-sources.txt
expected: 187 objects (objs-aleph-host-build-fsstd.txt)
derived:  187 sources (build-3ds/aleph-sources.txt)
RESULT: identical (0 differences)
```

Some files are compiled on purpose, because automake compiles them too:

- `RenderMain/OGL_*.cpp` and the `*_OGL.cpp` files in RenderOther are listed unconditionally upstream and guarded inside by `HAVE_OPENGL`. Only `Rasterizer_Shader.cpp` and `RenderRasterize_Shader.cpp` depend on `MAKE_OPENGL`, and they are excluded.
- `Videos/Movie.cpp` is always built. With `FILM_EXPORT` undefined it compiles upstream's own no-op `Movie` methods.
- `Misc/thread_priority_sdl_posix.cpp` is what automake picks for non-Windows targets. On the 3DS, `_POSIX_PRIORITY_SCHEDULING` is undefined, so it compiles to `return true` with no external symbols.

### Flags and includes

- The toolchain supplies `-march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft -mword-relocations -ffunction-sections -D__3DS__`. The Release configuration (the default) adds `-g -O2 -DNDEBUG`.
- C++ gets `-std=gnu++17` (`CMAKE_CXX_STANDARD 17` with extensions on), plus `-fexceptions -frtti`. Configure stops if `-fno-exceptions` or `-fno-rtti` appears in the C++ flags.
- C gets no `-std` flag, as under automake.
- All TUs get `-DHAVE_CONFIG_H -DA1_PLATFORM_3DS`.
- Include order: `build-3ds/generated` (for `confpaths.h`), the source directory, `ports/3ds` (for `config.h`), the Makefile.am's `AM_CPPFLAGS`, then the dependencies as `-isystem`.
- Configure stops if a stray `config.h` exists in the repo root or under `Source_Files` (none does).

**`confpaths.h`**: generated as `build-3ds/generated/confpaths.h` containing `#define PKGDATADIR "sdmc:/3ds/alephone"`. The value comes from the cache variable `A1_3DS_PKGDATADIR`. It replaces the CSeries `Makefile.am` rule and is **provisional**: runtime data paths are a later stage (AUDIT.md f).

Exact compile line for one TU (`make -C build-3ds VERBOSE=1 Users/dashielashie/Desktop/Aleph_3ds/Source_Files/CSeries/byte_swapping.o`, exit 0, no warnings):

```
/opt/devkitpro/devkitARM/bin/arm-none-eabi-g++ -DA1_PLATFORM_3DS -DHAVE_CONFIG_H
  -I<repo>/build-3ds/generated -I<repo>/Source_Files/CSeries -I<repo>/ports/3ds
  -I<repo>/Source_Files/Files -I<repo>/Source_Files/GameWorld -I<repo>/Source_Files/Input
  -I<repo>/Source_Files/Misc -I<repo>/Source_Files/ModelView -I<repo>/Source_Files/Network
  -I<repo>/Source_Files/RenderMain -I<repo>/Source_Files/RenderOther -I<repo>/Source_Files/Sound
  -I<repo>/Source_Files/XML -I<repo>/Source_Files/Network/Metaserver -I<repo>/Source_Files/TCPMess
  -I<repo>/Source_Files
  -isystem /opt/devkitpro/portlibs/3ds/include -isystem /opt/devkitpro/portlibs/3ds/include/AL
  -isystem /opt/devkitpro/portlibs/3ds/include/SDL2 -isystem /opt/devkitpro/portlibs/3ds/include/freetype2
  -isystem /opt/devkitpro/portlibs/3ds/include/libpng16 -isystem /opt/devkitpro/libctru/include
  -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft -mword-relocations -ffunction-sections
  -D__3DS__ -g -O2 -DNDEBUG -std=gnu++17 -fexceptions -frtti -DAL_LIBTYPE_STATIC
  -MD -MT CMakeFiles/a1_cseries.dir/<abs path>/CSeries/byte_swapping.o -MF ....o.d
  -o CMakeFiles/a1_cseries.dir/<abs path>/CSeries/byte_swapping.o -c <repo>/Source_Files/CSeries/byte_swapping.cpp
```

The dependency file shows that `<repo>/ports/3ds/config.h` was the `config.h` included. `Lua/lapi.c` compiled with the same flags minus the C++-only ones (no `-std`, `-fexceptions` or `-frtti`).

### config.h decisions (newlib 4.6.0 / libctru 2.7.0)

Headers were checked with `#include <h>`. Functions were checked with an autoconf-style link test using `-specs=3dsx.specs -lctru -lm`.

| Define | Value | Why |
| --- | --- | --- |
| `A1_FS_STD`, `A1_NO_NETWORK` | 1 | Minimal feature set; std::filesystem is verified working (ENVIRONMENT.md) |
| `HAVE_BOOST` | defined | Boost 1.92.0 headers; no `HAVE_BOOST_FILESYSTEM` |
| `HAVE_CXX17`, `HAVE_SDL`, `HAVE_SDL_TTF`, `HAVE_ZLIB` | 1 | |
| `HAVE_OPENAL`, `HAVE_SNDFILE` | 1 | OpenAL Soft and libsndfile are linked (no engine source tests these macros) |
| OpenGL, curl, zzip, png, miniupnpc, catch2, steam, nfd, film export | undefined | |
| `HAVE_{INTTYPES,STDINT,STDIO,STDLIB,STRINGS,STRING,SYS_STAT,SYS_TYPES,UNISTD}_H`, `STDC_HEADERS` | 1 | Headers compile; the `unistd.h` functions used (`access`, `close`) link |
| `HAVE_PWD_H` | **undefined** | `pwd.h` exists, but `getpwuid`, `getuid` and `getlogin` do not link. Its only user (`Logging.cpp`, an include only) is also inside `__unix__`, which devkitARM does not define |
| `HAVE_SNPRINTF`, `HAVE_VSNPRINTF` | 1 | Link |
| `HAVE_SYSCONF` | 1 | Links. newlib answers only `_SC_PAGESIZE`; its only user is film-export code that clamps the result to at least 1 |
| `HAVE_SYSCTLBYNAME` | undefined | No `sys/sysctl.h`; does not link |
| `LUA_USE_MKSTEMP` | 1 | Links (newlib `mktemp.o`). `/tmp` does not exist on the SD card, so `os.tmpname()` fails cleanly either way |
| `TARGET_PLATFORM` | `"3ds arm"` | |
| `PACKAGE_*`, `VERSION` | `20250930` | Configure warns if this drifts from `A1_DATE_VERSION` |

### Dependencies and link order

`arm-none-eabi-pkg-config --static --libs sdl2 SDL2_ttf freetype2 libpng zlib` prints:

```
-L/opt/devkitpro/portlibs/3ds/lib -lSDL2_ttf -lSDL2main -lSDL2 -lctru -lm -lfreetype -lbz2 -lpng16 -lm -lm -lctru -lz
```

The build uses a single `pkg_check_modules(A1_SDL REQUIRED IMPORTED_TARGET "sdl2>=2.0.16" SDL2_ttf freetype2 libpng zlib)` call, which keeps this order. OpenAL comes from `pkg_check_modules(openal>=1.14.0)` and sndfile from `pkg_check_modules(sndfile)`. A missing library stops configure, for example: `libopenal.a not found (...); run scripts/3ds/build-openal.sh`. (The scaffold first had `A1_AUDIO_NULL`/`A1_NO_SNDFILE` options for the fallbacks in the brief; both libraries built, the fallbacks were never implemented, and the options were removed so they could not produce a broken build.) Boost is headers only: configure requires version 1.69.0 or later and checks the same `boost/*.hpp` headers as `configure.ac`. Configure also stops if any dependency resolves to a `.so` or `.dylib`.

Resulting link line (from `build-3ds/CMakeFiles/alephone.dir/link.txt`, shortened):

```
arm-none-eabi-g++ <arch flags> -g -O2 -DNDEBUG -L$DEVKITPRO/libctru/lib -L$DEVKITPRO/portlibs/3ds/lib -specs=3dsx.specs
  shell.o shell_misc.o shell_options.o main.o sysbase_hooks.o -o alephone.elf
  -Wl,--start-group libcseries.a libfiles.a libvideos.a libgameworld.a libinput.a liba1lua.a libmisc.a
                    libmodelview.a libnetwork.a librendermain.a librenderother.a libsound.a libxml.a -Wl,--end-group
  libopenal.a libsndfile.a libSDL2_ttf.a libSDL2main.a libSDL2.a libctru.a libfreetype.a libbz2.a libpng16.a
  ... libm.a libz.a -lctru -lm
```

The final `build-openal.sh` keeps ELF hardening flags out of `openal.pc` (see the OpenAL section), so the link line has no `-z relro`/`-z now` and `ld` prints no warnings.

### Packaging

The build calls `ctr_generate_smdh(alephone.smdh NAME "Aleph One" DESCRIPTION "Marathon engine" AUTHOR "Aleph One developers / 3DS port by DashieLashie" ICON ports/3ds/res/icon.png)` and then `ctr_create_3dsx(alephone SMDH alephone.smdh)`. That produces `build-3ds/alephone.3dsx` (an ALL target, `alephone_3dsx`). I generated the SMDH on its own and decoded it: 14016 bytes, magic `SMDH`, title `Aleph One`, description `Marathon engine`, author `Aleph One developers / 3DS port by DashieLashie`.

### Configure result (2026-10-10, fresh `build-3ds`)

```
-- Checking for modules 'sdl2>=2.0.16;SDL2_ttf;freetype2;libpng;zlib'
--   Found sdl2, version 2.32.10
--   Found SDL2_ttf, version 2.24.0
--   Found freetype2, version 26.4.20
--   Found libpng, version 1.6.53
--   Found zlib, version 1.3.1
-- Checking for module 'openal>=1.14.0'
--   Found openal, version 1.25.2
-- Checking for module 'sndfile'
--   Found sndfile, version 1.2.2
-- Boost headers: /opt/devkitpro/portlibs/3ds/include (1.92.0)
-- Aleph One sources: 187 (list: .../build-3ds/aleph-sources.txt)
-- Summary: SDL2 2.32.10, SDL2_ttf 2.24.0, audio: OpenAL Soft 1.25.2, decoding: libsndfile 1.2.2, PKGDATADIR "sdmc:/3ds/alephone" (provisional), build type Release
-- Configuring done (0.9s)
-- Generating done (0.1s)
```

Configure produced 0 CMake warnings.

---

## Boost (headers only)

**Decision:** the 3DS build defines `A1_FS_STD`. ENVIRONMENT.md shows that std::filesystem compiles, links against newlib opendir/readdir and works in the emulator. So no compiled Boost library is built and **b2 is never run**. Every Boost library the 3DS source set uses is header-only (boost::system has been header-only since 1.69).

| | |
| --- | --- |
| Version | Boost **1.92.0** (released 2026-08-12, the newest release at least several weeks old; configure.ac needs >= 1.69) |
| Tarball | https://archives.boost.io/release/1.92.0/source/boost_1_92_0.tar.bz2 |
| SHA-256 | `5c1d40cb8e19adbf740a4ec2da35b3e58f3f5804b1dce44deb53df72193cbc6c` (computed locally; the same value is in upstream `boost_1_92_0.tar.bz2.json`, commit `afdfa32505af73e3d208144b3f623f0096cb62b6`, and on https://www.boost.org/releases/1.92.0/) |
| Installed | `$DEVKITPRO/portlibs/3ds/include/boost/` (16415 files, 184 MB) + `LICENSE_1_0.txt` + stamp `.aleph-boost-version` |
| Libraries | none |

```sh
scripts/3ds/install-boost-headers.sh   # download, verify, install boost/ only
scripts/3ds/verify-boost-headers.sh    # 3DS cross-compile + link checks
```

`install-boost-headers.sh`:
- pins the version and SHA-256, and also checks the upstream `.json` checksum at run time;
- downloads into its own fresh `build/3ds-deps/boost-1.92.0-download/` and extracts only `boost_1_92_0/boost` and `LICENSE_1_0.txt` into `build/3ds-deps/boost-1.92.0-src/`;
- stages the new tree next to `include/boost` and swaps it in atomically;
- is idempotent: if the stamp matches, it does nothing;
- refuses to replace an `include/boost` it did not install unless `FORCE=1` is set.

Logs are in `build/3ds-deps/logs/boost-headers-install.log` and `boost-verify-build.log`.

### Verification (devkitARM r68, arm-none-eabi-g++ 16.1.0)

Flags: `-march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft -D__3DS__ -std=gnu++17 -fexceptions -frtti -O2 -Wall -isystem $DEVKITPRO/portlibs/3ds/include -isystem $DEVKITPRO/libctru/include`

1. **All headers TU.** `verify-boost-headers.sh` generates one TU from every `<boost/...>` include in the 3DS source set. That is 24 files, excluding Network/** apart from network.h, network_games.* and network_dummy.cpp, plus Metaserver, StandaloneHub, TCPMess, Steam and the shader renderers. The 24 distinct headers are:
   - algorithm/hex, algorithm/string/{predicate,replace}
   - container/small_vector
   - iostreams/{categories,device/array,positioning,stream,stream_buffer}
   - lockfree/spsc_queue
   - property_tree/{ini_parser,ptree,xml_parser}
   - range/{adaptor/map,any_range}
   - system/error_code
   - tokenizer
   - unordered/unordered_map, unordered_map
   - uuid/{uuid,uuid_generators,uuid_io}
   - version
   - filesystem (only without A1_FS_STD)

   It compiles with **0 warnings**, both with `-DA1_FS_STD` and without it (boost/filesystem.hpp is compiled only, not linked).
2. **Runtime test program** (`build/3ds-deps/boost-verify/boost-probe.3dsx`). It uses spsc_queue<T, capacity<5>> with std::atomic_int, property_tree XML/INI round trips on iptree (including xml_writer_settings<std::string>), any_range with adaptors::values, iostreams stream_buffer<array_source/array_sink>, io::stream over a seekable device (the same shape as opened_file_device), replace_all/iends_with/starts_with, algorithm::hex, tokenizer<char_separator>, boost::unordered_map, small_vector, system::error_code and uuids::random_generator with to_string.
   - Linked with `-specs=3dsx.specs -L$DEVKITPRO/libctru/lib -lctru -lm` only.
   - **No Boost libraries, no -latomic, no undefined `boost::` or `__atomic_*` symbols.** Atomics are inline ldrex/strex.
   - The link map shows members only from libc, libm, libstdc++, libsysbase, libgcc and libctru.
   - **Not yet run:** no emulator is installed on the build host. When run, it writes `sdmc:/3ds/boost-probe.log`.

### CMake

There is no Boost CMake package config, and the toolchain's standard include dirs cover only libctru. Use:

```cmake
find_path(BOOST_INCLUDE_DIR boost/version.hpp REQUIRED)   # -> $DEVKITPRO/portlibs/3ds/include
add_library(Boost::headers INTERFACE IMPORTED)
target_include_directories(Boost::headers SYSTEM INTERFACE "${BOOST_INCLUDE_DIR}")
```

Define `A1_FS_STD`. Do not use `Boost::filesystem` or `Boost::system`. No `BOOST_*` defines are needed.

### Problem found: std::random_device has no entropy on the 3DS (affects boost::uuids)

In Boost 1.92, `boost::uuids::random_generator` is chacha20 seeded from `std::random_device`. devkitARM's libstdc++ `random.o` has no entropy source: it references no getentropy, arc4random or /dev/urandom, and its only tokens are `default` and `mt19937`. `_M_init` goes to `_M_init_pretr1`, which seeds mt19937 with 5489. Every freshly constructed generator therefore returns the same UUID (predicted on the host: `fb135fdb-2482-4d31-aa8c-351caf595b56`).

`WadImageCache::image_to_new_name` constructs one generator per call, so all cached images get the same file name. This needs a fix in WadImageCache.cpp guarded by `A1_PLATFORM_3DS`, for example a static generator seeded from `PS_GenerateRandomBytes`. newlib `getentropy`/`arc4random` cannot replace it: they need `_getentropy_r`, which no devkitARM/libctru library provides (libnosys only has a warning stub `_getentropy`).

---

## OpenAL Soft (static, 3DS)

**Result:** builds, installs and links. No audio stub was needed: the `A1_AUDIO_NULL` fallback from the brief was not implemented.

| | |
| --- | --- |
| Version | OpenAL Soft **1.25.2** (released 2026-05-12; newest release on 2026-10-10) |
| Source | `https://openal-soft.org/openal-releases/openal-soft-1.25.2.tar.bz2` |
| SHA-256 | `1dbaac44e7579d5bc8847ca8db4b2e8b9fd3961041f35ee20def4958301e1089`: computed locally; matches Homebrew `Formula/o/openal-soft.rb` and FreeBSD ports `audio/openal-soft/distinfo` (size 1132689). Upstream publishes no checksum or signature. |
| Language | C++20 (`CXX_STANDARD 20 REQUIRED`); devkitARM GCC 16.1 handles it. The optional C++20 modules need Ninja or VS2022, so they are explicitly turned off (`ALSOFT_ENABLE_MODULES=OFF`) |
| Script | `scripts/3ds/build-openal.sh` |
| Local patch | `scripts/3ds/patches/openal-soft-1.25.2/0001-altypes-make_strict-for-int-when-int32_t-is-long.patch` |
| Link-test source | `scripts/3ds/tests/openal-loopback-test.cpp` |
| Logs | `build/3ds-deps/logs/openal-{configure,build,install,linktest}.log`, `openal-install-manifest.txt` |
| Build time | about 41 s wall clock (8 cores) |

### Command

```sh
scripts/3ds/build-openal.sh
# overrides: OPENAL_VERSION/OPENAL_SHA256/OPENAL_URL, OPENAL_EMBED_HRTF=ON|OFF,
#            OPENAL_STL_HARDENING=None|Performance|Debug, WORK_DIR, JOBS
```

The script downloads into `build/3ds-deps/openal-1.25.2-download/` and checks the SHA-256. It extracts into `openal-1.25.2-src/` and applies the patch. It then configures with `$DEVKITPRO/cmake/3DS.cmake` (Unix Makefiles, Release = `-g -O2 -DNDEBUG`), builds and installs into `$DEVKITPRO/portlibs/3ds` without sudo. Finally it checks the installed files and symbols, and compiles and links a test `.3dsx` using the flags from the installed `openal.pc`.

### CMake options (every `ALSOFT_*` option 1.25.2 defines on a non-Windows, non-Apple target)

- `LIBTYPE=STATIC`, `CMAKE_INSTALL_LIBDIR=lib`
- Programs and extras: `ALSOFT_UTILS=OFF`, `ALSOFT_NO_CONFIG_UTIL=ON`, `ALSOFT_EXAMPLES=OFF`, `ALSOFT_TESTS=OFF`, `ALSOFT_INSTALL=ON`, `ALSOFT_INSTALL_CONFIG=OFF`, `ALSOFT_INSTALL_HRTF_DATA=OFF`, `ALSOFT_INSTALL_AMBDEC_PRESETS=OFF`, `ALSOFT_INSTALL_EXAMPLES=OFF`, `ALSOFT_INSTALL_UTILS=OFF`, `ALSOFT_SEARCH_INSTALL_DATADIR=OFF`, `ALSOFT_UPDATE_BUILD_VERSION=OFF`
- Toolchain: `ALSOFT_ENABLE_MODULES=OFF`, `ALSOFT_WERROR=OFF`, `ALSOFT_EAX=OFF`, `ALSOFT_DLOPEN=OFF`, `ALSOFT_DLOPEN_NOTES=OFF`, `ALSOFT_STATIC_LIBGCC=OFF`, `ALSOFT_STATIC_STDCXX=OFF`, `ALSOFT_RTKIT=OFF`, `ALSOFT_REQUIRE_RTKIT=OFF`, `ALSOFT_STL_HARDENING=Performance` (the upstream default, `-D_GLIBCXX_ASSERTIONS`)
- CPU extensions (the ARM11 has no SSE or NEON): `ALSOFT_CPUEXT_{SSE,SSE2,SSE3,SSE4_1,NEON}=OFF`, `ALSOFT_REQUIRE_{SSE,SSE2,SSE3,SSE4_1,NEON}=OFF`
- Backends, all OFF: `ALSOFT_BACKEND_{PIPEWIRE,PULSEAUDIO,ALSA,OSS,SOLARIS,SNDIO,JACK,COREAUDIO,OBOE,OPENSL,PORTAUDIO,SDL3,SDL2,WAVE}=OFF` and `ALSOFT_REQUIRE_{PIPEWIRE,PULSEAUDIO,ALSA,OSS,SOLARIS,SNDIO,JACK,COREAUDIO,OBOE,OPENSL,PORTAUDIO,SDL3,SDL2}=OFF`. **Null and Loopback are not options.** `CMakeLists.txt` always compiles `alc/backends/null.cpp` and `alc/backends/loopback.cpp` ("Default backends, always available"). Configure output: `Building OpenAL with support for the following backends: Null`.
- Options that exist only on Windows or macOS are not passed: `ALSOFT_BACKEND_{WASAPI,DSOUND,WINMM}`, `ALSOFT_REQUIRE_{WASAPI,DSOUND,WINMM}`, `ALSOFT_BUILD_ROUTER`, `ALSOFT_BUILD_IMPORT_LIB`, `ALSOFT_NO_UID_DEFS`, `ALSOFT_STATIC_WINPTHREAD`, `ALSOFT_OSX_FRAMEWORK`. `ALSOFT_ENABLE_SSE2_CODEGEN` only exists when SSE2 is found.
- **`ALSOFT_EMBED_HRTF_DATA=ON` (a deliberate choice).** The engine has an opt-in "HRTF (Headphones)" preference, off by default, and greys it out when `ALC_HRTF_STATUS_SOFT` says HRTF is unsupported. The 3DS has no OpenAL data directory, so without the embedded data set the feature could never be enabled. The cost is 160 KB of `.data` (`default_hrtf`, 0x27061 bytes). HRTF mixing is CPU-heavy, but it only runs when the player enables it. `OPENAL_EMBED_HRTF=OFF` drops it.
- Pre-seeded check results: `HAVE_WL_Z_NOEXECSTACK=OFF`, `HAVE_WL_Z_RELRO=OFF`, `HAVE_WL_Z_NOW=OFF`, `HAVE_WL_AS_NEEDED=OFF` (problem 2 below).
- `CMAKE_PROJECT_INCLUDE=<generated file>`, which sets `CMAKE_{C,CXX}_COMPILE_OPTIONS_PIC ""` (problem 3 below).

Results of the configure checks that matter: `CMAKE_HAVE_LIBC_PTHREAD` succeeds (newlib `pthread.h`, implemented in libsysbase on top of libctru). `HAVE_LIBATOMIC` fails, which is fine: no `-latomic` is needed because ARMv6K has `ldrex/strex(d)`. `pthread_setschedparam`, `pthread_setname_np` and `dlfcn.h` are not found, so the related code is compiled out.

### Installed files

`lib/libopenal.a` (101 MB, mostly `-g` debug info; the bundled fmt 11.2.0 `format.o`/`os.o` are included), `include/AL/{al,alc,alext,efx,efx-presets}.h`, `lib/pkgconfig/openal.pc`, `lib/cmake/OpenAL/{OpenALConfig,OpenALTargets,OpenALTargets-release}.cmake`. No shared library is installed.

```
$ arm-none-eabi-pkg-config --cflags openal
-I/opt/devkitpro/portlibs/3ds/include -I/opt/devkitpro/portlibs/3ds/include/AL -DAL_LIBTYPE_STATIC
$ arm-none-eabi-pkg-config --static --libs openal
-L/opt/devkitpro/portlibs/3ds/lib -lopenal -lm
```

### Using it from CMake

```cmake
find_package(Threads REQUIRED)         # required first (upstream config omits find_dependency)
find_package(OpenAL CONFIG REQUIRED)   # no version argument: no ConfigVersion file is installed
target_link_libraries(alephone PRIVATE OpenAL::OpenAL)   # adds AL_LIBTYPE_STATIC, include/ and include/AL
```

This was checked with a throwaway CMake project: it configures, builds and produces `oaltest.3dsx`. The link line ends with `libopenal.a -lm -lctru -lm`. Without `find_package(Threads)`, configuring fails with: `The link interface of target "OpenAL::OpenAL" contains: Threads::Threads but the target was not found.`

### Verification

`scripts/3ds/tests/openal-loopback-test.cpp` does the following:

- `alcIsExtensionPresent(NULL,"ALC_SOFT_loopback")`
- gets `alcLoopbackOpenDeviceSOFT`, `alcIsRenderFormatSupportedSOFT` and `alcRenderSamplesSOFT` with `alcGetProcAddress`, and checks the `alGetProcAddress` path the engine uses
- opens a loopback device and checks that 44100/stereo/short is supported
- reads `alcGetIntegerv(ALC_NUM_HRTF_SPECIFIERS_SOFT)`
- creates a context with `ALC_FORMAT_TYPE_SOFT=ALC_SHORT_SOFT`, `ALC_FORMAT_CHANNELS_SOFT=ALC_STEREO_SOFT`, `ALC_FREQUENCY=44100`, `ALC_HRTF_SOFT=ALC_FALSE`
- checks `AL_EXT_float32`, `AL_SOFT_direct_channels_remix` and `AL_SOFT_source_spatialize`
- plays a 0.1 s `AL_FORMAT_STEREO_FLOAT32` sine through an `AL_FILTER_LOWPASS` filter from `alGenFilters`, using both the direct symbols and the `alGetProcAddress` lookups
- renders 4×1024 frames with `alcRenderSamplesSOFT` and checks that the output is not silent.

Compile and link:

```
arm-none-eabi-g++ -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft -mword-relocations -ffunction-sections \
  -D__3DS__ -O2 -std=gnu++17 -fexceptions -frtti -Wall -Wextra -isystem $DEVKITPRO/libctru/include \
  -I$DEVKITPRO/portlibs/3ds/include -I$DEVKITPRO/portlibs/3ds/include/AL -DAL_LIBTYPE_STATIC -c openal-loopback-test.cpp
arm-none-eabi-gcc -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft -mword-relocations -ffunction-sections \
  -D__3DS__ -O2 -Wall -Wextra -isystem $DEVKITPRO/libctru/include -c ports/3ds/source/sysbase_hooks.c
arm-none-eabi-g++ -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft -specs=3dsx.specs -Wl,-Map,openal-loopback-test.map \
  openal-loopback-test.o sysbase_hooks.o -o openal-loopback-test.elf \
  -L$DEVKITPRO/libctru/lib -L$DEVKITPRO/portlibs/3ds/lib -lopenal -lm -lctru -lm
3dsxtool openal-loopback-test.elf openal-loopback-test.3dsx
```

- 0 compiler warnings, 0 linker warnings, **0 unresolved symbols**. `.got` is 8 bytes, the same as a plain libctru program.
- `size` (with `sysbase_hooks.o`): text 1,738,636, data 170,136, bss 939,708. The `.3dsx` is 1,935,128 bytes (sha256 `e97ff2a9…7bb8e4`).
- Every one of the 33 `al*`/`alc*` functions named in `Source_Files/` (including `alSource3f`, `alSource3i`, `alListener3i`, `alSourceRewind`, `alSourceQueueBuffers`/`Unqueue`, `alcDestroyContext`, `alcCloseDevice` and the EFX filter calls) is defined (`T`) in `libopenal.a`.
- The link test also links `ports/3ds/source/sysbase_hooks.c`: without it `alcCreateContext` fails at run time (pthread keys are ENOSYS, see `PATCHES.md` 4), so the test could not pass. The script checks that `__syscall_tls_create` is defined in the test ELF.
- **Not run.** There is no emulator or 3DS on the build host. To check on the device, run `openal-loopback-test.3dsx` and look for `RESULT: all passed` in `sdmc:/3ds/openal-test.log`.

### Problems found and how they were handled

1. **`int32_t` is `long` on devkitARM, so 1.25.2 does not compile.** All translation units fail with `common/altypes.hpp:114:7: error: no type named 'type' in 'struct al::make_strict<int>'` and `common/altypes.hpp:508:39: error: 'sys_uint' does not name a type`. `al::make_strict<>` has specializations only for the `int*_t` types, and arm-none-eabi GCC defines `__INT32_TYPE__` as `long int`. A compiler flag cannot fix this without changing `int32_t` everywhere, so a small patch adds constrained (`requires`) specializations for `int` and `unsigned`, active only when they differ from `int32_t`/`uint32_t`. The script applies it. Upstream master still has the bug as of 2026-10-10.
2. **ELF hardening linker flags ended up in `openal.pc` and the exported target.** `check_linker_flag` accepts `-Wl,-z,noexecstack`, `-z,relro`, `-z,now` and `--as-needed,--no-copy-dt-needed-entries` with arm-none-eabi-ld, so `openal.pc` read `Libs.private: -Wl,-z,noexecstack -Wl,-z,relro -Wl,-z,now -Wl,--as-needed,... -lm`, and the link warned `ld: warning: -z relro ignored`. Fixed by pre-seeding the four `HAVE_WL_*` cache values to OFF.
3. **Code compiled position-independent (`-fPIC`).** Upstream forces `POSITION_INDEPENDENT_CODE TRUE` on its helper targets, so the bundled fmt was compiled with `-fPIC`, which put a 0x60-byte GOT into the executable. A `CMAKE_PROJECT_INCLUDE` file clears the compiler's PIC option, and the script now fails if `-fPIC` appears in the build log.
4. **Upstream packaging gaps (not changed, worked around in the consumer):** `OpenALConfig.cmake` does not `find_dependency(Threads)`, and no `OpenALConfigVersion.cmake` is installed (see "Using it from CMake").
5. **Threads, atomics and newlib: links, but two run-time gaps.** `std::thread`, `std::mutex` and C++20 `std::atomic::wait` link against libstdc++ and libsysbase pthreads (`__syscall_thread_create` etc. come from libctru). No `-latomic` and no compat header are needed. **Correction (found in the stage 3 review):** `tss` uses `pthread_key_create`, which *links* but returned ENOSYS at run time, because libsysbase forwards it to the weak `__syscall_tls_*` hooks and libctru 2.7.0 defines none of them. `al::Context` holds a `tss<ALenum>` whose constructor throws on failure, so `alcCreateContext` would always return NULL. `ports/3ds/source/sysbase_hooks.c` supplies the hooks (`PATCHES.md` 4). It also replaces libsysbase's ENOSYS `sched_yield`, which made `StopEventThrd`'s `std::this_thread::yield()` loop (`al/event.cpp:238`) a pure busy-wait. The game and this link test both link it. The build log could not show this: only a link-map/disassembly audit did.
6. **Remaining warnings (21):** 20 `-Wcast-align` (pffft vector casts, `hrtf_loader.cpp`, `ringbuffer.h`) and 1 `-Wrange-loop-construct` (`al/source.cpp:2573`). They show up because ARM enforces alignment where x86 does not. The buffers involved come from OpenAL's own aligned allocators.

### Runtime notes (loopback device)

- Threads: **one per context**, the event thread. `al/event.cpp:218` `std::thread{EventThread, ctx}`, started by `alcCreateContext` (`alc/context.cpp:274`). Through libctru it gets a 32 KB stack, priority 0x3F and core 0. It sleeps in `atomic_wait` and is woken by the mixer. The loopback backend has no mixer thread; `NullBackend`'s thread only starts for `alcOpenDevice`, which the engine never calls. All mixing runs inside `alcRenderSamplesSOFT` on the SDL audio thread.
- Static init fills about 872 KB of bsinc resampler tables in `.bss`.
- Only the C mixer is built (no SIMD), with upstream STL hardening (`_GLIBCXX_ASSERTIONS`) and `-ftrivial-auto-var-init=pattern`. Watch the mixer's CPU use on Old 3DS. `OPENAL_STL_HARDENING=None` is available as an option.

---

## libsndfile (3DS)

**Status: built and installed. Static, no external codecs.** Script: `scripts/3ds/build-libsndfile.sh [FILE...]`

| | |
| --- | --- |
| Version | libsndfile **1.2.2**, the newest upstream release (2023-08-13) |
| Source | `https://github.com/libsndfile/libsndfile/releases/download/1.2.2/libsndfile-1.2.2.tar.xz` |
| SHA-256 | `3799ca9924d3125038880367bf1468e53a1b7e3686a934f098b7e1d286cdb80e`: computed from the release asset and identical to Homebrew's `libsndfile` formula checksum. Upstream publishes only a GPG `.asc` and no checksum file. The tarball's SHA-512 also matches Arch Linux's PKGBUILD (`a6e3ea3a…b5beee352a0`). |
| Installed | `$DEVKITPRO/portlibs/3ds/lib/libsndfile.a`, `include/sndfile.h`, `include/sndfile.hh`, `lib/pkgconfig/sndfile.pc`, `lib/cmake/SndFile/*`, `share/doc/libsndfile/*`. No shared libraries; the script fails if any are installed. |
| Link line | `arm-none-eabi-pkg-config --static --libs sndfile` → `-L/opt/devkitpro/portlibs/3ds/lib -lsndfile` (also needs `-lm`; the 3DS toolchain always adds `-lctru -lm`) |
| CMake | `find_package(SndFile CONFIG REQUIRED)` → `SndFile::sndfile`, or `pkg_check_modules(SNDFILE REQUIRED IMPORTED_TARGET sndfile)` → `PkgConfig::SNDFILE`. Both tested with the 3DS toolchain. |
| Logs | `build/3ds-deps/logs/libsndfile-{configure,build,install,verify,host-configure,host-build}.log` |

### Configuration

All option names were checked against 1.2.2's `CMakeLists.txt`. `BUILD_TESTING` comes from `include(CTest)`.

```
-DCMAKE_TOOLCHAIN_FILE=$DEVKITPRO/cmake/3DS.cmake -DCMAKE_INSTALL_PREFIX=$DEVKITPRO/portlibs/3ds
-DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DCMAKE_BUILD_TYPE=Release
-DBUILD_SHARED_LIBS=OFF -DENABLE_EXTERNAL_LIBS=OFF -DENABLE_MPEG=OFF -DENABLE_EXPERIMENTAL=OFF
-DBUILD_PROGRAMS=OFF -DBUILD_EXAMPLES=OFF -DBUILD_TESTING=OFF -DBUILD_REGTEST=OFF -DENABLE_CPACK=OFF
-DENABLE_PACKAGE_CONFIG=ON -DINSTALL_PKGCONFIG_MODULE=ON -DINSTALL_MANPAGES=OFF
-DENABLE_CPU_CLIP=OFF -DCPU_CLIPS_POSITIVE=FALSE -DCPU_CLIPS_NEGATIVE=FALSE
-DCMAKE_DISABLE_FIND_PACKAGE_{Ogg,Vorbis,FLAC,Opus,mpg123,mp3lame,Speex,SQLite3,ALSA,Sndio}=ON
CFLAGS=-Wno-error=incompatible-pointer-types   (prepended to the toolchain's CMAKE_C_FLAGS_INIT)
```

`SndFileChecks.cmake` calls `find_package()` for every codec no matter what the options say, and portlibs does contain libogg and libmpg123. The `CMAKE_DISABLE_FIND_PACKAGE_*` flags keep those from being picked up. After configuring, the script checks that `config.h` has `HAVE_EXTERNAL_XIPH_LIBS 0` and `HAVE_MPEG 0`, and that `CMAKE_C_FLAGS` still contains the 3DS architecture flags.

### Problems and workarounds

1. **CMake 4 rejects the project.** `cmake_minimum_required (VERSION 3.1..3.18)` uses two dots, not the `...` range syntax, and CMake 4.4.4 fails with `Compatibility with CMake < 3.5 has been removed from CMake.` Fix: `-DCMAKE_POLICY_VERSION_MINIMUM=3.5`.
2. **GCC 16 hard error in `src/alac.c`.**
   ```
   alac.c:430:45: error: passing argument 3 of 'alac_decode' from incompatible pointer type [-Wincompatible-pointer-types]
   note: expected 'int32_t *' {aka 'long int *'} but argument is of type 'int *'
   alac.c:443:60: error: passing argument 3 of 'alac_encode' from incompatible pointer type
   ```
   On arm-none-eabi newlib, `int32_t` is `long int`. Both types are 32-bit, so the layout is identical. The fix downgrades this diagnostic to a warning with `-Wno-error=incompatible-pointer-types` and leaves upstream unpatched. The build has exactly these 2 warnings and no others. The host build has none, because on macOS `int32_t` is `int`.

No threading, atomics or newlib gaps came up. All of `check_function_exists` linked for real against `-specs=3dsx.specs -lctru -lm`.

### Verification

`scripts/3ds/tests/sndfile-vio-test.c` makes the same libsndfile calls as `Sound/SndfileDecoder.cpp`:
- `sf_open_virtual` with `SF_VIRTUAL_IO` callbacks over an in-memory file;
- `sf_read_float(sf, (float*)buf, max_length/4)*4`;
- `sf_seek(0, SEEK_SET)` for rewind, plus SEEK_SET, SEEK_CUR and SEEK_END seeks;
- `sf_close`.

Its inputs are WAV and AIFF files (16-bit stereo, 22050 Hz, 3000 frames) that the test writes byte by byte.

* **3DS: compiled and linked, not run.** It builds with `arm-none-eabi-gcc … -Wall -Wextra` with 0 warnings, links with `-specs=3dsx.specs … -lsndfile -lctru -lm`, and packs with `3dsxtool` into `build/3ds-deps/libsndfile-1.2.2/test/sndfile-vio-test.3dsx`. `nm` shows `sf_open_virtual`, `sf_read_float`, `sf_seek`, `sf_close`, `sf_strerror` and `sf_command` defined, and the link map shows `libsndfile.a(sndfile.o)`. libsndfile adds about 427 KB of `.text` (528,780 bytes vs 101,516 for a minimal libctru program). It has not been run on hardware or in an emulator.
* **Host: run.** The same tarball and options were built with Apple clang 17 in `build/3ds-deps/libsndfile-1.2.2/build-host`, which is not installed. Result: **RESULT: all passed (0 failures)**. Details:
  * WAV `0x010002` and AIFF `0x020002`: channels, rate and frame count correct; all 6000 samples equal `int16/32768.0f` exactly; rewind and all seeks correct; a short read at EOF returns 5 frames.
  * Ogg (`OggS`) and FLAC (`fLaC`) streams: `File contains data in an unimplemented format.`
  * ID3v2/MP3 stream: `Format not recognised.`
  * The real `data/Scenarios/Marathon 2/Music.ogg`: **NOT OPENED: File contains data in an unimplemented format.**

**Major formats in this build (23):** AIFF, AU, AVR, CAF, HTK, IFF/SVX, MAT4, MAT5, MPC2K, PAF, PVF, RAW, RF64, SD2, SDS, IRCAM (SF), VOC, W64, WAV, NIST (WAV), WAVEX, WVE, XI.
**Subtypes (28):** PCM S8/16/24/32, U8, float32, float64, u-law, A-law, IMA/MS/VOX ADPCM, GSM 6.10, G721/G723-24/40, DWVW 12/16/24, NMS ADPCM 16/24/32, DPCM 8/16, ALAC 16/20/24/32.

**Not available: Ogg Vorbis, Ogg Opus, FLAC, MP3/MPEG.** The 3DS portlibs have only Tremor (`libvorbisidec`, integer, not the libvorbis + libvorbisenc API that libsndfile needs) and mpg123 without lame, and they have no usable libFLAC or libopus.

### Consequences / follow-up

* **Format gap (stub):** Marathon 2's `Music.ogg` (intro/menu music) does not play on 3DS. The same goes for any Ogg, Opus, FLAC or MP3 level music added through MML or Lua, and for replacement sounds or plugin audio in those formats. `SndfileDecoder::Open` fails and the music is silent. Sound effects (`Sounds.sndA`) use Aleph One's own parser and are unaffected. Fix: a follow-up `StreamDecoder` over Tremor (`vorbisidec`) and/or mpg123, tried in `Sound/Decoder.cpp` after `SndfileDecoder`.
* `SndfileDecoder` still produces 32-bit float, so OpenAL needs `AL_EXT_float32`.
* 1.2.2 does not contain the CVE-2022-33065 fix (signed overflows in several format parsers; Arch applies upstream commit `0754562e`). This only matters for malicious third-party audio.
* Upstream quirk on every CMake build: `config.h` has `HAVE_SSIZE_T 0` → `#define ssize_t intptr_t`. Both are 32-bit int on 3DS, so it is harmless.
