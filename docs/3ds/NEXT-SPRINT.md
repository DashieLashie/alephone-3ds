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
    occurred" screen, then a power-off. Main-thread `vhalt()`: the error screen's first
    `hidScanInput` faults (`hid.c:185`, address `0x10`); the port's screen was never
    visible (what the user saw was Luma's; corrected 2026-10-23).
    `vhalt()` on a second thread: the main thread faults in `hidScanInput` from
    `N3DS_PumpEvents`. `abort()` on a second thread: the main thread faults in newlib
    `_lseek_r` (address `0x18`) in a resource-file `fseek`. `error_screen()` on a second
    thread: nothing drawn (bottom framebuffer all black), then the app exits and the
    next program (probably the Homebrew Launcher) panics in `svcBreak`. So any fatal
    error ends in a forced power-off, not a return to the launcher.
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
15. **The 3dslink log mirror can freeze the game.** `tee_write()`
    (`ports/3ds/source/a1_3ds_platform.cpp`, port code on `3ds-port`) writes a line to
    the SD log, then `send()`s it on a blocking socket while holding the log lock; its
    comment says "stop mirroring rather than block", but a stalled Wi-Fi link blocks
    the `send()`, and every thread that logs then waits on the lock. Seen once on the
    console (2026-10-19, `build-3ds/hw/g0b/cl30-gpu-L0.stall1.txt`): the mirror fell
    behind at +50 s and the game loop froze for ~100 s after a BENCH line. Only with
    3dslink (a launcher start has no socket). A send timeout (`SO_SNDTIMEO`) or a
    non-blocking socket would let it drop the mirror instead; not tried.
16. **HOME > Close in a game takes upstream's slow quit path.** The port's
    `APTHOOK_ONEXIT` hook (`input_3ds.cpp`) can never fire in time: libctru calls it
    only from `aptExit` inside `__appExit`, after the log is closed
    (`CITRO3D_DESIGN.md` 5.6.1). So `exit_requested()` is false, and `SDL_QUIT` in a
    game opens upstream's (invisible) quit dialog, which draws a frame before
    `exit(0)` (with the GPU present, the frame that hangs). Fix: `exit_requested()` also
    returns `aptShouldClose()`; part of plan A piece 3.

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
   5.6.0 (designed, critic round 1 applied; round 2 pending):
   0. `3ds-port`, done: the SDL2 patch (item 14 b, `5cd45ce`) and the 3dslink mirror
      (item 15, `29a3326`; console check pending).
   1. `3ds-port`: `error_screen` draws through the console device (its text went to
      the log, so it never showed after boot), takes its own HID reference (item 14 a),
      logs d6.
   2. `3ds-port`: the fatal hand-off to the main thread (item 14 c): `--wrap=abort`,
      the `vhalt()` hook (engine edit, `PATCHES.md` 54), the audio-thread variant, the
      `svcExitProcess` fallback, `debug-fatal` modes 1-7.
   3. `3ds-port`: the A1 port side: `app_suspended()`, d1, `exit_requested()` also
      reads `aptShouldClose()` (backlog item 16), the `home-close` script command.
   4. Merge `3ds-port` into `stage10-c3d`.
   5. `stage10-c3d`: the HOME > Close hang (A1): the present gate (no present without
      GPU rights or while suspended/closing), bounded queue waits, the abandon path,
      d2-d5. Console: the runs of design 5.6.1 and 5.6.3.
B. **Zero-copy present** (critic P1 finding 3): `main_surface` in linear memory,
   read by the GPU, TEV reorders the channels; one engine edit (`PATCHES.md` 55+).
   Measured on the console against the current GPU present.
C. **The renderer:** M0-M5 (readback, no speedup; gate G2), M6 (direct mode, HUD +
   Map; gate G3), then M7 (supersampling), M9 (stereo). M8 (a Renderer preference)
   is optional; open questions 4-8, 10-12 are asked when their milestone comes up.
