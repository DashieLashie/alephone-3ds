# Task: next sprint of the Aleph One 3DS port: finish HUD + Map, then the open items

Repo: /Users/dashielashie/Desktop/Aleph_3ds, branch `3ds-port`. Aleph One
(Marathon engine) is ported to the Nintendo 3DS; stages 1-7 are done and documented in
`docs/3ds/`. The user tests on a New 3DS. This sprint has two parts: **A** finishes work
in progress, **B** is the backlog of open items that earlier sessions recorded.

**Status (2026-10-12, overnight session, commits `ec37c36`..`3ba6e23`).** The sprint
was split with the user into **stage 1** (everything that needs no console and no
decision from the user: done overnight) and **stage 2** (the console session and the
user's choices: the runbook at the end). Per item below: **Done**, **Stage 2**
(waiting for the console or the user), or **Deferred**.

**Status (2026-10-15, stage 2, commits `758f2f8`..).** The console session
ran on 2026-10-13 (benchmarks, the functional run, Classic, the demo
close, an SD write test). Stage 7 passed on the console (`758f2f8`). From
its findings: the zoom moved to the D-Pad (`c61151d`), and the Sounds file
is read in one read per header and sound (`30f3b72`, `PATCHES.md` 43), the
likely cause of slow sounds in loads, of part of the 12 s scenario reload
and of the start-of-level hitches. The user then left town: **the console
re-check of those changes is the "Catch-up runbook" at the end** (stage 3).

## Read first
- `docs/3ds/STAGE7.md`: the bottom screen (HUD + Map), now recorded from the Azahar
  side; "Hardware results (New 3DS)" is pending, and "Hardware checklist" is the list
  for stage 2. The first version's results are under "History".
- `docs/3ds/PERF.md`: "Hardware results", "The profiler" (how to read fps and frame
  counts), "Load times" (new: where level loads, saves and boot spend their time),
  "Next candidates".
- `docs/3ds/STAGE5.md`: the "Open items" table and "Hardware session".
- `docs/3ds/PATCHES.md`: every engine change is listed and numbered there (1-42). Engine
  edits (`Source_Files/`) are fenced with `#ifdef A1_PLATFORM_3DS`; port code is in
  `ports/3ds/`.
- Your memory notes (3dslink, sudo, Azahar).

## Rules carried over
- Measure before and after any performance change (profile build, `debug-perf`). Keep
  only changes with a measured gain and no visual regression.
- Every engine edit gets a `PATCHES.md` entry and the non-3DS check:
  `scripts/3ds/host-baseline/ppcheck.sh "<changed files>" "<TUs>"` (Docker). It now
  prints, for a DIFFER, the number of differing lines with every number normalised:
  0 there means only `__LINE__` values moved, which is acceptable.
- Test in Azahar first (`scripts/3ds/run-azahar.sh`, `scripts/3ds/tests/perf-bench.sh`),
  then on the console. Record results in the stage's doc, in the style of the existing ones.
- Anything that needs sudo: give the user the command for Terminal.app.
- Commit per finished piece, ending messages with the session's attribution lines.

## How to drive the console (proven 2026-10-12)
- Environment: `export DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM`.
  Builds: `cmake --build build-3ds` (normal) and `cmake --build build-3ds-prof`
  (`-DA1_PROFILE=ON`).
- Ask the user for the IP (it was 192.168.100.72). They switch between the **ftpd** app
  (SD card) and the **Homebrew Launcher netloader** (Y); the two can't run at once.
- Launch: `build-3ds/hw/sprint8/launch.sh IP <path>.3dsx <transcript>` in the background.
  It retries until the netloader answers. (`3dslink -s` does not exit when the connection
  fails, so the script kills failed attempts itself.) You can start it before the user
  presses Y. The transcript mirrors the whole game log, BENCH lines and the new
  `(+N ms)` time marks included. A lid close ends it with "Connection reset by peer";
  the rest of that run's log is in `sdmc:/3ds/alephone/log.txt`. Each boot moves the
  previous log to `log.prev.txt`.
- SD card: `scripts/3ds/sd-ftp.sh IP get|put|ls|rm ...`, files in `/3ds/alephone/`.
  Helpers for this sprint, in `build-3ds/hw/sprint8/` (not in git):
  `next.sh IP DONE_TAG DONE_LEVEL NEXT_LEVEL [MODE]` (fetch a run's log and captures into
  `DONE_TAG/`, upload the next `debug-perf`, or `rm` to delete it, and set
  `bottom_screen`), `setmode.sh`, `launch.sh`, and `debug-perf-L{0,6,20}` (with `log 1`).
- Benchmark: a `debug-perf` file (`level N`, `settle 8`, `measure 20`, `quit 1`, and
  `log 1` for per-window lines). The user presses A twice; the game quits on its own
  (it took about 2.5 min with the old 76-97 s level loads; expect less now). It saves `screen-perf-L<N>-{top,bottom}.bmp`.
  **busy** = frame − idle. Compare `frame` + `overlay` across modes, not `fps`
  (`PERF.md`, "The profiler").
- Monitor transcripts for `BENCH|exit|rror|abort|Connection reset`. Keep fetched files
  under `build-3ds/hw/sprint8/` (not in git). Delete `debug-perf` from the card at
  the end.

## A. Finish the HUD + Map revision (stage 7)
The user replaced stage 7's HUD and Map modes with one **HUD + Map** mode and dropped the
touch buttons (the console's buttons already do MAP / ACTION / NEXT WEAPON).
- **Done before this sprint** (`490f9ca`): the bottom screen is the HUD (320x80) over a
  320x160 map with its own zoom (a tap on the map: 2 → 1 → 3 → 2; a tap on the HUD does
  nothing; a drag looks). Bottom Screen offers HUD + Map / Classic, stored as 0 / 2.
1. **Done: Azahar.** Script 13 passes in Classic (legend on the bottom screen, map
   taps do nothing, drag looks, dialog and HOME come back) and again in HUD + Map; the
   terminal test (script 14) passes in both (400x240 font 10/12, 640x320 font 19/22,
   page down, B back). Script 14 tapped R to page, which terminals ignore; it now uses
   the terminal's keys (`ec37c36`). Open detail: A on the terminal's second page left it
   unchanged in Azahar; try it on the console. Classic measured again on the revision's
   code: busy 21.58 / 19.54 / 19.25 ms; HUD + Map 17.85 / 15.55 / 15.23 (3.7-4.0 below).
2. **Stage 2: console.** BENCH levels 0 / 6 / 20 in HUD + Map. Pass: busy at or below
   Classic + 1 ms (Classic on hardware: 18.24 / 17.84 / 17.55, so **19.24 / 18.84 /
   18.55**) and 30 fps with `gaps=0`; estimate from the first version ~13.7 / 12.8 /
   12.7. Then the functional run (STAGE7.md "Hardware checklist"): screens as
   designed, map zoom taps, HUD tap ignored, drag to look, START/B (3 round trips),
   terminal, HOME, lid close and open, quit to the menu; a short Classic check; the
   memory line at `main loop ended`.
3. **Done from the Azahar side, hardware rows pending:** `STAGE7.md` "Revision" became the
   record (modes, how it works, captures `img/stage7r-*.png`, Summary and DoD, Known
   limitations, Files, Testing; first version under "History") (`063edfd`). After stage
   2: fill "Hardware results (New 3DS)", the DoD line and `img/stage7r-hw-*.png`, and
   commit "3DS port: stage 7 revision, HUD + Map".
   - **Stage 2, the user's choices:** should the map zoom be remembered across sessions
     (today it starts at 2 at each launch)? Is a small zoom indicator or hint wanted?
   - **Done (2026-10-13/15).** Console: busy **14.02 / 13.50 / 13.54 ms** (pass,
     4.0-4.3 below Classic), 30 fps, `gaps=0`; the functional checklist passed
     (`STAGE7.md` "Hardware results"), committed as "3DS port: stage 7 revision,
     HUD + Map" (`758f2f8`). The user's choices: zoom **not** remembered, **no**
     indicator; taps and drags clashed, so the zoom is now on the D-Pad's map
     zoom keys and taps do nothing (`c61151d`, `PATCHES.md` 44; Azahar passes).
     Terminal's A on the second page: not tried (the terminal was one page).
     **Left:** the D-Pad zoom on the console (catch-up run C).

## B. Backlog: open items from earlier sessions

**Triage** (for the user; "console" = needs a console session):

| # | Item | Status | Cost left | Console? | Recommendation |
| --- | --- | --- | --- | --- | --- |
| A | Stage 7 revision on the console | **Done** (`758f2f8`): busy 14.02 / 13.50 / 13.54 ms, 4.0-4.3 below Classic; D-Pad zoom done in Azahar | S: re-check | yes | Catch-up runbook, run C |
| 1 | Level loads; slow saves | Loads **done** (console 2.9-6.2 s before 43; 0.8 s after, Run A/B 2026-10-18); **saves done: 75 s -> 4.4 s (the 2.8 MB preview write; 256x144 preview + "Saving..." box, 51-52, console-confirmed)** | none | no | Closed (optional: smaller or faster writes) |
| 2 | Hitches entering new areas | **Measured**: 72-314 ms frames in `world` in the first ~10 s; 43 is the likely fix | S: measure | yes | Catch-up runbook, runs A-B |
| 3 | Boot 41.8 s | **Measured**: scenario files 12.1 s, sound 9.4 s, dialog theme 5.2 s; 43 cuts part of the first two (Azahar) | S-M | yes | Measure 43; then the rest of the 12 s, the theme |
| 4 | Level 0's max frame | **Not the sound rate** (105 ms at 32728 Hz); same as item 2 | none | with 2 | Closed into item 2 |
| 5 | HUD/Map fewer frames than Classic | **Done**: a profiler artefact | none | no | Closed |
| 6 | HOME > Close in the demo | **Measured**: 13.4 s, 12.5 s of it the scenario reload; the APT exit hook had not run | S | yes | With item 3's reload; check the exit hook |
| 7 | C-Stick vertical look | **Done**: "feels right now" (2026-10-13) | none | no | Closed |
| 8 | Prefs `rate="44100"` | **Done**: 32728 on the card (2026-10-13); `audio` 10.2-10.8 → 7.7-7.8 ms | none | no | Closed |
| 9 | Player name from CFGU | **Done** (2026-10-17, `PATCHES.md` 50); **console confirmed 2026-10-18** ("Dashie" from the profile) | none | no | Closed |
| 10 | 400x240 menu layouts | Deferred | L | yes | Its own stage |
| 11 | Perf candidates | Deferred | M-L each | yes | HUD + Map leaves idle headroom |
| 12 | Backup bundle | **Done** (refreshed after each session) | none | no | Closed |
| 13 | devkitPro LD report | **Deferred by the user** (2026-10-12) | XS | for the repro | Kept as a note; see item 13 |
| 14 | Fatal error off the main thread | **Found** 2026-10-17 (Azahar, also without citro3d): no error screen | S-M | yes | Ask the user; see item 14 |

Sources in brackets. Statuses were verified against the docs on 2026-10-12.

**Loading, hangs and stutter (felt in play)**
1. **Level loads take 76-97 s, and the level change looks frozen and broken** until the
   next level has loaded. Saving at a pattern buffer is slow enough to look like a hang
   (not timed). [STAGE5 open item 8; PERF "Hardware results"]
   **Done in stage 1** (`570031b`, `PERF.md` "Load times"): timed marks in the load and
   save paths (`PATCHES.md` 41) showed `load_collections()` reading the shapes a few bytes
   at a time was 90% of a load. Each collection is now read with one read and parsed
   from memory (`PATCHES.md` 42): read+decode 28.4 s → 0.73 s, a level load ~31 s → 3-4 s
   in Azahar; benchmark captures pixel-identical, busy unchanged. Saves take ~0.25 s in
   Azahar; each writes 2.98 MB, mostly a 1280x720 BMP preview. **Stage 2:** time a
   level load and a save on the console (the marks are in the log); if the load still
   looks frozen, ask about a progress screen; if "save: file written" is slow, shrink
   the preview (320x180 at map scale 2).
   **Stage 2 (2026-10-13):** a level loads in **2.9-6.2 s** on the console (shapes
   0.49 s; the **sounds 1.4-4.6 s** lead); level 1 → 2 took 3.0 s and looked
   **fine** (no progress screen). A save took **69 s** in "save: file written",
   but an SD write test writes the same 2.98 MB in 0.66 s, which at the time was
   read as "a smaller preview would not fix it"; `save_game_file()` now has
   marks per step. The sounds were
   read 4 KB at a time: one read per sound now (`PATCHES.md` 43; Azahar 0.96 →
   0.50 s). **Run C on the console (2026-10-18):** the marks put the whole save in
   one step, writing the *preview* wad: **74.0 s and 66.8 s**, against 1.7-2.9 s
   for the game wad (about 215 KB). The preview was a 1280x720 BMP (2.8 MB: no
   PNG on the 3DS). So the slow part is the engine's write path (neither the
   card, 0.66 s for 2.98 MB, nor the render, 78 ms): both writes run at 40-100
   KB/s, which looks like many small flushed writes (libctru's SD driver
   flushes per write call; stdio passes small chunks) and not like card speed.
   **Fixed (2026-10-18, `PATCHES.md` 51-52; console-confirmed):** a 256x144
   preview (about 110 KB, 1/25 of the bytes) and a "Saving..." box on both
   screens before the save (the user asked for both). A save is now **4.4 s**
   (`save: begin` to `surplus saves checked`): game wad 1.6 s, preview wad
   2.6 s, was about 75 s; the box shows on both screens (user). The preview
   still writes at only ~42 KB/s, as before, so the write path (not the data
   alone) is slow: the chunking theory is not proven. **Left, optional:** bigger
   writes (a large stdio buffer or one `fwrite` per wad) would cut the 4.4 s
   further; a 128x72 preview (all the dialog shows) would save about 2 s.
2. **Short hitches when entering areas not seen before.** Never measured. [STAGE5 item 7]
   **Stage 2:** the `debug-perf-L*` files in `sprint8/` have `log 1`; play into new
   areas and read `max` per window.
   **Measured (2026-10-13):** one to four frames of 72-314 ms (level 20: 314) in
   the first ~10 s of each level, all in `world`; afterwards `max` stays at
   38-41 ms. The user still feels them. In Azahar 8 sounds (595 KB) load on first
   use in level 0's first seconds, inside `update_world()`; `PATCHES.md` 43 reads
   each with one read instead of 4 KB at a time. **Left:** catch-up runs A-B
   (per-window `max` before and after).
3. **Boot takes 41.8 s to the main menu** on the console ("preferences loaded" →
   "sound initialized" 9.6 s; "screen initialized" → `initialize_application()` done
   19.4 s). **Stage 1:** both stretches are split into marked steps (`PERF.md` "Load
   times", Azahar table; "environment (scenario files)" 4.1 s and sound 2.7 s lead
   there). **Stage 2:** read the console's split, then decide.
   **Measured (2026-10-13):** 41.8 s to the menu again; the scenario files 12.1 s,
   sound 9.4 s and the dialog theme 5.2 s are 26.7 s of it (`PERF.md` "Load
   times"). Both of the first two read every sound header 4 KB at a time
   (`PATCHES.md` 43: 2.59 → 0.51 s in Azahar); the scenario reload is now split
   into map, physics, shapes, sounds, resources. **Left:** catch-up run A's boot
   marks; then the largest remaining step.
4. **Level 0's longest frame is 107-120 ms** in every mode on the console (58 ms in the
   PERF session). The sound rate differed: the console's prefs say `rate="44100"`, PERF
   ran at 32728. **Stage 2:** try 32728 (item 8); with `log 1`, find when in the window
   it happens. [STAGE7 "Hardware results"]
   **Closed into item 2 (2026-10-13):** 105 ms at 32728 Hz, so not the rate; it is
   one of item 2's early-level `world` frames, which level 0's window catches
   because it starts 8 s in.
5. ~~HUD/Map counted about 0.5% fewer frames than Classic.~~ **Done, no code change**:
   the profiler leaves its overlay's time out of the wall clock and only Classic draws
   the overlay in game; frame + overlay is 32.98-32.99 ms in every mode, the engine's
   30.30 fps (`1000 / 30` = 33 ms timer). `PERF.md` "The profiler", `STAGE7.md`.
6. **HOME > Close during the main menu's demo takes 14.4 s** (hardware only; 0.34 s from
   a game). **Stage 1:** every step of `finish_game()` is marked. **Stage 2:** one
   HOME > Close during the demo; read the marks. [STAGE5 item 4]
   **Measured (2026-10-13):** 13.4 s, of which **12.5 s "environment reloaded"**,
   the same scenario reload as every quit to the menu (12.0 s) and boot. The
   reload is skipped when the system asked to close (`PATCHES.md` 22), but the log
   has no "the system asked the application to close" line: the APT exit hook had
   not run when the demo ended. **Left:** with item 3 (a faster reload helps both);
   check when the exit hook runs (the demo may end on the HOME press itself).

