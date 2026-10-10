# Aleph One 3DS port — build environment

Recorded 2026-10-10 on the stage-0 environment setup. No engine code has been
touched; everything here lives under `scripts/3ds/`, `ports/3ds/probe/` and
`docs/3ds/`.

## Status

| Step | Result |
| --- | --- |
| devkitPro pacman install | OK (v6.0.2 macOS `.pkg`, unsigned upstream) |
| `scripts/3ds/setup-env.sh` — all 11 requested packages | OK, all installed |
| `docs/3ds/portlibs-inventory.txt` | OK, 47 `3ds-*` packages |
| 3DS SDL2 packages in dkp-libs | **None.** Only SDL 1.2 exists for 3DS |
| SDL2 2.32.10 cross-build | OK, 0 warnings |
| SDL2_ttf 2.24.0 cross-build | OK, 0 warnings |
| SDL2 + SDL2_ttf installed into `$DEVKITPRO/portlibs/3ds` | OK (`build-sdl2.sh`) |
| Probe `.3dsx` compiles and links against portlibs SDL2 | OK, 0 warnings with `-Wall -Wextra` |
| `std::filesystem` compiles + links | OK, backed by real `opendir`/`readdir` |
| `std::filesystem` works at runtime | OK **in emulator** (Azahar 2126.2); not yet run on hardware |
| Exceptions, `dynamic_cast` at runtime | OK in emulator |
| SDL2 400x240 window + red fill | API calls succeed in emulator; red output not visually confirmed |

## Host

| | |
| --- | --- |
| OS | macOS 26.1 (25B5072a), arm64 |
| Host CMake | 4.4.4 (Homebrew — not part of devkitPro, must be installed separately) |
| git | 2.52.0 |
| devkitPro pacman | Pacman v6.0.2 / libalpm v13.0.2 |

## Toolchain versions

```
$ $DEVKITARM/bin/arm-none-eabi-gcc --version
arm-none-eabi-gcc (devkitARM) 16.1.0
Copyright (C) 2026 Free Software Foundation, Inc.

$ $DEVKITARM/bin/arm-none-eabi-ld --version
GNU ld (GNU Binutils) 2.46.0.20260210
```

| Package | Version |
| --- | --- |
| devkitARM | r68-1 |
| devkitarm-gcc | 16.1.0-1 |
| devkitarm-binutils | 2.46.0-1 |
| devkitarm-newlib | 4.6.0.20260123-5 |
| devkitarm-crtls | 1.2.6-1 |
| devkitarm-rules | 1.6.0-4 |
| devkitarm-cmake | 1.2.4-1 |
| dkp-cmake-common-utils | 1.5.3-1 |
| libctru | 2.7.0-1 |
| citro3d / citro2d | 1.7.1-2 / 1.7.0-1 |
| 3dstools (3dsxtool, smdhtool) | 1.3.1-3 |
| general-tools | 1.4.4-1 |

## Packages

Requested by `setup-env.sh` — all exist and are installed:

| Package | Version | Note |
| --- | --- | --- |
| 3ds-dev | — | *group*: devkitARM, libctru, citro2d/3d, 3dstools, picasso, 3dslink, tex3ds, catnip, cmake helpers, examples |
| 3ds-cmake | 1.5.2-1 | provides `$DEVKITPRO/cmake/3DS.cmake` |
| 3ds-pkg-config | 0.28-5 | |
| 3ds-zlib | 1.3.1-1 | |
| 3ds-libpng | 1.6.53-1 | |
| 3ds-freetype | 2.14.1-1 | depends on 3ds-bzip2 1.0.8, 3ds-libpng, 3ds-zlib |
| 3ds-libogg | 1.3.6-1 | |
| 3ds-libvorbisidec | 1.2.1-3 | |
| 3ds-mpg123 | 1.31.3-3 | |
| picasso | 2.7.2-3 | |
| 3dslink | 0.6.3-1 | |

### SDL

dkp-libs has **no** `3ds-sdl2`, `3ds-sdl2_ttf`, `3ds-sdl2_image`,
`3ds-sdl2_mixer` or `3ds-sdl2_net`. For the 3DS it ships only SDL 1.2:
`3ds-sdl` 1.2.15, `3ds-sdl_ttf` 2.0.11, `3ds-sdl_image` 1.2.12,
`3ds-sdl_mixer` 1.2.12, `3ds-sdl_gfx` 2.0.25. (Wii, GameCube, Switch and Wii U
all have SDL2 packages; the 3DS does not.)

SDL2 is therefore built from upstream with `scripts/3ds/build-sdl2.sh`:

| Library | Tag | Config |
| --- | --- | --- |
| SDL2 | `release-2.32.10` | static; N3DS video, audio, joystick, thread and timer drivers enabled |
| SDL2_ttf | `release-2.24.0` | static; system freetype 2.14.1; HarfBuzz off |

Full list of 3DS packages: [`portlibs-inventory.txt`](portlibs-inventory.txt).

## std::filesystem

**Compiles and links:** yes, with `-std=gnu++17 -fexceptions -frtti` and no
extra libraries (no `-lstdc++fs`).

**Real implementation, not a stub:** the linked
`std::filesystem::directory_iterator` constructor calls newlib's `opendir`
directly (`bl <opendir>` in the disassembly). libstdc++ was configured with
`_GLIBCXX_HAVE_DIRENT_H`, `_GLIBCXX_HAVE_STRUCT_DIRENT_D_TYPE`,
`_GLIBCXX_USE_LSTAT`, `_GLIBCXX_USE_MKDIR`, `_GLIBCXX_USE_CHDIR`,
`_GLIBCXX_USE_GETCWD`, `_GLIBCXX_USE_CHMOD`, `_GLIBCXX_USE_ST_MTIM`,
`_GLIBCXX_HAVE_READLINK`, `_GLIBCXX_HAVE_SYMLINK`, `_GLIBCXX_HAVE_TRUNCATE`.

