# Paste this into a fresh Claude Code session started in the stage10-c3d worktree

(`/Users/dashielashie/Desktop/Aleph_3ds/.claude/worktrees/stage10-c3d`, branch `stage10-c3d`.
Written 2026-10-26. It continues `KICKOFF_PLAN_B.md`; this session does the console part of plan B.)

---

We are on Stage 10 of the Aleph One 3DS port (the optional citro3d GPU renderer behind `A1_RENDER_C3D`).
**This session runs one console session on a real New 3DS: the B-a measurements that decide whether
B-b (the zero-copy present's engine edit) gets built.** I am at the console when I say so; ask me
for its IP first (it was 192.168.100.72). Do not build B-b in this session, whatever the result:
the go / no-go is my decision once I have seen the numbers.

## Where the project is

- Plan A (fixes) is done and console-verified. Plan B is designed: `docs/3ds/CITRO3D_DESIGN.md`
  section 5.7 (critic: round 1 REVISE, round 2 APPROVE).
- The estimate of what B saves (~0.1-1.3 ms per HUD + Map frame) has a lower end inside the
  console's noise, so B was split (design 5.7.1, 5.7.11):
  - **B-a, built** (commits `a35aa79f4`, `87b8357c8`; critic APPROVE; Azahar passes: `docs/3ds/PERF.md`
    "Stage 10 B-a"): port code only. `debug-c3d two_frame 1` runs today's staging present as two
    C3D frames (U: fill, flush, upload; D: waits for U, then the quad), exactly B-b's cost structure
    with the CPU copy still in place. Profile zones `c3d_fill`, `c3d_upload`, `c3d_vblank` (inside
    `present`, exclusive); `scripts/3ds/tests/perf-table.py` adds them and "present total";
    the `[c3d] stats` line carries `gpu_ms draw=... upload=...`. Default `two_frame 0` = the old
    present, unchanged.
  - **B-b, not built:** the engine edit (`PATCHES.md` 55) and the channel reorder. Built only on
    a console go.
- Azahar completes GPU work synchronously (`c3d_upload` reads 0 there), which is why only the
  console can decide.
- Nothing is pushed since `1fc356d`/`64f01c5`; do not push unless I ask. Backup bundle refreshed
  at `87b8357c8`.

## Read first, in this order

1. `CLAUDE.md` (all 8 rules; "Testing on the console"; "Stage 10").
2. `docs/3ds/NEXT-SPRINT.md`: "How to drive the console" (near the top), plan B (search for
   "B. **Zero-copy present**"), and **"B-a console runbook"**: that runbook is your procedure.
   Follow it step by step.
3. `docs/3ds/CITRO3D_DESIGN.md` 5.7.1 (baseline, goal, why the split), 5.7.4 (the two frames and
   the zones), 5.7.11 (B-a and the go formula), section 10 item 16.
4. `docs/3ds/PERF.md` "Console G0-b, first session" (the reference numbers and the table style),
   "Stage 10 B-a" (the Azahar figures).
5. Your memory notes on 3dslink and the console (`3dslink-from-claude`, `stage10-paused-at-g0`).

## The task

1. **Check the builds** in this worktree: `cmake --build build-3ds-c3d-prof` and
   `cmake --build build-3ds-c3d` (nothing to do / 0 warnings; `export DEVKITPRO=/opt/devkitpro
   DEVKITARM=/opt/devkitpro/devkitARM`). `git log --oneline -1` should be `87b8357c8` or a later
   docs commit.
2. **Run the B-a console runbook**, steps setup, 1 (the 12-run series, HUD + Map @30, L0/L6/L20,
   `two_frame 0`/`1` interleaved, 2 runs each), 2 (optional @60 look: ask me), 3 (functional run),
   then cleanup. Tell me before each step whether you need **ftpd** or the **Homebrew Launcher
   netloader** (Y), and when to press A. Rerun a benchmark that logs `gaps` ≠ 0 or no BENCH line.
3. **Compute the verdict** with `python3 -I scripts/3ds/tests/perf-table.py` on the transcripts:
   per level, `fill` = mean `c3d_fill` of the two `two_frame 0` runs; `extra` = mean present total
   (`two_frame 1`) − mean present total (`two_frame 0`); **saving = fill − extra**.
   **Go for B-b iff saving ≥ 0.5 ms on L0, L6 and L20.** Also report `c3d_upload` (the upload wait),
   `c3d_vblank`, `bottom`, `blit`, `max`, the `gpu_ms` values, and the sanity checks (`two_frame 0`
   present total near G0-b's 2.03 ms; both sides' `c3d_fill` agree).
4. **Record** (style of the existing sections; label console figures "Console (New 3DS),
   <date>"): `PERF.md` "Stage 10 B-a" (a console part: the table, the verdict, the functional run),
   `CITRO3D_DESIGN.md` 5.7.10 item 1 (VERIFIED/REFUTED with the numbers) and section 10 item 16,
   `NEXT-SPRINT.md` plan B status and the runbook's head ("Run on <date>: ..."). Raw transcripts,
   card logs and captures stay in `/Users/dashielashie/Desktop/Aleph_3ds/build-3ds/hw/b0/` (not in git).
   Commit as `stage10: docs: B-a console results (<verdict>)` with the session's attribution lines.
   Refresh and restore-test the backup bundle (`docs/3ds/STAGE5-PREREQS.md` section 8; the latest is
   `~/Documents/Aleph_3ds-backups/3ds-port+stage10-c3d-2026-10-26-0078bfd5f-87b8357c8.bundle`;
   restore test: a scratch repo, `git fetch --depth=1 <main checkout> <full fc4c274 hash>`, then
   fetch the bundle's heads, compare the tree).
5. **Report and stop:** the table, the verdict against the 0.5 ms rule, anything unexpected, and the
   choice I have to make: build B-b (design 5.7.2-5.7.9; the next session would get its own prompt),
   or stop B at the design and keep B-a as an off-by-default diagnostic, or revert B-a. Then plan C
   (the renderer, M0 first) waits for my word too.

## Standing rules and traps (the short list; `CLAUDE.md` has the rest)

- `3dslink` broadcast does not work here: launches go through
  `build-3ds/hw/b0/run.sh BUILD TAG` (made in the runbook's setup from `g0b/run.sh`), which uses
  `3dslink -a IP` under `script`. Kill stale 3dslink processes before each launch
  (`pgrep -fl '^/opt/devkitpro/tools/bin/3dslink'`). A 3dslink launch overwrites
  `sdmc:/3ds/alephone.3dsx`: put the wanted one back at the end (ask me which).
- ftpd and the netloader cannot run at once. "Connection reset by peer" mid-run is the lid or Wi-Fi
  sleep; the rest of the log is on the card (`log.txt`; each boot moves it to `log.prev.txt`).
- `scripts/3ds/sd-ftp.sh IP put LOCAL SDDIR` uploads under the local file's own name: copy configs to
  a scratch file named `debug-perf` / `debug-c3d` first. Scratch files go in your job/scratch dir,
  never loose in the worktree, and **quote every shell variable in helper scripts** (an unquoted
  `rm -rf` in an earlier session deleted tracked files).
- Luma dumps: `/luma/dumps/arm11/` (0-6 exist); list before and after; registers at 0x28;
  `arm-none-eabi-addr2line -e build-3ds-c3d[-prof]/alephone.elf`. Use `arm-none-eabi-nm`, never macOS `nm`.
- A hang at HOME > Close ("Closing software..." for good): I power off; fetch the log, record the
  last `[c3d]` lines, stop (do not retry blindly).
- Anything that needs `sudo`: give me the command for **Terminal.app**.
- Cleanup always: delete `debug-perf` and `debug-c3d` from the card, put my preferences back
  (`build-3ds/hw/b0/prefs-user`), kill leftover 3dslink processes.
- Subagents do not see this conversation; this session should not need any (no code changes). If
  something must change in code, stop and ask me first.
