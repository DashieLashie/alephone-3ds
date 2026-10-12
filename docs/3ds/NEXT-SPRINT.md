# Task: next sprint of the Aleph One 3DS port: finish HUD + Map, then the open items

Repo: /Users/dashielashie/Desktop/Aleph_3ds, branch `3ds-port`, commit `490f9ca`
("3DS port: stage 7 revision, HUD + Map (WIP, untested on hardware)"). Aleph One
(Marathon engine) is ported to the Nintendo 3DS; stages 1-7 are done and documented in
`docs/3ds/`. The user tests on a New 3DS. This sprint has two parts: **A** finishes work
in progress, **B** is the backlog of open items that earlier sessions recorded. Start
with A. Before starting B, show the user a triage table of B (item, cost, whether a
console session is needed, your recommendation) and let them choose the scope.

## Read first
- `docs/3ds/STAGE7.md`: the bottom screen; "Hardware results" (how the last console
  session ran and was recorded) and "Revision" (the WIP below).
- `docs/3ds/PERF.md`: "Status", "Hardware results", "The profiler", "Next candidates".
- `docs/3ds/STAGE5.md`: the "Open items" table and "Hardware session".
- `docs/3ds/PATCHES.md`: every engine change is listed and numbered there. Engine edits
  (`Source_Files/`) are fenced with `#ifdef A1_PLATFORM_3DS`; port code is in `ports/3ds/`.
- Your memory notes (3dslink, sudo, Azahar).

## Rules carried over
- Measure before and after any performance change (profile build, `debug-perf`). Keep
  only changes with a measured gain and no visual regression.
- Every engine edit gets a `PATCHES.md` entry and the non-3DS check:
  `scripts/3ds/host-baseline/ppcheck.sh "<changed files>" "<TUs>"` (Docker). DIFFER is
  acceptable only if the differences are `__LINE__` values; check that by diffing the
  preprocessed output with numbers normalised.
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
- Launch: `build-3ds/hw/stage7/launch.sh IP <path>.3dsx <transcript>` in the background.
  It retries until the netloader answers. (`3dslink -s` does not exit when the connection
  fails, so the script kills failed attempts itself.) You can start it before the user
  presses Y. The transcript mirrors the whole game log, BENCH lines included. A lid close
  ends it with "Connection reset by peer"; the rest of that run's log is in
  `sdmc:/3ds/alephone/log.txt`. Each boot moves the previous log to `log.prev.txt`.
- SD card: `scripts/3ds/sd-ftp.sh IP get|put|ls|rm ...`, files in `/3ds/alephone/`.
  Helpers from the last session: `build-3ds/hw/stage7/next.sh` (fetch a run's log and
  captures, upload the next `debug-perf`, set `bottom_screen`), `setmode.sh`.
- Benchmark: a `debug-perf` file (`level N`, `settle 8`, `measure 20`, `quit 1`, and
  `log 1` for per-window lines). The user presses A twice; the game quits on its own
  after about 2.5 min. It saves `screen-perf-L<N>-{top,bottom}.bmp`. **busy** = frame − idle.
- Monitor transcripts for `BENCH|exit|rror|abort|Connection reset`. Keep fetched files
  under `build-3ds/hw/<sprint-dir>/` (not in git). Delete `debug-perf` from the card at
  the end.

