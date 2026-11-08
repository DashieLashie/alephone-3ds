# Paste this into a fresh Claude Code session started in the stage10-c3d worktree

(`/Users/dashielashie/Desktop/Aleph_3ds/.claude/worktrees/stage10-c3d`, branch `stage10-c3d`. Written
2026-11-07 after M6 (tag `port-stage10-M6`). Earlier console prompt of the same kind: `KICKOFF_CONSOLE_M3-M5.md`.)

---

We are on Stage 10 of the Aleph One 3DS port (the optional citro3d GPU renderer behind `A1_RENDER_C3D`).
**This session runs `NEXT-SPRINT.md` "M6 console check, short version" on my real New 3DS, in its medium variant**
(my decision: steps 1-3, and step 4's bench at Framerate Target 60 only; **15 launches, about 40 minutes of my time**).
I am at the console when I say so; ask me for its IP first (it was 192.168.100.72). **Write no renderer code in this
session, whatever the result:** a failure is recorded and reported, and the fix is decided afterwards. I am short on time
and on usage: use no subagents, run no Azahar series, read only what the steps need. **Do not push.**

## Where the project is

- **M0-M6 are done in Azahar only** (tags `port-stage10-M0` … `-M6`). M6 is **direct mode**: an eligible HUD + Map game
  frame is drawn by citro3d into a 512x256 texture and never read back; the CPU overlays (crosshair, FPS, position,
  messages) go to a separate RGBA8888 surface, and the present composites the two in one GPU frame. Frames that cannot be
  direct (map, terminal, pause darkening, fades and damage flashes, Classic) are drawn by readback as in M5. The engine
  edit is `PATCHES.md` 47 (`screen.cpp` hooks 3a/3b/3c: on a direct frame the bottom screen is drawn *before* the present,
  so its CPU work hides the GPU's view frame). `stage10-bb` (the zero-copy present, B-b) was merged before M6.
- **Azahar results** (`docs/3ds/PERF.md` "Stage 10 M6"): the in-frame `direct_check` is exact (100.000 %, Δ 0 on the host
  GPU), the frame-indexed test passes @30 and @60, and **the first speedup of stage 10**: @60 direct holds 59.66-59.71 fps
  on L0 / L6 / L20 (software renderer 54.37 / 59.62 / 59.53, M5's readback 50.26-52.07); @30 busy direct − SW −6.04 /
  −4.20 / −5.01 ms.
- **Why this session exists.** Azahar cannot answer M6's hardware questions (design `CITRO3D_DESIGN.md` section 10, item
  13's "M6:" paragraph): whether PICA reads the texture target the way Azahar does (the two orientation choices
  `view_v_flip 1` and `band_from_end 1`, and the seed transfer), PICA's blend rounding, **the real GPU wait** (Azahar runs
  the GPU inside `C3D_FrameEnd`, so its `c3d_wait` is 0.01 ms and `gpu_ms view` 0.08), two C3D frames in flight across
  HOME / sleep / Close, the bottom screen's VBlank wait now landing before the composite (`bottom_pending`), and **the
  stage 10 goal itself** (design 0.5): *HUD + Map at Framerate Target 60 sustains ≥ 55 fps on L0/L6/L20 on a New 3DS.*
- **One build:** `build-3ds-c3d-prof` at `port-stage10-M6`. Commits after the tag are docs only.
- **Recorded decisions you must not re-open:** the capture gate's exception (design Review log "M6 acceptance, capture
  gate") and keeping the texture-cache hash (Review log "M6, the texture-cache hash, user decision").
- Git: never push to `origin`; the fork `alephone-3ds` only when I ask; **do not push**. Last backup bundle:
  `~/Documents/Aleph_3ds-backups/3ds-port+stage10-c3d+stage10-bb-2026-11-07-0078bfd5f-351ea17c2-5abc48f50.bundle`
  (refresh it at the end).

## Read first, in this order (only these)

1. `CLAUDE.md`: rules 4, 5 and 8, "Testing on the console", "Stage 10". Note: **macOS has no `timeout` command** here;
   never wrap commands in it.
2. `docs/3ds/NEXT-SPRINT.md`:
   - "How to drive the console" (near the top);
   - "M1 console check, short version": its **Needs** and **Mac setup** paragraphs (`$H/m1/run.sh` exists and is reused
     as is);
   - **"M6 console check, short version" (the last section of the file): your procedure**, with the pass rules, Azahar's
     reference values and how to read a failure.
3. `docs/3ds/PERF.md` "Stage 10 M6" (the Azahar references and tables) and "Stutter follow-up" (why the bench runs with
   `quiet 1`).

## Before I press anything (the Mac side, no console needed)

- `export DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM`. Confirm the code equals the tag:
  `git diff --stat port-stage10-M6 HEAD -- ports Source_Files scripts` prints nothing. Then
  `cmake --build build-3ds-c3d-prof` (expect "nothing to do" or a rebuild with 0 warnings).
- `H=/Users/dashielashie/Desktop/Aleph_3ds/build-3ds/hw` (the **main checkout**'s `build-3ds/hw`, outside git; write this
  session's files under `$H/m6c/`). Check that `$H/m1/run.sh`, `$H/sprint8/debug-perf-L0`, `-L6`, `-L20` and
  `$H/sprint8/setprefs.sh` exist. `run.sh` hard-codes the IP 192.168.100.72 (in its `launch.sh` line): if I give another
  one, change it there and say so. `run.sh` writes transcripts to `$H/m1/<tag>.txt`.
- Kill stale 3dslink processes and old transcript monitors: `pgrep -fl '^/opt/devkitpro/tools/bin/3dslink'`.
- The card files the runbook uses, in `scripts/3ds/tests/c3d/`: `m6-cal.cfg`, `m6-direct.cfg`, `m6-sync.cfg`,
  `m6-ineligible.cfg`, `m6-bench-direct.cfg`, `m5-bench-c3d.cfg`, `m5-bench-sw.cfg`. **Make the bench's quiet copies**
  (not in git): `mkdir -p $H/m6c/cfg` and, for each of the three bench cfgs, a copy in `$H/m6c/cfg/` with a last line
  `quiet 1`. The G3-b run uses `m6-bench-direct.cfg` **without** `quiet 1`.
- The analysis tools: `scripts/3ds/tests/c3d-m6cal.py DIR` (the frame-indexed captures, `.bmp` or `.png`) and
  `scripts/3ds/tests/perf-table.py` (BENCH tables; it knows M6's `c3d_overlay` / `c3d_composite` zones). Run Python with
  `-I`.
- **Preferences.** The runbook wants **HUD + Map**; steps 1 (first half), 2 and 3 run at **Framerate Target 30**, step 1's
  second half and step 4 at **60**. With ftpd, first fetch my `Marathon 2 Preferences` into `$H/m6c/prefs-user` to put
  back at the end (find its card path with `scripts/3ds/sd-ftp.sh IP ls /3ds/alephone`), then
  `$H/sprint8/setprefs.sh IP 0 30` (HUD + Map, 30); switch with `setprefs.sh IP 0 60` before the @60 runs.
- A 3dslink launch overwrites `/3ds/alephone.3dsx` on the card.

## The runs, in priority order (I do the button presses; I may stop early)

If time runs short, stop after any block; each block is recorded on its own. **Steps 1 and 2 first:** if the picture is
wrong on PICA, the bench is meaningless.

1. **Step 1, the frame-indexed test and orientation** (2 launches, ~6 min): `m6-cal.cfg` on L0 @30, fetch
   `/3ds/alephone/screen-c3d-cal-*.bmp` into `$H/m6c/cal30/` and delete them from the card (`sd-ftp.sh` has `rm` but no
   `rmdir`), run `c3d-m6cal.py`; then @60 into `$H/m6c/cal60/`.
2. **Step 2, in-frame equivalence, seed and sync** (2 launches, ~6 min, @30): `m6-direct.cfg` on L0, `m6-sync.cfg` on L6.
3. **Step 3, HOME / lid / map / HOME > Close** (1 launch, ~6 min, @30; no `debug-perf`, `m6-ineligible.cfg`): I play by
   hand; you watch the transcript and then read the card's `log.txt`.
4. **Step 4, the @60 bench** (10 launches, ~25 min): `debug-perf-L<N>` × the three quiet cfgs on L0, L6, L20, then one
   `m6-bench-direct.cfg` run on L0 without `quiet 1` for G3-b's `gpu_ms … view=`.

Driving reminders: **press A at once at the menu** (the main menu's demo starts ~13 s after the menu and would replace the
level); the ftpd app and the netloader cannot run at once (close ftpd, then Y in the Homebrew Launcher before each launch);
"Connection reset by peer" mid-run means the lid closed or Wi-Fi slept, the log continues on the card (fetch
`/3ds/alephone/log.txt` with `scripts/3ds/sd-ftp.sh IP get`; each boot moves the previous one to `log.prev.txt`). Between
launches change only the card files the next step names. Tell me before each launch which files you put on the card and
what I should do.

**Reading results.** The runbook has each step's pass rule and "Reading it" notes; follow them. In short:
- Step 1 decides the orientations: a band at another row → `band_from_end` is wrong; swapped corners or magenta on screen
  → `view_v_flip` is wrong. Record and **stop** if either is wrong (the fix is a one-line config default, decided after
  the session).
- Step 2: a uniform Δ of 1-2 in `direct_check` is PICA's blend rounding (record it; the gate is 8.3's 99.9 % within Δ 1);
  a mirrored or shifted picture points back to step 1.
- Step 4: the goal is **≥ 55 fps @60 on all three spots in direct mode**; also report busy, `c3d_wait` (the GPU time the
  bottom screen did not hide), `c3d_composite`, `c3d_vblank`, `bottom`, the direct share (`direct final`: `direct=` must
  be ≥ 95 % of `frames=`, else the run is not valid) and `bottom_pending`. Make the tables with
  `python3 -I scripts/3ds/tests/perf-table.py` on files holding the BENCH lines (one file per config, levels inside).
- A Luma dump: fetch `/luma/dumps/arm11/*.dmp`, resolve with `arm-none-eabi-addr2line -e build-3ds-c3d-prof/alephone.elf`
  (registers at offset 0x28), record, and go on only if the failure is understood.
- For each question in design section 10 item 13's "M6:" paragraph, say whether this session verified, refuted or did not
  reach it.

## Afterwards

- **Card cleanup:** ftpd deletes `/3ds/alephone/debug-perf`, `/3ds/alephone/debug-c3d` and any `screen-c3d-cal-*.bmp`;
  put my `Marathon 2 Preferences` back from `$H/m6c/prefs-user` (my usual settings: Framerate Target 60, HUD + Map).
- **Record** (rule 4, in the style of `PERF.md` "Stage 10 M3-M5 — console, short checks"): a new `PERF.md` section "Stage
  10 M6 — console, short check" with per launch the key lines (`c3d-m6cal.py` summary, `direct_check` worst line,
  `composite final`, `direct final`, `seed_check`, the step 3 observations), the @60 BENCH table for the three configs
  beside Azahar's, the goal's verdict (≥ 55 fps per spot), G3-b's `gpu_ms view`. Then the runbook's head in
  `NEXT-SPRINT.md` ("**Run on the console 2026-11-.., …**") and plan C's status paragraph; `CITRO3D_DESIGN.md` M6's
  status line, section 10 item 13's "M6:" paragraph (verified / refuted per question) and a Review log entry; `CLAUDE.md`
  "State of the project" (one sentence). Raw transcripts stay in `$H/m1/` and `$H/m6c/` (never in git).
- **Commit** `stage10: docs: M6 console check (<one-line result>)`, then refresh the backup bundle (recipe
  `STAGE5-PREREQS.md` section 8: `git bundle create <file> ^fc4c274 3ds-port stage10-c3d stage10-bb $(git tag -l
  'port-stage10-*')`, `git bundle verify`, and the restore test: a scratch repo fetches `fc4c274` from the main checkout
  with `--depth 1`, then the bundle's heads and tags, and `stage10-c3d` must equal the local branch) and update the
  project memory's stage 10 entry.
- **Report and stop:** per step, pass/fail with the key figures beside Azahar's; the @60 table and the goal's verdict;
  anything that differed and what it points to; which section 10 questions are now answered. Then my choice for what
  comes next: **M7** (2x2 supersampling; its kickoff is not written yet: offer to write `KICKOFF_M7.md`), or a fix first
  if something failed. Do not push.
