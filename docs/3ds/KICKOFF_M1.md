# Paste this into a fresh Claude Code session started in the stage10-c3d worktree

(`/Users/dashielashie/Desktop/Aleph_3ds/.claude/worktrees/stage10-c3d`, branch `stage10-c3d`. Written
2026-10-27. Earlier prompts: `KICKOFF_PROMPT.md` (stage 10), `KICKOFF_M0.md` (M0), `KICKOFF_PLAN_B.md`,
`KICKOFF_B_CONSOLE.md`, `KICKOFF_B_B.md` (plan B).)

---

We are on Stage 10 of the Aleph One 3DS port (the optional citro3d GPU renderer behind `A1_RENDER_C3D`).
**This session builds M1: flat-shaded walls, the first milestone where citro3d draws world geometry.**
It is Azahar-only plus host tests; the console is not needed and nothing waits on a decision. M1 runs only
on `debug-c3d` diff frames (the C3D side of the SW-vs-C3D diff); play frames stay software until M4.

## Where the project is

- P0, P1 (GPU present), G0-b, plan A (fixes) and B-a are done and console-verified. The user decided
  (2026-10-19) to build the renderer for M7 (supersampling) and M9 (stereo); speed is not the reason.
- **M0 is done** (2026-10-27, tag `port-stage10-M0` on `e971b1198`; commits `56674d2c1` M0, `4cb2db358`
  revise 1, `e971b1198` revise 2, `6d453cba3` review record). It built the seam (`render.cpp` hook 1,
  `PATCHES.md` 45), the record/replay diff harness, the class/fullbright filter, null mode (`R`), and the
  readback of an unlinked view target with a calibration. Azahar results: `PERF.md` "Stage 10 M0".
- **The M0 console check was deferred by the user (2026-10-27)**: M1 builds on a readback that only Azahar
  has run. Azahar completes GPU transfers synchronously and does not model the CPU cache, so it cannot show
  sync or cache-flush bugs. M0's critic rounds found three of them, all invisible in Azahar (design 0.3, the
  rows added in M0 revise 1 and 2). Treat every new GPU sync or flush in M1 the same way: prove it from the
  citro3d/libctru disassembly, not from an Azahar pass. Fold the deferred M0 steps into M1's console runbook
  (step 4 below).
- Plan B's zero-copy present is built **separately on branch `stage10-bb`** (worktree
  `.claude/worktrees/stage10-bb`, tag `port-stage10-B`, `PATCHES.md` 55; not merged by user decision).
  **Do not touch it, do not merge it, and do not edit `back/present.cpp` or
  `Source_Files/RenderOther/screen.cpp`** (B-b edits both). Keep doc edits to the M1 parts so the branches
  merge cleanly later.
- Hardware: one New 3DS. `CLAUDE.md` "Git": never push to `origin`; push to the fork only when the user
  asks. **Do not push.**

## Read first, in this order

1. `CLAUDE.md`: all 8 rules, "Platform traps", "Testing in Azahar" and "Stage 10". In a worktree the
   scenario submodule is empty, so pass `SCENARIO_SRC="/Users/dashielashie/Desktop/Aleph_3ds/data/Scenarios/Marathon 2"`.
2. `docs/3ds/CITRO3D_DESIGN.md`:
   - **0.3**: every row, especially the M0 revise rows:
     - an in-frame `C3D_Sync*` after draws needs `C3D_FrameSplit(GX_CMDLIST_FLUSH)` first;
     - never `C3D_RenderTargetDelete` inside a frame;
     - out-of-frame `C3D_Sync*` can return early, because of the sticky PPF event;
     - retirement is confirmed by `C3D_FrameBegin(0)`.
   - 0.5 (gates, G2).
   - 1.2-1.5 (the seam, hooks, API, fallback and discard).
   - **Section 2** (2.1 vertex format and `world.v.pica`, 2.2 the wall row of the table, 2.3 clipping and w
     clamps, 2.4 GPU state, command-buffer reserves and the GX budget).
   - 5.2 (readback mode).
   - Section 7: the intro, the **whole M0 block** (what exists now: the `debug-c3d` keys, the harness, the
     filter, null mode, the readback, the "Revise 1/2" bullets and **"Carried to M1"**), and the **M1 block**.
   - **Section 8** (8.1 the in-frame dual render with the **alpha-keeping** readback for side B, 8.2
     content and Azahar setup, 8.3 tolerances, where walls flat = 0 outside the edge band, 8.4 host tests).
   - 9.2 (ring sizes M0 fixed), 9.3, section 10 (console items, item 8 especially), section 11.
