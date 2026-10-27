# Paste this into a fresh Claude Code session started in the stage10-bb worktree

(`/Users/dashielashie/Desktop/Aleph_3ds/.claude/worktrees/stage10-bb`, branch `stage10-bb`, made from
`stage10-c3d` at `cb40b9db9` on 2026-10-26. Written 2026-10-26. It continues `KICKOFF_B_CONSOLE.md`.
This file lives on `stage10-c3d`; if it is not in your checkout, read it at
`/Users/dashielashie/Desktop/Aleph_3ds/.claude/worktrees/stage10-c3d/docs/3ds/KICKOFF_B_B.md`.)

---

We are on Stage 10 of the Aleph One 3DS port (the optional citro3d GPU renderer behind `A1_RENDER_C3D`).
**This session builds B-b, the zero-copy present's engine edit, on its own branch `stage10-bb`.** Do it
Azahar-only; the console is not needed and nothing here waits for a decision.

## Why this is a branch and not a merge

B-a was measured on the New 3DS on 2026-10-26 (`PERF.md` "Stage 10 B-a", "Console (New 3DS), 2026-10-26").
The design's rule for building B-b was a saving of at least 0.5 ms per frame at 30 fps on L0, L6 and L20.
The result was 0.435 / 0.350 / 0.355 ms: **no-go by the rule**. The user then decided (recorded in
`NEXT-SPRINT.md` plan B): build B-b anyway, on its own branch, and **do not merge it into `stage10-c3d`**.
The renderer (plan C, M0 first) goes on `stage10-c3d` meanwhile. B-b is re-measured at 60 fps once the
renderer exists and is merged only if it helps there. Two readings of the numbers to keep in mind:

- The upload wait costs 0.61 ms every run and stays in B-b. The CPU fill that B-b removes is 1.1 ms.
- `bottom` fell about 0.4 ms with `two_frame 1` (unexplained). If that move carried over to B-b the saving
  would be ~0.7-0.9 ms by busy time; if not, it is the 0.35-0.43 ms of the rule. Settling this is the point
  of your Azahar `blit` / `bottom` columns (5.7.9 item 4), and later of the console.
- @60 L0 (one pair): no regression with `two_frame 1`, and the VBlank wait absorbed the extra cost
  (`c3d_vblank` 1.08 -> 0.70). A CPU saving may turn into VBlank wait at 60.

So the goal is a **correct, off-by-default, cleanly separable** implementation that the user can measure
later. It is not a performance claim.

## Read first, in this order

1. `CLAUDE.md` (all 8 rules; "Stage 10"; "Testing in Azahar"). In a worktree the scenario submodule is empty:
   pass `SCENARIO_SRC="/Users/dashielashie/Desktop/Aleph_3ds/data/Scenarios/Marathon 2"` to Azahar runs.
   Run git from this worktree's own directory with literal arguments.
2. `docs/3ds/CITRO3D_DESIGN.md` **section 5.7 entire** (critic: round 1 REVISE, round 2 APPROVE): 5.7.2 (where
   the surface is allocated, the engine edit), 5.7.3 (format and the three-draw channel reorder), 5.7.4
   (synchronisation: frames U and D), 5.7.5 (which presents are zero-copy), 5.7.6 (gate, release, fatal paths
   from 5.6: not to be broken), 5.7.7 (how it is switched off), 5.7.8 (memory), 5.7.9 (tests), 5.7.10
   (UNVERIFIED list, with the 2026-10-26 console note on item 1), 5.7.11 (order), and section 7 "B — the
   zero-copy present" (B-b file list). Also 5.1 (the GPU present) and 5.6 (the gate; do not break it).
