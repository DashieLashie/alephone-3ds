# Paste this into a fresh Claude Code session started in the stage10-c3d worktree

(`/Users/dashielashie/Desktop/Aleph_3ds/.claude/worktrees/stage10-c3d`, branch `stage10-c3d`. Written
2026-10-26. The original stage 10 prompt is `KICKOFF_PROMPT.md`; plan B's are `KICKOFF_PLAN_B.md`,
`KICKOFF_B_CONSOLE.md` and `KICKOFF_B_B.md`.)

---

We are on Stage 10 of the Aleph One 3DS port (the optional citro3d GPU renderer behind `A1_RENDER_C3D`).
**This session builds M0, the renderer harness: the first milestone of plan C, the renderer.** It is
Azahar-only plus host tests; the console is not needed and nothing waits on a decision. M0 draws nothing
new on screen: it builds the machinery every later milestone is measured with.

## Where the project is

- P0, P1 (GPU present, tag `port-stage10-P1`), G0-b, plan A (fixes) and the B-a measurements are done and
  console-verified. The user decided (2026-10-19) to keep the GPU present and build the renderer for M7
  (supersampling) and M9 (stereo). Plan C is the next piece; **G1 is decided: build**.
- Plan B, the zero-copy present, is being built **separately on branch `stage10-bb`** (worktree
  `.claude/worktrees/stage10-bb`, `PATCHES.md` 55; the console result was a saving of 0.35-0.43 ms, under
  the design's 0.5 ms rule, so it is parked there and not merged). **Do not wait for it, do not merge it,
  and do not touch its files.** B-b and M0 don't overlap in code: B-b edits `screen.cpp` (:1140) and
  `back/present.cpp`; M0's engine edit is in `render.cpp` (patch **45**). Keep your doc edits to the M0
  parts (below) so the two branches merge cleanly later.
- Hardware: one New 3DS. Branch rules in `CLAUDE.md` "Git": never push to `origin`; push to the fork only
  when the user asks. **Do not push.**

## Read first, in this order

1. `CLAUDE.md` (all 8 rules; "Platform traps"; "Testing in Azahar"; "Stage 10"). In a worktree the scenario
   submodule is empty: pass `SCENARIO_SRC="/Users/dashielashie/Desktop/Aleph_3ds/data/Scenarios/Marathon 2"`.
2. `docs/3ds/CITRO3D_DESIGN.md`: 0.1-0.5 (the pixel contract, goal, gates, decisions), **section 7 intro and
   "M0 — renderer harness, calibration, null mode"** (the add / touch / accept list, quoted below), section 8
   (verification: 8.1 the in-frame dual render, 8.2 content and Azahar setup, 8.3 tolerances), and section 9
   (budgets: 9.2 ring sizes M0 fixes, 9.3 where the time goes). Also section 2-4 as they bear on M0
   (the hook, the state machine 2.4, the file layout) and section 10 (UNVERIFIED items).
3. `docs/3ds/PERF.md` "How to read these numbers", the profiler section, "Stage 10 P1".
4. `ports/3ds/render_c3d/` (the present side already exists: `back/context.cpp`, `back/present.cpp`,
   `core/config.*`, `shaders/present.v.pica`) and its host tests; `ports/3ds/CMakeLists.txt`;
   `docs/3ds/PATCHES.md` (**your number is 45**; 46-48 are M2/M6/M8's; 49-53 are used; 54-55 are taken).
5. The renderer's seam: `Source_Files/RenderMain/render.cpp` (where `render_view` / the frame's textured
   polygon drawing is called), `scottish_textures.*`, `shapes.cpp`, and the `.claude/agents/` definitions
   (`c3d-architect`, `c3d-critic`, `c3d-implementer`).

## The task: M0 (section 7 of the design is the spec; this is the summary)

- **Add:** `include/a1_c3d.h`; `front/frame.cpp` (`render_frame`, config, null mode, diff harness, stats);
  `front/filter.cpp` (class / fullbright filter); `core/{diff,bmp}.cpp`; host tests under
  `scripts/3ds/tests/c3d-host/`; `c3d-diff.py` (stdlib only, run with `python3 -I`); configs
  `scripts/3ds/tests/c3d/*.cfg`. New `debug-c3d` keys as section 7 lists them (`renderer sw|c3d`,
  `mode null|readback|direct`, `diff N`, `diff_frames N`, `classes`, `fullbright`, `dump`, `calibrate`, ...);
  the profiler zones it names.
- **Touch:** the `render.cpp` seam ("hook 1"), `PATCHES.md` 45. The engine edit is fenced
  `#if defined(A1_PLATFORM_3DS) && defined(A1_RENDER_C3D)` (check the design for the exact fence) and must
  pass `scripts/3ds/host-baseline/ppcheck.sh` (Docker): IDENTICAL, or DIFFER with 0 normalised lines.
- **Accept (all Azahar, labelled "Azahar — non-authoritative"):**
  (a) `ppcheck.sh` on `render.cpp`: identical or `__LINE__` only;
  (b) readback calibration: 4 coloured corner quads on a frame-indexed clear, 30 frames report
      `orientation ok, current frame ok`; record the flags in the design;
  (c) `renderer sw`, `diff 1`, all classes, 20 frames x L0 / L6 / L20 plus the menu demo: SW-vs-SW **0**
      differing pixels (this proves the harness before any GPU code exists);
  (d) `mode null` benchmark L0 / L6 / L20: `R` and the polygon / vertex / state-change counts, max and p99
      per frame; the ring sizes in 9.2 are fixed at 2 x max and written into the design.

## Order of work

1. **Check the design against the code first.** B-a and plan A changed `present.cpp` and `context.cpp`
   after the design was approved; M0 is the first milestone built on them. In a few lines note anything where
   the design and the code disagree (names, the config parser, the profiler zone table, `debug-c3d` keys
   already taken by 5.6 / 5.7). Fix the design text first, in the doc's revision-note style, and have
   `c3d-critic` (effort medium) review that revision (max 2 rounds) **before** code.
2. **Implement with `c3d-implementer`** (M0 only, in the smallest sensible pieces: core + host tests first,
   then front, then the seam and CMake), then `c3d-critic` on the commit, max 2 revise rounds. Commit as
   `stage10: M0 <summary>`; tag `port-stage10-M0`. Quote every shell variable in helper scripts.
3. **Acceptance runs** (a)-(d) above in Azahar; the default `A1_RENDER_C3D=OFF` builds must be unchanged:
   `arm-none-eabi-nm build-3ds/alephone.elf | grep -ciE 'citro|C3D_|a1_c3d'` = 0 with 0 warnings, and the OFF
   `alephone.elf` code identical before and after (`arm-none-eabi-objdump -d`, `build-3ds` and
   `build-3ds-prof`; build the "before" from the previous commit). Also re-run the P1 acceptance scripts
   (07, 08, 13, 14, 22) so the present and the gate are known unharmed.
4. **Record** results in `PERF.md` (a "Stage 10 M0 — Azahar, non-authoritative" section), the design's M0
   section (status line, calibration flags, ring sizes), `PATCHES.md` 45, and `NEXT-SPRINT.md` plan C status.
5. **Do not start M1.** M1 is flat-shaded walls; it gets its own prompt. If a console check of M0 would be
   useful (the calibration needs the real GPU's readback orientation, which Azahar may not reproduce),
   write it as a runbook in `NEXT-SPRINT.md` for the user and stop there (rule 8).

## Standing rules and traps

- Build dirs: `build-3ds-c3d` and `build-3ds-c3d-prof` for M0; `build-3ds` and `build-3ds-prof` must keep
  building and stay identical (the OFF build). `export DEVKITPRO=/opt/devkitpro
  DEVKITARM=/opt/devkitpro/devkitARM`. Zero warnings; `-O2`; no LTO (the rasteriser has strict-aliasing UB
  and LTO drops walls); `-DNDEBUG` removes `assert()`.
- Code layout: `core/` plain C++ (host-testable), `front/` engine headers only, `back/` `<3ds.h>` /
  `<citro3d.h>` only. Engine TUs never include `<3ds.h>`. Citro3d belongs to the main thread, and the
  release / fatal paths of 5.6 must not be broken.
- Rule 1: measure before and after; rule 7: cite the libctru / citro3d headers and examples for every
  PICA200 or citro3d claim, mark the rest UNVERIFIED. The readback orientation and the "current frame"
  semantics are exactly such claims.
- Azahar: `resolution_factor=1`, `texture_filter=0`, `texture_sampling=0`, `shaders_accurate_mul=true`,
  `is_new_3ds=true` (8.2). It models instruction cost, not cache misses; use it for correctness.
- Subagents do not see this conversation: give each everything it needs. Critic and architect at medium.
- Anything needing `sudo`: give the user the command for **Terminal.app**.
- Backup bundle after the approved commit: `git bundle create ... fc4c274..3ds-port fc4c274..stage10-c3d`
  (add `fc4c274..stage10-bb` if that branch has commits), recipe in `STAGE5-PREREQS.md` section 8,
  restore-tested. **Do not push.**

## Report and stop

What you built (branch, commit, tag), the Azahar results for (a)-(d) with their label, the ring sizes and
calibration flags, the diff of `PATCHES.md`, anything the design got wrong, and the choice for the user: M1
next, or a console check of M0's calibration first.
