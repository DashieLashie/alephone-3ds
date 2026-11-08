# Paste this into a fresh Claude Code session started in the stage10-c3d worktree

(`/Users/dashielashie/Desktop/Aleph_3ds/.claude/worktrees/stage10-c3d`, branch `stage10-c3d`. Written 2026-11-08 after the
M6 console check. Earlier prompts: `KICKOFF_PROMPT.md` (stage 10), `KICKOFF_M0.md` … `KICKOFF_M6.md`, `KICKOFF_CONSOLE_M6.md`.)

---

We are on Stage 10 of the Aleph One 3DS port (the optional citro3d GPU renderer behind `A1_RENDER_C3D`).
**This session builds M8: the Renderer preference** ("Renderer: Software / GPU (experimental)", default Software,
`<graphics c3d_renderer="0|1">`). It is the small milestone: design section 7, "M8" (three lines); `PATCHES.md` **48** is
reserved for it. **User decision 2026-11-08: the order is M8 → M9 (stereo) → M7 (supersampling) last**; every gate is measured
and recorded, none decides. Be economical: the user is short on time and usage (no subagents beyond a `c3d-critic` at effort
**medium** on the commit, at most 2 revise rounds; implement inline; no Azahar series that already passed on the same binary).

## Where the project is

- M0-M6 are done; the **M6 console check ran on 2026-11-07** (`PERF.md` "Stage 10 M6 — console, short check"): no hardware
  bug, direct mode 60.5 fps @60 on L0/L6/L20 on the New 3DS (the software renderer also holds 60.5 there), `direct_check` Δ 0.
  `CLAUDE.md` "State of the project" has the one-paragraph version.
- Today the GPU renderer is selected only by the test-only `sdmc:/3ds/alephone/debug-c3d` file (`renderer c3d`, `mode direct`,
  `play 1`, `textures 1`, `lit 1`, `overlays`, … ; keys in `ports/3ds/render_c3d/core/config.h`, design section 7 "Runtime
  config"). A normal user has no way to turn it on. M8 gives them one.
- The model to copy is **`PATCHES.md` 40**, the "Bottom Screen" preference: `Source_Files/Misc/preferences.cpp` has the
  `w_select` in the Graphics dialog (near the `bottom_screen_w` lines, ~1397), the default (~4291), the validate step (~4523),
  the save attribute (`root.put_attr`, ~3775) and the read (`root.read_attr`, ~4821); `preferences.h` has the field
  (`graphics_preferences_data`, ~107). Re-find these by grep: the line numbers are from 2026-11-08.

## The decision to ask the user first (it is theirs)

What does "GPU (experimental)" mean in shipping? Today's best configuration is `renderer c3d`, `mode direct`, `play 1`,
`textures 1`, `lit 1`, `overlays 1`, with `zero_copy 1` and `present gpu` already the defaults. The options to put in front of
them (recommend one, do not decide): **(a)** the preference turns on exactly that bundle and the `debug-c3d` file still
overrides it for tests; **(b)** the preference only enables the renderer and the file keeps selecting the mode. Also ask what
happens in Classic (`bottom_screen=2`, which is `not_native` and always readback) and on a fatal error / dialog (those paths
already fall back; check, do not assume). **Check what the code does before asking** (`config.cpp`, where `debug-c3d` is
parsed, and `context.cpp`) so the question is concrete.

## What to build

1. The preference: field, default 0 (Software), clamp/validate (0 or 1), Graphics-dialog entry, `<graphics c3d_renderer>`
   save/read; **only under `#ifdef A1_PLATFORM_3DS`, and only when the build has the renderer** (`A1_RENDER_C3D`): on the OFF
   build the entry must not appear and the OFF `alephone.elf` must still show **0** `citro|C3D_|a1_c3d` symbols
   (`$DEVKITARM/bin/arm-none-eabi-nm`, never macOS `nm`; `CLAUDE.md` rule 6).
2. The glue in `ports/3ds/render_c3d/` that reads the preference and applies the chosen configuration (the spec says it
   **switches at the next frame**: check what switching mid-game means for the texture cache, the direct texture target and the
   overlay surface; M6's `mode_changes` / `new_dest` machinery already handles mode changes, find out whether it covers
   renderer sw <-> c3d).
3. `PATCHES.md` 48 (the entry), and `scripts/3ds/host-baseline/ppcheck.sh "<changed files>" "<TUs>"` in Docker (Docker
   Desktop must be running; **check with plain `docker ps`, never wrap in `timeout`: macOS has none**). IDENTICAL is the goal.
4. Acceptance (the spec's, in Azahar): the preference **persists** (write, restart, read back) and **switches at the next
   frame**: a script that flips it in the Graphics dialog, then plays a few seconds; `[c3d]` lines appear only with it on;
   Software behaves exactly as before with it off (a short `perf-bench.sh` check that the numbers did not move, rule 1's
   "no regression"); the default `A1_RENDER_C3D=OFF` build and the host build unchanged.

## Rules that bite here

- Rule 2: every engine edit is fenced with `#ifdef A1_PLATFORM_3DS`, numbered (48), and passes `ppcheck.sh`.
- Azahar first (labelled "Azahar — non-authoritative"), the console later: **a console check of the preference is a short
  runbook to write in `NEXT-SPRINT.md`, not to run** unless the user is at the console. Azahar's emulated card is shared:
  take `~/Applications/Azahar-2126.2/user/.run-lock` (mkdir/rmdir) around every run; in a worktree pass
  `SCENARIO_SRC="/Users/dashielashie/Desktop/Aleph_3ds/data/Scenarios/Marathon 2"`.
- Commit `stage10: M8 <summary>`, tag `port-stage10-M8`, `PERF.md` section "Stage 10 M8" (the Azahar result), design M8's
  status line and a Review log entry, `CLAUDE.md` one sentence. Refresh the backup bundle (recipe `STAGE5-PREREQS.md` section 8;
  `git fetch --depth 1` needs the **full** `fc4c274359088614b58c91af5daa541504cb50cf` hash) and update the project memory's
  stage 10 entry. **Do not push.**
- Then stop and report. Next is **M9** (stereo): its spec must redo the VRAM budget first (design section 6 and Review log
  "M7 moved to the end": a second screen target and view target add about 887 KiB against 329 KiB free).