3. `docs/3ds/PERF.md`: "How to read these numbers", the profiler section, "Stage 10 P1", **"Stage 10 M0"**
   (the null-mode counts: about one draw per polygon, V max 387, I max 597 on the bench views).
4. The code:
   - `ports/3ds/render_c3d/`: `include/a1_c3d.h`, `front/frame.cpp` (`render_frame`, the diff harness,
     null mode), `front/filter.{h,cpp}` (the recorder and FilterRasterizer), `back/context.{h,cpp}`
     (`frame_begin`/`frame_end`, `gx_op`, the gate, release), `back/view.cpp` (the readback: copy its
     sequence), `back/present.cpp` (read-only), `core/*` (config, diff, bmp, stats, calibrate, classify,
     gx_queue), `shaders/present.v.pica`.
   - `ports/3ds/CMakeLists.txt` (picasso via CMake, no hand-embedded binaries) and the host tests
     `scripts/3ds/tests/c3d-host/`.
   - `scripts/3ds/tests/c3d-diff.py`, `scripts/3ds/tests/c3d/m0-*.cfg`.
5. The software reference that M1 transliterates:
   - `Source_Files/RenderMain/scottish_textures.cpp` (wall mapper 891-1001, especially `world_x` at
     916-954; polygon rejection 231-241, 449-459; Bresenham edges 1173-1219);
   - `RenderRasterize.cpp` (wall clipping and projection 502-589, pinning);
   - `render.cpp` (`render_view`, the hook);
   - `scottish_textures.h` (polygon_definition).
6. citro3d and PICA (rule 7: cite, never guess):
   - `$DEVKITPRO/libctru/include/citro3d.h`, `c3d/*` (`base.h`, `buffers.h`, `attribs.h`, `effect.h`,
     `texenv.h`, `renderqueue.h`, `maths.h`), `3ds/gpu/*`;
   - the examples in `/opt/devkitpro/examples/3ds/graphics/gpu/` (`textured_cube`, `proctex`,
     `immediate`, ...);
   - when the headers don't say, disassemble: `arm-none-eabi-ar x /opt/devkitpro/libctru/lib/libcitro3d.a renderqueue.o`
     then `arm-none-eabi-objdump -dr`.
7. `.claude/agents/` (`c3d-architect`, `c3d-critic`, `c3d-implementer`) and `PATCHES.md` (45 is M0's;
   **46-48 are reserved for M2/M6/M8**; 49-55 are used or taken; **M1 is planned without an engine edit**.
   If one turns out to be needed, take 56+, fence it, register it and run ppcheck).

## The task: M1 (section 7's M1 block is the spec; this is the summary)

