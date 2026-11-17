# Paste this into a fresh Claude Code session started in the stage10-c3d worktree

(`/Users/dashielashie/Desktop/Aleph_3ds/.claude/worktrees/stage10-c3d`, branch `stage10-c3d`. Written
2026-11-06. Earlier prompts: `KICKOFF_PROMPT.md` (stage 10), `KICKOFF_M0.md` … `KICKOFF_M3.md` (the milestones
before this one), `KICKOFF_M2_CONSOLE.md` (M2's short console check).)

---

We are on Stage 10 of the Aleph One 3DS port (the optional citro3d GPU renderer behind `A1_RENDER_C3D`).
**This session builds M4: sprites, weapons, invisibility, and play mode on (readback)** — the rectangles
(RLE sprites, flip, clip), the mask textures and the tinted blend, then the step that changes what the player
sees: **play frames drawn by citro3d** and read back into `world_pixels`, with every frame the milestone cannot
draw (landscape, static: M5) discarded to SW. Readback mode brings **no speedup** (design 0.5; the payoff is
M6); M4's job is correctness of the full frame.

**The user is short on time and on usage.** Be economical: read only what each step needs, run the critic and
the architect at effort **medium**, batch Azahar runs into one serial script, and do not re-run a series that
has already passed on the same binary. Implement inline in the main session (no `c3d-implementer`), as M2
session 2 and M3 were, unless a piece is large and well bounded. Ask nothing mid-run unless a decision is
genuinely the user's (a tolerance change always is). **M4 is probably two sessions** (rectangles, then play
mode): stop at a clean, committed piece and add a "Session 2" header to the top of this file, as
`KICKOFF_M2.md` has.

## Where the project is

- P0, P1 (GPU present), G0-b, plan A and B-a are done and console-verified. Plan B's zero-copy present is on its
  own branch `stage10-bb` (tag `port-stage10-B`, `PATCHES.md` 55, **not merged**: do not touch it, do not edit
  `back/present.cpp` or `Source_Files/RenderOther/screen.cpp`).
- **M0** (tag `port-stage10-M0`): the seam (`PATCHES.md` 45), record/replay diff harness, class/fullbright
  filter, null mode, readback calibration. **M1** (`-M1`): walls in flat colour, tie pixels as edge band.
  **M2** (`-M2`): textured walls/floors full bright, texture cache v1 (`PATCHES.md` 46), the `C3D_TexBind`
  restore, non-convex polygons as SW spans, the **neighbourhood colour gate** (user decision 2026-10-31).
- **M3** (tag `port-stage10-M3`, 2026-11-05; commits `087ab93cb`, `dd955459a` and the docs after them): the
  shade LUT on unit 1 (256x256 L8, host worst 1 level), texcoord1, two TEV stages, infravision, the weapon
  flash, negative ambient (SW's short-index wrap excluded), **unit 1 unbound after every lit frame** by a direct
  store into `__C3D_Context` (`back/shade.cpp`; `C3D_TexBind(1, NULL)` dereferences NULL + 12). Passes on
  L0/L6/L20, 2 films, infravision, under both Azahar backends; rings 2 048 / 4 096, draw reserve 564, command
  words 1304 / 282 / 10. `PERF.md` "Stage 10 M3".
- **Azahar's software backend faults when one draw samples FCRAM and VRAM textures together** (an emulator bug,
  bisected; design Review log "M3 acceptance, software-backend films"): under `AZAHAR_GRAPHICS_API=0` run
  anything with VRAM promotions at `gxq_budget 3` (all linear). Play mode makes this the common case: expect
  every software-backend M4 run to need it, and say so in each `m4-*.cfg`.
- **Console:** the M1 and M2 short checks passed; the "M3 console check, short version" (`NEXT-SPRINT.md`) and
  the full "M1 console check" are **not run**. Neither is this session's job.
- Hardware: one New 3DS. `CLAUDE.md` "Git": never push to `origin`; push to the fork only when the user asks.
  **Do not push.**

## Piece 0, before M4: M3's negative-ambient test (pending, user decision 2026-11-06)

Neither M3 film put a negatively lit polygon on screen (`c3d_neg_ambient_polys` 0). Close it first, on the
M3 binary (it needs no new renderer code):
- A solo Lua plugin `scripts/3ds/tests/plugins/Negative Light Test/`, after `Infravision Test/`, that sets
  `intensity` (and `delta_intensity` 0) of every state of every light below 0, e.g. −0.5 (`Lights[i].states[s]`;
  setter `Lua/lua_map.cpp`:2723). A state's intensity is read when the light **enters** that state
  (`GameWorld/lightsource.cpp`:340-341), so also force a state change at `init` (toggle `active`) and check the
  first run's counters before trusting it. Walls pin their ambient to `[0, 1]` (`RenderRasterize.cpp`:598-599):
  only floors and ceilings go negative. A `run-azahar.sh --negative-light-test` switch as `--infravision-test`.
- One lit L0 diff (`m3-diff.cfg` settings, a copy `m3-neglight.cfg`) per Azahar backend. **Pass:** M3's lit
  gate (`c3d-diff.py log … --colour walls,floors --shading lit --nonedge-pct 0.1 --within 98 --tol 4 --mean 1.5`),
  `c3d_neg_ambient_polys` > 0, `c3d_ambient_wrap` 0.
- Record it in `PERF.md` "Stage 10 M3" (a short "Negative ambient" paragraph), the design's M3 status and the
  plan C status in `NEXT-SPRINT.md` (replace "Pending Azahar test"). Commit `stage10: M3 negative-ambient test
  (…)`. If it fails, it is an M3 bug: fix it before M4 and report.

## Read first, in this order (only these)

1. `CLAUDE.md` (the 8 rules, "Platform traps", "Testing in Azahar", "Stage 10"). In a worktree the scenario
   submodule is empty: `SCENARIO_SRC="/Users/dashielashie/Desktop/Aleph_3ds/data/Scenarios/Marathon 2"`.
2. `docs/3ds/CITRO3D_DESIGN.md`:
   - **1.2 and 1.5** (the seam, and the per-frame selection table: the row "milestone < M4 → `false` in play" is
     what M4 removes; **discard** and "10 in a row → off until next level");
   - **2.2's rows Sprite / Weapon / Tinted** (rectangles are clipped on the CPU as SW does, one quad each;
     texcoord1 constant per rectangle from `depth`, `ambient_shade`; weapons at depth 0), 2.3, **2.4** (state,
     reserves, GX budget: a full frame is many more draws than M3's diff classes);
   - **3.2** (RLE decode per column, the **mask** kind, A8; clamp for sprites; POT padding), **3.3** (cache,
     pools, eviction: eviction is M5, so a full pool is a **discard** in M4);
   - **4.2-4.3** (the tinted row: `dst × 8·idx/255`, blend state in 2.4; invisibility/subtle);
   - **5.2** (readback mode: downstream unchanged; this is what play frames become);
   - section 7: the **M3 specification and status** (what exists now, and "Not in M3"), the **M4 block**, M5's
     (to know what not to do);
   - **8.1** (tinted over a grey clear, coverage = changed pixels), 8.3 rows "sprites, weapons (M4)", "tinted",
     "full frame (M4+)", 8.5; 9.1-9.2 (sprite collections in VRAM/linear); section 10;
   - the Review log entries "M0 pre-implementation, round 1" item 1 (record/replay: why weapons are traversed
     once), "M2 implementation, colour gate", "M3 acceptance, unit 1 after the frame", "M3 acceptance,
     software-backend films".
3. `docs/3ds/PERF.md`: "How to read these numbers", "Stage 10 M3", and the P0/P1 baselines a C3D-vs-SW bench is
   compared with.
4. The code as M3 left it, `ports/3ds/render_c3d/`: `front/c3d_rasterizer.*` (what a rectangle call does today:
   the class filter drops it), `front/filter.*`, `front/frame.cpp` (`render_frame`: diff frames, the summary),
   `core/geometry.*` (`c3d_supports`, reserves), `core/tex_convert.*` (the mask and RLE paths: written and
   host-tested in M0-M2, never drawn), `back/draw.cpp` (state save/restore, TEV), `back/texcache.*`,
   `back/shade.*`, `core/config.*`.
5. Engine, for the SW reference (cite lines, never paraphrase from memory): `scottish_textures.cpp`
   `texture_rectangle` (:652 and on: clip, `texture_x/dx`, flip, the shading table pick :738-758),
   `low_level_textures.h` (the tinted loop, :501-539), `render.cpp`:782-791 (tinted), :994 and :1069-1072
   (weapons, `SetForeground()`), `RenderPlaceObjs.cpp` (sprite depth and ambient), `shapes.cpp`:2010-2021.
6. citro3d (rule 7, cite, never guess): `c3d/renderqueue.h`, `c3d/effect.h` (blend, alpha test), `enums.h`
   (`GPU_A8`, blend factors), `c3d/texture.h` (clamp); disassemble when the headers don't say.
7. `.claude/agents/` and `PATCHES.md`: M4 should need **no engine edit**; 47/48 are reserved for M6/M8; if one
   is unavoidable, take 56+.

## The task: M4 (section 7's M4 block is the spec; summary)

- **Add:** rectangles in the front end and back end (RLE sprites via the existing conversion, horizontal flip,
  SW's CPU clip, one quad, `GPU_CLAMP_TO_EDGE`), texcoord1 per rectangle (sprites from depth/ambient, weapons
  depth 0, shadeless under infravision), the **mask** textures (A8) and the **tinted** blend (invisibility,
  subtle); diff classes `sprites`, `weapons`, `tinted` (tinted over the grey clear of 8.1). Then **play mode
  on**: a play frame the milestone supports is drawn by citro3d and read back into `world_pixels`; a frame with
  a landscape or static polygon, or one that hits a ring, pool, reserve or GX limit, is **discarded** and drawn
  in SW (1.5), "10 in a row → off until next level".
- **Accept (design M4 block, 8.3):** diff `sprites,weapons,tinted`: coverage outside the band ≤ 0.5 %, colour
  as M3's lit gate (tinted: ≥ 98 % Δ ≤ 3); **full frame ≥ 97 % Δ ≤ 4** (static excluded); scripts
  `12-sprite-check.txt`, `06-gameplay.txt`, 07, 13, 14 pass **in play mode**; a C3D-vs-SW bench (L0/L6/L20,
  `perf-bench.sh`) recorded, not gated (readback mode is not meant to be faster). Under both Azahar backends
  (software backend at `gxq_budget 3`). M3's lit gate, M2's full-bright gate, `present_diff` 0, the restore
  counters 0, M0's calibration and the P1 scripts unharmed; OFF builds unchanged.

## Decisions the spec must settle (and which are the user's)

- **The colour gates for sprites, weapons, tinted and the full frame.** 8.3's rows predate M2's neighbourhood
  rule. The spec says how the neighbourhood rule applies to rectangles (their texel steps are SW's
  `texture_dx`, not an interpolated UV: check how close the quad's UVs get) and to the full frame. **Any change
  to a tolerance is the user's decision** (recorded in the Review log): present it in plain language with the
  numbers that support it, and wait.
- **What a play frame is.** Where `render_frame` decides play vs SW, how a discard redraws the same frame in SW
  (no second traversal with side effects: the record/replay of M0 already gives one), what the summary logs per
  second (frames C3D / discarded / why), and how "10 in a row" resets at a level change.
- **The texture cache under a full frame.** Sprite collections are many small RLE frames; M2's cache has no
  eviction (M5). Size the pools (9.1-9.2) so L0/L6/L20 and the films fit, and make a full pool a counted discard,
  not a failure.
- **Tinted state vs the present.** The tinted blend changes blend state the present also uses: restore it after
  the view frame as M3 did for TEV stage 1 and unit 1, and prove it with `m2-present-diff.cfg`/`m3-present-diff`
  (`present_diff` 0) after a frame with an invisible monster.
- **The invisibility plugin** (design 4.3: `invisibility_duration`, `Lua/lua_player.cpp`:2412-2414): the
  player's own invisibility tints the weapon; an invisible monster needs a level or a film that has one. Pick
  how the tinted class gets covered and say why.

## What M3 taught (read before designing)

- **Every unit and stage you enable must be disabled again before the present.** citro3d cannot unbind unit
  ≥ 1 (`C3D_TexBind(unit≥1, NULL)` crashes); M3 writes the context directly and checks the layout at run time
  (`shade_check_bound`). A new piece of state (blend, a mask texture on a unit) gets the same treatment and a
  counter in `[c3d] cmdbuf final`.
- **Azahar's software backend is not a GPU**: it segfaults on undefined texcoords for an enabled unit, and on
  FCRAM+VRAM textures in one draw. When it faults, bisect before blaming the renderer (M3 took 9 runs; the
  helpers are in `build/m3/tools/`).
- **Rings and reserves are measured, not guessed.** The L08 film's span quads overflowed the vertex ring; a full
  play frame is bigger again. Log the high-water marks on the films and size from them.
- **Command words are deterministic**: re-measure `view_frame_words` / `draw_words` and the draw reserve (564)
  with rectangles and the tinted state.
- **Films time differently between runs**, so a counter seen in one run (flash checks) may be 0 in the next: gate
  on what every run shows, record the rest.
- **Traps:** never edit `run-azahar.sh` or a config while an Azahar run uses it; run Azahar only through the lock
  (`mkdir ~/Applications/Azahar-2126.2/user/.run-lock`, `rmdir` in a trap, also on failure; wait for
  `pgrep -i azahar` to be empty); copy `build-3ds/azahar-screens/` away right after each run; check
  `df -h /System/Volumes/Data` before long series (the disk filled up once during M3, from outside the session);
  keep helpers in `build/m4/tools/`, not the scratchpad (it was wiped during M3); quote every shell variable.

## Order of work

0. **Piece 0** above (negative ambient), committed on its own.
1. **Check the design against the code first** and write an **"M4 specification" block** in section 7, in the
   style of M3's (revision notes citing the code at the current HEAD): rectangles (geometry, clip, flip, RLE
   upload, clamp, texcoord1), mask textures and the tinted blend, the diff classes and the grey clear, play mode
   (selection, discard, redraw, the counters), the cache and pool sizes, the state the present relies on, the
   GX-queue and command-buffer accounting, profiler zones, the colour gates, the plugin(s) and films, configs
   (`m4-*.cfg`), host tests (RLE/mask conversion as drawn, rectangle UVs vs SW's `texture_dx`), acceptance.
   `c3d-critic` (effort **medium**) reviews it, **max 2 rounds**, before code. Commit `stage10: docs: M4
   specification (...)`. If a gate changes a tolerance, ask the user before step 2.
2. **Implement** in pieces, each built (4 builds, 0 warnings) and host-tested: rectangles (sprites, weapons) on
   diff frames → mask + tinted → play mode → configs, plugin, films. Then `c3d-critic` on the commit, max 2 revise
   rounds. Commit `stage10: M4 <summary>`; tag `port-stage10-M4` on approval.
3. **Acceptance** in Azahar on the final binary, in one serial script, both backends; the OFF builds: `objdump -d`
   of `build-3ds` / `build-3ds-prof` saved at the start and compared at the end, `nm` 0.
4. **Record:** `PERF.md` "Stage 10 M4" (Azahar — non-authoritative; the C3D-vs-SW bench); the design's M4 status
   and the Review log; `NEXT-SPRINT.md` plan C status and an **"M4 console check, short version"** at the end (a
   play-mode run on L0 with the sprites diff, and HOME > Close / suspend in play mode); `CLAUDE.md` "State of the
   project"; then the backup bundle (recipe `STAGE5-PREREQS.md` section 8, the command as in `KICKOFF_M1.md`
   with the tags up to `port-stage10-M4`, then `git bundle verify` and the restore test) and the project memory's
   stage 10 entry.
5. **Do not start M5** (landscapes, static, eviction).

## Standing rules

- Builds: `build-3ds-c3d`, `build-3ds-c3d-prof` for M4; `build-3ds`, `build-3ds-prof` must keep building and stay
  identical. `export DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM`. Zero warnings, `-O2`, no LTO.
  Layout: `core/` plain C++ (host-tested), `front/` engine headers only, `back/` `<3ds.h>`/`<citro3d.h>` only;
  engine TUs never include `<3ds.h>`; shaders via picasso in CMake.
- Docker for `ppcheck.sh` only if an engine file is edited (it should not be).
- Anything needing `sudo`: give the user the command for Terminal.app.

## Report and stop

Piece 0's result; what was built (commits, tag); the Azahar results per acceptance item with their label (both
backends; the films; play mode's discard counts and reasons); the colour gates used and whether the user changed
them; the reserves, rings, pools and command words; the C3D-vs-SW bench; anything the design got wrong and the
critic's verdicts; and the user's choice for what comes next (M5, or the M3/M4 short console checks first). Do
not push.
