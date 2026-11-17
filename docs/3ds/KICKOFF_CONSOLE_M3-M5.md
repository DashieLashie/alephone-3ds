# Paste this into a fresh Claude Code session started in the stage10-c3d worktree

(`/Users/dashielashie/Desktop/Aleph_3ds/.claude/worktrees/stage10-c3d`, branch `stage10-c3d`. Written
2026-11-07 after M5 (tag `port-stage10-M5`). Earlier console prompt of the same kind: `KICKOFF_M2_CONSOLE.md`.)

---

We are on Stage 10 of the Aleph One 3DS port (the optional citro3d GPU renderer behind `A1_RENDER_C3D`).
**This session runs the three short console checks on my real New 3DS, in one sitting: `NEXT-SPRINT.md` "M3 console
check, short version", "M4 console check, short version" and "M5 console check, short version"** (about 60-90
minutes of my time, 10-11 launches). I am at the console when I say so; ask me for its IP first (it was
192.168.100.72). **Write no renderer code in this session, whatever the result:** a failure is recorded and
reported, and the fix is decided afterwards. I am short on time and on usage: use no subagents, run no Azahar
series, read only what the steps need. **Do not push.**

## Where the project is

- **M0-M5 are done in Azahar only** (tags `port-stage10-M0` … `-M5`). M3: the shade LUT on texture unit 1 and two TEV
  stages (lighting). M4: sprites and weapons as one quad per rectangle, the A8 mask, the tinted blend, **play mode**
  (game frames drawn by citro3d and read back). M5: landscapes (two texture parts, 2 MiB + 128 KiB), static (LFSR
  noise on unit 1), eviction hardening; every play frame is now citro3d. Results: `docs/3ds/PERF.md` "Stage 10 M3",
  "Stage 10 M4", "Stage 10 M5"; design `docs/3ds/CITRO3D_DESIGN.md` section 7 (each milestone's status) and section 10
  item 6 (the "M3:", "M4:", "M5:" console questions).
- **The console has passed M1's and M2's short checks** (2026-11-02 / 11-03: readback, rings, texture flush and VRAM
  promotion work on hardware; `PERF.md` "Stage 10 M1 — console, short check", "Stage 10 M2 — console, short check").
- **Why this session exists.** Azahar models neither PICA's exact arithmetic (TEV rounding, float24 texture
  coordinates, the constant-colour blend) nor the copy and GPU-wait costs. Three milestones have never run on the
  hardware, and M6 (direct mode, next) changes how frames reach the screen, so a hardware bug found after M6 would be
  much harder to place. The M5 runbook's **G2-b bench** also gives M6 a real readback-vs-software baseline (Azahar:
  busy +2.04 / +4.88 / +3.51 ms on L0 / L6 / L20; Azahar understates copies, so expect worse).
