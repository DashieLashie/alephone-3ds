# Console session A6: verify plan A on the New 3DS

Handoff for a fresh agent, written 2026-10-24. Plan A (stage 10's fixes before the
renderer) is built and critic-approved, but tested only in Azahar. Azahar never takes GPU
rights away and retires GPU work at once, so the bugs these fixes target cannot show
there. This session decides whether they hold on hardware. It needs the user and the
console the whole time.

## Kickoff prompt (paste into a fresh session in `/Users/dashielashie/Desktop/Aleph_3ds`)

> Run the console session in `docs/3ds/CONSOLE-SESSION-A6.md` on branch `stage10-c3d`
> (worktree `.claude/worktrees/stage10-c3d`). Read `CLAUDE.md` first, then that file. I have
> the console ready; ask me for its IP.

## Read first (in this order)

1. `CLAUDE.md`: the rules, the console traps ("Testing on the console"), the git rules.
2. This file: it is the procedure and the order.
3. For the expected log lines, in `docs/3ds/NEXT-SPRINT.md` on `stage10-c3d`, plan A items
   4-6 (the end of the file) and backlog items 14, 15 and 16. **Plan A item 5's run list is
   authoritative for runs 1-4.** This file adds the order, the setup and the bookkeeping.
4. For background only, if a result is surprising: `docs/3ds/CITRO3D_DESIGN.md` 5.6.1
   (HOME > Close), 5.6.3 (the fatal hand-off) and 5.6.4 (the claims this session verifies).

## Progress: session 2 (2026-10-25), stopped after B5 and the start of B6

Console (New 3DS), `build-3ds-c3d`, 192.168.100.72, HUD + Map @60, every run started from the
Homebrew Launcher, logs read from the card after each run (raw logs in
`/Users/dashielashie/Desktop/Aleph_3ds/build-3ds/hw/a6/B1.log` .. `B5.log`, outside git). Not yet
written into `NEXT-SPRINT.md` / `CITRO3D_DESIGN.md` 5.6.4: do that in the final recording step.

| Run | Result |
| --- | --- |
| B1, mode 1 | PASS. Bottom screen "Aleph One stopped / debug-fatal mode 1: vhalt() on the main thread", not Luma's. Log: `[c3d] gfxExit call #1 (citro3d up: release first)`, release (queue retired after 0 ms, `C3D_Fini` returned), `FATAL:`, `ERROR: Aleph One stopped`, `error screen d6: thread main`, `gfxExit call #2 (citro3d down)`, `exit`. The relaunch hit mode 1 again (flag still set): launcher round trip works. |
| B2, mode 2 | PASS. Screen "abort() was called on thread 1781 ..."; log: `fatal error on thread 1781: handed to the main thread`, then the same release and error screen on main, `exit` (+103 s: the user read it). |
| B3, mode 3 | PASS. vhalt() on thread 1903, handed to main, release (0 ms), error screen, `exit`. |
| B4, mode 4 | PASS. `error_screen() on a second thread` (thread 2025) handed to main; **closes the critic's open P1 minor 4 on hardware** (a non-main thread never touches citro3d). |
| B5, mode 5 | PASS. In a HUD + Map game (681 presents): thread 2147 handed to main, release 0 ms, one screen, text stayed, launcher back. |

No new Luma dump in any run (still dumps 0-6). Each release logged `queue retired after 0 ms`
(55-270 ms since the last frame end), `final: ... gate_skipped=0 ... gx_refused=0`.