**Works at runtime:** yes in the Azahar 2126.2 emulator, with the probe at
`sdmc:/3ds/aleph-probe.3dsx`. Both the `error_code` and throwing overloads
iterated `sdmc:/3ds`, and a missing directory threw `filesystem_error`
(`No such file or directory`). **Not yet run on real hardware**; the emulator's
SD card is an HLE host folder, so a hardware run is still required.

Probe output from the emulator (`sdmc:/3ds/probe.log`):

```
PASS SDL_Init: SDL 2.32.10
  aleph-probe.3dsx
PASS filesystem(error_code): 1 entries in sdmc:/3ds
PASS filesystem(throwing): 1 entries
PASS filesystem(missing dir): caught: No such file or directory
PASS exceptions: probe exception 42
PASS dynamic_cast: hit=Circle miss=null typeid=N12_GLOBAL__N_16CircleE
PASS SDL_CreateWindow: 400x240 SDL_PIXELFORMAT_RGBA8888
PASS SDL red fill: top screen should be red for 3 s
RESULT: all passed
```

"SDL red fill" passing means `SDL_UpdateWindowSurface` returned 0; nobody has
looked at the screen yet.

## Build commands

All commands run from the repository root as a normal user. `sudo` is only
used inside the scripts, for pacman and for installing into portlibs. Run the
scripts from a real terminal (Terminal.app): `sudo` cannot prompt for a
password in a shell without a TTY.

```sh
# 0. One-time: install devkitPro pacman (macOS)
curl -LO https://github.com/devkitPro/pacman/releases/download/v6.0.2/devkitpro-pacman-installer.pkg
sudo installer -pkg devkitpro-pacman-installer.pkg -target /

# Environment (add to your shell profile)
export DEVKITPRO=/opt/devkitpro
export DEVKITARM=$DEVKITPRO/devkitARM

# Host CMake (not shipped by devkitPro on macOS)
brew install cmake

# 1. Packages + inventory
scripts/3ds/setup-env.sh

# 2. SDL2 + SDL2_ttf into $DEVKITPRO/portlibs/3ds
scripts/3ds/build-sdl2.sh

# 3. Probe
cmake -S ports/3ds/probe -B build/3ds-probe \
      -DCMAKE_TOOLCHAIN_FILE="$DEVKITPRO/cmake/3DS.cmake" \
      -DCMAKE_BUILD_TYPE=Release
cmake --build build/3ds-probe
# -> build/3ds-probe/aleph-probe.3dsx

# Run on hardware over Wi-Fi (Homebrew Launcher, press Y to listen)
$DEVKITPRO/tools/bin/3dslink build/3ds-probe/aleph-probe.3dsx
```

Exact compile and link lines the probe build produced:

```
arm-none-eabi-g++ -DPROBE_WITH_SDL -isystem <SDL2 include> -isystem <SDL2 include>/SDL2
  -isystem $DEVKITPRO/libctru/include
  -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft -mword-relocations
  -ffunction-sections -D__3DS__ -g -O2 -DNDEBUG
  -std=gnu++17 -fexceptions -frtti -Wall -Wextra -c source/main.cpp

arm-none-eabi-g++ -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft
  -mword-relocations -ffunction-sections -D__3DS__ -g -O2 -DNDEBUG
  -L$DEVKITPRO/libctru/lib -L$DEVKITPRO/portlibs/3ds/lib -specs=3dsx.specs
  main.o -o aleph-probe.elf $DEVKITPRO/portlibs/3ds/lib/libSDL2.a -lm -lctru -lm -lctru -lm
```

With SDL2 installed into portlibs, the toolchain finds it without any extra
flags (`SDL2_DIR` resolves to `$DEVKITPRO/portlibs/3ds/lib/cmake/SDL2`). The
emulator results above are from this build.

## Problems found during setup

1. **GPGME "Invalid crypto engine" with the raw pacman binary (macOS).**
   `/opt/devkitpro/pacman/bin/pacman` fails every signature check unless
   `/opt/devkitpro/pacman/bin` is on `PATH`, because GPGME finds
   `gpgconf`/`gpg` through `PATH`. The `/usr/local/bin/dkp-pacman` wrapper
   that the installer adds sets `PATH` itself; `setup-env.sh` prefers it and
   also exports the path for the fallback.
2. **`3ds-dev` is a group, not a package.** `pacman -Si 3ds-dev` reports "not
   found"; check with `-Sg` too. (This was a bug in the first draft of
   `setup-env.sh`, fixed before it ever ran an install.)
3. **`sudo` needs a TTY.** Running `setup-env.sh` from a non-interactive shell
   fails at the first `sudo` without installing anything.
4. **`NINTENDO_3DS` is undefined before `project()`.** The platform file is
   loaded by `project()`, so a toolchain guard must come after it.
5. **CMake 4.x** warns about SDL2's `cmake_minimum_required(VERSION 3.4...3.5)`
   at line 3300 but configures fine; no `CMAKE_POLICY_VERSION_MINIMUM`
   override was needed.

## Open items

- Run the probe on a New 3DS and confirm `probe.log` and the red top screen.
- SDL2_ttf is built but no program links it yet. Its exported CMake target
  links `Freetype::Freetype` only; static freetype also needs `libpng`, `libz`
  and `libbz2`, which will probably have to be added explicitly at link time.
- SDL2_image is not packaged for the 3DS either and is not built yet.