- **One build for all three runbooks:** `build-3ds-c3d-prof` at `port-stage10-M5`. Each runbook says "at its tag or
  later". M5's Azahar series re-ran M3's lit diff and M4's play and present checks on this binary with the same
  results (M3's lit gate is identical: walls 99.69 %, floors 99.87 %, `view_frame_words=1304`). Since M5, M4's
  `frame` class **includes landscapes**, so M4 step 1's frame figure can differ slightly from the M4-era Azahar
  number in that runbook (M5 Azahar L0 frame: 99.72 % vs M4's 99.70 %). Use `PERF.md` "Stage 10 M5" where it gives a
  newer reference.
- Git: never push to `origin`; the fork `alephone-3ds` only when I ask; **do not push**. Last backup bundle:
  `~/Documents/Aleph_3ds-backups/3ds-port+stage10-c3d+stage10-bb-2026-11-07-0078bfd5f-e656535cb-5abc48f50.bundle`
  (refresh it at the end).

## Read first, in this order (only these)

1. `CLAUDE.md`: rules 4, 5 and 8, "Testing on the console", "Stage 10".
2. `docs/3ds/NEXT-SPRINT.md`:
   - "How to drive the console" (near the top);
   - "M1 console check, short version": its **Needs** and **Mac setup** paragraphs (`$H/m1/run.sh` already exists
     and is reused as is);
   - **the three runbooks at the end of the file: "M3 console check, short version", "M4 …", "M5 …"**: your
     procedure, step by step, with the pass rules, Azahar's reference lines and how to read a failure.
3. `docs/3ds/PERF.md` "Stage 10 M5" (the newest Azahar references and G2-a's table); open "Stage 10 M3" / "M4" only
   when a runbook step points there.

## Before I press anything (the Mac side, no console needed)

- `export DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM`. Confirm the code equals the tag:
  `git diff --stat port-stage10-M5 HEAD -- ports scripts` prints nothing (later commits are docs only). Then
  `cmake --build build-3ds-c3d-prof` (expect "nothing to do" or a rebuild with 0 warnings).
- `H=/Users/dashielashie/Desktop/Aleph_3ds/build-3ds/hw` (the **main checkout**'s `build-3ds/hw`, outside git). Check
  that `$H/m1/run.sh`, `$H/sprint8/debug-perf-L0`, `-L6`, `-L20` and `$H/sprint8/setprefs.sh` exist. `run.sh`
  hard-codes the IP 192.168.100.72: if I give another one, change it there (and say so). `run.sh` writes transcripts
  to `$H/m1/<tag>.txt`.
- Kill stale 3dslink processes and old transcript monitors: `pgrep -fl '^/opt/devkitpro/tools/bin/3dslink'`.
- Check the files the runbooks put on the card exist in the worktree: `scripts/3ds/tests/c3d/` `m3-diff.cfg`,
  `m3-alpha2.cfg`, `m3-film-l08.cfg`, `m4-diff.cfg`, `m4-tinted.cfg`, `m4-play.cfg`, `m4-bench-sw.cfg`, `m5-diff.cfg`,
  `m5-lookup.cfg`, `m5-present-diff-l0.cfg`, `m5-bench-c3d.cfg`, `m5-bench-sw.cfg`; the plugin folders
  `scripts/3ds/tests/plugins/Invisibility Test` and `Look Up Test`.
- **Preferences.** The runbooks want **HUD + Map**; the M5 bench (step 4) wants **Framerate Target 30** to compare with
  Azahar. My card's preferences were fps_target 60 + HUD + Map last time. With ftpd, first fetch my
  `Marathon 2 Preferences` into `$H/m5c/prefs-user` (`mkdir -p $H/m5c`) to put back at the end, then
  `$H/sprint8/setprefs.sh IP 0 30` (HUD + Map, 30). Find the file's card path with `scripts/3ds/sd-ftp.sh IP ls
  /3ds/alephone` (as the B-a runbook did).
- A 3dslink launch overwrites `/3ds/alephone.3dsx` on the card.

## The runs, in priority order (I do the button presses; I may stop early)

If time runs short, stop after any block; each block is recorded on its own.

1. **M5 runbook, steps 1-4** (~25-35 min): landscapes on L6; the Look Up Test on L0 (plugin folder into the
   scenario's `Plugins/`); static + present diff on L0 (`m5-present-diff-l0.cfg`); the G2-b bench (6 launches:
   c3d and sw on L0, L6, L20).
2. **M4 runbook, step 4** (~5 min): HOME, sleep (lid) and HOME > Close in play mode: the first hardware test of play
   frames in flight across suspend.
3. **M4 runbook, steps 1-3** (~15 min): sprites/weapons diff on L0; tinted (Invisibility Test plugin); play mode on
   L20 with play checks. Step 3's software-renderer rerun can be skipped: the M5 bench already gives L20's numbers.
4. **M3 runbook, steps 1-2** (~10 min): lit diff on L0; the ×2 clamp (`m3-alpha2.cfg`). Step 3 (the L08 film) only if
   I want to go on.

Driving reminders: **press A at once at the menu** (the demo otherwise eats the diff frames); the ftpd app and the
netloader cannot run at once (close ftpd, then Y in the Homebrew Launcher before each launch); "Connection reset by
peer" mid-run means the lid closed or Wi-Fi slept, the log continues on the card (fetch `/3ds/alephone/log.txt`
with `scripts/3ds/sd-ftp.sh IP get`); on L0, compare checks **at equal coverage** (L0's walls coverage cycles on
the console). Between launches, change only the card files the next step names.

**Reading results.** Each runbook has its pass rules and "Reading it" notes; follow them. General rules: command
words (`[c3d] cmdbuf final`: `view_frame_words`, `draw_words`, `readback_words`) are deterministic, so any difference
from Azahar is real and must be reported. The console excludes more edge-tie pixels than Azahar (`tie_mismatch`),
which moves edge counts and can nudge colour figures slightly: that is expected, not a failure. A Luma dump: fetch
`/luma/dumps/arm11/*.dmp`, resolve with `arm-none-eabi-addr2line -e build-3ds-c3d-prof/alephone.elf` (registers at
offset 0x28), record and move to the next block only if the failure is understood. The known open hardware
questions are listed per milestone in design section 10 item 6: say for each one whether this session verified,
refuted or did not reach it.

## Afterwards

- **Card cleanup:** ftpd deletes `/3ds/alephone/debug-perf`, `/3ds/alephone/debug-c3d`, `/3ds/alephone/c3d-film.filA`
  if used, and both plugin folders; put my `Marathon 2 Preferences` back from `$H/m5c/prefs-user`.
- **Record** (rule 4, in the style of `PERF.md` "Stage 10 M2 — console, short check"): new `PERF.md` sections "Stage
  10 M3 — console, short check", "Stage 10 M4 — console, short check", "Stage 10 M5 — console, short check" (only for
  the blocks run): per launch the `c3d-diff.py` tables, the `[c3d] cmdbuf final`, `[c3d] play final` and `tex:` lines,
  the BENCH lines, pass/fail, and **G2-b**: busy(readback) − busy(SW) per level beside Azahar's. Then each runbook's
  head in `NEXT-SPRINT.md` ("**Run on the console 2026-11-.., passed / failed …**") and plan C's status paragraph;
  `CITRO3D_DESIGN.md` each milestone's status line, section 10 item 6 (verified / refuted per question) and a Review
  log entry; `CLAUDE.md` "State of the project" (one sentence). Raw transcripts stay in `$H/m1/` (never in git).
- **Commit** `stage10: docs: M3-M5 console checks (<one-line result>)`, then refresh the backup bundle (recipe
  `STAGE5-PREREQS.md` section 8; the command as in `KICKOFF_M1.md` with the tags up to `port-stage10-M5`, then
  `git bundle verify` and the restore test) and update the project memory's stage 10 entry.
- **Report and stop:** per block, pass/fail with the key figures beside Azahar's; G2-b's numbers; anything that
  differed and what it points to; which design section 10 questions are now answered. Then my choice for what comes
  next: M6 (`KICKOFF_M6.md`; it asks me two things first: merge `stage10-bb` or not, and start Docker Desktop), or a
  fix first if something failed. Do not push.