**State of the card when we stopped (clean it up first thing next time):** `debug-fatal` = mode 6
is on the card; `/3ds/alephone.3dsx` is the GPU build (`build-3ds-c3d`); prefs were set to
`bottom_screen=0 fps_target=60` (the user's prefs, same as before). Backups of the user's
original: `build-3ds/hw/a6/b/alephone-user-before.3dsx` (identical to `alephone-before.3dsx`) and
`b/prefs-before2`. B6 mode 6 was installed but **not run**.

**Left, in order:** B6 (mode 6, then 6b: the DSP must fall silent after the exit and after the
relaunch), B7 (modes 10, 11), shipping-build spot check (`build-3ds/alephone.3dsx`, repeat B1 and
B2), part C (netloader, `kill -STOP` 3dslink ~10 s; ftpd must not run), B8 and part D only if the
user says yes, then Cleanup and Recording (docs, `stage10: A6 console session results (part
B/C/D)` on `stage10-c3d`, identical item 14/15/16 text on `3ds-port`, backup bundle,
memory file).

## Session 3 (2026-10-26): finished; results are in `NEXT-SPRINT.md` items 5, 14-16

B6 (mode 6) and B7 mode 11 pass, the shipping-build spot check (B1, B2) passes. **B6b and B7
mode 10 fail on the console:** the fallback's `svcExitProcess` (citro3d up, nothing drawn) ends
in the system's "software error, the system will now restart" message and a reboot, not in the
launcher. Part C was inconclusive (10 s and 150 s freezes did not fill the socket buffers).
Mode 7 on the shipping build, run afterwards, restarts the console too (not citro3d-specific).
B8 on the GPU build and part D were not run (the user's choice). Notes for a future run: a 3dslink launch
overwrites `/3ds/alephone.3dsx` on the card; the log rate in game is too low for the
`kill -STOP` check, use a chattier flag; the card was cleaned up at the end (shipping build,
the user's own prefs, no `debug-*`).

## State at handoff

| Branch | Head | Contents |
| --- | --- | --- |
| `3ds-port` (main checkout) | `29fa83b` | pieces 0-3: SDL2 HID patch, non-blocking 3dslink mirror, error screen, fatal hand-off (`PATCHES.md` 54), `exit_requested()` reads `aptShouldClose()` |
| `stage10-c3d` (worktree) | this file's commit | the same merged in (piece 4), plus piece 5: present gate, bounded GPU waits, abandon, d2-d5 |

Nothing is pushed to the fork, and pushing is not part of this session. The card is
clean: no `debug-*` files. The user's preferences are HUD + Map @60.

## What "pass" means everywhere

After every run that ends the app (a Close, an error screen and A, a quit), the
**Homebrew Launcher comes back** and **Aleph One launches again from it**. Look for:
- **A failed exit:** a forced power-off, a black screen that stays, "Closing software..."
  that never ends, or Luma's "An exception occurred" screen. If one happens, ask the user to
  power-cycle, fetch `/luma/dumps/arm11/*.dmp` and `log.txt`, and record it. Do not retry
  blindly.
- **Missing logs:** raw fatal lines and the d1-d5 lines are only in `sdmc:/3ds/alephone/log.txt`.
  A launcher start has no 3dslink mirror, and the raw lines are never mirrored. Each boot
  moves the previous log to `log.prev.txt`, so fetch the log after each run, before the
  next launch.

## Setup

```sh
cd /Users/dashielashie/Desktop/Aleph_3ds/.claude/worktrees/stage10-c3d
export DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM
for d in build-3ds build-3ds-c3d; do cmake --build $d; done          # expect: nothing to do / 0 warnings
/opt/devkitpro/devkitARM/bin/arm-none-eabi-nm build-3ds/alephone.elf | grep -ciE 'citro|C3D_|a1_c3d'   # 0
H=/Users/dashielashie/Desktop/Aleph_3ds/build-3ds/hw/a6; mkdir -p $H     # results (ignored by git)
FTP=/Users/dashielashie/Desktop/Aleph_3ds/scripts/3ds/sd-ftp.sh
PREFS=/Users/dashielashie/Desktop/Aleph_3ds/build-3ds/hw/sprint8/setprefs.sh   # setprefs.sh IP MODE FPS
```

- **IP:** ask the user for the console's IP (it was 192.168.100.72). They run **ftpd** for
  card access. Runs start from the **Homebrew Launcher**, and the two can't run at once:
  tell the user each time which one you need.
- **Install** the GPU build as the launcher's copy:
  `$FTP IP put build-3ds-c3d/alephone.3dsx /3ds/`. That is `sdmc:/3ds/alephone.3dsx`,
  as in `STAGE4.md`:273, and it replaces the user's copy until the end of the session.
- **Back up the preferences** before changing them:
  `$FTP IP get "/3ds/alephone/Marathon 2 Preferences" $H/prefs-before`.
- **Set the mode with** `$PREFS IP MODE FPS`: `0 60` is HUD + Map @60, `2 30` is Classic @30.
- **Swap a flag file** (`debug-c3d` or `debug-fatal`) in three steps:
  1. `$FTP IP put <file> /3ds/alephone/`
  2. `$FTP IP rm /3ds/alephone/debug-xxx`. Ignore the error if the file is absent. A rename
     onto an existing file may fail on FAT and silently keep the old one.
  3. `$FTP IP mv /3ds/alephone/<file> /3ds/alephone/debug-xxx`
- **After every run:**
  - `$FTP IP get /3ds/alephone/log.txt $H/<run>.log`.
  - Check the boot line at the top: `[c3d] debug-c3d: present ..., suspend_gate X,
    retire_wait_ms Y ...` (or `[3ds] debug-fatal: mode N ...`). It must match the run. If it
    doesn't, the run doesn't count.
- **Name the logs** `<part>-<run>.log`, for example `A1-diag.log` or `B-mode2.log`.

## Order

Run the most diagnostic and least destructive runs first. Power Off goes last, because it
ends the session.

### Part A: HOME > Close with the GPU present (plan A item 5, design 5.6.1 runs 1-4)

| # | Config (`debug-c3d`) | Prefs | What the user does | Pass (details in plan A item 5) |
| --- | --- | --- | --- | --- |
| A1 | `scripts/3ds/tests/c3d/p1-home-close-diag.cfg` | `2 30` | start a game, HOME, Close | d1 `ONSUSPEND ... gpu right 1`, no hook after it. Then **either** ordering from item 5: `release abandoned (release)` after the `close_frame` breadcrumbs, **or** `release abandoned (frame_begin)`. Then `real gfxExit returned` and a normal close. `queue retired after X ms` with a large X: record X ("slow, not never"). A hang: the last `d3`/`release:` line names the wait; stop and report. |
| A2 | `p1-gpu.cfg` | `0 60`, `0 60`, `2 30` | HOME > Close in a game, three times | `gate closed (gpu_right 0, should_close 1, ...)`, `queue retired after 0 ms`, `release done`, `real gfxExit returned`, `exit`; launcher and relaunch each time |
| A3 | `p1-diff.cfg` | `0 60` | in a game: HOME and back ×2, lid sleep and wake ×2, the software keyboard (naming a save) confirmed and cancelled, POWER then **cancel** | right picture, `present_diff` 0 differences, `gate_skipped=0`, each d1 hook on main with gpu right 1, presents resume |
| A4 | `p1-gpu.cfg` | `0 60` | HOME > Close inside a dialog; HOME > Close at the main menu | unchanged from before, no new Luma dumps |

- **A `release abandoned` line in A2-A4 is a bug** (`retire_wait_ms` 500 hit in a normal
  release). Record it with the full log.
- **After part A,** remove `debug-c3d`. The default is the GPU present.

### Part B: fatal errors (backlog item 14's console list; design 5.6.3)

Each mode uses `debug-fatal` = `scripts/3ds/tests/fatal/mode-<N>.cfg`, with no
`debug-c3d`, so the GPU present is up as players would have it. The error fires about
20 s after the first present (at the main menu). Mode 5 fires in a game: start one.
Every screen is left with **one A**.

| # | Mode | Pass |
| --- | --- | --- |
| B0 | none | **Boot-time control:** rename `/3ds/alephone/Marathon2` to `Marathon2-x` (`$FTP IP mv`), launch. The port's error screen appears, A, launcher. Rename it back. |
| B1 | 1 | By eye: the bottom screen reads "Aleph One stopped" / "debug-fatal mode 1: vhalt() on the main thread", not Luma's exception screen. The log has `[c3d] release` before the error screen. |
| B2-B4 | 2, 3, 4 | The port's screen (the G0-b crashes: dumps 3, 4, 6), A, launcher, relaunch. |
| B5 | 5 | In a HUD + Map game: one screen, the text stays. |
| B6 | 6, 6b | Same as B2-B4, and **the DSP falls silent.** Listen after the exit and after relaunching. These end in `_Exit`/`svcExitProcess` without `ndspExit`. |
| B7 | 10, 11 | Real scheduling. Mode 10: `the request was taken back ... blocking` and the fallback line. Mode 11: the log shows the audio thread parked before A is pressed. |
| B8 | 7 | **Only if the user agrees:** the fallback ends in `svcExitProcess`, which skips `aptExit` (design 5.6.4 item 5). Same checks, including silence. With the GPU present up, expect `error screen: citro3d still up (fallback off main): logged only`: the top screen stays frozen and nothing is drawn. That is by design. |

- **Spot check of the shipping build:** install `build-3ds/alephone.3dsx` (software
  renderer) instead and repeat B1 and B2.
- **After part B,** remove `debug-fatal`.

### Part C: the 3dslink mirror (backlog item 15)

1. Launch `build-3ds-c3d/alephone.3dsx` over 3dslink with
   `/Users/dashielashie/Desktop/Aleph_3ds/build-3ds/hw/sprint8/launch.sh IP <3dsx> $H/C-mirror.txt`
   in the background, after killing stale 3dslink processes
   (`pgrep -fl '^/opt/devkitpro/tools/bin/3dslink'`). The user presses Y in the launcher.
2. In a game, `kill -STOP` the host 3dslink process for about 10 s, then `kill -CONT` it.
3. Pass:
   - the game keeps running;
   - the card's `log.txt` has `3dslink mirror stopped (stalled); the SD log goes on`;
   - the transcript stops at that point.

### Part D: last, Power Off in a game (piece 3, the bit-0x200 route)

With `p1-gpu.cfg` and `0 60`, in a game, hold POWER and choose Power Off. Pass:
- the console powers off normally;
- after the user powers it back on, `log.prev.txt` or `log.txt` shows the fast path, with
  no quit dialog (`terminals: 640x320`) after the d1 line.

Skip this part if the user would rather not power the console off.

## Cleanup (always, even after a failure)

1. Remove `debug-c3d` and `debug-fatal`.
2. Restore the preferences: `$FTP IP put $H/prefs-before ...`. Rename it to
   `Marathon 2 Preferences` the same rm-then-mv way, or simply run `$PREFS IP 0 60`.
3. Rename `Marathon2` back if B0 left it renamed.
4. Ask the user which `.3dsx` they want on the card. The default is the shipping
   `3ds-port` build, `/Users/dashielashie/Desktop/Aleph_3ds/build-3ds/alephone.3dsx`.
5. Kill leftover 3dslink processes.

## Recording (no console needed; after the session)

All recording is Azahar-style prose labelled "Console (New 3DS), <date>", in the existing
style. On `stage10-c3d`:
- `docs/3ds/NEXT-SPRINT.md`: plan A item 5 results, item 6 marked done, items 14, 15 and
  16 console results;
- `docs/3ds/CITRO3D_DESIGN.md`: the 5.6.4 status lines, each claim now VERIFIED or
  REFUTED;
- `docs/3ds/PERF.md`: a "Console plan A" section only if timings were read, such as
  `queue retired after X ms`;
- raw logs and dumps stay in `build-3ds/hw/a6/`, outside git.

Commit as `stage10: A6 console session results`. Items 14-16 also exist on `3ds-port`: put
the **identical** result text there in a `3DS port: docs: ...` commit, so the next merge is
clean. Then refresh the backup bundle (`STAGE5-PREREQS.md` 8, as the last one,
`~/Documents/Aleph_3ds-backups/3ds-port+stage10-c3d-<date>-<p>-<s>.bundle`, restore-tested)
and update the memory file `stage10-paused-at-g0.md`.

## If something fails

- **Code changes are not part of this session.** Record the failure, its log lines and any
  dump (registers at offset 0x28; resolve with `arm-none-eabi-addr2line -e
  build-3ds-c3d/alephone.elf`), and write the fix up as a next step.
- **Fixes go through the agents afterwards:** `c3d-implementer`, then `c3d-critic` at
  **medium effort** (the user's standing preference), then Azahar, then a short console
  re-check.
- **Plan B** (zero-copy present) and the renderer wait until plan A passes here. Plan B
  then needs the user's go.
