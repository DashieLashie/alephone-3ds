# Paste this into a fresh Claude Code session started in the stage10-c3d worktree

**Status: done (2026-11-07, one session; tag `port-stage10-M6`, `PERF.md` "Stage 10 M6").** No session 2 was needed.

(`/Users/dashielashie/Desktop/Aleph_3ds/.claude/worktrees/stage10-c3d`, branch `stage10-c3d`. Written
2026-11-07 at the end of M5. Earlier prompts: `KICKOFF_PROMPT.md` (stage 10), `KICKOFF_M0.md` … `KICKOFF_M5.md`.)

---

We are on Stage 10 of the Aleph One 3DS port (the optional citro3d GPU renderer behind `A1_RENDER_C3D`).
**This session builds M6: direct mode (HUD + Map)**, the renderer's only performance payoff. M0-M5 built a
complete renderer in **readback mode**: every world frame is drawn by citro3d, read back into the engine's
surface, and presented as before; that costs more than the software renderer (G2-a failed, below). Direct mode
keeps the view on the GPU: the view frame renders into a texture, the CPU overlays (crosshair, FPS, messages)
go to a separate surface, and the present composites the two. Readback and the copy disappear on eligible
frames.

**User decisions that frame this milestone (2026-11-07):** stage 10 builds **M6 → M7 (2x2 supersampling) → M8
→ M9 (stereoscopic 3D)** whatever the gates show; every gate (G3-a, G3-b, …) is measured and recorded, none
decides. M9 needs direct mode (design section 6).

**The user is short on time and on usage.** Be economical: read only what each step needs, run `c3d-critic` and
`c3d-architect` at effort **medium**, batch Azahar runs into one serial script, do not re-run a series that has
passed on the same binary. Implement inline (no `c3d-implementer`), as M2-M5 were. Ask nothing mid-run unless a
decision is genuinely the user's (a tolerance change always is). **M6 is probably two sessions** (spec, the
texture-cache hash and the direct path on diff frames; then overlays, eligibility, the composite's calibration,
the acceptance and G3-a): stop at a clean, committed piece and add a "Session 2" header to the top of this file.

## Where the project is

- P0, P1 (GPU present), G0-b, plan A and B-a are done and console-verified.
- **M0-M5** (tags `port-stage10-M0` … `-M5`; `PERF.md` "Stage 10 M0" … "Stage 10 M5"; Azahar only): the seam
  (`PATCHES.md` 45), the record/replay diff harness, walls, textures and the cache (`PATCHES.md` 46), the shade LUT
  on unit 1, sprites/weapons/tinted, **play mode on** (a world frame recorded once, replayed into C3D, read back),
  landscapes (two texture parts: a Marathon 2 sky is 540 × 1 024, part 0 is 1 024 × 512 RGBA8 = **2 MiB**), static
  (LFSR noise on unit 1), eviction hardening (`fail_every`, `vram_cap`, the cache audit, promotions blocked for their
  generation when the heap refuses). Since M5 **every play frame is C3D** (`scan` 0). Both Azahar backends pass.
- **G2-a failed, as predicted** (`PERF.md` "Stage 10 M5"): busy(readback) − busy(SW) = **+2.04 / +4.88 / +3.51 ms**
  on L0 / L6 / L20 (limit +1 ms); `c3d_draw` 8.4-10.2 ms (it includes the wait for the GPU) + `c3d_readback` 2.18 ms
  against SW's `render` 8.6-11.2 ms. `R_est` (the front-end floor) passes at ≤ 0.62 ms. This is the time M6 is meant
  to win back: the readback, the copy, and the CPU waiting for the GPU inside `c3d_draw`.
- **Plan B's zero-copy present** is on its own branch **`stage10-bb`** (tag `port-stage10-B`, `PATCHES.md` 55, **not
  merged**, user decision 2026-10-26; B-a measured below the 0.5 ms rule on the console). It edits
  `Source_Files/RenderOther/screen.cpp` (+7) and `back/present.cpp` (+166), the two files M6 must touch too. See
  "Decisions" below.