**Controls and settings**
7. **The C-Stick's vertical look is too fast.** **Done in stage 1** (`3ba6e23`,
   `PATCHES.md` 19): pitch at half the yaw's speed; full deflection from level to the
   ±30° limit in 0.23 s instead of 0.12 s (logged with script 16). **Stage 2:** the
   user tries it; the Controller Advanced vertical sensitivity and dead zone are the
   next knobs. Touch-look tuning: with HUD + Map, by feel. [STAGE5 item 2]
   **Done (2026-10-13):** "feels right now".
8. The console's prefs file still says `rate="44100"` (the default is the DSP's native
   32728 Hz). **Decided by the user (2026-10-12): change it to 32728.** Not applied
   yet: the console was not on ftpd. **Stage 2, first step:** with ftpd running,
   `build-3ds/hw/sprint8/setrate.sh IP` (sets only `<sound rate=...>`, keeps the rest,
   saves the original in `sprint8/prefs-rate/original`). Then the benchmarks run at
   32728, which also tests whether item 4's long frame goes away. [STAGE6 item 5]
   **Done (2026-10-13):** the card's prefs say 32728; the `audio` zone fell from
   10.2-10.8 to 7.7-7.8 ms; item 4's frame stayed.
9. Player name from the console profile (CFGU) instead of "Bob User". **Done in
   Azahar — non-authoritative (2026-10-17, branch `3ds-port-player-name`,
   `PATCHES.md` 50). Console, New 3DS (2026-10-18): the log line and Preferences >
   Player show the profile's name ("Dashie") on fresh prefs; the card's own prefs
   were restored afterwards.** Found on the way: HOME > Close with a dialog open
   crashed (`STAGE5.md` "HOME > Close inside a modal dialog"), fixed with an
   `atexit` `SDL_Quit()` and confirmed on the console. New
   preferences take the profile's name (config block 0x000A0000, UTF-16 ->
   UTF-8 -> the field's Mac Roman); a saved name is kept. Azahar, script
   `18-player-name-default.txt` with `--reset-prefs`: the profile's "AZAHAR" in
   the log, the Player dialog and the saved prefs. Faked with
   `debug-player-name`: empty, blank, a lone surrogate (D800) and a control
   character fall back to "Bob User" with the reason logged; "Eléonore" kept
   (Mac Roman has é); kana ("あいう", nothing Mac Roman can show) falls back;
   14 units are cut to the field's 10. [STAGE3 stub 4; STAGE5 item 6]

**Screens**
10. Menus, chapter screens and dialogs are still the 640x480 logical screen halved into
    320x240 in the middle of the 400x240 top screen. **Deferred** (its own stage).
    [STAGE5 Known limitations; STAGE5-PREREQS triage "400x240 menu layouts"]

**Performance candidates** (lower priority; HUD + Map is 3.7-4.0 ms below Classic in
Azahar) [PERF "Next candidates"]
11. Classic's `present` (8.7 ms, the largest zone on the console). Low resolution
    presented directly instead of doubled and averaged back. The view rendered without
    the 640x480 surface. The rasteriser (`scottish_textures`; LTO exposed strict-aliasing
    UB there). **Deferred.**

**Housekeeping**
12. **Done**: branch backup bundle refreshed (see `STAGE5-PREREQS.md` item 8). [STAGE5-PREREQS item 8]
13. The devkitPro linker-script report is drafted, not filed
    (`docs/3ds/devkitpro-3dsx-ld-report.md`). **Deferred by the user (2026-10-12)**;
    kept as a note. The port does not depend on it (`PATCHES.md` 12 works around the
    bug). If it is ever filed: run the reproducer (`scripts/3ds/tests/exidx-repro/`) on
    the console first (only verified in Azahar), check that `devkitarm-crtls` `master`
    has not fixed it since 2026-10-11, and the user posts it. [STAGE5-PREREQS item 9]