## A. Finish the HUD + Map revision (stage 7)
The user replaced stage 7's HUD and Map modes with one **HUD + Map** mode and dropped the
touch buttons (the console's buttons already do MAP / ACTION / NEXT WEAPON).
- **Done (commit above):** the bottom screen is the HUD (320x80) over a 320x160 map. The
  map has its own zoom: it starts at scale 2; a tap on the map zooms out one step
  (2 → 1 → 3 → 2); a tap on the HUD does nothing; a drag is still the look. The level
  name is kept out of the small map (`kMapNameSkip`). Bottom Screen offers HUD + Map /
  Classic; stored values 0 / 2 (an old 1 = Map becomes 0). `PATCHES.md` 37, 38 (removed;
  `vbl.cpp` is back to stage 3), 40 updated; the non-3DS check passed. Azahar: script 13
  passes in HUD + Map; BENCH busy L0 / L6 / L20 17.85 / 15.55 / 15.23 ms (`bottom`
  2.68-2.96; Classic 21.52 / 19.47 / 19.19).
- **To do:**
  1. Azahar: script 13 in Classic (`bottom_screen="2"` in the emulator's prefs file);
     the terminal test (`--terminal-test --script
     scripts/3ds/tests/input/14-terminal.txt`) in both modes.
  2. Console: BENCH levels 0 / 6 / 20 in HUD + Map. Pass: busy at or below Classic + 1 ms
     (Classic on hardware: 18.24 / 17.84 / 17.55) and 30 fps. Then a functional run:
     screens as designed, map zoom taps, HUD tap ignored, drag to look, START/B (3 round
     trips), terminal, HOME, lid close and open, quit to the menu. Then a short Classic
     check (a game start, dialog, quit). Note the memory line at `main loop ended`.
  3. `STAGE7.md`: turn "Revision" into the record (modes table, captures in
     `docs/3ds/img/`, Summary rows 2-3 and DoD, Known limitations, Files, Testing).
     Earlier HUD/Map results stay as history. Commit "3DS port: stage 7 revision, HUD + Map".
  - Open choices to confirm with the user on hardware: whether the zoom should be
    remembered across sessions (today it resets to 2 at launch), and whether a small
    zoom indicator or hint is wanted.

## B. Backlog: open items from earlier sessions
Sources in brackets. Statuses were verified against the docs on 2026-10-12.

**Loading, hangs and stutter (felt in play)**
1. **Level loads take 76-97 s, and the level change looks frozen and broken** until the
   next level has loaded. Saving at a pattern buffer is slow enough to look like a hang
   (not timed). [STAGE5 open item 8; PERF "Hardware results"] Measure where the time goes
   (timestamps or profiler zones in the load path) before choosing a fix: a faster load,
   or at least a progress screen that doesn't look broken.
2. **Short hitches when entering areas not seen before.** Never measured. [STAGE5 item 7]
   Use `debug-perf` with `log 1`.
3. **Boot takes 41.8 s to the main menu** on the console. From the 2026-10-12 log:
   "preferences loaded" (4.3 s) → "sound initialized" (13.9 s) is **9.6 s**, and
   "screen initialized" (14.4 s) → `initialize_application()` done (33.7 s) is
   **19.4 s**. `Plugins::enumerate()` is no longer the cost it was in STAGE4's Azahar table.
4. **Level 0's longest frame is 107-120 ms** in every mode on the console (58 ms in the
   PERF session). The sound rate differed: the console's prefs say `rate="44100"`, PERF
   ran at 32728. Try 32728; with `log 1`, find when in the window it happens.
   [STAGE7 "Hardware results"]
5. HUD/Map counted about 0.5% fewer frames than Classic (30.3 vs 30.4-30.5 fps) with more
   idle time. A guess, not checked: `present_bottom()` waits for a VBlank of its own.
   Recheck with HUD + Map. [STAGE7]
6. **HOME > Close during the main menu's demo takes 14.4 s** (hardware only; 0.34 s from
   a game). Next step: timestamps inside `finish_game()`. [STAGE5 item 4]

**Controls and settings**
7. **The C-Stick's vertical look is too fast** relative to the horizontal; the player
   ends up looking at the sky. Wanted: a lower vertical than horizontal sensitivity.
   Touch-look tuning was left for the touch redesign, which HUD + Map now is.
   [STAGE5 item 2]
8. The console's prefs file still says `rate="44100"` (the default is the DSP's native
   32728 Hz). Ask the user whether to change it. [STAGE6 item 5]
9. Player name from the console profile (CFGU) instead of "Bob User". Cosmetic,
   deferred twice by the user. [STAGE3 stub 4; STAGE5 item 6]

**Screens**
10. Menus, chapter screens and dialogs are still the 640x480 logical screen halved into
    320x240 in the middle of the 400x240 top screen. [STAGE5 Known limitations;
    STAGE5-PREREQS triage "400x240 menu layouts"]

**Performance candidates** (lower priority: the console had 20-21 ms of idle per frame
in the old HUD mode; HUD + Map is not measured there yet)
[PERF "Next candidates"]
11. Classic's `present` (8.7 ms, the largest zone on the console). Low resolution
    presented directly instead of doubled and averaged back. The view rendered without
    the 640x480 surface. The rasteriser (`scottish_textures`; LTO exposed strict-aliasing
    UB there).

**Housekeeping**
12. Branch backup bundles in `~/Documents/Aleph_3ds-backups/` stop at `d19dae0` (stage 6).
    Refresh with `git bundle` and test a restore. [STAGE5-PREREQS item 8]
13. The devkitPro linker-script report is drafted, not filed
    (`docs/3ds/devkitpro-3dsx-ld-report.md`). Filing it is the user's call.
    [STAGE5-PREREQS item 9]

**Accepted gaps** (no action unless the user asks): hyperlinks do nothing
(`PATCHES.md` 2); no Opus/FLAC; Lua `io`/`os` gaps; SDL2_image not built; Old 3DS
untested (no hardware); one controller binding per action; no keyboard for the Lua
console. [STAGE3 "Stubs", STAGE6, ENVIRONMENT, STAGE5]

## Finish with
A short report: what was done per item, the key numbers, what is still open, and the
next command if one is needed. Keep this file's list in step: mark items done or moved.
