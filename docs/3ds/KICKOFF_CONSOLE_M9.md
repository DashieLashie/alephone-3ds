# Paste this into a fresh Claude Code session started in the stage10-c3d worktree

(`/Users/dashielashie/Desktop/Aleph_3ds/.claude/worktrees/stage10-c3d`, branch `stage10-c3d`. Written
2026-11-13 after M9 (tag `port-stage10-M9`). Earlier console prompt of the same kind: `KICKOFF_CONSOLE_M6.md`.)

---

We are on Stage 10 of the Aleph One 3DS port (the optional citro3d GPU renderer behind `A1_RENDER_C3D`).
**This session runs `NEXT-SPRINT.md` "M9 console check" on my real New 3DS**: stereo 3D, built and tested in Azahar
only (**about 60 minutes of my time, 17 launches plus one per comfort value I try**). I am at the console when I say
so; ask me for its IP first (it was 192.168.100.72). **Write no renderer code in this session, whatever the result:**
a failure is recorded and reported, and the fix is decided afterwards. The one exception is the comfort defaults at
the very end, and only if I ask (a one-line change, see "Afterwards"). Use no subagents, run no Azahar series, read
only what the steps need. **Do not push.**

## Where the project is

- **M0-M6, M8, M6b and M9 are done in Azahar** (tags `port-stage10-M0` … `-M9`). M9 is **stereo 3D on direct frames**:
  the world vertex shader shifts each vertex sideways by an amount that depends on its eye depth `w` (`shift = e · P ·
  max(1 − w0/w, −near)` pixels, `e` = −1 left, +1 right); the frame's draws are recorded in a draw list and replayed
  per eye into two 512x256 eye textures; the present composites each eye to its own screen target (`GFX_LEFT`,
  `GFX_RIGHT`), overlays at zero parallax. No engine edit.
- **What I decided (2026-11-09), do not re-open:** "GPU (experimental)" turns stereo on and the 3D slider is the
  control (D1); the gap fill is on by default, 4 passes per frame (D2); **the weapon sits in front of the screen**,
  `stereo_weapon -50`, crosshair and overlays at zero parallax (D3); the depth eases back in at a constant rate after
  any flat frame (D4). The comfort values (`stereo_k`, `stereo_w0`, `stereo_near`, `stereo_weapon`, `stereo_ramp`)
  are first guesses **for this session to tune**.
- **Things that follow from that and will show on the console:** without a `debug-c3d` file the right-eye objects are
  always allocated and the VRAM texture pool's cap is 2 944 KiB instead of 3 584 (on L0 and L6 the 2 MiB landscape
  part is then sampled from linear memory); 3D mode (`gfxSet3D`) follows the Renderer preference; a frame that is not
  direct (map, terminal, a software frame) shows one image to both eyes, and the depth then ramps back in over
  `stereo_ramp 15` frames; the ramp also restarts at every level load; on every entry into stereo the right-eye
  texture is **copied from the left** (a VRAM to VRAM `GX_TextureCopy`, never run on hardware).
- **Azahar results** (`docs/3ds/PERF.md` "Stage 10 M9"): calibration T1 30 of 30 frames on both backends; identity
  (right eye = left eye at zero parallax) 0 differing pixels; @60 every stereo row holds 59.6-59.7 fps on L0 / L6 /
  L20; fill 1 adds +1.97 / +2.57 / +0.88 ms busy @30, fill 0 +0.40 / +1.02 / +0.35. **Azahar does not model pixel
  fill, which is what 2 or 4 passes multiply: these say little about the console.**
- **Why this session exists.** Azahar cannot answer (design `CITRO3D_DESIGN.md` section 10 item 13, the M9 items):
  the shader's `rcp` / `max` in float24 and the left-edge clipping on PICA; which framebuffer reaches which eye; what
  the 3D slider reads at rest and at the top; the cost of 2 and 4 view passes (`gpu_ms view`, fps @60), which is also
  the first real fill-rate figure for M7; whether the VRAM to VRAM copy is right and what it costs; HOME, sleep and
  Close with the right target linked; a single-frame mono flicker (the stereo swap happens in a queue callback); the
  parallax barrier's brightness on mono frames; and everything about how it looks: comfort, ghosting, the weapon, the
  crosshair over a deep scene, the gap fill against `stereo_fill 0`.
- **One build:** `build-3ds-c3d-prof` at `port-stage10-M9` (`63310e9b7`). Commits after the tag are docs only.
- **Not part of this session:** the "M8 console check" and "M6b console check" runbooks (both still not run). If time
  is left at the end, ask me whether to do either; do not start them on your own.
