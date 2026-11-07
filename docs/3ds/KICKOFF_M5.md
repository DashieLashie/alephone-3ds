# Session 2 (written 2026-11-07 at the end of session 1)

Session 1 did more than planned: the **spec** (`8566bdc5b`, critic REVISE then APPROVE in round 2) and the **whole M5 code**
(committed as "stage10: M5 session 1", no tag): landscapes (cut at row 512: the bitmaps are 540 × 1 024, design 7 M5
"Implementation, session 1" and Review log "M5 implementation, landscape size"), static (256² LFSR noise, the Lit/Static
unit-1 switch), `_solid` skipped, the play scan, `fail_every`, the cache audit, level-start and burst logs, `c3d-diff.py
--static`, the `m5-diff`, `m5-film`, `m5-static-film`, `m5-play`, `m5-play-film` configs (and `-swbe`). Host tests 0 failures;
4 builds 0 warnings; OFF builds identical, `nm` 0. Smoke runs passed (L6 landscapes exact; L06 static +0.10 points; L01 play
all C3D). Films chosen: L01, L06, L10, L16, L21.

**Session 2, in order:** (1) **ask the user** the pending decision: the play check leaves SW's static coverage out
(design 7 M5 "Diff checks", round 1 minor 3) — first L01 play run: 9 such checks, 2 795 pixels left out of 14.5 M;
(2) write the remaining configs: `m5-vram512.cfg`, `m5-fail50.cfg` (play over the L01 film, gate `forced_fails ≥ 10`),
`m5-fail1.cfg` (+ `--level-hop-test`, `--expect-offs 2`), `m5-present-diff.cfg` (play + a static film stretch:
`static_sets > 0`), `m5-bench-c3d.cfg` / `m5-bench-sw.cfg` (copies of M4's); `c3d-diff.py play` should show `static_px`;
(3) `c3d-critic` (effort medium) on the session-1 commit + fixes, max 2 rounds; (4) the acceptance series
(`build/m5/tools/`: copy M4's `series.sh` and adapt; `run1.sh` and `screen.sh` are there) on both backends; (5) the bench
and **G2-a**; (6) the records, tag `port-stage10-M5`, bundle, memory; (7) report G2-a with the options and stop.

---

# Paste this into a fresh Claude Code session started in the stage10-c3d worktree

(`/Users/dashielashie/Desktop/Aleph_3ds/.claude/worktrees/stage10-c3d`, branch `stage10-c3d`. Written
2026-11-07. Earlier prompts: `KICKOFF_PROMPT.md` (stage 10), `KICKOFF_M0.md` … `KICKOFF_M4.md` (the milestones
before this one), `KICKOFF_M2_CONSOLE.md` (M2's short console check).)

---

We are on Stage 10 of the Aleph One 3DS port (the optional citro3d GPU renderer behind `A1_RENDER_C3D`).
**This session builds M5: landscapes, static, eviction and fallback hardening** — the last milestone of
readback mode. After it, every world frame the engine can produce should be drawable by citro3d (the frames
that still go to SW are the ones the design sends there on purpose: see-through liquids, `_solid`, resource
discards). M5 ends at **gate G2-a** (design 0.5), which decides whether M6 (direct mode, the only performance
payoff) is worth building. Readback mode brings **no speedup**; M5's job is correctness and robustness.

**The user is short on time and on usage.** Be economical: read only what each step needs, run the critic and
the architect at effort **medium**, batch Azahar runs into one serial script, and do not re-run a series that
has already passed on the same binary. Implement inline in the main session (no `c3d-implementer`), as M2-M4
were. Ask nothing mid-run unless a decision is genuinely the user's (a tolerance change always is; so is G2-a's
outcome, below). **M5 is probably two sessions** (landscapes and static on diff frames, then play mode, the
hardening runs, the films and G2-a): stop at a clean, committed piece and add a "Session 2" header to the top of
this file, as `KICKOFF_M2.md` has.

## Where the project is

- P0, P1 (GPU present), G0-b, plan A and B-a are done and console-verified. Plan B's zero-copy present is on its
  own branch `stage10-bb` (tag `port-stage10-B`, `PATCHES.md` 55, **not merged**: do not touch it, do not edit
  `back/present.cpp` or `Source_Files/RenderOther/screen.cpp`).
- **M0-M3** (tags `port-stage10-M0` … `-M3`): the seam (`PATCHES.md` 45), record/replay diff harness, null mode,
  readback calibration; walls; textures and the cache (`PATCHES.md` 46); the shade LUT on unit 1, two TEV stages,
  infravision, flash, negative ambient; **unit 1 unbound after every lit frame** by a direct store into
  `__C3D_Context` (`back/shade.cpp`; `C3D_TexBind(1, NULL)` dereferences NULL + 12).
- **M4** (tag `port-stage10-M4`, 2026-11-06; `f389f85a5`, `2de08146a`, record `4a43e9b75`; `PERF.md` "Stage 10
  M4"): rectangles as one quad each (SW's fixed-point steps evaluated at the corners, `rect_bias` 1/131072
  texel), RLE and non-square textures, the A8 mask, the tinted constant-colour blend (restored after the frame),
  and **play mode on** in readback: a world frame is recorded once and replayed into C3D, or into SW when the
  `PlayScan` finds a reason (`landscape/static/other/bitmap/wrap/flash`) or C3D discards; 10 resource discards
  in a row → off until the texture-cache generation changes (a level load). Gates (user decisions): sprites and
  weapons by M2's neighbourhood rule at M3's lit numbers, the full frame over every pixel (`Coverage::All`, ≥ 97 %
  Δ ≤ 4, **landscapes and static excluded on both sides**). Command words up to 3 432 per view frame, draw
  reserve 564, rings 2 048 / 4 096 (high-water 1 440 / 2 154), 512 cache slots.
- **What M4 left to M5, in numbers:** the L01 film in play mode sent **2 646 landscape and 269 static** frames to
  SW; L0's and L6's bench spots are **all landscape** (0 C3D play frames), so M4's bench only measured L20.
- **G2-a is already in trouble.** Its second half is "Azahar busy(readback) ≤ busy(SW) + 1 ms". M4's L20 bench:
  SW busy 15.62 ms, play mode 19.11 ms (**+3.49 ms**: `c3d_draw` 8.37 + `c3d_readback` 2.18 against SW's `render`
  8.64). Landscapes will move L0 and L6 onto the same path. Read this before designing anything (below).
- **Azahar's software backend faults when one draw samples FCRAM and VRAM textures together** (an emulator bug,
  design Review log "M3 acceptance, software-backend films"): every software-backend run uses a `-swbe` config
  with `gxq_budget 3`. Note that `vram_cap 512` forces a mix of VRAM and linear textures, so its software-backend
  run may fault for that reason alone: bisect before blaming the renderer.
- **Console:** the M1 and M2 short checks passed; the "M3 console check, short version" and the "M4 console
  check, short version" (`NEXT-SPRINT.md`) are **not run**. Neither is this session's job.
- Hardware: one New 3DS. `CLAUDE.md` "Git": never push to `origin`; push to the fork only when the user asks.
  **Do not push.**

## Read first, in this order (only these)

1. `CLAUDE.md` (the 8 rules, "Platform traps", "Testing in Azahar", "Stage 10"). In a worktree the scenario
   submodule is empty: `SCENARIO_SRC="/Users/dashielashie/Desktop/Aleph_3ds/data/Scenarios/Marathon 2"`.
2. `docs/3ds/CITRO3D_DESIGN.md`:
   - **0.5** (the gate table: **G2-a** and G2-b, and what a failed b-half reopens);
   - **1.5** (per-frame selection, discard, "10 in a row"), **2.2's rows Landscape and Static**, 2.4 (state,
     reserves, GX budget);
   - **3.2** (the landscape padding: 270 → 512 by repeating the last real line so `GPU_CLAMP_TO_EDGE` = SW's `PIN`;
     `VertRepeat` splits), **3.3** (cache, pools, eviction, the level-start burst, the "How we test" paragraph:
     `vram_cap 512`, ≥ 16 new textures in one frame, `gxq_budget 4`);
   - **4.2-4.3** (landscapes shadeless `(1, 1)`; the **static** row: a 128x128 RGBA8 noise texture **on unit 1**,
     texcoord1 = screen position/128 + a per-draw offset, `A = (TEX0.A ×2)·noise.A`, alpha test `GEQUAL drop>>8`;
     the "approximated effects" list);
   - section 7: the **M4 specification and status** (what exists now, and "Not in M4"), the **M5 block** (short:
     the spec you write replaces it), M6's (to know what not to do);
   - **8.1**, **8.3** rows "landscape (M5)", "static (M5)", "full frame (M4+)", 8.4, 8.5; 9.1-9.2 (VRAM budget);
     section 10 (pre-warming, level-start latency); the "Runtime config" paragraph at the head of section 7 that lists
     `vram_cap` and `fail_every` (around line 1795);
   - the Review log entries "M4 pre-implementation, gates", "M4 commit review", "M4 acceptance, sprite gate",
     "M4 acceptance, play gate on killed runs", and item 10 "Landscape clamp" / 12 "Static noise follows
     surfaces" of the round that lists them.
3. `docs/3ds/PERF.md`: "How to read these numbers", "Stage 10 M0" (the null-mode `R` and `R_est` table G2-a uses),
   "Stage 10 M4" (the play table and the C3D-vs-SW bench).
4. The code as M4 left it, `ports/3ds/render_c3d/`: `front/filter.*` (`frame_excluded`, `RectSupport::Static`,
   the class of a polygon), `front/frame.cpp` (`PlayScan`, `run_play_frame`, `play_discard`, the summaries),
   `front/c3d_rasterizer.*`, `core/geometry.*`, `core/tex_convert.*` (no landscape kind yet: check),
   `core/texcache.*` + `back/texcache.*` (eviction, `vram_cap`, `linear_cap`, `alloc-fail`), `back/draw.cpp`
   (state save/restore, TEV, blend), `back/shade.*` (unit 1), `core/config.*`; `scripts/3ds/tests/c3d-diff.py`
   (log and play gates) and `build/m4/tools/` (`run1.sh`, `series.sh`: copy them to `build/m5/tools/`).
5. Engine, for the SW reference (cite lines, never paraphrase from memory): `scottish_textures.cpp` the landscape
   mapper (:1102-1170: per-line `source_x`/`source_y`, `PIN`, `VertRepeat`), :222-225 and :436-439 (transfer
   dispatch), `low_level_textures.h` the static loops (:590-645: SW's LFSR and colour formula; drop threshold),
   `render.cpp`:796-831 (static, 50 %, fade-out, pulsating, fold in/out → `_static` and `transfer_data`),
   `RenderRasterize.cpp` (where landscapes get their `ambient`/shading), `OGL_Render.cpp` for how the GL renderer
   maps landscapes (`LandscapeOptions`, for comparison only).
6. citro3d (rule 7, cite, never guess): `c3d/texture.h` (wrap modes, sizes up to 1024), `c3d/texenv.h`,
   `enums.h` (`GPU_RGB565`, alpha test, TEV scale), `c3d/effect.h`.
7. `.claude/agents/` and `PATCHES.md`: M5 should need **no engine edit**; 47/48 are reserved for M6/M8; if one is
   unavoidable, take 56+.

## The task: M5 (section 7's M5 block; summary)

- **Add:** landscape polygons (`_big_landscaped`: affine UVs per SW's per-line mapper, the 3.2 padding, splits at
  `VertRepeat` rows, shadeless); static on polygons and rectangles (`_static`: the noise texture, the drop
  threshold, transparent vs non-transparent bitmaps per 4.3); diff classes `landscape` and `static`; the play
  scan stops sending landscape and static frames to SW; **eviction and fallback hardening**: `vram_cap 512`
  runs, a new `fail_every N` (forced allocation failures, design config list), the first-10-frames upload counts
  logged, every failure a counted discard and never a wrong picture.
- **Accept (design M5 block, 8.3):** diff `landscape` (L6) coverage ≤ 0.1 %, colour ≥ 98 % Δ ≤ 2; `static`
  written-pixel density within ±2 points of SW; `vram_cap 512` and `fail_every 50` runs pass with evictions and
  discards counted; 5 films × 3 min with diff checks every 30 frames within tolerance; M4's play scripts and
  films pass again with landscapes and static now drawn by C3D; the full frame with landscapes **included**;
  everything M1-M4 accepted still passes, `present_diff` 0, the restore counters 0, OFF builds unchanged. Then
  **G2-a** measured and reported (below). Both Azahar backends (software backend at `gxq_budget 3`).

## Decisions the spec must settle (and which are the user's)

- **G2-a, before any code.** Write down exactly how it is measured (levels, configs, `R_est` from M0's table) and
  what M4's numbers already say: L20 is +3.49 ms against a +1 ms limit, and Azahar understates copies ~40 %
  (`PERF.md` "How to read these numbers"), so the console (G2-b) would likely be worse, not better. Estimate what
  landscapes do to L0/L6. **What happens on a failed G2-a is the user's decision**: M6 removes the readback and
  the copy, which is where the extra time is, so "go to M6 anyway" is a reasonable option; so are "stop at
  readback" and "measure on the console first". Present it in plain language with the numbers, at the end of
  the session, not by changing the gate.
- **The colour gates for landscapes and static.** 8.3's landscape row (≥ 98 % Δ ≤ 2) predates M2's neighbourhood
  rule; landscapes are full bright, so M2's full-bright numbers may apply as they are. Static's noise pattern is
  approximated by design: say how "written-pixel density" is measured on both sides (per draw? per frame?), and
  whether static stays excluded from the full frame (8.3 says yes). **Any change to a tolerance, or to what the
  full frame includes, is the user's decision** (Review log): present it with numbers and wait.
- **Unit 1 is shared.** M3 put the shade LUT on unit 1; 4.3 puts the static noise on unit 1 too. A static draw
  changes unit 1's texture and TEV; the next lit draw must get the LUT and M3's stages back, and the frame must
  still end with unit 1 unbound (`shade_check_bound`, `unit1_left_bound`). Specify the state machine, its restore
  counters, and the present-diff proof after a frame with static.
- **Landscape textures.** 512-tall RGBA8 is 1 MiB per landscape (270x512 padded); VRAM cap is 3 584 KiB and the
  linear cap 4 096 KiB. Pick the format (RGB565 if no alpha is needed: halve it, but host-test the conversion)
  and say how it fits beside a level's walls, sprites and the LUT. Check what `tex_convert` and the cache's
  slot shapes accept today.
- **Which frames still go to SW**, and why. With landscape and static drawn, list what is left in `PlayScan`
  (`other`, `bitmap`, `wrap`, `flash`) and whether any of it is cheap to take now; anything not taken stays as a
  counted reason.
- **`fail_every N`.** Not implemented (only `vram_cap` and `linear_cap` exist in `core/config.cpp`). Define it
  (every Nth texture allocation fails? every Nth view frame discards?), and what the acceptance run proves: the
  frame is redrawn by SW, the "10 in a row" rule behaves, and nothing leaks across the discard.
- **The films.** Five films × 3 min, chosen so landscapes (outdoor levels), static (teleports, the 50 %/fade
  transfer modes) and eviction pressure are all on screen; say which and why (`tests/replays/Marathon 2/Tooncinator
  Films/`, 30 films). Check with a first run that each class is covered (`--min-covered`) before gating on it.

## What M4 taught (read before designing)

- **Check the kickoff against the code.** M4's kickoff called the RLE path "written and host-tested"; it was not.
  This file can be wrong too: verify every claim it makes about the code at HEAD before relying on it.
- **Test spots may not show the class you gate.** M4's L6/L20 bench spots had no sprites, so the first series'
  sprite gates failed on `--min-covered` (correctly). Look at what each spot and film shows before writing the gate
  list; a narrowed gate is the user's decision.
- **Gate each check on its own**, not pooled (the critic's one major on M4's code); `--min-covered`,
  `--min-covered-checks`, `--strict` and `play` exist in `c3d-diff.py`. `[c3d] play final` prints only on a clean
  exit; killed runs (films, HOME scripts) fall back to the last per-second line.
- **Every piece of state you change is restored before the present and counted** (M3: unit 1, TEV stage 1; M4:
  the blend, `blend_sets` = `blend_restores`). Static adds unit 1's texture and TEV stages again.
- **Azahar's software backend is not a GPU** (texcoords on an enabled unit; FCRAM+VRAM in one draw): bisect before
  blaming the renderer. **Rings, reserves and pools are measured, not guessed:** landscapes are big polygons with
  splits, static adds a texture per draw: log the high-water marks on the films and size from them.
- **Command words are deterministic per scene**: re-measure `view_frame_words` / `draw_words` and the draw reserve
  (564); M3's diff stayed at 1304 through M4 (no word added without rectangles): keep that true for frames
  without landscapes or static.
- **Traps:** never edit `run-azahar.sh` or a config while an Azahar run uses it; run Azahar only through the lock
  (`mkdir ~/Applications/Azahar-2126.2/user/.run-lock`, `rmdir` in a trap, also on failure; wait for
  `pgrep -i azahar` to be empty); copy `build-3ds/azahar-screens/` away right after each run; check
  `df -h /System/Volumes/Data` before long series; keep helpers in `build/m5/tools/`, not the scratchpad; quote
  every shell variable; save the OFF builds' `objdump -d` **without its two header lines** (`| tail -n +3`) so the
  end-of-session compare is a plain `cmp`; in zsh, never `echo =====` (it is a command lookup).

## Order of work

1. **Check the design against the code first** and write an **"M5 specification" block** in section 7, in the
   style of M4's (revision notes citing the code at the current HEAD): landscapes (mapper, padding, splits,
   format, cache shape), static (noise texture, unit 1 state machine, drop threshold, polygons and rectangles),
   the diff classes and their gates, the play-scan changes, eviction and `fail_every`, the level-start counts,
   reserves/rings/pools, profiler zones, configs (`m5-*.cfg`, `-swbe` variants), host tests (landscape UVs vs a
   transliteration of :1102-1170, the padding and the split rows; the noise generator vs SW's LFSR statistics;
   the RGB565 conversion if chosen), the films, acceptance, and **how G2-a is measured**. `c3d-critic` (effort
   **medium**) reviews it, **max 2 rounds**, before code. Commit `stage10: docs: M5 specification (...)`. If a gate
   changes a tolerance, ask the user before step 2.
2. **Implement** in pieces, each built (4 builds, 0 warnings) and host-tested: landscapes on diff frames → static
   (polygons, rectangles) → play scan → eviction hardening and `fail_every` → configs, films. Then `c3d-critic` on
   the commit, max 2 revise rounds. Commit `stage10: M5 <summary>`; tag `port-stage10-M5` on approval.
3. **Acceptance** in Azahar on the final binary, in one serial script, both backends; the C3D-vs-SW bench on
   L0/L6/L20 (now all three should be C3D frames) and G2-a's numbers; the OFF builds' `objdump -d` compared with
   the start, `nm` 0.
4. **Record:** `PERF.md` "Stage 10 M5" (Azahar — non-authoritative; the bench and **G2-a's result**); the design's
   M5 status and the Review log; `NEXT-SPRINT.md` plan C status and an **"M5 console check, short version"** at
   the end (a landscape diff on L6, a static check, a `vram_cap 512` run, and the G2-b bench: readback vs SW on
   L0/L6/L20); `CLAUDE.md` "State of the project"; the backup bundle (recipe `STAGE5-PREREQS.md` section 8, the
   command as in `KICKOFF_M1.md` with the tags up to `port-stage10-M5`, then `git bundle verify` and the restore
   test) and the project memory's stage 10 entry.
5. **Do not start M6.** G2-a's outcome and what to do next are the user's.

## Standing rules

- Builds: `build-3ds-c3d`, `build-3ds-c3d-prof` for M5; `build-3ds`, `build-3ds-prof` must keep building and stay
  identical. `export DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM`. Zero warnings, `-O2`, no LTO.
  Layout: `core/` plain C++ (host-tested), `front/` engine headers only, `back/` `<3ds.h>`/`<citro3d.h>` only;
  engine TUs never include `<3ds.h>`; shaders via picasso in CMake.
- Docker for `ppcheck.sh` only if an engine file is edited (it should not be).
- Anything needing `sudo`: give the user the command for Terminal.app.

## Report and stop

What was built (commits, tag); the Azahar results per acceptance item with their label (both backends; the five
films; play mode's frames per reason before and after M5); the colour gates used and whether the user changed
them; the reserves, rings, pools, evictions, forced failures and command words; the C3D-vs-SW bench on
L0/L6/L20 and **G2-a: pass or fail, with the numbers, and the options in plain language**; anything the design
got wrong and the critic's verdicts; and the user's choice for what comes next (M6, the M3/M4/M5 short console
checks first, or stop at readback). Do not push.