3. `docs/3ds/PERF.md` "Stage 10 B-a" (Azahar and Console parts) and "Console G0-b".
4. `ports/3ds/render_c3d/back/present.cpp` (B-a's two-frame code is the base you extend: frames U and D),
   `back/context.cpp`, `core/config.*` and its host test, `docs/3ds/PATCHES.md` (**your number is 55**; 45-48 are
   reserved for M0/M2/M6/M8 and M0 will take 45 on the other branch).
5. `Source_Files/RenderOther/screen.cpp` around :1140 (the one creation site of `main_surface`) and the
   present seam (:2581).

## The task: B-b, exactly as 5.7 and section 7 say

Implement 5.7.2-5.7.8 (B-b list in section 7): the engine edit (`PATCHES.md` 55, `screen.cpp` creation site
fenced `#if defined(A1_PLATFORM_3DS) && defined(A1_RENDER_C3D)`), `a1_3ds::main_surface_pixels()` (declaration
in `a1_3ds_platform.h`, definition in `back/present.cpp`), the three one-channel REPLACE draws that reorder
the channels, path selection (zero-copy only for 400x240 HUD + Map at the right pitch and format; Classic,
menus, dialogs and odd surfaces keep the staging path byte for byte), `close_frame`'s matching draw, the
`zero_copy 0|1` key in `core/config.*` with a host test, and `scripts/3ds/tests/c3d/b-zero-copy.cfg`.
**Default of `zero_copy` stays what 5.7.7 says (check it) but the whole thing must remain off in the
default build.** Work in this order:

1. Re-read 5.7 against the code as it is now (B-a changed `present.cpp`) and write down in a few lines any
   place where the design and the code disagree. If something in the design must change, change the design
   text first (a revision note in the doc's style), then ask `c3d-critic` (effort medium) for a quick review.
   Do not silently deviate.
2. Implement with `c3d-implementer` (one piece), then `c3d-critic` on the commit, at most 2 revise rounds.
   Commit as `stage10: B <summary>`; tag `port-stage10-B`. Quote every shell variable in helper scripts.
3. Run the Azahar acceptance of 5.7.9 items 1-5, in this order of importance:
   - `present_diff` 0 RGB differences and 0 alpha-only pixels with `zero_copy 1`, HUD + Map @30 on L0/L6/L20,
     and scripts 07 (640x480 path), 08, 13, 14 (both Bottom Screen settings: the path switch must leave 0
     differences), 22 (HOME > Close); `present_screen_false_returns=0`; GX high-water <= 4; exactly one
     `[c3d] zero_copy: surface buffer` line across HUD + Map -> Classic -> HUD + Map.
   - The two negative controls (temporary builds, not committed): draws 2 and 3 swapped must make
     `present_diff` report G/B differences; skipping the stage-0 restore must show differences after a path
     switch.
   - Profile build, same binary both ways (`zero_copy 0` / `1`), HUD + Map @30 and @60 L0/L6/L20 plus
     Classic @60 L0 (must be unchanged): `present`, `c3d_wait`, `c3d_fill`, `c3d_upload`, `c3d_vblank`,
     **`blit` and `bottom`**, busy, `max`. Azahar completes GPU work synchronously (`c3d_upload` reads ~0), so
     it overstates the gain: label every figure "Azahar — non-authoritative".
   - The OFF builds: `arm-none-eabi-nm build-3ds/alephone.elf | grep -ciE
     'citro|C3D_|a1_c3d|main_surface_pixels'` = 0 with 0 warnings, **and** `arm-none-eabi-objdump -d` of the
     OFF `alephone.elf` (`build-3ds` and `build-3ds-prof`) identical before and after your change (build the
     "before" from `stage10-c3d`'s tip first). `scripts/3ds/host-baseline/ppcheck.sh` on `screen.cpp`
     (Docker): IDENTICAL, or DIFFER with 0 normalised lines.
4. Record the results in `PERF.md` (a "Stage 10 B-b" section, style of "Stage 10 B-a") and update
   `PATCHES.md` 55, `CITRO3D_DESIGN.md` 5.7.10/section 10 item 16 and `NEXT-SPRINT.md` plan B. To keep the
   later merge easy, **touch only those sections** of the docs; the other branch will be editing the rest.
5. Write the console runbook for the later session (the four series A/B in one build, `zero_copy 0|1`,
   @30 and @60, L0/L6/L20, 2 runs each, plus a functional run), in `NEXT-SPRINT.md` in the style of the
   "B-a console runbook". **Do not run it**: the user decides when. The console will be driven the same
   way as in `KICKOFF_B_CONSOLE.md`.

## Standing rules and traps

- Build dirs in this worktree are fresh (ignored via `.git/info/exclude` only in the main checkout's git
  dir; the worktrees share it): `build-3ds`, `build-3ds-prof`, `build-3ds-c3d`, `build-3ds-c3d-prof`. Configure
  them with `cmake -S ports/3ds -B <dir> [-DA1_RENDER_C3D=ON] [-DA1_PROFILE=ON]`; first builds take a while.
  `export DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM`.
- Zero compiler/linker warnings; `-O2`; no LTO; `-DNDEBUG` removes `assert()`. Engine TUs never include
  `<3ds.h>`. Citro3d belongs to the main thread; nothing in B-b touches it from another.
- Never end the process with a bare `svcExitProcess`.
- Rule 7: cite `$DEVKITPRO/libctru/include`, `citro3d.h`, `c3d/*` and the examples under
  `/opt/devkitpro/examples/3ds/graphics/gpu/` for every PICA / TEV claim; mark the rest UNVERIFIED.
- Subagents do not see this conversation: give each everything it needs. Critic and architect run at
  effort medium.
- Anything needing `sudo`: give the user the command for **Terminal.app**.
- **Do not push, do not merge into `stage10-c3d`, do not touch `3ds-port`.** Backup bundle: after the
  approved commit, include `stage10-bb` (`git bundle create ... fc4c274..3ds-port fc4c274..stage10-c3d
  fc4c274..stage10-bb`, recipe in `STAGE5-PREREQS.md` section 8) and restore-test it.

## Report and stop

What you built (branch, commit, tag), the Azahar table with its label, whether the two readings above
(the `bottom` move; the @60 behaviour) show up in Azahar, the diff of `PATCHES.md`, and the console runbook
the user needs to run. Then stop: whether B-b is merged waits for the 60 fps measurement with the renderer.
