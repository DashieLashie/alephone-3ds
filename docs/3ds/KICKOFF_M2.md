# Paste this into a fresh Claude Code session started in the stage10-c3d worktree

(`/Users/dashielashie/Desktop/Aleph_3ds/.claude/worktrees/stage10-c3d`, branch `stage10-c3d`. Written
2026-10-27. Earlier prompts: `KICKOFF_PROMPT.md` (stage 10), `KICKOFF_M0.md`, `KICKOFF_M1.md` (the
milestones before this one), `KICKOFF_PLAN_B.md`, `KICKOFF_B_CONSOLE.md`, `KICKOFF_B_B.md` (plan B).)

---

We are on Stage 10 of the Aleph One 3DS port (the optional citro3d GPU renderer behind `A1_RENDER_C3D`).
**This session builds M2: textured walls and floors (full bright) and texture cache v1.** M2 is the first
milestone with texture uploads and the first engine edit since M0 (hooks 2a/2b, `PATCHES.md` 46). Like
M1 it runs only on `debug-c3d` diff frames (side B of the SW-vs-C3D diff); play frames stay software
until M4.

**The user is short on time and on usage.** Be economical: read only what each step needs, run the
critic and the architect at effort **medium**, batch Azahar runs into one serial script, and do not
re-run a series that has already passed on the same binary. Ask nothing mid-run unless a design decision
is genuinely the user's.

## Where the project is

- P0, P1 (GPU present), G0-b, plan A and B-a are done and console-verified. Plan B's zero-copy present is
  on its own branch `stage10-bb` (tag `port-stage10-B`, `PATCHES.md` 55, **not merged**: do not touch it,
  do not edit `back/present.cpp` or `Source_Files/RenderOther/screen.cpp`).
- **M0 done** (tag `port-stage10-M0`): the seam (`render.cpp`, `PATCHES.md` 45), the record/replay diff
  harness, the class/fullbright filter, null mode, the readback calibration.
- **M1 done 2026-10-27** (commits `9d24a03b3` spec, `54d1e03c4` M1, `ac9a0241a` revise 1 = tag
  `port-stage10-M1`, `08f4f19b3` docs, `dba25ae08` short console runbook). citro3d draws opaque walls in
  flat colour on diff frames. Azahar: 0 non-edge coverage mismatches on L0/L6/L20 × 20 and 200 film
  frames; every edge mismatch is a tie pixel. Results: `PERF.md` "Stage 10 M1"; design section 7 "M1
  specification" + "M1 status"; Review-log entries "M1 pre-implementation" (3), "M1 implementation, tie
  pixels", "M1 commit review".
