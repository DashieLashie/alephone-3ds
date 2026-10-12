# Aleph One 3DS port: performance

Recorded 2026-10-11 on branch `3ds-port`, on top of stage 6 (`d19dae0`).
Goal: measure first, then optimise; keep only changes with a measured gain
and no visual regression. Engine changes are listed in
[`PATCHES.md`](PATCHES.md) (entries 34 onwards).

## Status (2026-10-11)

**Kept: the direct framebuffer present (a, a').** In Azahar the game goes
from **~15 fps to the 30 fps cap** on all three test spots, with 11-14 ms
of every 33 ms frame left idle (busy 66.9 → 21.5 ms on level 0, −68%); the picture is the same as before (within
1 per colour channel). No compiler flag change, no default resolution
change, no forced frame cap: each was measured and either gained nothing,
broke the picture, or took something visible away (details per
experiment below).

**Not yet measured on hardware.** Azahar's timing has no cache or memory
model (see "How to read these numbers"), so the console's absolute frame
times are unknown; the checklist at the end measures them and decides the
two open questions (whether to force 30 fps, whether Low resolution
should be recommended).

## Results

Every figure is ms per frame in Azahar (see "How to read these numbers").
**busy** = frame − idle: the work per frame. Once the game reaches the 30 fps
cap the frame time stays 33 ms and the gain shows as idle time instead.

| Step | Change | busy, L0 / L6 / L20 | fps | Picture | Kept |
| --- | --- | --- | --- | --- | --- |
| baseline | stage 6 build (re-run with a fixed seed, `bc7fc46`) | 66.9 / 63.8 / 63.7 | 15.0-15.7 | | |
| a | direct framebuffer present | **23.1 / 21.0 / 20.7** (−66%) | **30.3** (capped) | identical to SDL's within ±1 per channel | **yes** |
| a' | present loop fitted to registers (found through b) | **21.5 / 19.5 / 19.3** (−7% vs a) | 30.4 | byte-identical to a | **yes** |
| b | `-O3` / `-ffast-math` / LTO / LTO + `-fno-strict-aliasing` | −0.3…−0.7% / ±0.3% / −3.5…−12% / −4.5% | 30.3 | LTO: **walls missing**; LTO + no-strict-aliasing: identical | no (none of them) |
| c | half-resolution rendering, 2x upscale (the engine's Resolution: Low) | 16.7 / 16.1 / 15.8 (−24 to −28% vs a) | 30.3 | **visibly coarser**: loses the 2x2 supersampling | no (stays an option) |
| d | force the 30 fps cap, no interpolation | (30 is already the default, without interpolation) | 60 / Unlimited: **42.8 / 48.0 / 48.2** | forcing 30 shows fewer frames to anyone who chose 60 | no (decide on hardware) |

## Experiment a: direct framebuffer present

**Change** (`ports/3ds/source/video_3ds.cpp`, `PATCHES.md` 35).
`MainScreenUpdateRects()` hands the 640x480 screen surface to
`a1_3ds::present_screen()`, which writes the top framebuffer itself:

- **One pass.** For each output pixel, the 2x2 block of the surface is
  averaged, converted XRGB8888 → RGBA8 (`<< 8`; the LCD ignores alpha) and
  stored at its rotated place (the LCD scans 240-pixel columns, so screen
  pixel (x, y) is `fb[x * 240 + 239 - y]`). The 40-pixel bars left and
  right are cleared once per framebuffer, not every frame.
- **Same picture as SDL.** The engine asks SDL for *linear* scaling
  (`SDL_HINT_RENDER_SCALE_QUALITY`, `screen.cpp`), and SDL's bilinear
  filter centres output pixel x on source 2x + 0.5: at exactly 2:1 that is
  the 2x2 average. `UHADD8` (ARMv6 SIMD halving add, `__uhadd8`) averages
  four channels in one instruction; it rounds down, so a channel can be 1
  below SDL's. A first version sampled single pixels (nearest neighbour):
  3.6 ms instead of 7.7, but **half the HUD's pixels differed** from SDL's
  output (aliasing), so it was rejected.
- **Cache-friendly order.** 4 output rows per pass: the 8 source rows are
  read left to right, and every framebuffer column receives one 16-byte run
  (half a cache line). Azahar cannot show the effect (no cache model). (The
  first version did 8 rows per pass; see "a'" below.)
- **VBlank sync.** `gfxScreenSwapBuffers(GFX_TOP, false)` takes effect at
  the next VBlank. Before writing, if that swap is still pending
  (`gspIsPresentPending`), it waits for the VBlank (`gspWaitForVBlank`):
  otherwise it would draw into the buffer still on screen and tear. At 30
  fps on a 60 Hz LCD it rarely waits. Then `GSPGPU_FlushDataCache` on the
  framebuffer and the swap, as SDL's driver did.
- SDL's path stays as the fallback for any other surface size or format
  (logged once: `[3ds] video: direct present ...`).

**Numbers** (`perf-bench.sh exp-a2`, against `baseline2`; ms per frame):

| Level | | fps | frame | busy | render | hud | blit | **present** | idle |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 0 | before | 14.95 | 66.90 | 66.88 | 12.37 | 1.77 | 1.44 | 50.40 | 0.01 |
| 0 | after | 30.32 | 32.97 | **23.10** | 12.37 | 0.90 | 1.44 | **7.65** | 9.88 |
| 6 | before | 15.68 | 63.80 | 63.80 | 10.09 | 1.69 | 1.44 | 49.73 | 0.01 |
| 6 | after | 30.36 | 32.94 | **20.95** | 10.08 | 0.89 | 1.44 | **7.74** | 11.98 |
| 20 | before | 15.69 | 63.73 | 63.72 | 9.65 | 1.70 | 1.44 | 49.80 | 0.01 |
| 20 | after | 30.36 | 32.95 | **20.70** | 9.65 | 0.89 | 1.44 | **7.73** | 12.25 |

`present` −85% (50 → 7.7 ms); the game reaches the 30 fps cap and has
10-12 ms to spare per frame. `hud` halved too: it is counted per frame, and
the HUD is redrawn per game tick (30 Hz), so at 30 fps it is spread over
twice as many frames. Nearest-neighbour variant (rejected): present 3.6 ms.

**a' (follow-up, found through experiment b).** With 8 output rows per
pass, GCC kept the 16 source-row pointers on the stack and reloaded them
for every pixel (`objdump` of `video_3ds.o`); LTO's code generation
happened to avoid that, which is where its present gain came from. Now 4
rows per pass, 8 row pointers advanced in place (they stay in registers),
and each horizontal pixel pair is one 64-bit load: about 11 instructions
per output pixel instead of ~18. Same arithmetic, so the output is
byte-identical (0 differing pixels on all three levels):

| Level | busy (a) | busy (a') | present (a) | present (a') |
| --- | --- | --- | --- | --- |
| 0 | 23.10 | **21.52** | 7.65 | **6.02** |
| 6 | 20.95 | **19.48** | 7.74 | **6.20** |
| 20 | 20.70 | **19.27** | 7.73 | **6.24** |

**Picture** (top screens at the end of the run, fixed seed, compared with
the baseline's pixel by pixel): levels 0 and 6 differ in 7.5k / 7.9k of
96k pixels, **by at most 1** per channel (the rounding above); the bars are
identical. Level 20 differs more in the view (animated water, captured at
a different moment) and by at most 1 in the HUD.


## Experiment b: compiler flags

**Builds.** The experiment a code at `-O2` (devkitPro's Release flags,
`-g -O2 -DNDEBUG`, with `-march=armv6k -mtune=mpcore -mfloat-abi=hard`)
against `-O3`, `-O2 -ffast-math`, `CMAKE_INTERPROCEDURAL_OPTIMIZATION=ON`
(GCC `-flto`, engine only: SDL, OpenAL and the codecs are prebuilt
archives) and LTO with `-fno-strict-aliasing`. Each in its own build
directory, `perf-bench.sh exp-b-<variant>`.

| Level | build | busy | vs -O2 | render | present | `.text` |
| --- | --- | --- | --- | --- | --- | --- |
| 0 | -O2 (a) | 23.10 | | 12.37 | 7.65 | 7.15 MB |
| 0 | -O3 | 23.02 | −0.3% | 12.27 | 7.70 | 7.57 MB |
| 0 | -ffast-math | 23.18 | +0.3% | 12.38 | 7.68 | 7.16 MB |
| 0 | LTO | 20.30 | −12.1% | 11.15 | 6.02 | 7.01 MB |
| 0 | LTO, no strict aliasing | 22.03 | −4.6% | **12.97** | 5.97 | |
| 6 | -O2 (a) | 20.95 | | 10.08 | 7.74 | |
| 6 | -O3 | 20.89 | −0.3% | 10.00 | 7.81 | |
| 6 | -ffast-math | 20.89 | −0.3% | 10.08 | 7.68 | |
| 6 | LTO | 18.58 | −11.3% | 9.22 | 6.21 | |
| 6 | LTO, no strict aliasing | 19.98 | −4.7% | **10.80** | 6.03 | |
| 20 | -O2 (a) | 20.70 | | 9.65 | 7.73 | |
| 20 | -O3 | 20.56 | −0.7% | 9.58 | 7.71 | |
| 20 | -ffast-math | 20.69 | −0.1% | 9.65 | 7.72 | |
| 20 | LTO | 19.97 | −3.5% | 10.53 | 6.14 | |
| 20 | LTO, no strict aliasing | 19.78 | −4.4% | **10.38** | 6.09 | |

- **`-O3`**: −0.3 to −0.7%, about the run-to-run spread, for 414 KB
  more code (+6% `.text`). Azahar charges nothing for instruction-cache
  misses; with the ARM11 MPCore's 16 KB L1 instruction cache the larger
  code can cost more than it gains. **Not kept.**
- **`-ffast-math`**: no change (the rasteriser and world are fixed-point;
  float is in sound and a few setup paths). The demo replay
  (`12-sprite-check.txt`) is pixel-identical to `-O2` wherever a capture
  lands on the same frame (3 of 10; the rest differ by capture timing
  only), monsters included. Only risk, no gain. **Not kept.**
- **LTO**: the largest number (−12% on level 0), but **it renders wrong**.
  With the fixed seed the `-O2` build matches the baseline to ±1 per
  channel at every spot, while the LTO build drops a whole wall of the
  room seen through the opening on level 0 (black) and draws stray pixels
  near the floor; level 6's building interior differs as well. Part of the
  "gain" is polygons not drawn. Level 0, `-O2` left, LTO right (zoomed):

  ![LTO drops a wall](img/perf-b-lto-wall.png)

  The brief's warning was invisible *enemies* (an upstream report). The
  main menu's demo replay (`tests/input/12-sprite-check.txt`) shows
  monsters under LTO here (top `-O2`, bottom LTO, captured at slightly
  different moments of the replay), so that exact symptom did not
  reproduce, but the missing wall is the same kind of failure:

  ![Monsters in the demo, -O2 and LTO](img/perf-b-lto-sprites.png)

- **LTO + `-fno-strict-aliasing`**: the picture is **byte-identical** to
  `-O2` at levels 0 and 6 (0 differing pixels; level 20's water is at a
  different animation phase). So the LTO breakage is strict-aliasing
  undefined behaviour in the engine that whole-program inlining exposes.
  But without strict aliasing the rasteriser is **5-8% slower**; the net
  −4.5% comes entirely from `present` (7.7 → 6.0 ms). **Not kept**: the
  present gain is available without LTO (below), and the renderer's
  aliasing bug stays a trap for any future LTO attempt.

**What LTO pointed at.** At `-O2` the present loop kept its 16 source-row
pointers on the stack (ARM has 13 general registers), reloading them for
every pixel; LTO's code generation happened to do better. Rewriting the
loop to fit in registers is a code change to experiment a (next section).

## Experiment c: half-resolution rendering

**Change: none needed.** The engine already has it: Preferences >
Graphics > Rendering Options > Resolution: **Low** (`scmode_highres="false"`)
renders the world view at half width and height (320x160 instead of
640x320) and `quadruple_surface()` doubles it into the screen surface.
Measured with `PREFS="scmode_highres=false" perf-bench.sh exp-c` on the
experiment a build:

| Level | | busy | render | blit | present | idle |
| --- | --- | --- | --- | --- | --- | --- |
| 0 | high (a) | 23.10 | 12.37 | 1.44 | 7.65 | 9.88 |
| 0 | **low** | **16.73** | **5.41** | 1.30 | 8.16 | 16.23 |
| 6 | high (a) | 20.95 | 10.08 | 1.44 | 7.74 | 11.98 |
| 6 | **low** | **16.14** | **4.79** | 1.30 | 8.22 | 16.80 |
| 20 | high (a) | 20.70 | 9.65 | 1.44 | 7.73 | 12.25 |
| 20 | **low** | **15.77** | **4.31** | 1.30 | 8.12 | 17.15 |

The rasteriser takes 44-56% of its high-resolution time (a quarter of the
pixels, plus per-column and per-polygon costs that do not shrink); busy
time falls 24-28%.

**Picture.** The top screen shows the 640x320 view at 320x160. At high
resolution each screen pixel is the average of 4 rendered pixels (the
2x2 average of experiment a): 4x supersampling. At low resolution each
screen pixel is one rendered pixel. Half the view's pixels differ, by up to
148-228 of 255: textures are grainier, the fence mesh and distant detail
alias, and edges step more (left high, right low; zoomed 2x):

![High vs low resolution](img/perf-c-full-vs-half.png)

**Decision: not the default.** It is a visible loss on the console's
screen, and with experiment a the default already reaches 30 fps in
Azahar. It stays available as the existing Resolution: Low option (no code
change), the first thing to try if the hardware turns out slower than
30 fps. In low resolution the doubling to 640x480 followed by the 2x2
average in the present is wasted work (blit + present ~9.5 ms); a present
straight from the half-size buffer would save most of it, if Low becomes
important.

## Experiment d: 30 fps cap, no interpolation

**What the engine already does.** The default Framerate Target is 30, the
world's tick rate, and at exactly 30 the engine does no interpolation
(`init_interpolated_world()`, `get_heartbeat_fraction()` return early).
The experiment was to *force* that on the 3DS: clamp `fps_target` to 30 in
`validate_graphics_preferences()` and hide the option. So it can only
change things for players who pick 60, 120 or Unlimited. Measured with
`PREFS="fps_target=60"` and `"fps_target=0"` on the experiment a build:

| Level | target | fps | busy / frame | world | present | idle |
| --- | --- | --- | --- | --- | --- | --- |
| 0 | 30 | 30.32 | 23.10 | 0.36 | 7.65 | 9.88 |
| 0 | 60 | **42.77** | 23.38 | **1.57** | 6.67 | 0.01 |
| 0 | Unlimited | 42.78 | 23.37 | 1.57 | 6.67 | 0.00 |
| 6 | 30 | 30.36 | 20.95 | 0.36 | 7.74 | 11.98 |
| 6 | 60 | **48.02** | 20.82 | **1.41** | 6.61 | 0.01 |
| 20 | 30 | 30.36 | 20.70 | 0.48 | 7.73 | 12.25 |
| 20 | 60 | **48.17** | 20.76 | **1.48** | 6.86 | 0.01 |

- Interpolation costs ~1.1 ms per frame (`world` 0.4 → 1.5: the
  interpolated object and camera positions), so forcing 30 saves that much
  per frame, plus all the idle time: at 60/Unlimited the main thread never
  sleeps (idle 0), as with upstream's Unlimited (`STAGE6.md`, loop audit).
- But in Azahar the game is now fast enough to show **43-48 interpolated
  frames per second** instead of 30. Forcing 30 would take that away from
  anyone who chose it: fewer frames on screen, i.e. a visible regression
  for them, and no change at all for the default.
- 60 and Unlimited behave the same here (neither reaches 60).

**Decision: not kept for now** (no commit of the clamp). Whether 60 is
worth offering depends on the console's real frame time, which Azahar
does not give (no cache model): if a New 3DS cannot get clearly above ~40
fps with a 60 target (hardware checklist), the 43-48 fps advantage is
gone, uneven pacing on the 60 Hz LCD and a CPU that never sleeps remain,
and the clamp should go in. It is six lines:

```diff
 	// Misc/preferences.cpp, graphics dialog
+#ifndef A1_PLATFORM_3DS
 	table->dual_add(fps_target_w->label("Framerate Target"), d);
 	table->dual_add(fps_target_w, d);
+#endif
 	// validate_graphics_preferences(), in the A1_PLATFORM_3DS block
+	clamp(preferences->fps_target, static_cast<int16>(30));
```

## Baseline (stage 6 build + profiler, `3d2a6ca`)

**Where.** Three Marathon 2 levels, at the level's start position, facing
its start direction, standing still (monsters and liquids still move).
`perf-bench.sh baseline`: 8 s settle, 20 s measured, 2 runs per level,
Azahar 2126.2 in New 3DS mode, no DSP firmware (below).

| Level | Name | The spot |
| --- | --- | --- |
| 0 | Waterloo Waterpark | indoor room, textured walls and floor |
| 6 | Ex Cathedra | outdoor canyon, open sky, lit building ahead |
| 20 | My Own Private Thermopylae | animated water (liquid surface) |

| ![L0](img/perf-L0-top.png) | ![L6](img/perf-L6-top.png) | ![L20](img/perf-L20-top.png) |
| --- | --- | --- |
| level 0 | level 6 | level 20 |

**Baseline, ms per frame** (Release `-O2`, 32-bit colour, high resolution,
30 fps target; the default preferences):

| Level | fps | frame | max | world | render | hud | blit | present | other |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 0 | 14.96 | 66.88 | 122.4 | 0.70 | 12.31 | 1.77 | 1.44 | **50.44** | 0.23 |
| 6 | 15.64 | 63.95 | 118.8 | 0.56 | 10.26 | 1.69 | 1.44 | **49.78** | 0.21 |
| 20 | 15.71 | 63.64 | 115.2 | 0.83 | 9.71 | 1.69 | 1.44 | **49.70** | 0.26 |

Re-run with a fixed seed (`bc7fc46`, `baseline2`; the reference for the
experiments): 66.90 / 63.80 / 63.73, within 0.3% of these.

Run-to-run spread of the frame time: 0.1-0.9 ms (66.81-66.95, 63.49-64.41,
63.56-63.71). `idle` is 0.01: the frame never finishes early, so the 30 fps
cap never sleeps.

**What it says.** About **15 fps, and three-quarters of every frame is
`present`**: SDL copying the 640x480 screen surface into a texture,
scaling it into its 400x240 window surface, and its N3DS driver copying
that into the framebuffer one rotated pixel at a time. The rasteriser
(`render`, 10-12 ms) is the only other large item; world simulation is
under 1 ms at these spots.

### How to read these numbers (Azahar, not hardware)

- Azahar's `svcGetSystemTick()` counts **emulated** time: the guest's
  instruction stream at a fixed cost per instruction, and the emulator's
  own speed or the host's load does not change it (a run made during a
  full rebuild and one made idle differ by 0.2%). That is what makes runs
  repeatable to under 1%.
- It does **not** model caches, memory latency or VFP timing. Copy loops
  (present, blit) are dominated by memory traffic on a console; here they
  cost what their instructions cost. Relative changes in instruction count
  are measured well; cache effects (tiling, `-O3` unrolling vs code size)
  are not measured at all.
- **The figures are therefore relative**: they rank changes, and show
  where the time goes; the console's absolute frame times will differ.
  The hardware checklist at the end measures the kept build on a New 3DS.
- **Without sound.** Benchmarks run without `dspfirm.cdc`
  (`--no-dspfirm`). With sound, level 0 measured 67.60 ms/frame against
  66.92 without (audio callback 19.7 ms per frame on the audio thread), so
  the audio thread barely touches the main thread's time in Azahar; on a
  console it runs on core 1. Without it the runs are simpler (no music
  start-up in the window).


## The profiler (`A1_PROFILE`)

A compile-time option of the 3DS build, off by default:

```
cmake -S ports/3ds -B build-3ds-prof -DA1_PROFILE=ON
cmake --build build-3ds-prof
```

`ports/3ds/source/profile_3ds.cpp` times zones of the main thread with
`svcGetSystemTick()` (the 268 MHz ARM11 clock). Times are **exclusive**: a
zone nested in another is subtracted from it. Each figure is milliseconds
per displayed game frame (one `render_screen()` call).

| Zone | Covers | Where |
| --- | --- | --- |
| world | `update_world()`: the 30 Hz game ticks (physics, monsters, projectiles), minus the HUD | `Misc/interface.cpp` `idle_game_state()` |
| render | `render_view()`: the software rasteriser into the world-view buffer | `RenderOther/screen.cpp` |
| hud | the HUD buffer update (`update_interface()`, nested in world) and its blit to the screen surface | `GameWorld/marathon2.cpp`, `screen.cpp` |
| blit | world-view buffer → screen surface (`update_screen()`) | `screen.cpp` |
| present | screen surface → display (`MainScreenUpdateRects()`) | `screen.cpp` |
| idle | the in-game frame pacing sleep (30 fps cap) | `shell.cpp` `main_event_loop()` |
| other | frame time not in any zone: event polling, crosshair, messages, ... | |
| audio | the audio callback (OpenAL mixing + music decoding), on the audio thread: on a console core 1, so not part of the frame | `audio_3ds.cpp` |
| overlay | drawing the profiler's own overlay, once a second; excluded from the frame times | |

**Overlay.** In a profile build the bottom screen shows, once a second, the
figures above for the last second in place of the controls legend:

![Profiler overlay](img/perf-overlay-bottom.png)

**Benchmark mode.** If `sdmc:/3ds/alephone/debug-perf` exists:

```
level 6       # level index; BEGIN NEW GAME starts it instead of level 0
settle 8      # seconds of play before measuring
measure 20    # seconds measured
quit 1        # quit after the result (as HOME > Close)
log 1         # also log every 1-second window
```

The profiler waits for the level's first frames, lets `settle` seconds
pass, averages `measure` seconds, logs one line and saves both screens
(`screen-perf-L<level>-*.bmp`):

```
[prof] BENCH level=0 seconds=20.8 gaps=0 frames=312 fps=14.97 frame=66.81 max=118.92 world=0.65 render=12.29 hud=1.77 blit=1.44 present=50.43 idle=0.01 overlay=0.11 other=0.23 audio=0.00
```

`gaps` counts stalls over 0.5 s inside the measured window (should be 0).

**Running it.**

| Command | Does |
| --- | --- |
| `scripts/3ds/run-azahar.sh --perf LEVEL [build-3ds-prof/alephone.3dsx]` | one benchmark in Azahar (input script `tests/input/11-perf-bench.txt` presses A to start the game; Azahar is stopped when the game quits) |
| `scripts/3ds/tests/perf-bench.sh LABEL [3dsx]` | the series: `LEVELS` (default `0 6 20`) × `RUNS` (default 2), results in `build/perf/LABEL.txt` and the end-of-run screens in `build/perf/LABEL-L<n>-*.png`. `PREFS="attr=value ..."` overrides `<graphics>` preferences for the series |
| `python3 -I scripts/3ds/tests/perf-table.py build/perf/A.txt [B.txt ...]` | Markdown tables: means per level, and each file against the first |

## Menus and dialogs

Every present goes through the new path, not only the game: the main
menu captured through it (`run-azahar.sh --screens`, normal build)
matches stage 4's capture through SDL except for the highlighted BEGIN NEW
GAME (`PATCHES.md` 23, added after that capture); the credits screen and
the in-game HUD render as before. `log.txt` says which path is in use:

```
[3ds] video: direct present (640x480 SDL_PIXELFORMAT_RGB888 -> 320x240 RGBA8 framebuffer)
```

## Hardware checklist (New 3DS)

About 25 minutes. Azahar gave the ranking; the console gives the real
frame times.

1. Build the profiler build: `cmake -S ports/3ds -B build-3ds-prof
   -DA1_PROFILE=ON && cmake --build build-3ds-prof`.
2. Upload a `debug-perf` file to `sdmc:/3ds/alephone/` (`scripts/3ds/sd-ftp.sh
   IP put debug-perf /3ds/alephone`):
   ```
   level 0
   settle 8
   measure 20
   quit 1
   ```
   (Without `quit 1` the game keeps running afterwards.)
3. `cmake --build build-3ds-prof --target run-3dslink`, press A twice (BEGIN
   NEW GAME, chapter screen). Wait for the game to quit (~2 min with the
   level load). Fetch `log.txt` (one `[prof] BENCH level=0 ...` line) and
   `screen-perf-L0-top.bmp` / `-bottom.bmp`. Repeat with `level 6` and
   `level 20`. Sound may stay on (the audio thread runs on core 1).
4. Compare with the Azahar table (experiment a'): busy 21.5 / 19.5 / 19.3,
   present 6.0-6.2, render 12.4 / 10.1 / 9.7 ms. Copy loops (present, blit)
   will cost more on the console (memory traffic); if the frame no longer
   fits in 33 ms (`idle` near 0, fps below 30), try Resolution: Low
   (Preferences > Graphics > Rendering Options) and repeat level 0.
5. **Experiment d's decision:** set Framerate Target 60 (Preferences >
   Graphics) and repeat level 0. Above ~40 fps: keep the option. Near 30
   or below: apply the six-line clamp in "Experiment d".
6. Play a minute of level 0 normally (moving, turning, firing) and watch
   the overlay: `fps` near 30, `max` (the longest frame) and any tearing on
   the top screen while turning (there should be none: the swap waits for
   VBlank).

## Next candidates (not done)

Ranked by the profile after a': `render` (9.7-12.4 ms) is now the largest
item, then `present` (6.0-6.2), `blit` (1.4) and `hud` (0.9).

- **Low resolution done properly:** in Resolution: Low the half-size view
  is doubled to 640x480 and then averaged back down (blit + present
  ~9.5 ms of wasted copying). Presenting the half-size buffer directly
  would make Low cost little more than its rasteriser time.
- **Skip the 640x480 surface for the view:** the world view could be
  rendered at 640x320 and presented straight from `world_pixels` (with the
  HUD from its own buffer), saving `blit` and one full pass over the view.
- **The rasteriser** (`RenderMain/`): the next real target, e.g. the
  texture-mapping inner loops in `scottish_textures`; measured per zone
  with this profiler before and after. LTO showed there is strict-aliasing
  UB there; fixing it would also make an LTO build safe to evaluate.

## Files

| Path | Purpose |
| --- | --- |
| `ports/3ds/source/profile_3ds.cpp`, `ports/3ds/include/a1_3ds_profile.h` (new) | the profiler, overlay rows, benchmark mode |
| `ports/3ds/source/video_3ds.cpp` (new) | the direct present |
| `ports/3ds/source/input_3ds.cpp`, `audio_3ds.cpp`, `include/a1_3ds_platform.h` | overlay drawing and quit for the profiler; audio callback time; `present_screen()` |
| `ports/3ds/CMakeLists.txt` | `A1_PROFILE` option, the two new sources |
| `Source_Files/…` (4 files) | `PATCHES.md` 34-35 |
| `scripts/3ds/run-azahar.sh` | `--perf LEVEL` |
| `scripts/3ds/tests/perf-bench.sh`, `perf-table.py` (new) | benchmark series and tables |
| `scripts/3ds/tests/input/11-perf-bench.txt`, `12-sprite-check.txt` (new) | start a benchmark game; capture the demo replay |