- **Console:** the M1 and M2 short checks passed; the "M3/M4/M5 console check, short version" runbooks
  (`NEXT-SPRINT.md`) are **not run**. None is this session's job.
- Hardware: one New 3DS. `CLAUDE.md` "Git": never push to `origin`; push to the fork only when the user asks.
  **Do not push.**

## Read first, in this order (only these)

1. `CLAUDE.md` (the 8 rules, "Platform traps", "Testing in Azahar", "Stage 10"). Scenario in a worktree:
   `SCENARIO_SRC="/Users/dashielashie/Desktop/Aleph_3ds/data/Scenarios/Marathon 2"`.
2. `docs/3ds/CITRO3D_DESIGN.md`:
   - **0.3** (work starts at `C3D_FrameEnd`), **0.5** (the gate table: **G3-a**, G3-b), the user decisions under it;
   - **5.1** (the P1 present: VBlank guard, the gate, suspend), **5.3** (direct mode: two C3D frames per game frame,
     overlay surface, bands not boxes), **5.4** (eligibility, else readback for that frame), **5.5** (risks, how we
     test without hardware), **5.6** (suspend and `error_screen()` off the main thread), **5.7** (B: what `stage10-bb`
     does, to judge the overlap);
   - section 6 (why M9 needs direct mode), section 7's **M5 status**, the **M6 block** (short: the spec you write
     replaces it) and M7's (to know what not to do), **9.1-9.2** (VRAM: M5's note on the 2 MiB part and promotion
     behaviour), section 10 (items 6 and 10, the frame-indexed test, `alt_load`);
   - the Review log entries "M4 commit review" (minor 4: the cache lookup scans 512 slots), "M5 code review, round
     1 / 2", "M5 acceptance".
3. `docs/3ds/PERF.md`: "How to read these numbers", "Stage 10 P1" (the present's cost), "Stage 10 M5" (the bench and
   G2-a). `git show stage10-bb:docs/3ds/PERF.md` "Stage 10 B-b" only if the B decision needs it.
4. The code: `back/present.cpp` (the GPU present, `present_diff`, the VBlank guard), `back/context.*`,
   `front/frame.cpp` (`run_play_frame`, readback), `back/draw.cpp`, `core/texcache.*` (`lookup`), `core/config.*`;
   `Source_Files/RenderOther/screen.cpp` (`render_screen`, the world-view blit, `native_screen()` :128-131,
   `using_default_gamma` :152, the overlay drawing, `screen_shared.h` :550-777) and `PATCHES.md` 45/46 (how the
   existing hooks are fenced). Cite lines; never paraphrase engine code from memory.
5. citro3d (rule 7, cite, never guess): `c3d/renderqueue.h` (`C3D_RenderTargetCreateFromTex` :63, frame begin/end,
   linking), `c3d/texture.h`, `gx.h` (display transfer flags, :33-38), `/opt/devkitpro/examples/3ds/graphics/gpu/`
   (render-to-texture examples).

## The task: M6 (section 7's M6 block; summary)

- **Touch:** `screen.cpp` hook 3 (**`PATCHES.md` 47**, reserved): on an eligible frame, overlays go to
  `overlay_surface()` (400x240 ARGB8888 with alpha) and the world-view blit is skipped.
