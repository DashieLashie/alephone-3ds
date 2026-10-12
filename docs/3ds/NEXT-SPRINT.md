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

## B. Backlog: open items from earlier sessions

**Triage** (for the user; "console" = needs a console session):

| # | Item | Status | Cost left | Console? | Recommendation |
| --- | --- | --- | --- | --- | --- |
| 1 | Level loads 76-97 s, look frozen; slow saves | **Fix done** (Azahar 31 s → 3-4 s), marks in place | S: measure | yes | Measure first thing in stage 2; then decide on a progress screen and the save preview |
| 2 | Hitches entering new areas | Stage 2 | S | yes | Measure with `log 1` during the B1 runs |
| 3 | Boot 41.8 s | Marks in place | S-M | yes | Measure; fix only what dominates |
| 4 | Level 0's max frame 107-120 ms | Stage 2 | S | yes | With item 8: one run at 32728 Hz |
| 5 | HUD/Map fewer frames than Classic | **Done**: a profiler artefact | none | no | Closed |
| 6 | HOME > Close in the demo 14.4 s | Marks in place | S | yes | One run in stage 2 |
| 7 | C-Stick vertical look too fast | **Done** (pitch at 0.5×) | S: feel | yes | Try in stage 2; tune with the vertical sensitivity / dead zone |
| 8 | Prefs `rate="44100"` | **Decided: change to 32728** (user, 2026-10-12) | XS | ftpd only | First step of stage 2: `sprint8/setrate.sh IP` |
| 9 | Player name from CFGU | Deferred (twice) | S | no | Skip unless asked |
| 10 | 400x240 menu layouts | Deferred | L | yes | Its own stage |
| 11 | Perf candidates | Deferred | M-L each | yes | HUD + Map leaves idle headroom |
| 12 | Backup bundle | **Done** | none | no | Closed |
| 13 | devkitPro LD report | **Deferred by the user** (2026-10-12) | XS | for the repro | Kept as a note; see item 13 |

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
2. **Short hitches when entering areas not seen before.** Never measured. [STAGE5 item 7]
   **Stage 2:** the `debug-perf-L*` files in `sprint8/` have `log 1`; play into new
   areas and read `max` per window.
3. **Boot takes 41.8 s to the main menu** on the console ("preferences loaded" →
   "sound initialized" 9.6 s; "screen initialized" → `initialize_application()` done
   19.4 s). **Stage 1:** both stretches are split into marked steps (`PERF.md` "Load
   times", Azahar table; "environment (scenario files)" 4.1 s and sound 2.7 s lead
   there). **Stage 2:** read the console's split, then decide.
4. **Level 0's longest frame is 107-120 ms** in every mode on the console (58 ms in the
   PERF session). The sound rate differed: the console's prefs say `rate="44100"`, PERF
   ran at 32728. **Stage 2:** try 32728 (item 8); with `log 1`, find when in the window
   it happens. [STAGE7 "Hardware results"]
5. ~~HUD/Map counted about 0.5% fewer frames than Classic.~~ **Done, no code change**:
   the profiler leaves its overlay's time out of the wall clock and only Classic draws
   the overlay in game; frame + overlay is 32.98-32.99 ms in every mode, the engine's
   30.30 fps (`1000 / 30` = 33 ms timer). `PERF.md` "The profiler", `STAGE7.md`.
6. **HOME > Close during the main menu's demo takes 14.4 s** (hardware only; 0.34 s from
   a game). **Stage 1:** every step of `finish_game()` is marked. **Stage 2:** one
   HOME > Close during the demo; read the marks. [STAGE5 item 4]

**Controls and settings**
7. **The C-Stick's vertical look is too fast.** **Done in stage 1** (`3ba6e23`,
   `PATCHES.md` 19): pitch at half the yaw's speed; full deflection from level to the
   ±30° limit in 0.23 s instead of 0.12 s (logged with script 16). **Stage 2:** the
   user tries it; the Controller Advanced vertical sensitivity and dead zone are the
   next knobs. Touch-look tuning: with HUD + Map, by feel. [STAGE5 item 2]
8. The console's prefs file still says `rate="44100"` (the default is the DSP's native
   32728 Hz). **Decided by the user (2026-10-12): change it to 32728.** Not applied
   yet: the console was not on ftpd. **Stage 2, first step:** with ftpd running,
   `build-3ds/hw/sprint8/setrate.sh IP` (sets only `<sound rate=...>`, keeps the rest,
   saves the original in `sprint8/prefs-rate/original`). Then the benchmarks run at
   32728, which also tests whether item 4's long frame goes away. [STAGE6 item 5]
9. Player name from the console profile (CFGU) instead of "Bob User". Cosmetic,
   deferred twice by the user. **Deferred.** [STAGE3 stub 4; STAGE5 item 6]

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