- Git: never push to `origin`; the fork `alephone-3ds` only when I ask; **do not push** (M9 itself is not pushed
  yet). Last backup bundle:
  `~/Documents/Aleph_3ds-backups/3ds-port+stage10-c3d+stage10-bb-2026-11-13-5e39e4f38-63310e9b7-5abc48f50.bundle`
  (verified, **not restore-tested**: do the restore test when you refresh it at the end).

## Read first, in this order (only these)

1. `CLAUDE.md`: rules 4, 5 and 8, "Testing on the console", "Stage 10". Note: **macOS has no `timeout` command**
   here; never wrap commands in it.
2. `docs/3ds/NEXT-SPRINT.md`:
   - "How to drive the console" (near the top);
   - "M1 console check, short version": its **Needs** and **Mac setup** paragraphs (`$H/m1/run.sh` exists and is
     reused as is);
   - **"M9 console check" (the last section of the file): your procedure**, with each launch's `debug-c3d` contents,
     the pass rules, Azahar's reference values and how to read a failure. **Follow its "Order of the session" line**,
     not the step numbers: step 1, then step 5, then 2, 2b, 3 (look only), 3b, 4, 6, 7, comfort tuning last.
3. `docs/3ds/PERF.md` "Stage 10 M9" (the Azahar references and tables), "Stage 10 M6 — console, short check" (the
   style of the record, and M6's console figures: direct 60.5 fps @60, `gpu_ms view` 1.3-1.5 ms for one pass) and
   "Stutter follow-up" (why the bench runs with `quiet 1`).

## Before I press anything (the Mac side, no console needed)

- `export DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM`. Confirm the code equals the tag:
  `git diff --stat port-stage10-M9 HEAD -- ports Source_Files scripts` prints nothing. Then
  `cmake --build build-3ds-c3d-prof` (expect "nothing to do" or a rebuild with 0 warnings).
- `H=/Users/dashielashie/Desktop/Aleph_3ds/build-3ds/hw` (the **main checkout**'s `build-3ds/hw`, outside git; write
  this session's files under `$H/m9c/`). Check that `$H/m1/run.sh`, `$H/sprint8/debug-perf-L0`, `-L20` and
  `$H/sprint8/setprefs.sh` exist. `run.sh` hard-codes the IP 192.168.100.72 (in its `launch.sh` line): if I give
  another one, change it there and say so. `run.sh` writes transcripts to `$H/m1/<tag>.txt`.
- Kill stale 3dslink processes and old transcript monitors: `pgrep -fl '^/opt/devkitpro/tools/bin/3dslink'`.
- The card files the runbook uses, in `scripts/3ds/tests/c3d/`: `m9-t1.cfg`, `m9-bench-mono.cfg`, `m9-bench-s0.cfg`,
  `m9-bench-f1.cfg`, `m9-bench-f0.cfg` (these four **already have `quiet 1`**), `m9-gms-mono.cfg`, `m9-gms-f1.cfg`,
  `m9-gms-f0.cfg` (the same without `quiet 1`), `m9-console.cfg`, `m9-console-fill0.cfg`,
  `m9-console-identity.cfg`. **Make one copy** (not in git): `mkdir -p $H/m9c/cfg` and `m9-gms-f1.cfg` with a last
  line `reseed 10` as `$H/m9c/cfg/m9-gms-f1-reseed.cfg` (step 2b's copy-cost run). Comfort-tuning variants of
  `m9-console.cfg` also go in `$H/m9c/cfg/`, one changed key per file.
- **Check every cfg against the parser before the session** (`ports/3ds/render_c3d/core/config.cpp`, the key list in
  `core/config.h`): a mistyped key is only logged as `ignored` and the launch then tests nothing. After each launch,
  grep the transcript for `ignored` first.
- The Terminal Test plugin for step 4: `scripts/3ds/tests/plugins/Terminal Test` goes into the card's
  `Marathon2/Plugins/` (find the card path with `scripts/3ds/sd-ftp.sh IP ls`), and comes off again at the end.
- The analysis tool: `scripts/3ds/tests/perf-table.py` (BENCH tables; check that it prints the `c3d_stereo` zone,
  and if it does not, read the BENCH lines directly rather than changing the tool mid-session). Run Python with `-I`.
- **Preferences.** The runbook wants **HUD + Map, Framerate Target 60** throughout. With ftpd, first fetch my
  `Marathon 2 Preferences` into `$H/m9c/prefs-user` to put back at the end, then `$H/sprint8/setprefs.sh IP 0 60`.
  Step 6 is the only one that needs the Renderer preference itself (no `debug-c3d` file on the card): I flip it by
  hand in Preferences > Graphics.
- A 3dslink launch overwrites `/3ds/alephone.3dsx` on the card.

## The runs, in the session's order (I do the button presses and the looking; I may stop early)

If time runs short, stop after any block; each block is recorded on its own. **Blocks A and B first:** if the eyes
are wrong or the console hangs with the right target linked, nothing after them means anything.

- **A. Runbook step 1, T1 on the hardware** (1 launch, ~3 min): `debug-perf-L0` + `m9-t1.cfg`. 30 lines
  `stereo_cal: tex L 5/5 R 5/5 fb L 5/5 R 5/5`. I look with the slider up and tell you which eye sees the red square
  further left.
- **B. Step 5, HOME / sleep / Close with the right target linked** (1 launch, ~5 min; no `debug-perf`,
  `m9-console.cfg`): I play by hand, slider up; you watch the transcript and then read the card's `log.txt`
  (`release done`, `3D mode off (citro3d released)`, no `release abandoned`).
- **C. Step 2, the @60 bench, `quiet 1`** (6 launches, ~18 min): mono, s0, f1, f0 on L0; mono and f1 on L20.
  **Slider position does not matter here** (the cfgs set `stereo_slider`), but tell me to leave it up so I see the
  picture.
- **D. Step 2b, `gpu_ms view`** (4 launches, ~10 min, L0, not quiet): `m9-gms-mono.cfg`, `m9-gms-f1.cfg`,
  `m9-gms-f0.cfg`, then `$H/m9c/cfg/m9-gms-f1-reseed.cfg`.
- **E. Step 3, the hardware slider and the look** (1 launch, ~8 min; no `debug-perf`, `m9-console.cfg`): I play L0
  and describe what I see; you ask me the runbook's questions one at a time (walls and doors, the weapon, the
  crosshair over a deep corridor and a near wall, sprites, the sky, near edges, the ramp when I move the slider,
  ghosting) and write my answers down verbatim. From the log: `slider=` at rest and at the top.
- **F. Step 3b, `stereo_fill 0`** (1 launch, ~3 min): the same corridor and doorway; which looks better.
- **G. Step 4, identity, re-entries and black specks** (1 launch, ~8 min; `m9-console-identity.cfg` + the Terminal
  Test plugin), then **the ramp after a level change** (1 launch, `m9-console.cfg`). The saved-game part only if a
  save exists on the card; else skip it and say so in the record.
- **H. Step 6, menus in 3D mode and the preference** (1 launch, ~6 min; **no `debug-c3d` file**).
- **I. Step 7, VRAM**: no launch; read the `tex:` and `vram_free_kib` lines of the launches above.
- **J. Comfort tuning, last** (one launch per value I want to try): `stereo_k` 64 / 96 / 128, `stereo_w0` 512 / 1024 /
  2048, `stereo_near` 50 / 100 / 200, `stereo_weapon` −50 / 0 / −100, `stereo_ramp` 8 / 15 / 30. One key at a time;
  record my pick for each, or "kept".

Driving reminders: **press A at once at the menu** (the main menu's demo starts ~13 s after the menu and would
replace the level); the ftpd app and the netloader cannot run at once (close ftpd, then Y in the Homebrew Launcher
before each launch); "Connection reset by peer" mid-run means the lid closed or Wi-Fi slept (expected in block B),
the log continues on the card (fetch `/3ds/alephone/log.txt` with `scripts/3ds/sd-ftp.sh IP get`; each boot moves
the previous one to `log.prev.txt`). Between launches change only the card files the next step names. Tell me before
each launch which files you put on the card, where the slider should be, and what I should do and look at.

**Reading results.** The runbook has each step's pass rule and notes; follow them. In short:
- **A:** a `tex` short of 5/5 is the shader's float24 maths or the left-edge clipping (record which quad: the 4th is
  the far limit, the 5th the left edge); `fb R` short of 5/5 with `tex R 5/5` is the right target's transfer or its
  eye; **eyes swapped** (I see the right eye's picture with the left eye) is a one-line fix decided afterwards.
  Record and **stop the stereo blocks** if the eyes are swapped or T1 fails; blocks B and the mono rows of C and D
  can still run.
- **B:** a hang, a Luma screen or `release abandoned`: fetch the log and any dump, record, and ask me before going on.
- **C:** the question is whether **fill 1 holds ≥ 55 fps @60 on L0 and L20**; if not, whether fill 0 does. Also
  s0 against mono (the cost of 3D mode and the allocation alone) and `direct final` (`direct=` ≥ 95 % of `frames=`,
  else the run is not valid) and `stereo final` (`stereo=` all frames on f1 / f0, 0 on s0, `replay_discards` 0,
  `w_low` 0).
- **D:** mean and max `view=` per row; f1 and f0 against this session's mono row (not only against M6's 1.3-1.5 ms);
  the reseed row minus f1, times 10, is one copy's GPU time, or "not measurable".
- **G:** every `stereo_check` line `differ=0 is3d=1 distinct=1`; a BAD line prints up to 8 pixels: record them all.
  `r_copies` rises by 1 at each re-entry.
- A Luma dump: fetch `/luma/dumps/arm11/*.dmp`, resolve with
  `arm-none-eabi-addr2line -e build-3ds-c3d-prof/alephone.elf` (registers at offset 0x28), record, and go on only if
  the failure is understood.
- For each M9 question in design section 10 item 13, say whether this session verified, refuted or did not reach it.

## Afterwards

- **Card cleanup:** ftpd deletes `/3ds/alephone/debug-perf`, `/3ds/alephone/debug-c3d`, the Terminal Test plugin and
  any `screen-c3d-*` files; put my `Marathon 2 Preferences` back from `$H/m9c/prefs-user`, **with the Renderer
  preference as I say at the end** (ask me: GPU with stereo, or Software).
- **Comfort defaults, only if I ask:** new defaults for `stereo_k`, `stereo_w0`, `stereo_near`, `stereo_weapon`,
  `stereo_ramp` are a change of the default values in `ports/3ds/render_c3d/core/config.h` / `config.cpp` plus the
  host test of the defaults (`scripts/3ds/tests/c3d-host/build.sh` must pass), the four builds with 0 warnings, and
  the OFF build's `nm` check (rule 6). Nothing else in the code changes in this session.
- **Record** (rule 4, in the style of `PERF.md` "Stage 10 M6 — console, short check"): a new `PERF.md` section
  "Stage 10 M9 — console" with, per launch, the key lines (`stereo_cal`, `stereo final`, `direct final`,
  `composite final`, the `stereo_check` worst line, `slider=` at rest and at the top, the block B log lines), the
  @60 BENCH table (mono / s0 / f1 / f0) beside Azahar's, the `gpu_ms view` table, the copy's cost, the VRAM figures
  beside Azahar's 116 / 2 770 / 2 706 KiB, my observations in my words, and the comfort picks. Then the runbook's
  head in `NEXT-SPRINT.md` ("**Run on the console 2026-11-.., …**"); `CITRO3D_DESIGN.md`: M9's status line, section
  10 item 13's M9 items (verified / refuted / not reached per question), every **UNVERIFIED** mark in the M9 block
  that this session settled, and a Review log entry; `CLAUDE.md` "State of the project" (one sentence). The fill
  figures (`gpu_ms view` with 2 and 4 passes) also go where M7's block can find them: they are M7's first evidence.
  Raw transcripts stay in `$H/m1/` and `$H/m9c/` (never in git).
- **Commit** `stage10: docs: M9 console check (<one-line result>)` (and, separately, `stage10: M9 comfort defaults
  from the console (<values>)` if I asked for them), then refresh the backup bundle (recipe `STAGE5-PREREQS.md`
  section 8: `git bundle create <file> ^fc4c274 3ds-port stage10-c3d stage10-bb $(git tag -l 'port-stage10-*')`,
  `git bundle verify`, and the restore test: a scratch repo fetches `fc4c274` from the main checkout with
  `--depth 1`, then the bundle's heads and tags, and `stage10-c3d` must equal the local branch) and update the
  project memory's stage 10 entry.
- **Report and stop:** per block, pass/fail with the key figures beside Azahar's; the @60 table and whether fill 1
  holds 55 fps; `gpu_ms view` for 1, 2 and 4 passes; anything that differed and what it points to; which section 10
  questions are now answered; my comfort picks. Then my choices for what comes next: a fix first if something
  failed; fill 1 or fill 0 as the default if the bench says fill 1 is too slow (D2 is mine to revisit); the M8 and
  M6b console checks; **M7** (2x2 supersampling, last; its kickoff is not written yet: offer to write
  `KICKOFF_M7.md`, which must start from design 9.1's VRAM note and this session's fill figures). Do not push.
