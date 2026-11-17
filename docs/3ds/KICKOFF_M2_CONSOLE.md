# Paste this into a fresh Claude Code session started in the stage10-c3d worktree

(`/Users/dashielashie/Desktop/Aleph_3ds/.claude/worktrees/stage10-c3d`, branch `stage10-c3d`. Written
2026-11-03. It follows `KICKOFF_M2.md`; this session runs M2's console check with me at the console.)

---

We are on Stage 10 of the Aleph One 3DS port (the optional citro3d GPU renderer behind `A1_RENDER_C3D`).
**This session runs one short console session on my real New 3DS: `NEXT-SPRINT.md` "M2 console check,
short version" (two launches, 15-20 minutes of my time).** I am at the console when I say so; ask me for
its IP first (it was 192.168.100.72). Write no renderer code in this session, whatever the result: a
failure is recorded and reported, and the fix is decided afterwards. I am short on time and on usage:
use no subagents, run no Azahar series, read only what the steps need.

## Where the project is

- **M2 is done in Azahar** (2026-11-02, tag `port-stage10-M2`; commits `1c3b9c6f5` parts 6-7, `38ca4868e`
  review fixes, `eb55bd24e` docs). citro3d draws textured walls and floors (full bright) on `debug-c3d`
  diff frames: side B of a SW-vs-C3D comparison; play frames stay software until M4. Results:
  `docs/3ds/PERF.md` "Stage 10 M2"; design `docs/3ds/CITRO3D_DESIGN.md` section 7, "M2" status and the
  Review log entries "M2 commit review" and "M2 acceptance, load test colour".
- **Why this check exists.** Azahar has no CPU cache and runs GPU transfers synchronously, so it cannot
  show sync bugs. M2 added two things only hardware can test (design 7, M2 specification, "Upload
  model"; Review log "M2 commit review", "Needs console verification"):
  1. each texture is converted by the CPU straight into a linear-memory `C3D_Tex`, then flushed with a
     ranged `GSPGPU_FlushDataCache` of exactly `t->size` bytes (`back/texcache.cpp` `texcache_get`);
  2. a later view frame promotes it to VRAM with `C3D_SyncTextureCopy(src, 0, dst, 0, size,
     GX_TRANSFER_RAW_COPY(1))`, queued after `frame_begin()` and before the clear
     (`texcache_frame_start`). The slot samples VRAM once that frame has retired (`on_retired`).
- **The console already passed M1's short check** (2026-11-02: readback calibration 30/30, walls 0
  non-edge, sentinel 0, command words equal to Azahar's; `PERF.md` "Stage 10 M1 — console, short
  check"). So the readback and the rings work on hardware, and this check only tests the texture path.
  The full "M1 console check" (Classic, the present check, discard paths, L6/L20) is still not run and
  is **not** this session's job.
- Git: never push to `origin`. The fork `alephone-3ds` is pushed only when I ask; do not push. The last
  backup bundle is `~/Documents/Aleph_3ds-backups/3ds-port+stage10-c3d+stage10-bb-2026-11-02-0078bfd5f-eb55bd24e-5abc48f50.bundle`.

## Read first, in this order (only these)

1. `CLAUDE.md`: rules 4, 5 and 8, "Testing on the console", "Stage 10".
2. `docs/3ds/NEXT-SPRINT.md`:
   - "How to drive the console" (near the top);
   - "M1 console check, short version": its **Needs** and **Mac setup** paragraphs. `$H/m1/run.sh`
     already exists and is reused as is;
   - **"M2 console check, short version"** (the end of the file): **your procedure, step by step**,
     with the pass rules and Azahar's reference lines.
3. `docs/3ds/PERF.md` "Stage 10 M2": the Azahar table, to compare against.

## Before I press anything (the Mac side, no console needed)

- `export DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM`. Confirm the code equals the tag:
  `git diff --stat port-stage10-M2 HEAD -- ports scripts` prints nothing (later commits are docs only).
  Then `cmake --build build-3ds-c3d-prof` (expect "nothing to do" or a rebuild with 0 warnings).
- `H=/Users/dashielashie/Desktop/Aleph_3ds/build-3ds/hw` (the **main checkout**'s `build-3ds/hw`, outside
  git). Check that `$H/m1/run.sh` and `$H/sprint8/debug-perf-L0` exist. `run.sh` hard-codes the IP
  192.168.100.72: if I give another one, change it in `run.sh` (and say so).
- Kill stale 3dslink processes and old transcript monitors: `pgrep -fl '^/opt/devkitpro/tools/bin/3dslink'`.
- The game on the console must be in **HUD + Map** (Bottom Screen setting, the default). Ask me to check
  it if unsure; do not change my other preferences.
- A 3dslink launch overwrites `/3ds/alephone.3dsx` on the card (today it is already the c3d-prof build).

## The runs (the runbook's steps; I do the button presses)

1. **Textured diff on L0**: ftpd puts `debug-perf` (from `$H/sprint8/debug-perf-L0`) and
   `scripts/3ds/tests/c3d/m2-diff.cfg` as `debug-c3d`; then `$H/m1/run.sh build-3ds-c3d-prof m2s-l0`;
   I press A twice; it quits after the BENCH line; then the runbook's `c3d-diff.py ... --colour` command.
2. **Deferred promotions**: `m2-gxq4.cfg` as `debug-c3d` (`debug-perf` stays);
   `$H/m1/run.sh build-3ds-c3d-prof m2s-gxq4`; the same `c3d-diff.py` command.

Remember the ftpd app and the netloader cannot run at once: I close ftpd, then press Y in the Homebrew
Launcher before each launch. "Connection reset by peer" mid-run means the lid closed or Wi-Fi slept; the
log continues on the card (fetch `/3ds/alephone/log.txt` with `scripts/3ds/sd-ftp.sh IP get`).

**What decides pass or fail (the runbook has the detail):** `c3d-diff.py` exits 0, `sentinel=0` on every
line, 0 discards, and the 20 checks of a run are **identical to each other** (the view is static). The
console is expected to be close to Azahar but **not equal to the digit**: it excludes more edge-tie
pixels (`tie_mismatch` 380 vs Azahar's 140 in M1's check), which moves `cov_mismatch` edge counts and can
nudge the colour figures. If check 1 is worse than the rest, suspect the conversion flush. If checks
drift after check 1 (run 1) or over the first ~6 checks (run 2, one promotion per frame), suspect the
TextureCopy promotion. Command words (`[c3d] cmdbuf final`: `view_frame_words=1288 draw_words=268
readback_words=8`) are deterministic, so any difference from Azahar is real and must be reported.

## Afterwards

- **Card cleanup:** ftpd deletes `/3ds/alephone/debug-perf` and `/3ds/alephone/debug-c3d`. Optionally
  fetch nothing else (`dump 0` in both configs).
- **Record** (rule 4, in the style of the existing entries): `PERF.md` new section "Stage 10 M2 — console,
  short check" (both runs: the `c3d-diff.py` tables, per-class `ties` / `tie_mismatch`, the `[c3d] tex …
  [final]` and `[c3d] cmdbuf final` lines, pass/fail); the runbook's head in `NEXT-SPRINT.md` ("**Run and
  passed / failed on the console 2026-11-..**") and plan C's status paragraph; `CITRO3D_DESIGN.md` M2 status
  line and the "Needs console verification" sentence of the Review log entry "M2 commit review"
  (verified / refuted); `CLAUDE.md` "State of the project" (one sentence). Raw transcripts stay in
  `$H/m1/` (never in git).
- **Commit** `stage10: docs: M2 console check (<one-line result>)`, then refresh the backup bundle
  (recipe `STAGE5-PREREQS.md` section 8; the command and restore test as in `KICKOFF_M1.md`, with
  `port-stage10-M2` added to the tag list) and update the project memory's stage 10 entry.
- **Report and stop:** pass/fail per run with the key figures beside Azahar's, anything that differed
  and what it points to, and my choice for what comes next (M3 lighting, kickoff to be written; or the
  full "M1 console check" in a longer session; or a fix first if this failed). Do not push.