- **Nothing of M0/M1 has run on the console.** User decision (2026-10-27): start M2, defer the console.
  But **the texture upload code (TextureCopy promotions, linear→VRAM, their flushes and waits) is not to
  be written until `NEXT-SPRINT.md` "M1 console check, short version" has run on the console** (two
  launches, 15-20 min, the user's time). Azahar completes transfers synchronously and has no CPU cache,
  so it cannot show sync/flush bugs; M0's critic rounds found three of them. Before you start, check
  `NEXT-SPRINT.md` / `PERF.md` for a "Stage 10 M1 — console, short check" result:
  - **run and passed** → build all of M2;
  - **not run** → build everything up to the upload code (spec, conversion, tiling, cache logic, the
    front end's texcoords, the hooks), then stop and report; the upload code is the next session;
  - **run and failed** → stop and report: the fix to M1's path comes first.
- Hardware: one New 3DS. `CLAUDE.md` "Git": never push to `origin`; push to the fork only when the user
  asks. **Do not push.**

## Read first, in this order

1. `CLAUDE.md` (all 8 rules, "Platform traps", "Testing in Azahar", "Stage 10"). In a worktree the
   scenario submodule is empty: `SCENARIO_SRC="/Users/dashielashie/Desktop/Aleph_3ds/data/Scenarios/Marathon 2"`.
2. `docs/3ds/CITRO3D_DESIGN.md`:
   - **0.3** (every row; the M0/M1 ones above all), 1.3 (hooks 2a/2b), 1.5;
   - **2.1-2.4** (vertex format, `world.v.pica`, **2.2's floor/ceiling/liquid and wall texcoord rows**,
     the ½-pixel question, the per-polygon `floor(min u/v)`; 2.3 clamps; 2.4 state and budgets);
   - **section 3 whole** (3.1 source data, 3.2 conversion, 3.3 upload model, cache, pools, eviction,
     the deferred flush on `collections_changed()`), 4.3 (transfer modes, as far as M2 needs);
   - section 7: the **M1 specification and M1 status** (what exists now and how it was decided), the
     **M2 block**, M3's (to know what not to do);
   - 8.1-8.4 (8.2: M2 gates run under **both** Azahar backends, host GPU and software rasteriser;
     8.3: walls/floors full bright ≤ 0.1 % coverage, ≥ 98 % Δ ≤ 2, mean ≤ 1.0), 9.1-9.2 (VRAM and linear
     budgets), section 10 items 5, 7, 9, 10.
3. `docs/3ds/PERF.md`: "How to read these numbers", "Stage 10 M0", "Stage 10 M1".
4. The code as M1 left it: `ports/3ds/render_c3d/` — `core/geometry.*` (vertex, `wall_w`,
   `accept_polygon`, fans, `DrawKey`, `c3d_supports`, `tie_pixels`, the reserves 508 + 20),
   `core/ring.h`, `core/diff.*`, `core/calibrate.*`, `core/config.*`, `core/gx_queue.h`;
   `front/c3d_rasterizer.*`, `front/filter.*` (`c3d_only`), `front/frame.cpp` (`c3d_check`);
   `back/draw.*` (the view frame), `back/view.cpp` + `view_shared.h` (shared readback),
   `back/context.*` (`frame_begin`/`frame_end`, `gx_op`/`gx_split`, the `C3D_BindProgram` wrapper, the
   command-buffer measurement), `back/present.cpp` (read-only), `shaders/world.v.pica`;
   `scripts/3ds/tests/c3d-host/`, `c3d-diff.py`, `scripts/3ds/tests/c3d/m1-*.cfg`.
5. The SW reference M2 transliterates: `scottish_textures.cpp` (wall mapper 891-1001 incl. the row
   texcoord; floor mapper ~1040-1100 with `_SCALE_2X/4X`; transparent texels in `low_level_textures.h`
   `write_pixel<…, check_transparent>`), `RenderRasterize.cpp` 440-460 (floor origin/vector, scale) and
   502-589 (walls), `shapes.cpp` (collections, `load_collections`, `unload_all_collections`: hooks
   2a/2b at 1494-1496 and 1179-1193), `textures.h` (bitmaps).
6. citro3d (rule 7, cite, never guess): `c3d/texture.h`, `c3d/texenv.h`, `c3d/renderqueue.h`,
   `3ds/gpu/*`, tex3ds (`/opt/devkitpro/tools/bin/tex3ds`, for the tiling test), the examples
   `textured_cube`, `proctex`; disassemble when the headers don't say
   (`arm-none-eabi-ar x /opt/devkitpro/libctru/lib/libcitro3d.a texture.o`, `objdump -dr`).
7. `.claude/agents/` (`c3d-architect`, `c3d-critic`, `c3d-implementer`) and `PATCHES.md` (45 = M0,
   **46 = M2's hooks 2a/2b**, 47/48 reserved for M6/M8, 49-55 used; 56+ for anything unplanned).

## The task: M2 (section 7's M2 block is the spec; summary)

- **Add:** `core/tex_convert.cpp` (3.2: 8-bit shapes + shading table → the GPU format, host-tested with
  real-format tables), `core/tiling.cpp` (Morton/8x8 tiling, **host-tested against tex3ds**),
  `back/texcache.cpp` (3.3: linear pool, VRAM pool with promotion by TextureCopy, LRU, caps, the
  deferred flush); floors, ceilings and liquids (`texture_horizontal_polygon`, 2.2's row), **transparent
  walls** (texel 0 → alpha 0 + the alpha test of 2.4, so M1's `c3d_only` exclusion goes away),
  `_SCALE_2X/4X`; texcoords in `world.v.pica` (perspective-correct via clip × w, **UNVERIFIED**: M2's
  floor diff fails visibly if wrong); hooks **2a/2b** = `a1_c3d::collections_changed()` (engine edit,
  `PATCHES.md` 46, fenced `#if defined(A1_PLATFORM_3DS) && defined(A1_RENDER_C3D)`, `ppcheck.sh` in
  Docker).
- **Accept (design M2 block, 8.3):** host tests of 3.3 incl. tex3ds; diff `walls,floors` with
  `fullbright 1`: coverage ≤ 0.1 % outside the band, ≥ 98 % of common non-edge pixels Δ ≤ 2, mean ≤ 1.0,
  on L0/L6/L20 + the film, **under both Azahar backends**; the UV offset (0 vs −0.5) measured and
  recorded; `15-load-test.txt` logs deferred flushes and diffs after the level change pass; a level
  start with ≥ 16 new textures in one frame passes with queue high-water ≤ 24; a `gxq_budget 4` run
  passes (promotions deferred, not discarded); M1's walls coverage still passes; M0 calibration and P1
  scripts 07, 08, 13, 14, 22 (and `m1-present-diff.cfg`) unharmed; the OFF builds unchanged.

## What M1 taught (read before designing)

- **State the present relies on:** it binds its program, AttrInfo **and `C3D_TexBind(0, &s_tex)`
  (`present.cpp`:453)** once at init and never again. M1 restores program (`__real_C3D_BindProgram`,
  never through the wrapper), AttrInfo and BufInfo. **M2 binds textures to unit 0: it must restore the
  present's texture binding** (no getter: find a way that does not edit `present.cpp`, e.g. a wrapper as
  for the program, and prove it with `m1-present-diff.cfg`-style runs: `present_diff` 0).
- **Sync rules carried:** a ranged `GSPGPU_FlushDataCache` of exactly the bytes the GPU will read
  (rings, linear textures, staging) before they are submitted; an in-frame `C3D_Sync*` after draws needs
  `C3D_FrameSplit(GX_CMDLIST_FLUSH)` first; `C3D_FrameBufClear` (the clear) has no split, so clear only
  before the first draw; targets created/deleted out of frame; retirement = `C3D_FrameBegin(0)`
  returned; command-buffer globals are zeroed by `C3D_FrameEnd`, so measure inside the frame; every GX
  op through `gx_op`/`gx_split` (`C3D_TexLoadImage` into VRAM is a TextureCopy = a GX op).
- **Coverage:** SW's wall edge walk is pixel-centre sampling; floors use `build_x_table` (row-based;
  check whether the same holds and whether the tie-pixel mask needs a row variant). Tie pixels count as
  band (design M1 "Implementation notes" (a)); keep logging `ties` / `tie_mismatch`.
- **Colour** (new in M2): SW reads `shading_table[texel]` at the top table with `fullbright 1`
  (M0 block "Classes"); the GPU side must produce the same ARGB from the converted texture. Δ is
  judged on common non-edge pixels.
- **Traps:** on macOS's case-insensitive disk a port file named like an engine header shadows it
  (M1 renamed `rasterizer.h` to `c3d_rasterizer.h`); the toolchain names embedded shader headers after
  the library (`a1_c3d_world_shaders_shbin.h`); `shaderInstanceFree()` frees the instance (malloc it);
  **never edit `run-azahar.sh` or a config while an Azahar run that uses it is in progress** (bash reads
  scripts lazily: an M1 run died with a syntax error mid-run); run Azahar only through a lock: wait for
  `pgrep -i azahar` to be empty, `mkdir ~/Applications/Azahar-2126.2/user/.run-lock`, run, `rmdir`
  (also on failure, a trap); copy `build-3ds/azahar-screens/` away right after each run.

## Order of work

1. **Check the design against the code first** and write an **"M2 specification" block** in section 7,
   in the style of M1's (revision notes citing the code at the current HEAD): keys M2 adds, texture
   formats and the conversion, the tiling, pool sizes (9.1/9.2) and caps, promotion and eviction rules,
   the deferred flush and hooks 2a/2b, texcoords (2.2) and the UV-offset test, the texture-binding
   restore, GX-queue and command-buffer accounting for uploads, the transparent-wall change, the floor
   coverage rule, the colour metric, both Azahar backends, profiler zones (a `c3d_upload`-style zone for
   texture uploads: 7 says M0-M5's uploads get their own name), configs, host tests, acceptance.
   `c3d-critic` (effort **medium**) reviews it, **max 2 rounds**, before code. Commit
   `stage10: docs: M2 specification (...)`.
2. **Implement with `c3d-implementer`** in pieces, each built (4 builds, 0 warnings) and host-tested:
   core conversion + tiling (+ tex3ds test) → cache logic (pure parts in `core/`) → the engine hooks
   (patch 46 + `ppcheck.sh`) → the front end (floors, texcoords, transparent walls) → **the upload code
   (only if the short console check passed; see above)** → harness/configs. Then `c3d-critic` on the
   commit, max 2 revise rounds. Commit `stage10: M2 <summary>`; tag `port-stage10-M2` on approval.
   Give every subagent everything it needs (they don't see this conversation); tell them to quote every
   shell variable.
3. **Acceptance** in Azahar (above) on the final binary, in one serial script; the OFF builds:
   `objdump -d` of `build-3ds` / `build-3ds-prof` saved at the start and compared at the end, `nm` 0.
4. **Record:** `PERF.md` "Stage 10 M2 — Azahar, non-authoritative"; the design's M2 status line;
   `PATCHES.md` 46; `NEXT-SPRINT.md` plan C status and an "M2 console check" (folded into the pending
   M1 console runbooks); `CLAUDE.md` "State of the project"; then the backup bundle (recipe:
   `STAGE5-PREREQS.md` section 8; command as in `KICKOFF_M1.md` with `port-stage10-M2` added).
5. **Do not start M3** (lighting).

## Standing rules

- Builds: `build-3ds-c3d`, `build-3ds-c3d-prof` for M2; `build-3ds`, `build-3ds-prof` must keep building
  and stay identical. `export DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM`. Zero warnings,
  `-O2`, no LTO. Layout: `core/` plain C++ (host-tested), `front/` engine headers only, `back/`
  `<3ds.h>`/`<citro3d.h>` only; engine TUs never include `<3ds.h>`.
- Docker for `ppcheck.sh` (~1 min): if `docker` hangs the daemon died (disk full once): `open -a Docker`,
  wait for `docker info`.
- Disk: check `df -h /System/Volumes/Data` before long series (it filled during M0).
- Anything needing `sudo`: give the user the command for Terminal.app.

## Report and stop

What was built (commits, tag), the Azahar results per acceptance item with their label, the UV offset
chosen, pool sizes and caps, anything the design got wrong and the critic's verdicts, and the user's
choice for what comes next (M3, or the console checks first).