**Found by stage 10's P1 follow-ups**
14. **A fatal error on a thread other than main never reaches the error screen**
    (Azahar — non-authoritative, 2026-10-17, branch `stage10-c3d`, `debug-c3d`
    `present cpu` so citro3d plays no part). `test_vhalt 2` (`abort()` on a second
    thread): nothing is logged after the call, the error screen does not appear;
    Azahar logs one null read in newlib's `_write_r` (the first stdio call of
    `die_with_message`). `test_vhalt 3` (`vhalt()` on a second thread): `SDL_Quit`
    runs there (`gfxExit` logged, `FATAL:` logged), then the process faults in a loop
    in libctru `hidScanInput` (HID shut down by `SDL_Quit` while the main thread still
    scans input). Even the main-thread `vhalt()` (`test_vhalt 1`, `present cpu`) logs
    ~8200 null reads in `hidScanInput` in `abort()`'s second error screen, after
    `SDL_Quit` shut HID down (seen in P1 too: the log stops at "exit"). An assertion
    on the audio thread would end the same way: the 3DS
    returns to the Homebrew Launcher, or hangs, with no message. The citro3d side is
    handled (critic P1 minor 4, `PERF.md` "P1 follow-ups"). A fix (e.g. hand the whole
    fatal path to the main thread, or stop the main thread before `shutdown_application()`)
    is a design choice for the user.
    **Console, 2026-10-19 (New 3DS, `PERF.md` "Console G0-b, second session"): worse
    than in Azahar.** The null reads are data aborts there: Luma's "An exception
    occurred" screen, then a power-off. Main-thread `vhalt()`: the error screen shows
    and is readable, leaving it faults in `hidScanInput` (`hid.c:185`, address `0x10`).
    `vhalt()` on a second thread: the main thread faults in `hidScanInput` from
    `N3DS_PumpEvents`. `abort()` on a second thread: the main thread faults in newlib
    `_lseek_r` (address `0x18`) in a resource-file `fseek`. `error_screen()` on a second
    thread: nothing drawn (bottom framebuffer all black), then the app exits and the
    next program (probably the Homebrew Launcher) panics in `svcBreak`. So any fatal
    error ends in a forced power-off, not a return to the launcher.
    **Investigated 2026-10-19 (Azahar, a scratch build with `hidInit`/`hidExit`
    wrapped at link time to log each caller; no repo change): `SDL_Quit()` shuts HID
    down for good.** HID is reference-counted in libctru (`hidInit`/`hidExit`
    disassembly: `ldrex`/`strex` on a counter; `hidExit` unmaps the shared memory and
    nulls its pointer only at 0). Count at boot: 3 (libctru's `__appInit`,
    `N3DS_VideoInit`, `N3DS_JoystickInit`). `vhalt()` -> `shutdown_application()` ->
    `SDL_Quit()` releases 3: `SDL_JoystickQuit` **twice**, then `N3DS_VideoQuit`, so the
    count reaches 0 before the error screen polls input. Cause, SDL2 2.32.10 `SDL.c`
    `SDL_QuitSubSystem`: inside `SDL_Quit` (`SDL_bInMainQuit`) the game-controller
    branch quits `SDL_INIT_JOYSTICK` ("game controller implies joystick"), then the
    joystick branch quits it again (refcount still 1, and `SDL_bInMainQuit` forces it);
    `N3DS_JoystickQuit` calls `hidExit()` each time. The engine starts both
    (`shell.cpp`:244: `SDL_INIT_JOYSTICK|SDL_INIT_GAMECONTROLLER`). This explains dump 3
    (main-thread `vhalt`: the error screen's `hidScanInput` reads the null shared
    memory, address `0x10`, `hidScanInput+0x28` `ldr r1, [r3, #16]`) and dump 4
    (`vhalt` on a second thread: the main thread's next `hidScanInput`). A normal exit
    is unaffected (nothing polls HID after `SDL_Quit`). The `abort()`-on-a-second-thread
    case (dump 6, `_lseek_r`) is a different mechanism, not traced yet. Fix options
    (the user decides): the error screen takes its own HID reference (`hidInit()` before
    its loop, `hidExit()` after; port code only); and/or make `N3DS_JoystickQuit`
    idempotent in our SDL build (`build-sdl2.sh`; an upstream SDL report would be
    outward-facing); and, for errors on other threads, hand the whole fatal path to the
    main thread so `SDL_Quit` never runs under a live main loop.
    **Causes (2026-10-19/23):** dumps 3 and 4: SDL2 2.32.10's `SDL_Quit` releases HID
    one time more than it took (traced on `3ds-port`, `b43f9e6`); dump 6: newlib's
    signal handlers are per thread, so `abort()` off the main thread skips `on_abort`
    and runs libctru's whole exit (`__appExit`: sdmc unmounted, heap freed) on that
    thread while the main thread runs on (`CITRO3D_DESIGN.md` 5.6.3). **User decision
    (2026-10-19), all three:** (a) the error screen takes its own HID reference; (b) the
    SDL patch, **done** (`5cd45ce`, `scripts/3ds/patches/SDL2-release-2.32.10/`); (c)
    errors on other threads hand the fatal path to the main thread
    (`CITRO3D_DESIGN.md` 5.6.3, with `--wrap=abort` and a fenced `vhalt()` edit,
    `PATCHES.md` 54).
    **Decided 2026-10-23 and in progress** (design `CITRO3D_DESIGN.md` 5.6.0, on
    `stage10-c3d`): piece 0 (the SDL2 patch, `N3DS_JoystickQuit` releases HID once, `5cd45ce`)
    is done; **piece 1 done on `3ds-port` (console check pending):** `error_screen()` draws
    through the console device, not stdout (`input_3ds.cpp`: `console_init_keep_log`,
    `console_write_on`; no `devoptab_list` swaps in `error_screen`; stdout stays the log), holds
    its own HID reference (`hidInit()` before its loop, `hidExit()` after) and logs a d6 line
    (`error screen d6: thread main|other (id N); stdout device after consoleInit: console|log|other`,
    expected `log`). Azahar (non-authoritative): the boot-time error screen (scenario folder
    missing) still draws, d6 `log`; a post-boot error screen, also after `SDL_Quit` as on the
    `vhalt()` path, draws its text on the bottom screen, A leaves it and the process exits with
    0 null reads. Console check: the boot-time control (rename `Marathon2`, as `STAGE4.md`'s
    hardware checklist item 5), and by eye the port's error screen after boot once piece 2 can
    trigger one (`vhalt()` on the main thread); pieces 2 and 3 are still to do.
    Critic follow-ups (piece 1): `s_error_shown` is set once the text is drawn; while the
    error screen is up the port's own bottom-screen console output is off (a HOME round trip
    no longer redraws the legend over it); without a HID reference the screen stays 10 s and
    polls no input. **For the piece-4 merge:** in the c3d build `error_screen_here()` calls
    `a1_c3d_release_top()` first (comment at the spot); if it returns false (citro3d still
    up) it must log only: no `console_init_keep_log`, no wait loop, return.
    **Piece 2 done on `3ds-port` (console check pending), `PATCHES.md` 54:** a fatal error
    on any thread ends on the main thread's error screen (`CITRO3D_DESIGN.md` 5.6.3):
    `fatal_from_other_thread()` (static buffers, compare-exchange, the loser blocks and never
    draws, 5 s acknowledgement wait, then an untimed kernel wait; the fallback draws on the
    requesting thread through `error_screen_here(raw_log)` and ends with `svcExitProcess`),
    serving at `present_screen()`, `present_bottom()` and `__wrap_hidScanInput` on main
    (not inside the APT hook or SDL's event filter), `fatal_exit()` picking its variant from
    `s_audio_parked` (a parked SDL audio thread: no `SDL_Quit`, no OpenAL, `gfxExit()` then
    `_Exit(1)`; `on_suspend`/`on_resume` skip their SDL audio calls), `--wrap=abort`, the
    `vhalt()` hook. **Deviations from the design text:** (1) the winner copies title and body
    into the static buffers after the compare-exchange and publishes `s_fatal_ready`
    (copying first let a loser overwrite the winner's text); (2) the requester takes an
    unacknowledged request back with an `exchange` on `s_fatal_acked` before its fallback
    (so a late main cannot also serve it); (3) `log_raw()` tries the tee lock for up to
    200 ms before writing without it (two unlocked writes on the SD file raced on its
    offset and lost the "handed to the main thread" line in Azahar); (4) mode 7's lock is
    taken by main at every service point and present tick, not only at presents (main served
    the request through `__wrap_hidScanInput` otherwise); (5) `error_screen_here` has no
    citro3d branch on `3ds-port` (merge note above); (6) the fallback builds `std::string`s
    and uses the console (heap), unlike the request's static buffers: there main is blocked on
    a lock the requester holds or on one of the engine's, never inside newlib's malloc (malloc
    calls out to nothing), so the heap is usable; the remaining risk is a corrupted heap
    faulting the fallback, accepted for a last resort. Test: sdmc `debug-fatal`
    (`mode N`, `delay S`, `lock 1`, `first 0|1`), `run-azahar.sh --fatal FILE`, configs
    `scripts/3ds/tests/fatal/`, scripts `scripts/3ds/tests/input/23-fatal-in-game.txt`,
    `24-fatal-error-a.txt`, `25-fatal-home-a.txt` (script 17 and its two As exist on
    `stage10-c3d` only). **Azahar, non-authoritative, 2026-10-23** (every run: error text on
    the bottom screen, d6 `log`, `azahar_log.txt` 0 null reads): mode 1 `vhalt()` on main:
    one screen, one A, `exit`; mode 2
    `abort()` on a second thread: probes `on_abort` (main) / `SIG_DFL` (thread), handed over,
    shown by main, `exit`; modes 3, 4 likewise; mode 5 in a HUD + Map game: two captures 5 s
    apart with identical bottoms; mode 6 (audio callback): parked, HOME round trip with no
    SDL audio call, `fatal exit, audio thread parked`, `exit`; mode 6b (main in
    `SDL_LockAudio`): no acknowledgement, fallback after 5 s on the audio thread, raw log
    lines, `svcExitProcess`; mode 7: fallback on the second thread, no second hand-off line;
    mode 8 in both orders: one screen drawn by main, the loser logs one line and blocks, audio
    variant both times; modes 9, 10, 11 as described next. Mode 1 (review round 1, MAJOR 1): `vhalt()`'s own `system_alert_user()` only logs on the
    3DS (the screen is in `alert_user()`, which `vhalt()` does not call), so `vhalt_hook()` on main
    keeps the message (`s_vhalt_pending`) and `__wrap_abort` shows it ("Aleph One stopped" +
    the vhalt text, exactly one `[3ds] ERROR:` line, no "abort() was called" screen) before
    `fatal_exit()`; a bare `abort()`/`halt()` still goes to `on_abort`. Mode 9 (`error_screen()`
    on main, then `abort()`) covers the other `__wrap_abort` branch (a screen already shown: no
    second one, `fatal_exit()`). Mode 10 (main asleep 7 s at the failure): the requester takes the
    request back after 5 s and draws the fallback, main wakes, logs `the request was taken back
    ... blocking` at its next service point and blocks there (it must not draw over the fallback).
    Mode 11 (the audio callback fails while main shows its own `error_screen()` and then
    `exit(1)`s like `alert_user()`'s callers): `error_screen()` on main ends in `fatal_exit()` when
    the audio thread is parked (the `exit(1)` would run `SoundManager::Shutdown` -> `SDL_CloseAudio`
    and block on the parked thread's mixer lock); all three pass in Azahar. **`run-azahar.sh
    --fatal` stops Azahar 2 s after the log's `exit`/`svcExitProcess` line; both lines are
    printed before steps that could still hang (`_Exit` -> `__appExit`, `svcExitProcess`), and
    `azahar_log.txt` has no end-of-emulation marker, so "the process ended" is a console-only
    check (the "0 null reads" count only says no guest memory fault was logged).** `--perf`'s stop
    condition is unchanged (`exit` only). **Frame-path cost** (Azahar, non-authoritative,
    profile builds, Framerate Target 60, HUD + Map, L0, 2 runs each, build of `612173a4d`
    against this one): `present` 1.46 / 1.47 ms before, 1.45 / 1.46 after; busy (`frame - idle`)
    18.45 before, 18.43 after (frame 19.11 / 19.11 against 19.08 / 19.07): no change. **Raw log lines are not mirrored to 3dslink** (the
    "handed to the main thread" line, a blocked loser's line, the whole fallback, "main thread:
    the request was taken back"): on the console read them in `log.txt` on the card, not in the
    host transcript. Azahar cannot show: whether HOME, the launcher and the DSP survive
    `svcExitProcess` without `aptExit` (5.6.4 item 5), real scheduling and lock contention,
    by-eye readability, the launcher after each run. **Console runs (all pending):** the
    boot-time control and `debug-fatal` modes 1, 2, 3, 4 (the three second-thread runs of G0-b,
    plus main), 6 and 6b: by eye the port's screen, not Luma's; A ends it; the Homebrew
    Launcher starts again and launches Aleph One (dump 5); mode 7 only if the user wants the
    `aptExit`-skipping exit tried. Remove `debug-fatal` from the card afterwards.
    Console list in order: (a) the boot-time control (rename `Marathon2`); (b) mode 1: by eye
    the vhalt text ("Aleph One stopped" + "debug-fatal mode 1: vhalt() on the main thread"), not
    Luma's exception screen; (c) modes 2, 3, 4 (the three second-thread runs of G0-b): the
    port's screen, A, then the launcher returns and Aleph One launches again; (d) modes 6 and
    6b: the same, and **the DSP falls silent** (the audio variant and the fallback end in `_Exit`
    / `svcExitProcess`, which do not run `ndspExit`, and SDL's audio close is skipped: the DSP
    may keep reading wave buffers from freed linear memory; also listen after relaunching);
    (e) mode 7 only if the user wants the fallback's `svcExitProcess` tried, with the same silence
    check. After each run read the raw lines in `log.txt` on the card. Every "process ended"
    claim is a console check (see above).
    **Critic round 2 (last) on `05c6d27`: APPROVE**, all round-1 findings closed. Its minors,
    applied in the next commit: (1) `__wrap_abort` clears `s_vhalt_pending` (`exchange(false)`),
    so an `abort()` inside the vhalt screen goes on to `__real_abort` (bounded) instead of
    recursing to a stack overflow; (2) a request's text is no longer lost when it is not shown:
    a blocked loser logs its title and body, and main, when it acknowledges a request during its
    own fatal path (mode 11), logs "in its own fatal path; the request's text, not shown:" and
    the text; (3) the error screen's no-HID branch (10 s, no `hidScanInput`) calls
    `fatal_service_point()`, so another thread's request is acknowledged there and the fallback
    does not draw over main's screen. **Deferred:** (4) HOME or sleep during the fallback runs
    `apt_hook` -> `on_suspend` on the requesting thread (`printf` through the tee lock,
    `SDL_PushEvent`): if main failed to acknowledge because it is stuck in `tee_write` (item 15),
    the fallback freezes at HOME; fix: skip logging and SDL calls in `apt_hook` off main, or
    item 15's fix removes the cause (console: mode 7 + HOME). (5) Piece-4 merge note: with
    `A1_RENDER_C3D`, a request taken back leaves main blocked forever, possibly between
    `C3D_FrameBegin` and `C3D_FrameEnd`, and the fallback only logs (5.6.2 a): a frozen frame
    until `svcExitProcess` is the design; the fallback must never call citro3d off main.
    Azahar (non-authoritative, 2026-10-24, build with (1)-(3), script 24): modes 1, 2, 6, 7,
    8a, 8t, 9, 10, 11 as before (one screen each, `exit` or `svcExitProcess`); mode 1's bottom
    capture shows the vhalt text; 8a/8t log the loser's text after its "blocked" line; mode 11
    logs the audio thread's text after "in its own fatal path"; 0 null reads in the last run's
    `azahar_log.txt` (it is overwritten each run).
    **Console (New 3DS), 2026-10-24 to 2026-10-26 (A6 sessions 1-3; `build-3ds-c3d` unless noted; 192.168.100.72; every run started
    from the Homebrew Launcher; raw logs in `build-3ds/hw/a6/`, outside git).** Session 1, B0:
    boot-time control passes. `Marathon2` renamed to `Marathon2-x`, no `debug-c3d`/`debug-fatal`:
    the port's own screen on the console, text as the code builds it ("Aleph One cannot start /
    Scenario folder not found: sdmc:/3ds/alephone/Marathon2 (chosen by: default) / Copy a
    scenario's files ... / Log: sdmc:/3ds/alephone/log.txt"), not Luma's exception screen; one A
    ends it, the Homebrew Launcher returns and Aleph One launches again. The log has `ERROR: Aleph
    One cannot start`, `error screen d6: thread main (id 964); stdout device after consoleInit:
    log`, `gfxExit call #1 (citro3d down)` (the error came before citro3d started, nothing to
    release), `real gfxExit returned`, `exit`. No new Luma dump (still dumps 0-6).
    **B1-B5 (session 2, 2026-10-25): all pass.** Mode 1: the bottom screen reads "Aleph One stopped /
    debug-fatal mode 1: vhalt() on the main thread", not Luma's; log: `gfxExit call #1 (citro3d up:
    release first)`, release (queue retired after 0 ms, `C3D_Fini` returned), `FATAL:`, `ERROR:
    Aleph One stopped`, `error screen d6: thread main`, `gfxExit call #2 (citro3d down)`, `exit`.
    Modes 2 (thread 1781), 3 (1903) and 4 (2025): handed to the main thread, the same release, one
    screen on main, `exit`; mode 4 closes the critic's open P1 minor 4 on hardware (a second thread
    never touches citro3d). Mode 5, in a HUD + Map game (681 presents): thread 2147 handed to main,
    release 0 ms, one screen, the text stayed, launcher back.
    **B6, mode 6 (session 3, 2026-10-26): pass.** vhalt() in the SDL audio callback on thread 284:
    `fatal error on thread 284: handed to the main thread`, release (queue retired after 0 ms, 96 ms
    since the last frame end), one screen drawn by main, `fatal exit, audio thread parked: gfxExit,
    no SDL_Quit`, `real gfxExit returned`, `exit`. The user: the port's screen, one A, launcher
    back; **silent** after the error and in the launcher; the sound was normal again after the game
    closed. No new dump.
    **B6b: FAIL on the console.** Mode 6 + lock 1 (main takes SDL's audio lock in a loop): thread 359
    `handed to the main thread`, then `not acknowledged in 5 s; shown on this thread`, `error screen:
    citro3d still up (fallback off main): logged only`, `fallback error screen left;
    svcExitProcess`. The screen: no port error screen; the console went to the HOME menu with the
    system's own fatal-error message (a software error forcing the software to close; press A and
    the console restarted). The DSP check could not be made (the reboot). No new Luma dump. Azahar
    ends this run cleanly; the console does not, so 5.6.3's row 6b ("accepted, as long as it ends
    cleanly") is not met on hardware.
    **B7, mode 10: FAIL on the console, the same way.** Thread 292 `error_screen()`, main asleep 7 s:
    `not acknowledged in 5 s; shown on this thread`, citro3d up so nothing drawn (`logged only`),
    `svcExitProcess` at once; the user saw "An error has occurred forcing the software to close. The
    system will now restart." and the console restarted. The expected `the request was taken back
    ... blocking` line never appears: with citro3d up the fallback exits without waiting, so main
    never wakes (by design, 5.6.2 a). No new dump. **Mode 11: pass.** `error_screen()` on main first
    (release 0 ms), the audio thread's vhalt text logged as `main thread: in its own fatal path; the
    request's text, not shown`, `fatal exit, audio thread parked`, `exit`; launcher back, silent.
    **Spot check, shipping build (`build-3ds/alephone.3dsx`, software): B1 and B2 pass.** No `[c3d]`
    lines; mode 1: `error screen d6: thread main`, `exit`; mode 2: thread 533 handed to main, one
    screen, `exit`; launcher back both times.
    **Mode 7 on the shipping build (session 3, 2026-10-26, same day):
    FAIL.** `build-3ds/alephone.3dsx` (software, no citro3d): thread 757 `handed to the main
    thread`, `not acknowledged in 5 s; shown on this thread`, `error screen d6: thread other`, the
    fallback screen drawn, A, `fallback error screen left; svcExitProcess`; the console showed the
    system's "An error has occurred forcing the software to close. The system will now restart."
    and restarted. No new dump. B8 on the GPU build was not run (6b and mode 10 took its path).
    Part D: see item 16. **Finding and next step (no code was changed
    in these sessions):** with citro3d up, the fallback (main not serving for 5 s) ends in
    `svcExitProcess` without `aptExit`/`gspExit`, and on a New 3DS that makes the system restart the
    console instead of returning to the launcher. The fallback is only reached when main stalls
    for 5 s (the test modes do it on purpose), but the exit must still be clean. Mode 7 on the
    shipping build restarts the console too, so the cause is not citro3d or the GPU right: it is
    the exit itself (`svcExitProcess` with no `aptExit`; UNVERIFIED which skipped call exactly) and
    it affects the shipping `3ds-port` build as well, wherever the fallback is reached. Next: a fix
    through `c3d-implementer` and `c3d-critic` (medium), Azahar, and a console re-check of mode 7,
    6b and 10. Candidates to weigh: end the fallback through the normal exit (`aptExit` first, if it
    is safe off the main thread), or stop using the fallback exit when it can be avoided.
    The card was cleaned up (no `debug-fatal`).
    **Fix built 2026-10-26 (overnight, no console): piece A7 on `stage10-c3d`, UNVERIFIED on the
    console** (design `CITRO3D_DESIGN.md` 5.6.3 step 3a; `stage10-c3d` NEXT-SPRINT plan A item 7;
    commits `4981782`, `db10ade`, `5da39e0`, `5bd0867`; critic round 1 REVISE (docs), round 2
    APPROVE, medium effort). libctru 2.7.0's normal end sends `APT_PrepareToCloseApplication`
    (in `aptExit()`) and then `APT_CloseApplication` (`aptExitProcess`, called through
    `__system_retAddr` at the end of `__libctru_exit`, instead of `svcExitProcess`); the fallback
    sent neither, the best-supported cause of the restarts (UNVERIFIED: the system's side is not
    in libctru). The fallback now ends with `gfxExit()` (software build only; the GPU build must
    not touch the GPU state off main), `aptExit()` and `APT_CloseApplication`, without the heap
    frees (all thread stacks are in the heap: dump 6's class), behind a 5 s watchdog that ends in
    `svcExitProcess` (the old behaviour) if a step hangs; `apt_hook` on the fallback thread logs
    one raw line and returns (piece 2 review, deferred item 4); d7 logs the launcher's run flags
    at boot. Rejected: the full libctru exit on the requesting thread (frees the heap under live
    threads), direct `APT_*` calls, parking every thread, and making main serve more often (still
    needs a clean fallback). Port code only (no `PATCHES.md` entry), in files shared with
    `3ds-port`: **`3ds-port` still has the old fallback** until the user decides how to carry A7
    over. **Azahar — non-authoritative, 2026-10-26** (`build-3ds` and `build-3ds-c3d` of
    `stage10-c3d` at `5bd0867`; four builds 0 warnings; `arm-none-eabi-nm` OFF builds 0 citro3d
    symbols, ON 132): d7 `env: homebrew 0, run flags 0x0 (aptworkaround 0, aptreinit 0,
    aptchainload 0), return address null` (so `aptExit` takes its close branch); modes 6b, 7, 10 in
    both builds: `not acknowledged in 5 s; shown on this thread`, the screen (software; with the
    GPU present `citro3d still up (fallback off main): logged only`), `fallback exit: starting the
    5 s watchdog`, `fallback exit: gfxExit` (software only), `fallback exit: should close 0, run
    flags 0x0 (...), return address null; aptExit`, `fallback exit: APT_CloseApplication via
    __system_retAddr`; software mode 10 also logs `main thread: the request was taken back by its
    thread (fallback screen); blocking` (main woke during the screen); no `watchdog fired` (a
    temporary build with a 60 s sleep before `aptExit` showed `watchdog fired (stuck in aptExit);
    svcExitProcess`). Modes 1-5, 6, 8a, 8t, 9, 11 in both builds unchanged (one `[3ds] ERROR`, one
    screen, `exit`); scripts `22-c3d-home-close.txt` (the dialog frame, `exit without
    shutdown_application`) and `-fast` (no dialog frame, `finish_game` 23 ms after the hook) in both
    builds as before; 0 null reads in every run's `azahar_log.txt`. Azahar ended every run before;
    it proves the order of the steps, not the console's reaction. **Console:** the "Fallback exit
    runbook" at the end of `stage10-c3d`'s NEXT-SPRINT.md (modes 7 software, 6b and 10 GPU, with
    A7's builds from the `stage10-c3d` worktree).
15. **The 3dslink log mirror can freeze the game.** `tee_write()`
    (`ports/3ds/source/a1_3ds_platform.cpp`, port code on `3ds-port`) writes a line to
    the SD log, then `send()`s it on a blocking socket while holding the log lock; its
    comment says "stop mirroring rather than block", but a stalled Wi-Fi link blocks
    the `send()`, and every thread that logs then waits on the lock. Seen once on the
    console (2026-10-19, `build-3ds/hw/g0b/cl30-gpu-L0.stall1.txt`): the mirror fell
    behind at +50 s and the game loop froze for ~100 s after a BENCH line. Only with
    3dslink (a launcher start has no socket). A send timeout (`SO_SNDTIMEO`) or a
    non-blocking socket would let it drop the mirror instead; not tried.
    **Fixed on `3ds-port` 2026-10-19 (console check pending):** `tee_write()` sends
    with `MSG_DONTWAIT` (`sys/socket.h`:22; libctru has no `SO_SNDTIMEO`) and, while the
    send buffer is full, waits for room with `poll(POLLOUT)` (`poll.h`) in 100 ms steps,
    500 ms per write at most; past that it closes the socket and writes `[3ds] 3dslink
    mirror stopped (stalled); the SD log goes on` to the SD log. Console check: during a
    3dslink run, `kill -STOP` the host's 3dslink process for ~10 s: the game must keep
    running, the SD log must show that line, and the transcript stops there.
    **Console (New 3DS), 2026-10-26 (A6 session 3, `build-3ds-c3d` over 3dslink, HUD + Map @60):
    inconclusive.** The host 3dslink was stopped with `kill -STOP` in a game, first for 10 s, then
    for 150 s. The game kept running (60 presents per second throughout, 1220 to about 11900
    presents), but the stall path never ran: no `3dslink mirror stopped (stalled)` line in the card
    log, and the transcript caught up after each `kill -CONT`. Why: the code gives up only when
    `send()` returns EAGAIN for 500 ms (`kLinkStallMs`), i.e. when the console's socket send buffer
    is full; the in-game log is one ~170-byte stats line per second, and the host kernel keeps
    accepting bytes while its 3dslink process is stopped, so 150 s (~26 KB) does not fill both
    buffers. The "~10 s" step of the check cannot trigger it at this log rate. **Next step:** repeat
    with a chattier log (a debug flag that logs per frame, such as `debug-input`) or a much longer
    freeze. Also: a 3dslink launch **overwrites `sdmc:/3ds/alephone.3dsx` on the card** with the
    file it sends (the shipping build was replaced by the GPU build); put the wanted `.3dsx` back
    afterwards.
    **Revised check (2026-10-26, overnight, no console; the steps are part C of the "Fallback exit
    runbook" at the end of `stage10-c3d`'s NEXT-SPRINT.md).** Host-side reproduction,
    `scripts/3ds/tests/link-stall/` on `stage10-c3d`
    (`run.sh` builds `stall-repro.c`: `tee_write()`'s send loop copied, over a loopback TCP
    connection whose reader never reads, like a stopped 3dslink; macOS, not the console): the
    stall path runs 0.5 s after the buffers fill, as designed, but the buffers take **388-550 KB**
    first (send 65-147 KB plus receive 327-408 KB, whatever sizes were asked for: the kernel
    raises small requests). At the in-game ~170 B/s that is 38-54 min, which is why the 10 s and
    150 s freezes showed nothing. `debug-input` with both sticks moving logs about **16 KB/s**
    (Azahar — non-authoritative, 2026-10-26, a scripted stick wiggle: 4796 `input` lines, 241 KB
    in 15.1 s), so 25-35 s of freeze would be enough at the host's loopback sizes. New steps: C1
    `debug-input` + `kill -STOP` + both sticks moving for 60 s (up to 4 min); C2, if C1 shows
    nothing, a `pfctl` drop rule for the console's address on the Mac (sudo, Terminal.app): no
    ACKs at all, so only the console's send buffer has to fill. The Wi-Fi path's and the 3DS soc
    service's buffer sizes are UNVERIFIED. No code change.

16. **HOME > Close in a game took the slow path: `APTHOOK_ONEXIT` fires too late**
    (design `CITRO3D_DESIGN.md` 5.6.1 c, piece 3, branch `stage10-c3d`). `exit_requested()` read
    only the flag set by `APTHOOK_ONEXIT`, which libctru calls from `aptExit()` inside
    `__appExit`, after the engine has handled the close; so `PATCHES.md` 16/22's fast path
    (`shell.cpp`:1522, `interface.cpp`:2417, :2990, :3056) never ran on hardware and a game opened
    the invisible quit dialog (a draw after the close, `exit(0)` on its next `SDL_QUIT`).
    **Done on `3ds-port` (console check pending):** `exit_requested()` also returns
    `aptShouldClose()` (set by `APTCMD_WAKEUP_CANCEL` inside `aptMainLoop()`, before SDL's first
    `SDL_QUIT`). New in `input_3ds.cpp`: `a1_3ds::app_suspended()` (`s_in_home || s_asleep`,
    declared in `a1_3ds_platform.h`; nothing calls it on this branch, the stage 10 present gate
    will); `a1_3ds::main_thread_id()`; diagnostic **d1**: every APT hook logs
    `[3ds] apt hook <ONSUSPEND|ONRESTORE|ONSLEEP|ONWAKEUP|ONEXIT> on thread <id> (main <id>): gpu
    right <0|1>, should close <0|1>`; script command `home-close [should-close]`; scripts
    `22-c3d-home-close.txt` (before-the-fix path) and `22-c3d-home-close-fast.txt` (after). No
    engine edit, so no `PATCHES.md` entry. Nothing here touches the renderer (the gate and
    bounded waits are piece 5, stage 10).
    **Deviations from the design text:** (1) `home-close should-close` is an extra option: a script
    cannot flip `aptShouldClose()` in Azahar, so the option sets the flag `exit_requested()` reads
    and models the console after the fix; plain `home-close` is exactly the design's command (the
    pre-fix path, including the quit dialog). (2) The scripts are two files, not one. (3) d1's
    `gpu right` is `gspHasGpuRight()`; ONEXIT is also logged, but on hardware that line is lost
    (log already closed).
    **Azahar — non-authoritative** (`build-3ds`, Marathon 2): `22-c3d-home-close.txt`: d1 line
    `ONSUSPEND on thread 1 (main 1): gpu right 1, should close 0`, then a `terminals: 640x320`
    frame (the quit dialog) and `exit without shutdown_application (a dialog's exit)`, 0.5 s after
    the command: the old behaviour, as the design says it should be for the plain command.
    `22-c3d-home-close-fast.txt`: d1 line, then `finish_game: begin` 0.3 ms later, no dialog frame,
    `exit` 0.2 s later. Unchanged: `06-gameplay.txt` (HOME 3 s: ONSUSPEND/ONRESTORE both
    on the main thread, then `quit`: `finish_game` at once, `exit`), `08-demo-close.txt`, the HOME
    and sleep parts of `07-suspend-menus.txt` (each hook logged, resume lines as before), and the
    fatal regression `--fatal mode-2.cfg` + script 24 (one `ERROR: Aleph One stopped`, `exit`).
    Builds: `build-3ds` and `build-3ds-prof`, 0 warnings; `arm-none-eabi-nm build-3ds/alephone.elf | grep -ciE 'citro|C3D_|a1_c3d'`
    = 0 (a bare `grep -ci c3d` also matches addresses such as `006c3d54`; macOS's `nm`
    cannot read ARM ELF and prints nothing, so it "passes" vacuously: the checks recorded
    on 2026-10-24 before the merge used it and were re-done with `arm-none-eabi-nm`: 0).
    **Console checks pending:** HOME > Close in a game: the log (SD card) shows the d1 line
    `ONSUSPEND on thread <main> (main <same>) gpu right 1 should close 0` and no hook after it, then
    `finish_game` without a `terminals: 640x320` dialog frame; the launcher returns and Aleph One
    starts again; HOME > Close at the main menu and in a dialog unchanged; HOME and back, and sleep,
    in a game: both hooks on the main thread with gpu right 1. (Item 14's HOME/sleep gate runs
    belong to piece 5.) Added by the critic: the software keyboard (confirm and cancel): hooks
    paired, `should close` stays 0, the game goes on; Power button > Power Off in a game: the
    fast path (bit 0x200), no quit dialog.
    **Critic: APPROVE** (round 1, 2026-10-24, medium effort). It checked libctru's binary:
    `aptShouldClose()` tests bits 0x600 of a flag word that only `WAKEUP_CANCEL` (0x400, in
    `aptWaitForWakeUp`) and the order-to-close/shutdown event (0x200, `aptEventHandler`, on
    libctru's APT thread) set, and nothing clears; sleep and a normal HOME round trip set
    neither. So `exit_requested()` can turn true between two polls (power-off during a normal
    `finish_game` skips its fades: harmless). The d1 line is inside `FatalGuard` (no fatal
    service point in the hook); ONEXIT now logs twice, both lost on hardware (log closed).
    **Console (New 3DS), 2026-10-24 (A6 session; `build-3ds-c3d`, launcher starts, logs read from the
    card):** the fast path works. In every HOME > Close (in a game: `p1-gpu.cfg` HUD + Map @60 ×2
    and Classic @30 ×1, plus the diagnostic config's Classic @30 run; once in a dialog; once at
    the main menu) the log has one d1 line `ONSUSPEND on thread <main> (main <same>): gpu right 1,
    should close 0` and no hook after it, then (in a game) `finish_game: begin` without a
    `terminals: 640x320` dialog frame, `main loop ended`, `exit`; the launcher returns and Aleph
    One launches again every time. Hook to `exit`: 3.0-4.8 s in a game with `p1-gpu.cfg` (about
    2.6-3.1 s of it before `finish_game` begins), 9.4 s with the diagnostic config (5 s of it the
    abandoned queue wait, see `CITRO3D_DESIGN.md` 5.6.4), 2.8 s in a dialog, 3.5 s at the main
    menu; 1.0 s for the POWER route below. HOME and back ×2 (3.5 s and 3.6 s) and lid sleep and wake ×2 (1.1 s and 0.9 s):
    `ONSUSPEND`/`ONRESTORE` and `ONSLEEP`/`ONWAKEUP`, each pair on the main thread with gpu right
    1. The software keyboard (player name, main menu): confirmed once and cancelled once, hooks
    paired (`ONSUSPEND`, `ONRESTORE`) around `keyboard: confirmed (button 2, result 2)` and
    `keyboard: cancelled (button 0, result 1)`, the game goes on. (Saves in game do not open a
    keyboard: they save at once from a pattern buffer, so the name edit stands in for them.)
    HOME > Close at the main menu: unchanged. No new Luma dump.
    **POWER button, short press:** on this console it closes the running software even when the
    power menu is then cancelled: the log has `ONSUSPEND` (gpu right 1, should close 0), no
    `ONRESTORE`, and the exit about 1 s later (gate closed, `queue retired after 0 ms`, `release
    done`, `exit`); the user lands on the HOME menu. So the runbook's "POWER then cancel, presents
    resume" expectation is wrong for this console: the cancel path is the same close as a Close,
    and it is clean. **Part D (hold POWER > Power Off) was not run** (2026-10-26: the user chose
    not to power the console off), so the "no quit dialog
    after the d1 line" check for that route is still open (the POWER route above was taken at
    the main menu, so it also shows no dialog).

**Accepted gaps** (no action unless the user asks): hyperlinks do nothing
(`PATCHES.md` 2); no Opus/FLAC; Lua `io`/`os` gaps; SDL2_image not built (so save
previews are BMPs); Old 3DS untested (no hardware); one controller binding per action;
no keyboard for the Lua console. [STAGE3 "Stubs", STAGE6, ENVIRONMENT, STAGE5]

## Stage 2 runbook (with the user and the console)
1. **Questions first** (no console needed): the two map-zoom choices (A.3). Items 8
   and 13 are decided (rate to 32728; the report deferred). Items 9-11 stay deferred
   unless the user says otherwise.
2. **Build and set up:** `cmake --build build-3ds && cmake --build build-3ds-prof`; the
   user starts ftpd; ask for the IP; `build-3ds/hw/sprint8/setrate.sh IP` (item 8).
3. **Benchmarks, profile build** (A.2, B1, B2, B4): `build-3ds/hw/sprint8/next.sh IP - - 0 0`
   (debug-perf level 0, HUD + Map), user switches to the netloader, run
   `build-3ds/hw/sprint8/launch.sh IP build-3ds-prof/alephone.3dsx build-3ds/hw/sprint8/hm-L0-3dslink.txt`
   in the background and monitor it; the user presses A twice. The transcript has the
   BENCH line and the level load's `(+N ms)` marks. Then `next.sh IP hm-L0 0 6`, ... ,
   `next.sh IP hm-L20 20 rm`. These run at 32728 Hz (item 8): note it, and compare
   level 0's `max` and the `audio` zone with the stage 7 runs (44100 Hz).
4. **Functional run, normal build** (A.2): STAGE7.md "Hardware checklist" step 3, plus:
   a save at level 1's pattern buffer (marks: "save: ..."), the level change at its exit
   (marks: "level change: ..."; does it still look frozen?), the C-Stick look (item 7),
   and the terminal's A on the second page. Then step 4 (Classic, briefly).
5. **HOME > Close during the menu's demo** (item 6): boot, wait for the demo, HOME >
   Close; fetch `log.txt` (the transcript ends at the close) and read the
   "finish_game: ..." marks. The boot's marks (item 3) are in every log.
6. **Record:** STAGE7.md "Hardware results (New 3DS)" and captures; PERF.md "Load times"
   console column; STAGE5 open items; this file. Delete `debug-perf` from the card.
   Commit per piece; refresh the backup bundle.

## Catch-up runbook (stage 3: the console re-check, when the user is back)

**Done (2026-10-18):** runs A-C on the console; results in backlog items 1-3, 6
and 9, `PERF.md` "Load times", `PATCHES.md` 50-53. Kept for the record.

Everything below needs the console; nothing else is waiting. The builds at
`30f3b72` have the D-Pad zoom, the sound reads (`PATCHES.md` 43) and the new
marks. The card is ready: prefs `rate="32728"`, `bottom_screen="0"`, no
`debug-perf`. Same driving as "How to drive the console"; IP was
192.168.100.72. Kill any leftover 3dslink first
(`pgrep -fl '^/opt/devkitpro/tools/bin/3dslink'`): one left from a run that
ended with a lid close holds port 17491 and the next launch cannot bind.

1. **Build:** `export DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM;
   cmake --build build-3ds && cmake --build build-3ds-prof`.
2. **Run A, profile build, level 0:** `build-3ds/hw/sprint8/next.sh IP - - 0`,
   netloader, `launch.sh IP build-3ds-prof/alephone.3dsx build-3ds/hw/sprint8/s3-L0-3dslink.txt`,
   A twice. Compare with `hm-L0-3dslink.txt` (2026-10-13): boot marks ("sound:
   sounds file opened", "environment: ..." against 9.4 s and 12.1 s), the load's
   "entering_map: monster sounds" (was 3.96 s as "sounds"), per-window `max`
   and `world` in the first windows (was 80-112 ms), busy (was 14.02).
3. **Run B, level 20:** `next.sh IP s3-L0 0 20`, the same; compare with
   `hm-L20-3dslink.txt` (first window 314 ms, sounds 4.61 s, busy 13.54).
4. **Run C, normal build:** `next.sh IP s3-L20 20 rm`. New game (HUD + Map):
   D-Pad Down/Up zoom the bottom map between 1 and 3 (a click at the ends),
   taps do nothing, a drag looks, SELECT opens the top map and Up/Down zoom
   that one; does the start of the level still hitch? Save at level 1's
   pattern buffer (the "save_game_file: ..." marks split the 69 s), quit to
   the main menu ("environment: ..." marks split the 12 s), wait for the
   demo, HOME > Close (is there a "system asked the application to close"
   line?). Fetch `log.txt` over ftpd. Player name (backlog item 9): the log's
   `[3ds] player name from the console profile: "..."` line shows the console's
   profile name (on a card with saved preferences the name in them is kept:
   to see it in Preferences > Player, move `Marathon 2 Preferences` aside with
   ftpd first and put it back afterwards). **The player-name part of run C is
   done (2026-10-18).**
5. **Decide with the user** from the numbers: the slow save step (fix it, or a
   "Saving..." message as the user asked if it cannot be fixed); the next
   step of the scenario reload and boot (the dialog theme's 5.2 s too).
   Measure before and after; keep only measured gains.
6. **Record:** `PERF.md` "Load times" (console after 43), `STAGE7.md` (D-Pad
   zoom on the console), `STAGE5.md` open items, this file; commit per piece;
   refresh the backup bundle (`STAGE5-PREREQS.md` item 8).

## Stage 10 G0-b runbook (the citro3d GPU present on the console)

Stage 10 is **paused at G0** (user's decision, 2026-10-16; `CITRO3D_DESIGN.md`
0.5). G0-b decides whether to keep the GPU present (P1, tag `port-stage10-P1`):
**HUD + Map `present` drops ≥ 2 ms with no visible change**. Then the user
decides G1 (build the 3D renderer). Same driving as "How to drive the console";
kill leftover 3dslink first.

**All console steps are done** (2026-10-19, two sessions: `PERF.md` "Console
G0-b, first session" and "second session"). In short:

- `present`, GPU against software: HUD + Map −2.24 to −2.30 ms @30, −1.54 to −1.89
  @60 (+0.3-1.0 ms moves into `bottom`); Classic −2.21 to −2.43 @30, −1.26 to −1.28
  @60. The worst frame (`max`) is up to 22 ms longer. The software build already
  runs HUD + Map @60 at ~60 fps.
- Pass: no tearing (every capture frame N), `present_diff` 0 differences, the odd
  surface (pixel-exact), the L20 water and the "Saving..." box (by eye), HOME and
  back, sleep and wake, HOME > Close in a dialog, the citro3d release before
  `gfxExit` on every error path, the off-main-thread hand-off, no crash in citro3d.
- **Blocker: HOME > Close in a game hangs** (2 of 2 tries in the second session):
  a present runs after the suspend, then `release()` never returns ("Closing
  software..." for good, power button only). Not seen in the software build.
- Not citro3d, but found on the way: every fatal error ends in a Luma exception and
  a power-off (backlog item 14, now console-confirmed); `error_screen()` on a second
  thread draws nothing; the 3dslink mirror can freeze the game (backlog item 15).

The card is clean (`debug-*` deleted) and the prefs are the user's (HUD + Map,
Framerate Target 60). The helpers stay in `build-3ds/hw/` (not in git):
`g0b/run.sh BUILD TAG` (console IP 192.168.100.72 written in it) and
`sprint8/setprefs.sh IP MODE FPS`.

**Decided (user, 2026-10-19; `CITRO3D_DESIGN.md` 0.5, open questions 3 and 9):**
keep the GPU present, build the 3D renderer aiming at M7 (2x2 supersampling) and
M9 (stereo 3D, HUD + Map only), try the zero-copy present, and fix what the console
found. The plan, in order (each piece: Azahar first, then a console check; stage 10
pieces go through `c3d-architect` / `c3d-critic` / `c3d-implementer` as in
`CLAUDE.md`):

A. **Fixes** (before anything builds on P1), in the order of `CITRO3D_DESIGN.md`
   5.6.0 (design critic-approved, `3233e60`). **Handoff, 2026-10-24** (a fresh session
   continues from here; nothing below is pushed to the fork yet):
   0. **Done** on `3ds-port`: the SDL2 patch, HID released once (item 14 b,
      `5cd45ce` + `b829aff`; SDL reinstalled into portlibs), and the 3dslink mirror no
      longer blocks (item 15, `29a3326`).
   1. **Done** on `3ds-port`: `error_screen` draws through the console device, holds its
      own HID reference, survives HOME, logs d6 (`94e3036` + review follow-up `612173a`;
      critic APPROVE).
   2. **Done** on `3ds-port`: the fatal hand-off to the main thread (`8d66595` + round-1
      fixes `05c6d27` + round-2 minors `b1495f1`; `PATCHES.md` 54, `--wrap=abort`,
      `debug-fatal` modes 1-11 incl. 6b/8a/8t, all clean in Azahar). Critic: round 1
      REVISE, round 2 APPROVE (2026-10-24); two items deferred to `3ds-port` NEXT-SPRINT
      item 14 (HOME during the fallback; the merge note in 4).
   3. **Done** on `3ds-port`: the A1 port side (design 5.6.1 a, c): `exit_requested()` also
      returns `aptShouldClose()`, `app_suspended()` (unused until piece 5), d1 (every APT hook
      logged with thread, GPU right, should-close), script command `home-close [should-close]`,
      scripts `22-c3d-home-close.txt` / `22-c3d-home-close-fast.txt` (`1199548` + follow-up
      `189857e`; `3ds-port` NEXT-SPRINT item 16). Critic round 1 APPROVE (medium effort).
   4. **Done** (2026-10-24): `3ds-port` (`189857e`) merged into `stage10-c3d`. Conflicts:
      `CLAUDE.md` (kept this branch's stage 10 state), NEXT-SPRINT items 14-16 (both copies
      auto-merged; one kept, with this branch's "Causes / user decision" paragraph). Merge
      adaptations: (a) `present_screen()` runs `fatal_test_tick` and `fatal_service_point()`
      before the GPU branch (they were in `present_screen_cpu()`, never reached with citro3d
      presenting); (b) `error_screen_here()`: on main `a1_c3d_release_top()` first; in 5.6.3's
      fallback (off main, `raw_log`) only the new `a1_c3d_owns_top()` (one atomic load), because
      `release_from_any_thread()` `printf`s through the tee lock and waits 2 s for a main thread
      known to be blocked; with citro3d up the fallback logs `citro3d still up (fallback off
      main): logged only`, draws nothing, no wait (design 5.6.2 a; critic round 2 item 5: never
      citro3d off main). Four builds 0 warnings; `arm-none-eabi-nm`: OFF builds 0 citro3d
      symbols, ON 119. Azahar — non-authoritative (`build-3ds-c3d`, GPU present): `debug-fatal`
      modes 1, 2, 6, 11: release on main, then the error screen, `exit`; modes 7, 10: the
      fallback line above, `svcExitProcess`; scripts 22 fast (no dialog frame) and plain (the
      dialog frame; Azahar keeps GPU rights, so no hang); 0 null reads in each run.
   **Next: 6 (the console session), then B.**
   5. **Done (Azahar; console check pending), 2026-10-24:** the HOME > Close hang (A1, design
      5.6.1 a/b), `stage10-c3d` only, no engine edit (no `PATCHES.md` entry), commit
      `stage10: A5 ...`. What it is, in the order of the code:
      - `render_c3d/core/gate.h` (new, pure, host-tested): `GateInputs` (`!gpu_right ||
        should_close || suspended`), `GateTracker` (d5 transitions, per-episode and total skip
        counts), `retire_wait` (poll a non-blocking try with a 1 ms sleep between, up to a
        bound; a template, so the host test drives it with a fake queue and clock), `FrameStart
        { Ok, AlreadyInFrame, Abandoned }`. Host test:
        `scripts/3ds/tests/c3d-host/build.sh` (36 checks: the parser incl. the four new keys,
        the gate, the tracker, the retire loop; also the first host test of `core/`, so the
        directory is new).
      - `back/context.cpp`: `gate_closed()` = `!gspHasGpuRight() || aptShouldClose() ||
        a1_3ds::app_suspended()`; `serve_release()` (the former `ensure_up()` branch, now
        step 1 of the present); `gate_present()` (step 2: Skip = closed and citro3d up or
        surrendered, return true with no citro3d call; Cpu = closed and citro3d not up, return
        false; with `suspend_gate 0` it only watches and logs); `ensure_up()` refuses to
        initialise while the gate condition holds (step 3); `frame_begin()` returns the
        tri-state, and polls `C3D_FrameBegin(C3D_FRAME_NONBLOCK)` with 1 ms sleeps up to
        `retire_wait_ms` **only when the gate condition holds** (the measured path with the gate
        open is the unchanged `C3D_FrameBegin(0)`); `release_and_fini()` always uses the bounded
        wait instead of `C3D_FrameBegin(0)` and, past the bound, `abandon()`s: no `C3D_Fini`, no
        `present_free`, `s_off`/`s_surrendered`/`s_abandoned` set, `s_up` false last, and
        `__wrap_gfxExit` still calls the real `gfxExit()`. No `gxCmdQueueWait(q, N > 0)` anywhere
        (`gx.h`:125). d2: a state line first (`in_frame`, gate and its inputs, frames ended with
        the gate closed, skipped presents, ms since the last frame end), then one line per
        step. d4: `real gfxExit returned`. d5: `gate closed at present N (...)`, `gate open
        again after K skipped presents`, `gate_skipped=` in the stats line and in `[c3d] final`.
      - `back/present.cpp`: the order of 5.6.1 a in `a1_c3d_present()` (serve release, gate,
        `ensure_up()`); an `Abandoned` frame_begin returns true without drawing and without the
        "already inside a frame" branch; d3 breadcrumbs (before/after `frame_begin` inside
        `frame_begin`, before the VBlank wait, before and after `frame_end` with `gspHasGpuRight()`
        and in-frame), printed only while the gate condition holds; `present_close_frame()`
        (`close_frame 1`: in the release, when the gate condition holds, one forced frame that
        draws the present quad from the present texture, no upload).
      - `debug-c3d` keys (`core/config.h` + parser + host test): `suspend_gate 0|1` (default 1),
        `retire_wait_ms N` (default 500, 0 = unbounded, max 600000), `force_abandon 0|1`,
        `close_frame 0|1`. Configs in `scripts/3ds/tests/c3d/`: `p1-home-close-diag.cfg`
        (`suspend_gate 0`, `retire_wait_ms 5000`, `close_frame 1`), `p1-abandon.cfg`,
        `p1-unbounded.cfg` (`suspend_gate 0`, `retire_wait_ms 0`: the old behaviour, to
        reproduce the hang on purpose; not needed by any run).
      **Deviations from the design text:** (1) d2/d5 log the gate's inputs as `gpu_right`,
      `should_close`, `suspended` (`app_suspended()` = `in_home || asleep`); the design names
      `in_home` and `asleep` separately, but only the combined value is exported, and the
      accessors are in `input_3ds.cpp`/`a1_3ds_platform.h`, shared with `3ds-port` (5.6.0: fixed
      there, not here). (2) `frame_begin()` also returns `AlreadyInFrame` when our own
      `s_in_frame` is set, without calling citro3d (a NONBLOCK poll would otherwise spin out the
      whole bound on a refused `C3D_FrameBegin`). (3) `ensure_up()` returns false without
      setting `s_off` while the gate condition holds, so citro3d can still start later (the gate
      at boot is open anyway). (4) The release's state line and step lines are printed for every
      release, not only gate-closed ones (they cost nothing outside an exit). (5) `crumb` (d3)
      lines go through `fflush(stdout)` each, so the last breadcrumb is on the card before a
      hang. (6) A release started by a thread other than main waits `kHandoffWaitMs` = 2 s
      (`context.cpp`); with `p1-home-close-diag.cfg` the main thread's release can take up to
      5 s + 5 s, so an off-main caller would give up first in that config (only matters for
      run 1 if an error screen on a second thread coincides; not expected).
      **Azahar — non-authoritative** (`build-3ds-c3d`, Marathon 2; Azahar keeps GPU rights,
      retires GPU frames synchronously and cannot hang a queue, so `queue retired after 0 ms`
      every time and the gate closes through `in_home` only):
      - script 22 plain (`home-close`: ONSUSPEND hook, then `SDL_QUIT`, the invisible quit dialog)
        with `p1-gpu.cfg`: `apt hook ONSUSPEND ... gpu right 1`, `gate closed at present 558
        (gpu_right 1, should_close 0, suspended 1)`, 3 presents skipped (the dialog's draw
        included, no citro3d call), release state line `gate closed ... skipped presents 3`,
        every d2 line, `release done`, `final: ... gate_skipped=3 gate_episodes=1`, `real
        gfxExit returned`, `exit`. Script 22 fast: gate never observed (no present after the
        hook), 0 skipped, same release lines, `exit`.
      - `p1-home-close-diag.cfg` (`suspend_gate 0`, 5000 ms, `close_frame 1`), script 22 plain:
        `gate closed ... ; suspend_gate 0: presenting anyway`, d3 breadcrumbs around three
        presents (`frame_begin ... retired in 0 ms`, VBlank, `before frame_end (gpu right 1, in
        frame 1)`, `after frame_end`), release: `close_frame 1: one forced present frame`, its
        breadcrumbs, `queue retired after 0 ms`, `release done`, `frames_gate_closed=4`, `real
        gfxExit returned`, `exit`. Fast script: same release path.
      - `p1-abandon.cfg` (`force_abandon 1`), scripts 22 plain and fast: `release abandoned
        (release): force_abandon 1, not polling`, `final: ... abandoned=1`, `real gfxExit
        returned`, `exit`; no CPU present afterwards.
      - `test_vhalt` 1, 2, 3, 4 (`p1-vhalt*.cfg`, script 17), against pieces 1-2: one `release`
        each (modes 2-4 served by the main thread, d6 `thread main`, `stdout device ... log`),
        `queue retired after 0 ms`, `release done`, `real gfxExit returned`, `exit`; mode 1 also
        logs gfxExit #2 (`citro3d down`).
      - `debug-fatal` modes 1, 2, 7, 11 (script 24, `--fatal`): 1, 2 and 11 release on main then the
        error screen then `exit`; 7 logs `citro3d still up (fallback off main): logged only` and
        `svcExitProcess` (no citro3d call off main), as before the piece.
      - Regression, `p1-gpu.cfg` and `p1-diff.cfg`, scripts 07, 08, 13, 14, 19: all run to the
        end (07 and 14 have no `quit` and are stopped by the timeout, so no `final` line; their
        last `[c3d] stats` line is read), **0 skipped presents** in every run (the scripted `home`
        and `sleep` hold the main thread in the hook), `present_screen_false_returns=0`,
        `present_diff` 0 differences in every check (diff-08: 71 checks, diff-13: 31, diff-14: 24,
        diff-19: 3; 07: all logged checks 0), gx high-water 3. Frame-indexed `p1-calibrate.cfg` and
        `p1-calibrate-altload.cfg`: 24 captures each, every one uniform and showing the logged
        frame number; `p1-odd.cfg`: odd picture captures taken, 120 odd presents, 0 false
        returns. `azahar_log.txt`: no guest fault in any run.
      - Builds: all four, 0 warnings; `arm-none-eabi-nm build-3ds/alephone.elf` and
        `build-3ds-prof/alephone.elf | grep -ciE 'citro|C3D_|a1_c3d'` = 0. Frame cost of the gate:
        `PERF.md` "Stage 10 plan A piece 5".
      **Console runs still to do (the single session, item 6; design 5.6.1 runs 1-4).** Build
      `build-3ds-c3d` (or `-prof`), install over the launcher as before, **start every run from
      the Homebrew Launcher**, not 3dslink: the pass criterion is that "Closing software..."
      ends, the launcher comes back and Aleph One launches again. Put the config on the card with
      `scripts/3ds/sd-ftp.sh IP put scripts/3ds/tests/c3d/<cfg> /3ds/alephone/`, remove the old one
      (`sd-ftp.sh IP rm /3ds/alephone/debug-c3d`; a rename onto an existing file may fail on the
      card's FAT and silently keep the previous config) and rename the new one to `debug-c3d`
      (`sd-ftp.sh IP mv /3ds/alephone/<cfg> /3ds/alephone/debug-c3d`). **Check each run's config
      at the top of `log.txt`:** the boot line `[c3d] debug-c3d: ... suspend_gate X,
      retire_wait_ms Y ...` must match the run (run 1: `suspend_gate 0`). Read the card's
      `/3ds/alephone/log.txt` afterwards (the d1-d5 lines are in it; a launcher start has no
      3dslink mirror). Delete `debug-c3d` at the end.
      1. **Diagnostic**, `p1-home-close-diag.cfg`, Classic @30 (as `home-close-1`), in a game:
         HOME, then Close. Expect `apt hook ONSUSPEND on thread <main> (main <same>): gpu right 1`
         and no hook line after it; the `d3 close_frame:` breadcrumbs with `gpu right 0` before
         the forced `frame_end`; then `did not retire` / `release abandoned (release): the GPU
         queue did not retire after 5000 ms`, `real gfxExit returned`, a normal close.
         **Also a pass (the other ordering):** a natural present after the hook runs first, its
         bounded `frame_begin` succeeds (citro3d's ONSUSPEND hook drained the queue) and its
         `frame_end` submits without rights; the release's `close_frame` then abandons inside
         `frame_begin`: `release abandoned (frame_begin): the GPU queue did not retire after
         5000 ms`, the `gpu right 0` breadcrumb is that present's `before frame_end`, and there
         is no `close_frame: after frame_begin` and no `release: waiting for the queue` line.
         Either ordering confirms the last UNVERIFIED step (GSP never completes work without rights). If
         `queue retired after X ms` with a large X: slow, not never; report X. If it hangs: the
         last `d3`/`release:` line on the card names the wait; stop and report. Then relaunch.
      2. **The fix**, `p1-gpu.cfg`: HOME > Close in a game three times (HUD + Map @60 twice,
         Classic @30 once). Expect the release's state line `gate closed (gpu_right 0, should_close 1, ...)`,
         and `gate closed at present N` with >= 1 skipped present only if a present came before
         the quit (with piece 3's fast path there may be no present, so no such line), `queue retired
         after 0 ms`, `release done`, `real gfxExit returned`, `exit`. Each close returns to the
         launcher and Aleph One starts again.
      3. `p1-diff.cfg`: HOME and back in a game twice, sleep (lid) and wake in a game twice: the
         picture is right, `present_diff` shows 0 differences, `gate_skipped=0` in the stats lines
         (predicted: the main thread is blocked in `aptMainLoop()` for both), each d1 hook on the
         main thread with gpu right 1. Also the software keyboard (e.g. naming a save) confirmed
         and cancelled, and POWER then cancel, both with `p1-gpu.cfg`: library-applet suspend /
         power menu, the gate never seen closed or reopened with presents resuming (a skipped
         ONRESTORE would leave `s_in_home` set: every present skipped, frozen top screen).
      4. No regression: HOME > Close in a dialog and at the main menu; no new Luma dumps.
      Also read off the first run: whether `retire_wait_ms` 500 is ever hit in a normal release
      (an error screen right after a busy frame): a `release abandoned` line outside run 1 is a bug.
      Reading d2: "ms since the last frame end" is genuinely 0 when the release is served from
      `present_bottom`'s service point right after the top present (`test_vhalt` 2-4,
      `debug-fatal` 2).
      **Critic round 1 on `6b152e4`: APPROVE** (2026-10-24, medium effort; runbook findings 1, 2,
      6 applied above). Deferred: (3) after an abandon, `a1_c3d_owns_top()` and
      `release_from_any_thread()` report citro3d gone, so an error screen during exit does
      `consoleInit` on the top screen while citro3d's `onQueueFinish` is still bound (it would
      `gfxScreenSwapBuffers` on the GSP thread if the abandoned queue ever retired): decide
      log-only-after-abandon or document the accepted race (5.6.1 b); reachable only with an
      abandon plus an error at exit, or the forced configs. (4) `kHandoffWaitMs` (2000) is
      shorter than a main release under `p1-home-close-diag.cfg` (up to 5 + 5 s); fix later as
      max(2000, 2 x `retire_wait_ms` + 500); not reachable in run 1. (5) 500 ms default kept.
      Nits: `p1-unbounded.cfg` is a deliberate reproduction only; `ensure_up()`'s
      `if (s_up) return !serve_release();` is dead in practice.
      **Console (New 3DS), 2026-10-24 (A6 session, `build-3ds-c3d`, 192.168.100.72, every run
      started from the Homebrew Launcher, logs read from the card; raw logs in
      `build-3ds/hw/a6/`, outside git).** Runs 1-4 all pass; each close returned to the launcher
      and Aleph One launched again; no new Luma dump (still dumps 0-6).
      - **Run 1, `p1-home-close-diag.cfg`, Classic @30 (`A1-diag.log`).** Boot line `suspend_gate 0,
        retire_wait_ms 5000, close_frame 1`. `ONSUSPEND on thread 287 (main 287): gpu right 1,
        should close 0`, no hook after it. Release: `gate closed (gpu_right 0, should_close 1,
        suspended 1)`, `close_frame 1: one forced present frame`, `d3 frame_begin: after, retired
        in 0 ms` (citro3d's own ONSUSPEND hook drained the queue, as predicted), `d3 close_frame:
        before frame_end (gpu right 0, in frame 1)`, `after frame_end`, `release: waiting for the
        queue (up to 5000 ms)`, then **`release abandoned (release): the GPU queue did not retire
        after 5000 ms`**, `final: ... abandoned=1`, `real gfxExit returned`, `exit`: the first
        ordering. **GSP did not complete work submitted without GPU rights, for the full 5 s
        ("never", not "slow").** The close took 9.4 s from the hook (5 s of it that wait); the user
        saw a slow but clean close.
      - **Run 2, `p1-gpu.cfg` (`A2-1/2/3.log`).** Boot line `suspend_gate 1, retire_wait_ms 500,
        close_frame 0`. HUD + Map @60 twice and Classic @30 once: `gate closed at present 833 / 964
        / 747 (gpu_right 0, should_close 1, suspended 1)` (the gate was seen closed by a present
        after the hook, in all three), release state line `gate closed ... skipped presents 1 / 1
        / 2`, **`queue retired after 0 ms`** each time, `release done: C3D_Fini returned; linear
        free 17788 KiB, VRAM free 6144 KiB`, `real gfxExit returned`, `exit`;
        `gate_skipped=1 / 1 / 2`, `gate_episodes=1`. Hook to `exit` 3.3 / 3.0 / 4.8 s ("much
        faster", the user).
      - **Run 3, `p1-diff.cfg`, HUD + Map @60 (`A3.log`).** `present_diff final: 166 checks, 0 with
        RGB differences` (0 of 96000 pixels in every logged check); HOME and back ×2 (3.5 s, 3.6
        s), lid sleep and wake ×2 (1.1 s, 0.9 s), the name keyboard confirmed (a library-applet
        `ONSUSPEND`/`ONRESTORE` pair around it): every hook on the main thread with gpu right 1,
        presents resumed after each, `gate_skipped=0`, `gx_refused=0`, `odd_surface=0`, 5000
        presents. The software keyboard was the main menu's Player name (saves in game do not open
        one). POWER then cancel: see below.
      - **Run 4, `p1-gpu.cfg`, HUD + Map @60 (`A4-1.log`, `A4-2.log`).** HOME > Close with a
        dialog/menu showing (`terminals: 640x320`, after several HOME round trips): gate closed,
        `queue retired after 0 ms`, `release done`, `exit`, `gate_skipped=0`; HOME > Close at the
        main menu after cancelling the name keyboard (`keyboard: cancelled (button 0, result 1)`,
        hooks paired): the same. **No `release abandoned` line in runs 2-4** (the `retire_wait_ms`
        500 was never hit in a normal release; an error screen right after a busy frame is part
        B, not run yet).
      - **POWER (short press) then cancel (in `A3.log`).** Not a resume: the app is closed. The
        log has `ONSUSPEND` (gpu right 1, should close 0), no `ONRESTORE`, the gate closed at
        the release, `queue retired after 0 ms`, `release done`, `exit` about 1 s later; the user
        is left at the HOME menu. The earlier expectation "presents resume after the cancel"
        does not hold on this console; the close is clean.
      **Parts B and C (sessions 2-3, 2026-10-25/26):** see items 14 and 15 (B1-B5, 6, 11 and the
      shipping spot check pass; 6b and mode 10 end in the system's restart; part C inconclusive).
      **Mode 7 on the shipping build (2026-10-26) also restarts the console** (item 14). **Not run:**
      B8 on the GPU build (skipped by the user's choice) and part D (hold POWER > Power Off,
      declined by the user).
   6. **Done on the console (2026-10-26) with two findings** (see the results under item 5 and in
      items 14-16): part A, B0-B5, 6, 11 and the shipping spot check pass; **6b and mode 10 fail**
      (the fallback's `svcExitProcess` makes the system restart the console, item 14); part C
      is inconclusive (item 15); B8 and part D were not run. Open: the fallback exit fix (mode 7 on the shipping
      build shows it is not citro3d-specific), then a console re-check of mode 7, 6b and 10, part C
      with a chattier log, part D.
      **One console session** (procedure and order for a fresh agent: `docs/3ds/CONSOLE-SESSION-A6.md`) for all of it: design 5.6.1 runs 1-4 (written out in item 5), the `debug-fatal`
      list in item 14 (boot-time control, mode 1 by eye, modes 2-4, 6, 6b; the launcher
      relaunches Aleph One after each; the DSP falls silent after 6/6b/7), the mirror
      check (item 15: `kill -STOP` the host 3dslink), and read the card's `log.txt`
      (raw fatal lines are not mirrored). Workflow per piece: implementer, then critic
      (max 2 revise rounds); design: architect then critic.
      **The fallback exit fix is piece 7 (built overnight 2026-10-26, Azahar only, UNVERIFIED on
      the console).**
   7. **A7, the fallback's exit (built 2026-10-26; Azahar passes; console check pending):**
      design `CITRO3D_DESIGN.md` 5.6.3 step 3a (decision and rejected options), commits
      `4981782` (decision), `db10ade` (code), `5da39e0` + `5bd0867` (review round 1), critic
      round 2 APPROVE (medium effort). The fallback now ends through `gfxExit` (software build
      only), `aptExit()` and `APT_CloseApplication` (libctru's deferred close, through
      `__system_retAddr`), without the heap frees, behind a 5 s watchdog; `apt_hook` on the
      fallback thread logs one raw line and returns; d7 logs the launcher's run flags at boot.
      Port code only (`ports/3ds/source/a1_3ds_platform.cpp`, `input_3ds.cpp`,
      `ports/3ds/include/a1_3ds_platform.h`, `scripts/3ds/run-azahar.sh`): no engine edit, no
      `PATCHES.md` entry. **These files are shared with `3ds-port`, which still has the old
      fallback** (the shipping build restarts the console in mode 7); A7 is on `stage10-c3d`
      only, by the user's instruction for this task (design 5.6.0 row A7); carrying it over is
      the user's decision once the console passes. Results: item 14. Next: the "Fallback exit
      runbook" at the end of this file (F0-F3, optional F4/F5, then part C; part D only if the
      user says yes).
B. **Zero-copy present** (critic P1 finding 3): `main_surface` in linear memory,
   read by the GPU, TEV reorders the channels; one engine edit (`PATCHES.md` 55+).
   Measured on the console against the current GPU present.
C. **The renderer:** M0-M5 (readback, no speedup; gate G2), M6 (direct mode, HUD +
   Map; gate G3), then M7 (supersampling), M9 (stereo). M8 (a Renderer preference)
   is optional; open questions 4-8, 10-12 are asked when their milestone comes up.

## Fallback exit runbook (piece A7 on the console; when the user is back)

Written 2026-10-26 (overnight, no console). Piece A7 (`stage10-c3d`, `db10adee1`; design
`CITRO3D_DESIGN.md` 5.6.3 step 3a) changes how the fatal fallback ends: instead of a bare
`svcExitProcess()` (the system's "An error has occurred forcing the software to close. The
system will now restart." in A6, item 14) it runs `gfxExit()` (software build only),
`aptExit()` and `APT_CloseApplication` (through `__system_retAddr`), behind a 5 s watchdog.
**Everything here is UNVERIFIED on the console**: that the missing APT close was the cause,
that `aptExit()` off the main thread ends cleanly on hardware, and whether `gspExit` matters.
Driving as in "How to drive the console"; setup, flag swaps and log fetching exactly as
`CONSOLE-SESSION-A6.md` "Setup", with `H=/Users/dashielashie/Desktop/Aleph_3ds/build-3ds/hw/a7`
(`mkdir -p $H`) and the user's own preferences (HUD + Map @60: the modes fire at the main
menu, 20 s after the first present). Every run starts from the **Homebrew Launcher** (not
3dslink: the raw fallback lines are only on the card, and a 3dslink launch overwrites
`sdmc:/3ds/alephone.3dsx`).

**Builds.** Both from the `stage10-c3d` worktree, where A7 is:
`build-3ds/alephone.3dsx` (software renderer, A7) and `build-3ds-c3d/alephone.3dsx` (GPU
present, A7). **Not** the main checkout's `build-3ds` (`3ds-port`, the shipping build: it does
not have A7). Check first: `cmake --build build-3ds && cmake --build build-3ds-c3d` (nothing to
do / 0 warnings), and `$DEVKITARM/bin/arm-none-eabi-nm build-3ds/alephone.elf | grep -ciE
'citro|C3D_|a1_c3d'` prints 0.

**Pass for F1-F3:** the port's error screen where it can be drawn (software build; with the
GPU present up nothing is drawn by design, the top screen stays frozen until the exit), then
the **Homebrew Launcher** comes back and Aleph One launches again from it; **no** system
restart message, **no** Luma exception screen, **no new Luma dump** (dumps 0-6 exist:
`/luma/dumps/arm11/`, list it before and after); silent (listen after the exit and after the
relaunch). After each run fetch `log.txt` into `$H/F<n>.log` before the next launch and check
its boot lines: `[3ds] debug-fatal: mode N ...` must match the run.

0. **F0, d7 (read off F1's log, no extra run):** the boot line `[3ds] env: homebrew <0|1>,
   run flags 0x<hex> (aptworkaround <0|1>, aptreinit <0|1>, aptchainload <0|1>), return address
   <set|null>`. Record it in `CITRO3D_DESIGN.md` 5.6.4 item 6. Prediction from libctru's
   `aptExit()` (`apt.c`:371-440): `aptreinit 0` and no close order give the close branch, i.e.
   the line `fallback exit: APT_CloseApplication via __system_retAddr` below. `aptreinit 1` gives
   `APT_Finalize` and `svcExitProcess (aptExit left __system_retAddr unchanged: ...)`: record it;
   **expect the restart then** (libctru would call the loader's return address after freeing
   the heap, which the fallback cannot do: design 5.6.3 step 3a.4), and report it as the
   finding rather than running F2/F3 blind. Azahar shows `homebrew 0, run flags 0x0`, return
   address null.
1. **F1, mode 7, software build** (install `build-3ds/alephone.3dsx` of the worktree as
   `/3ds/alephone.3dsx`; `debug-fatal` = `scripts/3ds/tests/fatal/mode-7.cfg`). The strongest
   case: the screen is drawn, and the exit has `gfxExit` + APT close. Expected log tail (Azahar
   order): `fatal error on thread N: handed to the main thread`, `not acknowledged in 5 s; shown
   on this thread`, `ERROR: debug-fatal mode 7`, `error screen d6: thread other ...`, (A pressed)
   `fallback exit: starting the 5 s watchdog`, `fallback exit: gfxExit`, `fallback exit: should
   close 0, run flags ... ; aptExit`, `fallback exit: APT_CloseApplication via __system_retAddr`.
   The user: the screen reads "debug-fatal mode 7 / A second thread failed holding a lock the
   main thread needs"; one A; then the launcher.
2. **F2, mode 6b, GPU build** (install `build-3ds-c3d/alephone.3dsx`; `mode-6b.cfg`): `fatal
   error on thread N: handed`, `not acknowledged`, `error screen: citro3d still up (fallback off
   main): logged only`, `starting the 5 s watchdog`, **no** `gfxExit` line, the `aptExit` line,
   `APT_CloseApplication via __system_retAddr`. No screen (by design), so no A: the exit follows
   within about a second of the 5 s timeout. Also the DSP check from A6 B6 that the restart
   prevented: silent after the exit, normal sound after the relaunch.
3. **F3, mode 10, GPU build** (`mode-10.cfg`): the same lines as F2 (the error is
   `debug-fatal mode 4`'s text). Main sleeps 7 s; the exit should come at about 5 s, before it
   wakes, so no `the request was taken back` line is expected.

**Reading a failure (stop after the first; do not retry blindly; ask the user to power-cycle
if it hangs).**
- `fallback exit: watchdog fired (stuck in <step>); svcExitProcess`: that step hung on
  hardware; the restart that follows is the old behaviour. Record the step.
- `fallback exit: APT_CloseApplication returned; svcExitProcess`: `APT_CloseApplication` did
  not end the process; the restart that may follow is the old path. Record it.
- `watchdog fired (stuck in APT_CloseApplication)`: NS took more than 5 s to end the process
  and the watchdog raced it; record whether the launcher came back anyway.
- The `APT_CloseApplication via __system_retAddr` line, then the system's restart message: the
  APT close was not enough (or not the cause). If F1 passed and F2/F3 restart, `gspExit` matters
  in the GPU build (5.6.3 step 3a.2: that build ends holding the GPU right, GSP event thread
  alive): the next candidate is a bare `GSPGPU_ReleaseRight()` on the fallback thread before
  `aptExit` (one IPC, no citro3d call, no thread join; critic round 1, 5), a new piece. If F1 restarts too: the remaining candidates are
  the other `__appExit` calls (`fsExit`, `hidExit`, `srvExit`) or the heap teardown itself; then
  the narrower fix (make the fallback rarer: a longer acknowledgement bound, main serving more
  often; 5.6.3 step 3a "Rejected" C2) is the next decision, the user's.
- Luma's exception screen or a new dump: fetch it (registers at 0x28; `arm-none-eabi-addr2line
  -e build-3ds[-c3d]/alephone.elf`), record, stop.
- `apt hook <NAME> on the fallback thread: ignored` lines are expected only if HOME (or POWER)
  is pressed while the fallback runs; don't press it in F1-F3 (optional F4 below).
4. **F4 (optional, only if F1 passed):** F1 again, and press HOME while the fallback screen is
   up, then back, then A. Expected: `apt hook ONSUSPEND on the fallback thread: ignored` (and
   ONRESTORE), the error text still there after HOME (on main it survives a HOME round trip,
   script 25 in Azahar; on the fallback thread this is untested), then F1's exit lines and the
   launcher. This is deferred item (4) of piece 2's review, which A7 addresses; no Azahar script
   reaches it (HOME during the fallback), so the console is its only test.
5. **F5 (optional, only if F1 passed):** F1 again, and choose **HOME > Close** while the
   fallback screen is up. The only way to reach `aptExit()`'s close-order branch and its ONEXIT
   hook off main: expected `apt hook ONSUSPEND on the fallback thread: ignored`, the screen's
   loop ends (`aptMainLoop()` false), then the exit lines with `should close 1`, `apt hook ONEXIT
   on the fallback thread: ignored` (expected: the fallback never closes the log fd; its absence
   is a finding), `APT_CloseApplication
   via __system_retAddr`, then the launcher (as every HOME > Close in A6, item 16), no restart.

**Then part C (3dslink mirror, item 15) with the new step, and part D only if the user says
yes.** Part C (`build-3ds-c3d`, over 3dslink this time, HUD + Map @60):
- **C1 (no sudo):** put an empty `debug-input` on the card (`$FTP IP put` an empty file as
  `/3ds/alephone/debug-input`). Launch with `.../build-3ds/hw/sprint8/launch.sh IP
  build-3ds-c3d/alephone.3dsx $H/C1-mirror.txt` in the background (kill stale 3dslink first:
  `pgrep -fl '^/opt/devkitpro/tools/bin/3dslink'`), start a game, then `kill -STOP` the host's
  3dslink process and have the user **move both sticks continuously** (circles) for 60 s, up to
  4 min if no line has appeared, then `kill -CONT`. Why: the stall path needs the console's send
  buffer full, and a stopped 3dslink leaves the host kernel's receive buffer in the way. Host
  reproduction (`scripts/3ds/tests/link-stall/run.sh`, macOS loopback, tee_write's loop copied):
  388-550 KB are accepted before the stall path runs, whatever buffer sizes are asked for;
  that is 38-54 min at the in-game ~170 B/s, and about 25-35 s at the ~16 KB/s that `debug-input`
  logs with both sticks moving (measured in Azahar — non-authoritative, 2026-10-26, a scripted
  stick wiggle: 4796 input lines, 241 KB in 15.1 s). The Wi-Fi path and the 3DS's own socket
  buffer sizes are UNVERIFIED, hence the 4 min ceiling.
- **C2 (if C1 shows no stall line; needs sudo, so the user runs it in Terminal.app):** drop the
  console's packets on the Mac, a real "stalled link" (no ACKs, so only the console's send
  buffer has to fill):
  `printf 'block drop quick from IP to any\nblock drop quick from any to IP\n' | sudo pfctl -a com.apple/a1-stall -f -`
  with `IP` replaced by the console's address the user gave this session (it was
  192.168.100.72; a stale address blocks nothing and C2 would read as "no stall")
  then `sudo pfctl -E` (note the token it prints); keep the sticks moving for 60 s; then
  `sudo pfctl -a com.apple/a1-stall -F rules` and `sudo pfctl -X <token>`. Check with
  `sudo pfctl -a com.apple/a1-stall -s rules` (empty afterwards). The rule also blocks ftpd: remove
  it before fetching the log. (macOS's `/etc/pf.conf` loads `anchor "com.apple/*"`, so the
  anchor is evaluated.)
- **Pass (C1 or C2):** the game keeps running (the card log's `[c3d] stats` lines keep coming
  each second through the freeze), the card's `log.txt` has `[3ds] 3dslink mirror stopped
  (stalled); the SD log goes on` (with C2, `(host went away)` is possible if TCP gives up
  first: record which; both mean the game did not freeze), and the transcript stops there.
- Afterwards: delete `debug-input`; put the wanted `.3dsx` back (the 3dslink launch overwrote it).

**Cleanup (always):** delete `debug-fatal` and `debug-input`; the user's preferences stay
(F1-F3 do not change them); reinstall the shipping build
(`/Users/dashielashie/Desktop/Aleph_3ds/build-3ds/alephone.3dsx`, `3ds-port`) unless the user
wants another; kill leftover 3dslink processes; remove any pf rule (C2).

**Record:** `NEXT-SPRINT.md` item 14 (the F runs, "Console (New 3DS), <date>"), item 15 (part
C), plan A item 7; `CITRO3D_DESIGN.md` 5.6.4 items 5 and 6 (VERIFIED or REFUTED); items 14-15
text mirrored identically to `3ds-port`. If F1-F3 pass, carrying A7 to `3ds-port` (the shipping
build still has the old fallback) is the user's decision (cherry-pick of the A7 commit, or the
next merge).
