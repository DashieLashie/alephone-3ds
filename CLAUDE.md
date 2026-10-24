# CLAUDE.md

This repo is **Aleph One** (the open-source Marathon engine, C++/SDL2) with a **Nintendo 3DS port** on top.
Everything in `Source_Files/` is upstream code; the port is a thin layer of fenced patches plus `ports/3ds/`.
The upstream `README.md` is about the desktop engine. The port's history, decisions and measurements are in
`docs/3ds/` (index at the end). This file is the short version: the rules and the traps.

## State of the project

- Upstream base: tag `release-20260930` (`fc4c274`, v1.11.1). Port branch: **`3ds-port`** (checked out in the main
  checkout, `/Users/dashielashie/Desktop/Aleph_3ds`). There is no local `main`.
- Stages 0-7 are done and passed on a real **New 3DS** (Marathon 2 plays to the end of level 1+, controls, audio,
  HUD + Map on the bottom screen, 30 fps cap), plus a performance stage and a load-time sprint (`NEXT-SPRINT.md`).
- **Stage 10** (optional citro3d GPU renderer behind `A1_RENDER_C3D`) lives on branch **`stage10-c3d`**, in the worktree
  `.claude/worktrees/stage10-c3d`. P0 (software baseline) and P1 (GPU present, tag `port-stage10-P1`) are done.
  The G0-b console runs were all done on 2026-10-19 (`PERF.md` "Console G0-b", two sessions). **User decisions
  (2026-10-19):** keep the GPU present, G1 = build the renderer aiming at M7 (supersampling) and M9 (stereo),
  try the zero-copy present. The plan is the end of `docs/3ds/NEXT-SPRINT.md`: fixes first (HOME > Close in a
  game hangs with the GPU present; backlog items 14 and 15), then zero-copy, then M0-M6, M7, M9.
- Nothing is pending on `3ds-port`: the "Catch-up runbook (stage 3)" in `NEXT-SPRINT.md` (a sprint stage, not port
  stage 3) was run on the console on 2026-10-18. Its optional leftovers are in the backlog.
- Hardware: one **New 3DS / New 2DS**. Every Old 3DS figure comes from Azahar's Old 3DS mode and is unverified.

## Git

- Two remotes. `origin` is **upstream Aleph One** (`Aleph-One-Marathon/alephone`): read-only for us, never push to it and
  never open PRs against it. `alephone-3ds` is **the user's fork** (`github.com/DashieLashie/alephone-3ds`), where the
  port is published: it already holds `3ds-port`, `stage10-c3d` and the tag `port-stage10-P1` (the user pushed them).
  Pushing to the fork is allowed, but it is outward-facing: do it only when the user asks (or has said to, for the
  task at hand), push the branch you worked on, never force-push, and never push to `origin`. The clone is shallow
  (boundary `fc4c274`); the fork accepts pushes as is.
- Backups: the fork is one copy; local git bundles in `~/Documents/Aleph_3ds-backups/` (`fc4c274..<branch>`,
  refreshed and restore-tested after approved commits; recipe in `docs/3ds/STAGE5-PREREQS.md` section 8) are the other.
- Commit subjects: `3DS port: <summary>` on `3ds-port`, `stage10: <id> <summary>` on `stage10-c3d` (one commit and a
  `port-stage10-<id>` tag per milestone). One commit per finished piece.
- Run git from the checkout's own directory with literal arguments. In a worktree session a compound
  `cd X && git ...` or anything that `source`s a string is rejected; a path containing `source` (as in
  `ports/3ds/source/`) can trip the same guard, so glob it (`ports/3ds/s*`).
- Build dirs (`build-3ds*/`, including `build-3ds/hw/`) are ignored through `.git/info/exclude`, not `.gitignore`.
  Hardware logs and captures under `build-3ds/hw/` are never in git.

## Repo map

| Path | What |
| --- | --- |
| `Source_Files/` | upstream engine. `RenderMain/` is the software rasteriser (`render.cpp`, `scottish_textures`, `shapes.cpp`) and the OpenGL renderer (`OGL_*`, not built on 3DS: `HAVE_OPENGL` is undefined); `RenderOther/screen.cpp` is the screen/HUD/present seam; `Sound/` is OpenAL-as-mixer; `Input/`, `Misc/` (prefs, interface), `Files/`, `CSeries/` (paths) |
| `ports/3ds/` | the port: `CMakeLists.txt` (derives the 187 engine sources from the `Makefile.am`s at configure time), `config.h`, `include/`, `source/` (platform, audio, input, video present, bottom screen, profiler, toolchain fixes), `render_c3d/` (stage 10), `probe/`, `res/` |
| `scripts/3ds/` | setup and dependency builds (SDL2, OpenAL, libsndfile, Boost headers), `run-azahar.sh`, `sd-ftp.sh`, `host-baseline/` (Docker: Linux build, `ppcheck.sh`), `tests/` (input scripts, plugins, `perf-bench.sh`, `c3d/` configs) |
| `docs/3ds/` | the port's documentation; `PATCHES.md` is the register of every engine edit |
| `.claude/agents/` | stage 10 subagents: `c3d-architect`, `c3d-critic` (read-only), `c3d-implementer` (one milestone at a time) |
| `data/Scenarios/` | **submodules** (Marathon, Marathon 2, Infinity). Empty in a worktree, see Azahar below |