- **Add:** the view frame into a texture-backed target (`C3D_FrameEnd` lets the GPU run while the CPU draws the
  overlays and the bottom screen); the composite frame in `a1_c3d_present()` (view quad `ONE, ZERO`, overlay bands
  `ONE, ONE_MINUS_SRC_ALPHA`); overlay bands (8-row aligned, ≤ 4 `gx_op` per frame, last frame's bands cleared);
  per-frame eligibility (5.4) with readback as the fallback; the per-frame mode logged.
- **Prerequisite (M4 commit review minor 4):** the texture cache's `lookup` scans all 512 slots per new draw key;
  a hash on `(bitmap, tables)` before direct mode, **measured** (rule 1).
- **Accept:** direct vs readback captures with the overlays on, ≤ 1 per channel; ineligible frames (map, terminal,
  gamma, fades, Classic) logged as readback; the frame-indexed test in direct mode shows N or N−1 at @30 and @60
  with `alt_load`; GX high-water ≤ 24; scripts 07, 13, 14 pass; the M1-M5 gates unchanged; present diff 0 where it
  still applies; OFF builds unchanged. Then **G3-a** (VRAM 9.1, command-buffer and queue budgets at 800x480 in
  Azahar) measured and recorded, and the **C3D-vs-SW bench** (direct mode vs SW on L0/L6/L20, HUD + Map @30 and @60):
  this is the first bench that can show a speedup.

## Decisions the spec must settle (and which are the user's)

- **B-b and M6 (the user's).** M6 edits `screen.cpp` and `present.cpp`; `stage10-bb` edits both. Options: (a) build
  M6 on `stage10-c3d` without B and merge B later (conflicts land on whoever merges second); (b) merge `stage10-bb`
  first, then build M6 on it (B's zero-copy then serves the readback frames M6 still has: menus excluded, Classic,
  ineligible frames). Write down what each costs in the spec; **ask before merging anything**.
- **Patch 47's non-3DS check needs Docker** (`scripts/3ds/host-baseline/ppcheck.sh`). The Docker daemon was not
  running at the end of M5: the user starts Docker Desktop (no sudo). Check `docker info` early; if it is down, ask.
- **The overlay surface's memory:** 400x240 ARGB8888 = 375 KiB, linear or VRAM; with the view texture (512x256
  RGBA8 = 512 KiB, plus depth) against 9.1's budget and M5's landscape parts (2 MiB + 128 KiB, VRAM when they fit).
  **Landscapes at M7's 800x480 targets:** decide in M6's spec whether the landscape part stays RGBA8 in VRAM, drops to
  RGB8/RGB565, or stays in FCRAM (design 9.1, M5 note; deferred to M7 but the budget is set here).
- **Eligibility conditions** (5.4) against the code at HEAD: `native_screen()`, overhead map, terminal,
  `using_default_gamma`, `get_keyboard_controller_status()` (`darken_world_window`), fades and flashes (the engine
  edits pixels after the view: check every path that writes into the world view after `render_view`), Lua HUD.
  A missed condition shows an un-faded view: list each with its line.
- **The band orientation** in the tiled overlay texture is **UNVERIFIED** (5.3): the spec says how M6's calibration
  (a frame-indexed band pattern) settles it.
- **Profiler zones** for direct mode (the GPU wait now moves to the composite's `C3D_FrameBegin`), so the bench can
  say where the time went.

## What M5 taught (read before designing)

- **Check the kickoff against the code.** This file can be wrong: verify every claim it makes at HEAD.
- **Test spots may not show the class you gate** (L20 and two films had no sky; per-film static stayed under the
  40 000-pixel minimum). Look at what each spot shows before writing the gate list. Overlays are opt-in: FPS,
  position, messages and crosshair must be switched on for the captures (a config key or a test plugin).
- **Count your run's length before asking for N checks** (M5's static film asked for 800 checks of a ~488-check film
  and never printed its summary). **Killed runs print no final line**: `present_diff final`, `[c3d] play final` and
  similar come only on a clean exit (`--perf` runs exit cleanly; films and HOME scripts are killed): prefer `--perf`
  spots for anything that needs a final line.
- **Azahar's software backend is not a GPU:** one-column-late float24 misses on the span path, FCRAM+VRAM in one
  draw faults (`-swbe` configs, `gxq_budget 3`). Bisect before blaming the renderer.
- **Every piece of state is restored and counted** (unit 1, TEV, blend, static); direct mode adds render targets and
  a second frame per game frame: count frames begun/ended, targets bound/restored.
- **The heap can refuse what the cap allows** (M5's promotion fix): any new VRAM block (view texture, overlay) is
  allocated with a fallback and counted, never assumed.
- **Traps:** never edit a config or script while an Azahar run uses it (appending to a running bash script works,
  editing it does not); Azahar only through the lock (`mkdir ~/Applications/Azahar-2126.2/user/.run-lock`, `rmdir`
  in a trap); copy `build-3ds/azahar-screens/` away after each run; `df -h /System/Volumes/Data` before long series;
  helpers in `build/m6/tools/` (copy `build/m5/tools/run1.sh` and `series.sh`); quote every shell variable,
  `${var:?}` in `rm`; OFF builds' `objdump -d | tail -n +3` saved at the start for a plain `cmp` at the end; in zsh
  never `echo =====`; a `cd` persists between Bash calls: use absolute paths.

## Order of work

1. **Check the design against the code first** and write an **"M6 specification" block** in section 7 in the style of
   M5's (revision notes citing the code at HEAD): the hash (measured before and after), the view target and its
   memory, hook 3 (patch 47's exact text and fence), the overlay surface and bands, the composite frame, eligibility
   with every condition's line, the frame-indexed calibration, suspend/HOME/`error_screen()` with two frames in
   flight, profiler zones, configs (`m6-*.cfg`, `-swbe`), host tests (band rounding and merging, eligibility table,
   the hash), acceptance, **how G3-a and the bench are measured**, and the B-b decision's options. `c3d-critic`
   (effort **medium**), **max 2 rounds**, before code. Commit `stage10: docs: M6 specification (...)`. Ask the user
   the B-b question and any tolerance change before step 2.
2. **Implement** in pieces, each built (4 builds, 0 warnings) and host-tested: the texture-cache hash (bench before
   and after) → the view into a texture + composite on diff frames, no overlays → hook 3 (patch 47, `ppcheck.sh`
   IDENTICAL or 0 normalised lines, `PATCHES.md` entry) and overlay bands → eligibility and fallback → play mode in
   direct mode → configs. Then `c3d-critic` on the commit, max 2 revise rounds. Commit `stage10: M6 <summary>`; tag
   `port-stage10-M6` on approval.
3. **Acceptance** in Azahar on the final binary, one serial script, both backends; the bench on L0/L6/L20 @30 and @60
   (direct vs SW, and vs M5's readback numbers) and G3-a; the OFF builds' `objdump -d` compared with the start, `nm` 0.
4. **Record:** `PERF.md` "Stage 10 M6" (Azahar — non-authoritative); the design's M6 status and Review log;
   `NEXT-SPRINT.md` plan C status and an **"M6 console check, short version"** (direct vs readback on L0, overlays
   on, HOME/sleep/Close with two frames in flight, the bench @30/@60 = the first real speed test, G3-b's
   `C3D_GetDrawingTime`); `PATCHES.md` 47; `CLAUDE.md` "State of the project"; the backup bundle (recipe
   `STAGE5-PREREQS.md` section 8, tags up to `port-stage10-M6`, `git bundle verify`, restore test) and the project
   memory's stage 10 entry.
5. **Do not start M7.** Report and stop.

## Standing rules

- Builds: `build-3ds-c3d`, `build-3ds-c3d-prof` for M6; `build-3ds`, `build-3ds-prof` must keep building and stay
  identical (patch 47 is fenced `#ifdef A1_RENDER_C3D` inside `A1_PLATFORM_3DS`, so the OFF objects must not change:
  check). `export DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM`. Zero warnings, `-O2`, no LTO. Layout:
  `core/` plain C++ (host-tested), `front/` engine headers only, `back/` `<3ds.h>`/`<citro3d.h>` only; engine TUs
  never include `<3ds.h>`; shaders via picasso in CMake.
- Anything needing `sudo`: give the user the command for Terminal.app.

## Report and stop

What was built (commits, tag); the hash's measured effect; the Azahar results per acceptance item with their label
(both backends); the per-frame mode counts (direct / readback and why); the overlay captures; GX high-water, command
words, VRAM use; **the bench, direct mode vs SW and vs readback, @30 and @60**, and G3-a's numbers; anything the
design got wrong and the critic's verdicts; the B-b decision as taken; and the user's choice for what comes next
(M7, the M3-M6 short console checks first, or something else). Do not push.