- **Add:**
  - `core/geometry.cpp` (per-vertex wall data, host-tested);
  - `front/rasterizer.cpp` (walls become draw items; every other class is `unsupported` and discards the
    frame's C3D side);
  - `back/draw.cpp` (vertex/index rings, batching of consecutive same-state polygons, the reserves of 2.4,
    a constant colour per `ShapeDesc`);
  - the world vertex shader `shaders/world.v.pica` as 2.1 gives it (M1 needs position and w; texcoords can
    be constant until M2);
  - `debug-c3d film <path>`, which plays a film through `handle_open_document()` (`shell.h`:161, as
    `tests/replay_film_test.cpp`:68). The films are in `tests/replays/Marathon 2/Tooncinator Films/` and
    have to reach the emulated card: decide how and document it.
- **Carried to M1 from M0 (design M0 block, "Carried to M1", and the M0 critic):**
  1. **The alpha-keeping readback** for diff side B (8.1: coverage B = alpha ≠ 0). M0's conversion forces
     `0xFF`. Add an alpha check to the calibration (for example a corner quad with alpha 0x80).
  2. **Ring sizes:** M0's 9.2 sizes (2 × 1 024 vertices, 2 × 2 048 indices) come from three fixed bench
     views. Re-measure V/I max with null mode over the menu demo and a film or scripted walk before sizing
     the rings, then log the ring high-water each frame and at release.
  3. `GxQueue::take` charges the flushing split 2 entries where the queue holds 1, so `gx_high_water` is an
     upper bound. If the per-frame readback brings the frame near the budget of 24, charge it exactly 1.
- **Rules M1 must keep** (0.3, 2.4, 5.2):
  - a ranged `GSPGPU_FlushDataCache` of exactly the ring bytes written, before the GPU reads them;
  - `C3D_FrameSplit(GX_CMDLIST_FLUSH)` before the in-frame readback transfer;
  - the retirement wait `frame_begin`/`frame_end` pair before reading the staging;
  - target create and delete out of frame;
  - `C3D_FrameBegin` return checked;
  - every GX op through `gx_op`;
  - the command-buffer reserve checked before every draw (overflow is a crash, and a discard is not a
    recovery: `C3D_FrameSplit` frees nothing);
  - citro3d state saved and restored around the view frame, as `view.cpp` does with the BufInfo, because
    the present sets only its own state;
  - main thread only, and the release and fatal paths of 5.6 unbroken.
- **Accept** (Azahar, labelled "Azahar — non-authoritative", except the deterministic counts):
  - diff `walls`, coverage only (8.1): 0 mismatches outside the 1-px edge band and ≤ 2% inside, on
    20 frames × L0/L6/L20 plus 200 frames of a film;
  - 0 ring overflows;
  - max `C3D_GetCmdBufUsage()`, words per draw and per `C3D_FrameEnd` logged, and `kDrawReserveWords` /
    `kEndReserveWords` set to 2 × the measured worst case;
  - a `cmdbuf_kib 32` run, and a `gxq_budget 4` run (2.4, "How we test"), discard and fall back to SW
    without a crash;
  - host test: w against SW `world_x` (`scottish_textures.cpp`:916-954), relative error ≤ 1e-3;
  - the calibration's alpha check passes.

## Order of work

1. **Check the design against the code first.** The M1 block is five lines written before M0 existed; M0's
   harness, config parser, `gx_op`, frame kinds and readback now fix many of its details.
   - Write an **"M1 specification" block**, in the style of M0's (the revision-note style, citing the
     code as of the current HEAD). It should state:
     - the `debug-c3d` keys M1 adds (`film`, `cmdbuf_kib`, `gxq_budget`, ring sizes, and `renderer c3d`
       with `diff N` and `classes walls` now drawing side B in C3D);
     - how side B's frame runs inside the harness, given record/replay (one traversal, replayed into each
       side);
     - the alpha readback;
     - the reserve measurement;
     - the film path;
     - the profiler zones;
     - the 8.3 tolerance for walls.
   - Have `c3d-critic` (**effort medium**) review that revision, max 2 rounds, **before** code. Commit it
     as `stage10: docs: M1 specification (...)`.
2. **Implement with `c3d-implementer`**, M1 only, in the smallest sensible pieces:
   - core geometry plus host tests first;
   - then `back/draw.cpp` and the shader;
   - then `front/rasterizer.cpp` and the harness wiring;
   - then `film`.

   Then `c3d-critic` on the commit, max 2 revise rounds. Commit as `stage10: M1 <summary>` (one commit
   plus revise commits); tag `port-stage10-M1` when the critic approves. Quote every shell variable in
   helper scripts (an implementer once deleted files through an unquoted zsh variable).
3. **Acceptance runs** in Azahar (above), plus:
   - The default `A1_RENDER_C3D=OFF` builds must not change: `arm-none-eabi-nm build-3ds/alephone.elf | grep -ciE 'citro|C3D_|a1_c3d'`
     must print 0, with 0 warnings. The OFF `alephone.elf` must have identical code before and after:
     compare `arm-none-eabi-objdump -d` output for `build-3ds` and `build-3ds-prof`, saving the "before"
     disassembly at the start.
   - Re-run the M0 calibration (`m0-readback-cal.cfg`, `m0-readback-cal-resize.cfg`) and the P1 scripts
     07, 08, 13, 14 and 22, so the present, the gate and the readback are known unharmed.