## Build

```sh
export DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM       # also in ~/.zshenv
cmake -S ports/3ds -B build-3ds                    # normal           -> build-3ds/alephone.3dsx
cmake -S ports/3ds -B build-3ds-prof -DA1_PROFILE=ON                      # frame profiler + benchmark mode
cmake -S ports/3ds -B build-3ds-c3d  -DA1_RENDER_C3D=ON                   # stage 10 (branch stage10-c3d only)
cmake -S ports/3ds -B build-3ds-c3d-prof -DA1_RENDER_C3D=ON -DA1_PROFILE=ON
cmake --build <dir>
```

Release is devkitPro's `-g -O2 -DNDEBUG`; the build warns if `-O2` is missing. The stage 3 build was recorded at 0 compiler/linker warnings (`STAGE3.md`); don't add any.
Setup from scratch (devkitPro, SDL2, OpenAL, libsndfile, Boost headers): `docs/3ds/ENVIRONMENT.md`, `STAGE3.md`.
Host (Linux, Docker) builds and the preprocessor check: `docs/3ds/AUDIT.md` section 0, `STAGE2.md`.

## Rules (carried over from every stage)

1. **Measure before and after** any performance change (profile build, `debug-perf`); keep only changes with a measured
   gain and no visual regression. Many ideas were measured and rejected (`PERF.md` experiments b, c, d). Read it first.
2. **Every engine edit** (anything under `Source_Files/`) is fenced with `#ifdef A1_PLATFORM_3DS`, gets a numbered
   entry in `docs/3ds/PATCHES.md`, and passes the non-3DS check:
   `scripts/3ds/host-baseline/ppcheck.sh "<changed files>" "<TUs>"` (Docker). IDENTICAL is the goal; DIFFER is fine only
   if the printed normalised line count is 0 (just `__LINE__` shifts). Next free patch numbers: 45-48 are reserved by
   stage 10 for M0/M2/M6/M8; 49-53 are used; take 54+ for anything else.
