# Aleph One 3DS port: performance

Recorded 2026-10-11 on branch `3ds-port`, on top of stage 6 (`d19dae0`).
Goal: measure first, then optimise; keep only changes with a measured gain
and no visual regression. Engine changes are listed in
[`PATCHES.md`](PATCHES.md) (entries 34 onwards).

## Results

Every figure is ms per frame in Azahar (see "How to read these numbers").
**busy** = frame − idle: the work per frame. Once the game reaches the 30 fps
cap the frame time stays 33 ms and the gain shows as idle time instead.

| Step | Change | busy, L0 / L6 / L20 | fps | Picture | Kept |
| --- | --- | --- | --- | --- | --- |
| baseline | stage 6 build (re-run with a fixed seed, `bc7fc46`) | 66.9 / 63.8 / 63.7 | 15.0-15.7 | | |
| a | direct framebuffer present | **23.1 / 21.0 / 20.7** (−66%) | **30.3** (capped) | identical to SDL's within ±1 per channel | **yes** |

| c | half-resolution rendering, 2x upscale (the engine's Resolution: Low) | 16.7 / 16.1 / 15.8 (−24 to −28% vs a) | 30.3 | **visibly coarser**: loses the 2x2 supersampling | no (stays an option) |

<!-- EXPERIMENTS -->

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
- **Cache-friendly order.** 8 output rows per pass: the 16 source rows are
  read left to right, and every framebuffer column receives one 32-byte run
  (a cache line). Azahar cannot show the effect (no cache model).
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

**Picture** (top screens at the end of the run, fixed seed, compared with
the baseline's pixel by pixel): levels 0 and 6 differ in 7.5k / 7.9k of
96k pixels, **by at most 1** per channel (the rounding above); the bars are
identical. Level 20 differs more in the view (animated water, captured at
a different moment) and by at most 1 in the HUD.


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