4. **Record** results:
   - `PERF.md`: a "Stage 10 M1 — Azahar, non-authoritative" section.
   - The design's M1 block: status line, reserves, ring sizes, `C3D_Init` size.
   - `PATCHES.md`: only if an engine edit was needed.
   - `NEXT-SPRINT.md`: plan C status, and an **"M1 console check" runbook** that also carries the deferred
     "M0 console check" steps (calibration 1, 1b, 2), then M1's diff on hardware.
   - `CLAUDE.md` "State of the project".
   - Then the backup bundle (below).
5. **Do not start M2** (textured walls and floors, patch 46). It gets its own prompt.

## Standing rules and traps

- **Builds:**
  - `build-3ds-c3d` and `build-3ds-c3d-prof` for M1; `build-3ds` and `build-3ds-prof` must keep building
    and stay identical.
  - `export DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM`.
  - Zero warnings, `-O2`, and no LTO (it drops walls).
  - `-DNDEBUG` removes `assert()`.
  - Your editor's clang diagnostics on `back/*.cpp` (`'3ds.h' file not found`) are noise: it lacks
    devkitPro's include path.
- **Code layout:**
  - `core/` is plain C++ and host-testable.
  - `front/` uses engine headers only.
  - `back/` uses `<3ds.h>` / `<citro3d.h>` only.
  - Engine TUs never include `<3ds.h>`.
- **libctru trap found in M0:** `shaderInstanceFree()` frees the instance itself, so a shader instance must
  come from `malloc` (`back/view.cpp`, `find_projection_location`).
- **Azahar:**
  - Setup per 8.2: `resolution_factor=1`, `texture_filter=0`, `texture_sampling=0`,
    `shaders_accurate_mul=true`, `is_new_3ds=true`.
  - **Another session may share the emulated SD card**: before every run, wait until `pgrep -i azahar`
    finds nothing, then `mkdir ~/Applications/Azahar-2126.2/user/.run-lock`; run; `rmdir` it, also on
    failure (use a trap). Tell subagents to do the same.
  - Check each run's `[c3d] debug-c3d:` log line against the intended config.
  - `run-azahar.sh` empties `build-3ds/azahar-screens/` every run and converts the card's `c3d-*.bmp` to
    PNG there, so copy captures away before the next run.
  - Guest faults land in `~/Applications/Azahar-2126.2/user/log/azahar_log.txt`.
- **Docker** (only needed if an engine file changes: `scripts/3ds/host-baseline/ppcheck.sh`, about 1 minute):
  if `docker` commands hang, the daemon has died. It happened when the disk filled on 2026-10-26.
  `open -a Docker` and wait for `docker info` to answer; don't leave a hung `docker run` in the background.
- **Disk:** the disk filled during M0 (build dirs plus Azahar). Check `df -h /System/Volumes/Data` before
  long series.
- **Subagents** don't see this conversation, so give each one everything it needs. Run the critic and the
  architect at effort **medium**.
- **Anything needing `sudo`:** give the user the command to run in **Terminal.app**.
- **Backup** after the approved commit: `git bundle create ~/Documents/Aleph_3ds-backups/3ds-port+stage10-c3d+stage10-bb-<date>-<3ds-port>-<stage10-c3d>-<stage10-bb>.bundle fc4c274..3ds-port fc4c274..stage10-c3d fc4c274..stage10-bb port-stage10-P1 port-stage10-B port-stage10-M0 port-stage10-M1`,
  then `git bundle verify`, then a restore test: fetch `release-20260930` at depth 1 into a scratch repo,
  then the bundle, and compare tree hashes. Recipe: `STAGE5-PREREQS.md` section 8. **Do not push.**

## Report and stop

Report:
- what you built: branch, commits, tag;
- the Azahar results for each acceptance item, with their label;
- the measured reserves, the ring sizes and the `C3D_Init` size;
- anything the design got wrong, and the critic's verdicts;
- the choice for the user: M2 next, or the "M1 console check" (which now includes M0's) first.