3. Port code goes in `ports/3ds/`. Engine TUs never include `<3ds.h>` (libctru's `u8`, `Handle`... must not leak).
4. **Azahar first, then the console.** Record results in the stage's doc in the style of the existing ones.
   Azahar results are labelled "Azahar — non-authoritative".
5. Anything that needs `sudo`: give the user the command to run in **Terminal.app** (a `!` command has no TTY and fails
   silently). Verify the install afterwards.
6. Never break the host build or the default (software renderer, `A1_RENDER_C3D=OFF`) 3DS build. On the OFF
   `alephone.elf`, `$DEVKITARM/bin/arm-none-eabi-nm build-3ds/alephone.elf | grep -ciE 'citro|C3D_|a1_c3d'` must
   print 0. Not macOS's `nm` (it cannot read ARM ELF and prints nothing), not a bare `c3d` (matches addresses).
7. No guessing about citro3d/PICA200: read `$DEVKITPRO/libctru/include`, `citro3d.h`, `c3d/*`, and the examples in
   `/opt/devkitpro/examples/3ds/graphics/gpu/`; cite them. `CITRO3D_DESIGN.md` marks unverified claims **UNVERIFIED**.
8. When the user is away ("working overnight"): do only work that needs neither the console nor a decision (Azahar runs,
   code, docs, measurements, commits). Write console sessions and choices up as a runbook for their return, as
   `NEXT-SPRINT.md` does. Ask nothing mid-run; outward-facing actions (reports, pushes, even to the fork) wait.

## Platform traps (all cost a debugging session once)

**Toolchain.** devkitARM r68 / gcc 16.1, libctru 2.7.0, citro3d 1.7.1. **No LTO**: it drops walls from the picture (the
rasteriser has strict-aliasing UB; `PERF.md` experiment b). `-DNDEBUG` compiles out standard `assert()`. `int32_t` is
`long` on newlib (`vbl.cpp` fix, `%d` formats). C++ exceptions only work because of `exidx_fix.c` (a devkitARM `3dsx.ld`
bug; report drafted in `devkitpro-3dsx-ld-report.md`). `std::random_device` has no entropy: seed from `PS_GenerateRandomBytes`
(`PATCHES.md` 14). There is no environment (`getenv` is NULL, no `$HOME`); all paths come from `a1_3ds::user_dir()`
(`sdmc:/3ds/alephone`) and `scenario_dir()`. No SDL2 package exists for the 3DS: SDL2/SDL2_ttf are built by
`scripts/3ds/build-sdl2.sh`.

**Threads.** Horizon schedules by strict priority and threads are cooperative: a spin loop starves its core. Every wait
must sleep or block (`sched_yield` is `svcSleepThread(1)`); four main-thread busy-waits were fixed in stage 6
(`PATCHES.md` 32-33). Single-player runs the main thread (all rendering) plus the SDL audio thread (OpenAL mixing and
music decoding; core 1, priority 0x19, 80 KiB stack) plus one OpenAL event thread. Don't add threads casually. Rendering and
citro3d belong to the main thread: the critic's open P1 minor 4 is that `error_screen()` called from another thread
must not touch citro3d (`NEXT-SPRINT.md`, G0-b item 7).

**Memory.** Main stack is 1 MiB (`__stacksize__`; 32 KiB overflowed). The heap area is 96 MiB; the linear heap is
sized in `a1_3ds_platform.cpp` (`__ctru_linear_heap_size`) and **must fit inside it** or the console fails to boot
(Azahar does not: `PATCHES.md` 25; New 3DS: heap 98 304 KiB, linear 19 960 KiB). The audio/music path uses bounded
chunks (<= 8 KiB PCM per request); never load a whole track. citro3d costs about 1 MiB of linear heap; VRAM is 6 MiB.

**Screens.** The top screen is 400x240; menus and dialogs stay 640x480 logical and are halved. In game, HUD + Map
(default, `bottom_screen=0`) renders the view 1:1 at 400x240 and draws HUD + overhead map on the bottom screen
(320x240); Classic (`bottom_screen=2`) is the 640x480 halved view with the HUD. Both screens are written straight into
the framebuffers by `video_3ds.cpp` / `bottom_3ds.cpp`, not through SDL's present. With citro3d up it owns the top screen:
call `GX_*` only through citro3d's queue, and release citro3d before `gfxExit` (the `--wrap=gfxExit` hook).

**Stubs / lost by design:** no hyperlinks, no network (`A1_NO_NETWORK`), no OpenGL, no film export, no Opus/FLAC,
no HRTF/3D sound/surround, no pthread-key destructors. `STAGE3.md` "Stubs and lost functionality" is the full list.

## Testing in Azahar

Azahar 2126.2 is a portable install in `~/Applications/Azahar-2126.2/` (`run-azahar.sh` finds it by glob).
It models instruction cost, **not cache misses**: it overstates `render` ~40% and understates copies/`present`
~40% against the console (`PERF.md` "How to read these numbers"). Use it for correctness and relative checks; the
console decides performance.

```sh
scripts/3ds/run-azahar.sh --screens --timeout 60                    # boot, print log.txt, PNGs -> build-3ds/azahar-screens/
scripts/3ds/run-azahar.sh --script scripts/3ds/tests/input/NN-x.txt --timeout 150   # drive buttons/sticks/touch/HOME
scripts/3ds/run-azahar.sh --perf LEVEL build-3ds-prof/alephone.3dsx                  # one benchmark
PREFS="fps_target=60 bottom_screen=0" RUNS=1 scripts/3ds/tests/perf-bench.sh LABEL [3dsx]   # series: L0/L6/L20
python3 -I scripts/3ds/tests/perf-table.py build/perf/A.txt [B.txt ...]              # tables
```

Gotchas: **in a worktree the scenario submodule is empty**, so pass
`SCENARIO_SRC="/Users/dashielashie/Desktop/Aleph_3ds/data/Scenarios/Marathon 2"`; `run-azahar.sh` empties
`build-3ds/azahar-screens/` every run (copy captures away); script screenshots are not pixel-comparable across builds
(random seed, several starts), so diff rendering with `perf-bench.sh`'s end-of-run captures; Azahar must be launched
double-forked and needs SIGKILL when a modal is up (the script does both); `screencapture`/AppleScript/LLDB don't work
from this shell (the game saves its own screens via the `debug-screens` flag). Guest faults land in
`user/log/azahar_log.txt` (flushed on exit): resolve with `arm-none-eabi-addr2line -e build-3ds/alephone.elf`.
Level loads take 3-4 s now, but scripts keep long waits (`--timeout 150`). Run Python on untrusted files with `-I`.

**Runtime flag files** in `sdmc:/3ds/alephone/` (empty or with settings, deleted when done): `debug-perf` (benchmark),
`debug-audio`, `debug-input`, `debug-input-script`, `debug-decoders`, `debug-entropy`, `debug-screens`, and `debug-c3d`
(citro3d back end settings, `ports/3ds/render_c3d/core/config.h`).

**Reading profiler output** (`PERF.md` "The profiler"): `busy = frame - idle`. Compare `frame + overlay`, not `fps`; the
real rate at the cap is 30.30 fps (integer `1000/30` ms timer). `gaps` must be 0.

## Testing on the console

Ask the user for the console's IP and for them to press **Y** in the Homebrew Launcher (netloader). The ftpd app and the
netloader can't run at once. Full procedure: `NEXT-SPRINT.md` "How to drive the console".

- `cmake --build ... --target run-3dslink` fails here (UDP broadcast). Use
  `script -q <transcript> /opt/devkitpro/tools/bin/3dslink -a <IP> -s <3dsx>` in the background (`script` gives the pty;
  through a pipe nothing shows until exit). `3dslink -s` never exits on a failed connect, and a leftover process keeps
  port 17491: `pgrep -fl '^/opt/devkitpro/tools/bin/3dslink'`, kill stale ones (and old transcript monitors) before each launch.
- SD card files: `scripts/3ds/sd-ftp.sh IP get|put|ls|rm`, in `/3ds/alephone/`. Each boot moves `log.txt` to `log.prev.txt`.
- "Connection reset by peer" mid-run is the lid closing or Wi-Fi sleeping; the log continues on the SD card.
- Luma crash dumps: `/luma/dumps/arm11/*.dmp` via ftpd, registers at offset 0x28.
- Delete `debug-perf` / `debug-c3d` from the card at the end of a session.

## Stage 10 (branch `stage10-c3d`)

Goal (user-confirmed 2026-10-16): **HUD + Map at Framerate Target 60 sustains >= 55 fps on L0/L6/L20 on a New 3DS**,
no regression at 30, Classic @60 no worse than 53.16 fps. At 30 fps the software renderer already idles over half the
frame, so "faster at 30" is not a goal. Order: P0 baseline -> P1 GPU present -> **G0** -> (user's G1) -> M0-M5
renderer in readback mode (no speedup) -> M6 direct mode (the payoff) -> M7-M9 optional. The design is
`docs/3ds/CITRO3D_DESIGN.md` (critic-approved round 3; section 10 is the "Needs console verification" list,
section 7 the milestones). Code layout under `ports/3ds/render_c3d/`: `core/` plain C++ (host-testable), `front/`
engine headers only, `back/` `<3ds.h>`/`<citro3d.h>` only, `shaders/*.v.pica` (picasso via CMake, no hand-embedded
binaries). Workflow for a milestone: `c3d-implementer` builds it, `c3d-critic` reviews the commit, max 2 revise rounds,
then tag. `docs/3ds/KICKOFF_PROMPT.md` is the original prompt.

## Documentation index (`docs/3ds/`)

| Doc | Answers |
| --- | --- |
| `NEXT-SPRINT.md` | rules carried over, how to drive the console, open backlog, **the pending runbooks** (stage 2, catch-up, G0-b) |
| `PATCHES.md` | every upstream-source edit, numbered, with its non-3DS check |
| `PERF.md` | profiler, benchmark method, every measurement and rejected experiment, load times, stage 10 P0/P1 |
| `STAGE7.md` / `STAGE6.md` / `STAGE5.md` / `STAGE4.md` | bottom screen (HUD + Map) / audio / controls (open items table) / boot + paths + diagnostics |
| `STAGE3.md`, `STAGE3-PREREQS.md`, `STAGE5-PREREQS.md` | first link, stubs list, dependency builds; decisions and the backup recipe |
| `STAGE2.md`, `AUDIT.md` | Linux build with the 3DS feature set (`A1_NO_NETWORK`, `A1_FS_STD`, `AudioBackend`); the source audit |
| `ENVIRONMENT.md` | toolchain versions, install steps, setup problems |
| `CITRO3D_DESIGN.md`, `KICKOFF_PROMPT.md` | stage 10 |
| `devkitpro-3dsx-ld-report.md` | draft bug report for the exidx bug (not filed: outward-facing) |

Reading order for a new task: this file, the stage doc for the area, `PATCHES.md` for what is already changed there,
then `PERF.md` if it touches speed.
