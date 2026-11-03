# Paste this into a fresh Claude Code session started in the stage10-c3d worktree

(`/Users/dashielashie/Desktop/Aleph_3ds/.claude/worktrees/stage10-c3d`, branch `stage10-c3d`. Written
2026-11-03. Earlier prompts: `KICKOFF_PROMPT.md` (stage 10), `KICKOFF_M0.md`, `KICKOFF_M1.md`,
`KICKOFF_M2.md` (the milestones before this one), `KICKOFF_M2_CONSOLE.md` (M2's short console check).)

---

We are on Stage 10 of the Aleph One 3DS port (the optional citro3d GPU renderer behind `A1_RENDER_C3D`).
**This session builds M3: lighting** — the shade LUT, texcoord1, the two TEV stages of design 4.2,
infravision (shadeless), negative ambient and the weapon flash, on walls and floors. Like M1 and M2 it
runs only on `debug-c3d` diff frames (side B of the SW-vs-C3D diff); play frames stay software until M4.

**The user is short on time and on usage.** Be economical: read only what each step needs, run the
critic and the architect at effort **medium**, batch Azahar runs into one serial script, and do not
re-run a series that has already passed on the same binary. M2 session 2 was implemented inline by the
main session (no `c3d-implementer`) to save usage; do the same unless a piece is large and well bounded.
Ask nothing mid-run unless a decision is genuinely the user's (a tolerance change always is). If the
work does not fit one session, stop at a clean, committed piece and add a "Session 2" header to the top
of this file, as `KICKOFF_M2.md` has.

## Where the project is

- P0, P1 (GPU present), G0-b, plan A and B-a are done and console-verified. Plan B's zero-copy present is
  on its own branch `stage10-bb` (tag `port-stage10-B`, `PATCHES.md` 55, **not merged**: do not touch it,
  do not edit `back/present.cpp` or `Source_Files/RenderOther/screen.cpp`).
- **M0** (tag `port-stage10-M0`): the seam (`PATCHES.md` 45), the record/replay diff harness, the
  class/fullbright filter, null mode, the readback calibration.
- **M1** (tag `port-stage10-M1`): opaque walls in flat colour; tie pixels count as edge band.
- **M2** (tag `port-stage10-M2`, 2026-11-02; commits `1c3b9c6f5`, `38ca4868e`, `eb55bd24e`): textured walls
  and floors **full bright**, texture cache v1 (linear upload + ranged flush, VRAM promotions,
  generations, hooks 2a/2b = `PATCHES.md` 46), the `C3D_TexBind` restore, non-convex polygons as SW's
  spans, the **neighbourhood colour gate** (user decision 2026-10-31, design Review log "M2
  implementation, colour gate") and one recorded exception (Review log "M2 acceptance, load test colour").
- **Console:** the M1 and M2 short checks **passed** (2026-11-02 / 2026-11-03; `PERF.md` "Stage 10 M1 —
  console, short check", "Stage 10 M2 — console, short check"): readback, ring flushes, the texture flush
  and the TextureCopy promotion work on hardware; command words equal Azahar's. The full "M1 console
  check" (Classic, the present check, discard paths, L6/L20) is still not run; it is not this session's job.
- Hardware: one New 3DS. `CLAUDE.md` "Git": never push to `origin`; push to the fork only when the user
  asks. **Do not push.**

## Read first, in this order (only these)

1. `CLAUDE.md` (the 8 rules, "Platform traps", "Testing in Azahar", "Stage 10"). In a worktree the
   scenario submodule is empty: `SCENARIO_SRC="/Users/dashielashie/Desktop/Aleph_3ds/data/Scenarios/Marathon 2"`.
2. `docs/3ds/CITRO3D_DESIGN.md`:
   - **section 4 whole** (4.1 SW model, **4.2 the shade LUT, texcoord1, self-luminous, the TEV table and
     its UNVERIFIED clamp**, 4.3 the rows M3 touches: infravision, self-luminous, weapon flash, per-column
     lighting; the approximated-effects list and "How we test without hardware");
   - 0.3 (the rows M2 added), 2.1-2.4 (the vertex format and `world.v.pica`: where texcoord1 goes; 2.4
     state and budgets), 3.2 (the `shadeless` / `shadeless_keyed` conversion kinds, already in
     `core/tex_convert` and host-tested but not drawn);
   - section 7: the **M2 specification and M2 status** (what exists now: TEV, alpha test, sentinel alpha
     0x01, the `SHRT_MAX` shading-depth clamp M3's texcoord1 keeps, "Not in M2"), the **M3 block**, M4's
     (to know what not to do);
   - 8.1-8.4 (8.3 row "walls/floors lit (M3)": ≤ 0.1 % coverage, ≥ 98 % Δ ≤ 4, mean ≤ 1.5; 8.4 "shade LUT
     vs SW formula"), 9.1-9.2 (the LUT's 64 KiB of VRAM), section 10 item 6 (TEV precision);
   - the Review log entries "M2 implementation, colour gate", "M2 commit review", "M2 acceptance, load
     test colour".
3. `docs/3ds/PERF.md`: "How to read these numbers", "Stage 10 M2" and its console subsection (note there:
   on L0 the console's view is not static — likely, not verified, the NPC fight the level starts with —
   so compare checks at equal coverage).
4. The code as M2 left it, `ports/3ds/render_c3d/`: `core/tex_convert.*` (kinds, self-luminous A 128),
   `core/geometry.*` (vertex, texcoords, `c3d_supports`: shadeless is refused today), `core/config.*`
   (`fullbright`), `front/c3d_rasterizer.*` (what a polygon carries: ambient shade, depth),
   `front/filter.*`, `front/frame.cpp` (`c3d_check`), `back/draw.*` (TEV, alpha test, the view frame),
   `back/texcache.*`, `back/context.*` (`gx_op`, the `C3D_TexBind` / `C3D_BindProgram` wrappers and the
   restore), `shaders/world.v.pica`; `scripts/3ds/tests/c3d-host/`, `c3d-diff.py` (`--colour`),
   `scripts/3ds/tests/c3d/m2-*.cfg`.
5. The SW reference M3 transliterates: `scottish_textures.cpp`:131-170 (depth shade, ambient combine,
   the 256-level quantisation, negative ambient), `RenderRasterize.cpp`:460 and 598-599 (ambient per
   polygon), `screen.cpp`:1466 (`maximum_depth_intensity`, the weapon flash), `calculate_shading_table`
   (its shadeless call at :1137, as design 4.2 cites it), `render.cpp`:860-979 (transfer modes folded
   into geometry), `Lua/lua_player.cpp`:2412-2414 (`infravision_duration`).
6. citro3d (rule 7, cite, never guess): `c3d/texenv.h`, `enums.h`:331-383 (TEV sources and operands),
   `c3d/texture.h` (L8, nearest, clamp, unit 1), `3ds/gpu/*`; the examples in
   `/opt/devkitpro/examples/3ds/graphics/gpu/` that use two texture units or TEV combiners; disassemble
   when the headers don't say.
7. `.claude/agents/` (`c3d-architect`, `c3d-critic`, `c3d-implementer`) and `PATCHES.md` (M3 should need
   **no engine edit**; 47/48 are reserved for M6/M8; if one is unavoidable, take 56+).

## The task: M3 (section 7's M3 block is the spec; summary)

- **Add:** `core/shade_lut.cpp` (256x256 L8, `LUT(u=s, v=a) = SW level/255`, built once, host-tested
  against a transliterated `calculate_shading_table` on every `(d, a)` grid point, ≤ 1 level);
  **texcoord1** per vertex `(M/65536 − d/8192, a/65536)`, `a < 0` → `(0, −a)`, shadeless → `(1, 1)`
  (vertex format, `world.v.pica`, the front end); the LUT uploaded once to VRAM and bound to unit 1;
  **TEV 4.2** (2 stages, the alpha x2 trick, alpha test `GEQUAL 128`); **self-luminous** through
  texture A 128; **infravision** (draw the shadeless kinds; `c3d_supports` stops refusing
  `_SHADELESS_BIT`); **negative ambient**; the **weapon flash** (`M` per frame); `fullbright 0` diff
  frames (lit).
- **Accept (design M3 block, 8.3):** host LUT ≤ 1 level everywhere; diff `walls,floors` lit, ≥ 98 % Δ ≤ 4,
  mean ≤ 1.5, coverage ≤ 0.1 % outside the band, on L0/L6/L20 + **2 films**, under **both** Azahar
  backends (host GPU and `AZAHAR_GRAPHICS_API=0`); infravision via a solo Lua plugin; M2's full-bright
  gate still passes (`fullbright 1`); `tex_restore_mismatch` 0, `present_diff` 0 (`m2-present-diff.cfg`);
  M1 walls coverage, M0 calibration, P1 scripts 07, 08, 13, 14, 22 unharmed; OFF builds unchanged.

## Decisions the spec must settle (and which are the user's)

- **The colour gate for lit frames.** 8.3's lit row predates the M2 decision; M2's gate is the
  neighbourhood rule. The spec proposes how the neighbourhood rule applies with lighting (lighting is
  per pixel on the GPU and per column/row in SW, ≤ 1 level: design 4.3) and with which Δ / mean. **Any
  change to a tolerance is the user's decision** (recorded in the Review log, as M2's): present it in
  plain language with the host numbers that support it, and wait.
- **The two films.** `m2-film.cfg` uses "M2 L01 Waterloo Waterpark" (in the worktree's
  `tests/replays/Marathon 2/Tooncinator Films/`). Pick a second one that covers what L01 does not: dark
  areas (low ambient, depth shading), gunfire (the weapon flash), and if any level has it, negative
  ambient; say why in the spec.
- **The infravision plugin.** It does not exist yet: write it after `scripts/3ds/tests/plugins/Terminal
  Test/` (a solo Lua plugin setting `infravision_duration`), with its own config.
- **TEV and unit-1 state vs the present.** The present binds its program, AttrInfo and
  `C3D_TexBind(0, …)` once at init, and sets **TEV stage 0 only** (plus the depth/write mask) before every
  draw (`back/present.cpp` `set_present_draw`, :309-330); it never touches stage 1. So a second TEV stage
  left configured by a view frame **would apply to the present**: the spec must reset it after every view
  frame (to what `C3D_TexEnvInit` gives, cited from citro3d) or prove it is a pass-through. Likewise unit 1
  (the LUT) and texcoord1 in the AttrInfo: say how each is restored or proved harmless, without editing
  `present.cpp` (M2 used a `--wrap` and `tex_restore_mismatch`), and prove it with `m2-present-diff.cfg`
  (`present_diff` 0) after a lit view frame.
- **The TEV clamp** (4.2 UNVERIFIED: the x2 alpha relies on outputs clamping to [0,1]). Azahar's two
  backends may not agree with each other or with PICA: the M3 diff catches it in Azahar; the hardware
  answer needs the console (below).

## What M2 taught (read before designing)

- **Full bright first, then lit.** Keep `fullbright 1` working as M2's regression; the lit path is new
  state, not a replacement.
- **Sync rules** carried from M1/M2: a ranged `GSPGPU_FlushDataCache` of exactly the bytes the GPU
  reads before they are submitted (the LUT is one more linear → VRAM promotion through M2's verified
  path); in-frame `C3D_Sync*` after draws needs `C3D_FrameSplit(GX_CMDLIST_FLUSH)` first; clear only
  before the first draw; every GX op through `gx_op` / `gx_split`.
- **Command words are deterministic**: log `[c3d] cmdbuf final` (M2: `view_frame_words=1288
  draw_words=268 readback_words=8` on L0) and re-measure the draw reserve (536 today) with the extra
  TEV stage and texcoord1.
- **The sentinel** relies on alpha 0x01, a value no draw writes (texture A ∈ {0, 128, 255}); the alpha
  x2 trick must keep it so (check the TEV alpha output for A 128 inputs).
- **Diff runs on L0**: on the console the view moves (PERF.md M2 console subsection); in Azahar the 20
  checks were identical. Judge per view when they are not.
- **Traps:** never edit `run-azahar.sh` or a config while an Azahar run uses it (bash reads scripts
  lazily); run Azahar only through the lock (`mkdir ~/Applications/Azahar-2126.2/user/.run-lock`, `rmdir`
  in a trap, also on failure; wait for `pgrep -i azahar` to be empty); copy `build-3ds/azahar-screens/`
  away right after each run; Azahar can hang in its own teardown after a clean game exit on `--script`
  runs without `--timeout` (kill it); a port file named like an engine header shadows it on macOS's
  case-insensitive disk; quote every shell variable in helper scripts.

## Order of work

1. **Check the design against the code first** and write an **"M3 specification" block** in section 7,
   in the style of M2's (revision notes citing the code at the current HEAD): the LUT (format, build,
   upload, binding, VRAM budget), texcoord1 (vertex format, shader, `a < 0`, shadeless, the `SHRT_MAX`
   depth clamp, the flash), the TEV stages and alpha test, self-luminous, infravision, the state the
   present relies on, the GX-queue and command-buffer accounting, profiler zones, the colour gate
   proposal, the films and the plugin, configs (`m3-*.cfg`), host tests, acceptance. `c3d-critic` (effort
   **medium**) reviews it, **max 2 rounds**, before code. Commit `stage10: docs: M3 specification (...)`.
   If the gate proposal changes a tolerance, ask the user before step 2.
2. **Implement** in pieces, each built (4 builds, 0 warnings) and host-tested: the LUT + its host test →
   texcoord1 (vertex, shader, front end) → TEV + the LUT on unit 1 + the state restore → infravision,
   negative ambient, the flash → configs, the plugin, the second film. Then `c3d-critic` on the commit,
   max 2 revise rounds. Commit `stage10: M3 <summary>`; tag `port-stage10-M3` on approval.
3. **Acceptance** in Azahar (above) on the final binary, in one serial script; the OFF builds:
   `objdump -d` of `build-3ds` / `build-3ds-prof` saved at the start and compared at the end, `nm` 0.
4. **Record:** `PERF.md` "Stage 10 M3" (Azahar — non-authoritative); the design's M3 status line and the
   Review log; `NEXT-SPRINT.md` plan C status and an **"M3 console check, short version"** at the end
   (two launches as M2's: a lit L0 diff and a dark/flash case; it answers the TEV clamp and TEV precision
   on PICA, design section 10 item 6); `CLAUDE.md` "State of the project"; then the backup bundle (recipe
   `STAGE5-PREREQS.md` section 8; the command as in `KICKOFF_M1.md` with `port-stage10-M2` and
   `port-stage10-M3` added, then `git bundle verify` and the restore test) and the project memory's
   stage 10 entry.
5. **Do not start M4** (sprites, weapons, play mode).

## Standing rules

- Builds: `build-3ds-c3d`, `build-3ds-c3d-prof` for M3; `build-3ds`, `build-3ds-prof` must keep building
  and stay identical. `export DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM`. Zero warnings,
  `-O2`, no LTO. Layout: `core/` plain C++ (host-tested), `front/` engine headers only, `back/`
  `<3ds.h>`/`<citro3d.h>` only; engine TUs never include `<3ds.h>`; shaders via picasso in CMake.
- Docker for `ppcheck.sh` only if an engine file is edited (it should not be).
- Disk: check `df -h /System/Volumes/Data` before long series.
- Anything needing `sudo`: give the user the command for Terminal.app.

## Report and stop

What was built (commits, tag), the Azahar results per acceptance item with their label (both backends;
the films), the host LUT result, the colour gate used and whether the user changed it, the reserves and
command words, anything the design got wrong (the TEV clamp above all) and the critic's verdicts, and
the user's choice for what comes next (M4, or the M3 short console check first, or the full M1 console
check). Do not push.
