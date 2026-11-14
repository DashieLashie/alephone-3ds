# Paste this into a fresh Claude Code session started in the stage10-c3d worktree

(`/Users/dashielashie/Desktop/Aleph_3ds/.claude/worktrees/stage10-c3d`, branch `stage10-c3d`. Written 2026-11-13 after the
partial M9 console check (docs commit `3c96f658d`, `PERF.md` "Stage 10 M9 — console"). The tag `port-stage10-M9` is the build the
defects were found on. Earlier kickoff of the same shape: `KICKOFF_M6B.md`.)

---

We are on Stage 10 of the Aleph One 3DS port (the optional citro3d GPU renderer behind `A1_RENDER_C3D`). **This session fixes
the two stereo defects the M9 console check found**, in Azahar first, then a short console check with me at the console. M7
(supersampling) waits until this is done. Do **not push**; nothing is pushed yet.

## What was found (my words, on the New 3DS, stereo only; mono is clean)

1. **The sky bleed.** "When I move around or turn the camera the texture of ceilings and walls was dragging or 'bleeding' on the
   skybox." In **both eyes**; gone with 3D off and gone in the mono run; **not removed by `stereo_fill 0`**; L0 has a sky, L20 does not
   (a closed room), so it is untested elsewhere. It shows when the camera moves, not at rest.
2. **The edge strip.** A slim vertical strip, about 5 px, at a screen edge, only where a texture change falls on that edge ("I need to
   align a texture with the edge of the screen I want to test"). **The left eye fails at its right edge; the right eye fails at its left
   edge** (each eye is fine at its other edge). Gone at slider 0, back when raised; while the slider ramps the edge texture looks "out of
   phase with itself". **Not removed by `stereo_fill 0`**, and it was also seen with fill 1.

Everything else I judged fine: depth, the weapon in front of the screen (D3), the crosshair, sprites, the ramp, no ghosting. Fill 1
holds 60.5 fps @60 on L0/L20; `gpu_ms view` 1.09 / 2.43 / 3.84 ms for 1 / 2 / 4 passes. **Do not re-open the user decisions D1-D4**
(`KICKOFF_CONSOLE_M9.md`). I would also like the sky "a bit further back, if possible": a wish, not part of this fix (see "Not in this
session").

## What the design already says (read these, they are the leads, none verified)

- **The engine clips polygons at the vertical window lines** (design Review log "M9 specification: critic round 1 ...": "the source
  of the gaps"), and "an engine edit to widen the clip windows" was **explicitly left out of M9** (M9 block, "Not in M9"). Geometry
  that the engine cuts at the 400-px window edge is never sent to the renderer, so a shifted eye pass has **nothing to draw in a strip of
  about the shift's width at the opposite edge**, and the centre pass under it (fill 1) has nothing there either: it is clipped at the
  same lines. That fits the edge strip exactly (left eye shifts one way and loses its right edge, the mirror for the other eye;
  survives `stereo_fill 0` and fill 1; only visible where a texture change falls there, because elsewhere the leftover pixel looks the
  same). A guess to test, not a finding. The eye textures are 512 wide, 112 columns wider than the view.
- **`r_clears=0`:** the eye textures are never cleared between frames (M6's "no clear" rule), so a pixel nothing draws shows the
  eye's previous frame: a strip that "drags".
- **Landscapes are drawn at the maximum shift** (a landscape vertex carries `w = kWPerspMax`, design M9 spec) while the walls around
  them shift by their own depth. Where a wall meets the sky the two shifts differ, so a sliver of the sky's area is either uncovered or
  covered twice, and what shows through is stale or the centre pass's wall pixels. That is a candidate for the sky bleed. Also the
  sky's polygon extent is cut by the engine's clipping; and the sky's texture is **two parts** (2 MiB + 128 KiB, rows cut at 512,
  sampled from linear memory at the 2 944 KiB cap on L0/L6): check that the second part and the first move together.
- **The fill pass's own limits** (design "Known limitations"): the centre pass is drawn under each eye, which for the sky means a
  second landscape draw at zero shift under the shifted one.

## Plan

**Read first (only these):** `CLAUDE.md` (rules 2, 4, 6, 8; the platform traps), `docs/3ds/PERF.md` "Stage 10 M9 — console" (the
defects in my words) and "Stage 10 M9" (T4 gaps, T2 identity), `CITRO3D_DESIGN.md` section 6 and the **M9 block** (the stereo spec, the
draw list, the fill pass, "Known limitations", T-numbers), `docs/3ds/PATCHES.md` for what is already edited in the engine. Code:
`ports/3ds/render_c3d/back/stereo.cpp`, `back/draw.cpp` (the replay and passes), `back/view.cpp`, `core/stereo.cpp`,
`front/frame.cpp`, `front/c3d_rasterizer.cpp` + `core/landscape.cpp` (how a landscape is issued), `shaders/world.v.pica`, `core/config.h`.

1. **Reproduce both in Azahar before touching code** (the console is the only place they have been seen). Azahar passed T2/T4/T5, so
   the tests did not catch them: say so plainly if Azahar also shows them, and if it does not, find which condition differs (the
   slider at 100, the hand-played motion, L0's sky). Use `stereo 1`, `stereo_slider 100` on L0 with the camera turning at a wall/sky
   edge, with `stereo_check` / the eye read-backs and the `screen-c3d-*` capture machinery (the game saves its own screens;
   `scripts/3ds/run-azahar.sh --perf 0 --c3d <cfg>`; in a worktree pass `SCENARIO_SRC="/Users/dashielashie/Desktop/Aleph_3ds/data/Scenarios/Marathon 2"`;
   take `~/Applications/Azahar-2126.2/user/.run-lock` around every run). Compare each eye's texture against the mono frame with a
   per-pixel diff: **write the test that shows the defect first** (a gate, host-testable if possible in `core/`), so the fix is
   measured and not eyeballed. A 3-minute film with the camera turning at the sky is the best regression input; `scripts/3ds/tests/`
   has film and input-script helpers.
2. **Diagnose each defect separately** with the existing probes (`stereo_gaps 1` magenta clears with `stereo_fill 0` show exactly
   which pixels no pass covers: if the edge strip is magenta, it is uncovered; if the sky sliver is magenta it is the landscape's shift;
   if neither is, it is content). Report what you find per defect, with figures, **before** choosing a fix.
3. **Choose the fix per defect and ask me** (an `AskUserQuestion` with your recommendation first); the candidates I can see now:
   - *Edge strip:* (a) **widen the engine's clip windows by the maximum shift on stereo frames**: an engine edit under
     `#ifdef A1_PLATFORM_3DS`, a `PATCHES.md` entry (57+), the `ppcheck.sh` check (IDENTICAL or normalised 0), costs extra geometry
     per frame and touches the engine's window logic: check what the rasteriser's window variables are first; (b) **crop**: show a
     slightly zoomed or inset view so the uncovered strip is outside the visible 400 px (needs the eye textures' spare columns and a
     present-side change: no engine edit; loses a few columns of the picture); (c) **mask the strip** (draw black or the nearest column
     over it): cheapest, honest about the limit. Say what each costs in fps, picture and code.
   - *Sky bleed:* whichever the diagnosis points at, for example landscapes shifted by the depth of the neighbouring surface at the
     seam, or drawn in the centre pass only, or the sky given a shift that matches what it borders. Do not guess: step 2 decides.
4. **Implement** with the usual loop for this branch: the code, **`c3d-critic` at effort medium on the commit** (max 2 revise rounds,
   user decision), the gates below, then the records. I implement inline (no `c3d-implementer`) unless the change is large.
5. **Gates (Azahar, both backends where M9 required both):** the new test fails before the fix and passes after; T2 identity (right
   eye = left eye at zero parallax, 0 differing pixels) and T4 gaps still as recorded; **T5 regressions**: M6/M6b gates with
   `stereo 0`, and `stereo 1` at `stereo_slider 0` (`stereo=0`); fps @60 on L0/L6/L20 with fill 1 no worse than 55 and **recorded
   beside this session's console figures** (`PERF.md` "Stage 10 M9 — console": 60.54 / 60.46); `c3d-host` test green
   (`scripts/3ds/tests/c3d-host/build.sh`), the four builds with 0 warnings, and rule 6's `nm` check on the OFF build (must print 0).
6. **Console short check** (with me; write it as a runbook first, see `NEXT-SPRINT.md` "M9 console check"): the two defects looked at
   by eye on L0 with the camera turning, each eye alone, slider low / full; **then finish the blocks the M9 check did not reach: F** (fill
   0 against fill 1 by eye), **G** (`m9-console-identity.cfg` + Terminal Test plugin: identity across a map, terminal, new game and
   saved game; the ramp after a level change), **H** (menus in 3D mode and the Renderer preference), **J** (comfort tuning, one key at a
   time: `stereo_k`, `stereo_w0`, `stereo_near`, `stereo_weapon`, `stereo_ramp`). Configs are in `scripts/3ds/tests/c3d/m9-console*.cfg`.
   Ask me for the console's IP first (it was 192.168.100.72) and tell me before each launch which files go on the card.

## Not in this session

- The sky "further back": with the current model (`shift = e · P · max(1 − w0/w, −near)`) the sky already sits at the largest shift
  for a given `P`, so a deeper sky means a landscape-only extra shift or a larger `stereo_k`. Raise it with me after the defects are
  fixed; do not slip it into the fix.
- M7 (supersampling), Classic mode, Old 3DS, the M8 and M6b console checks (ask me at the end if time is left; do not start them).
- Any change to D1-D4 or to the comfort defaults before block J has been run.

## Rules that bite here

- Engine edits (anything under `Source_Files/`) are fenced with `#ifdef A1_PLATFORM_3DS`, get a `PATCHES.md` number (**next free: 57**),
  and pass `scripts/3ds/host-baseline/ppcheck.sh "<changed files>" "<TUs>"` (Docker; check `docker ps` plainly, there is no `timeout`
  command on this Mac). Port code stays in `ports/3ds/`.
- Measure before and after (rule 1); keep nothing without a measured gain or a gate that now passes.
- No guessing about PICA200/citro3d: read `$DEVKITPRO/libctru/include`, `citro3d.h`, `c3d/*`, and cite them; mark anything unverified
  **UNVERIFIED** in the design.
- Commits: `stage10: M9 fix: <summary>` on `stage10-c3d`, one per finished piece, a tag `port-stage10-M9b` when the console agrees. Back up
  the bundle after approved commits (`STAGE5-PREREQS.md` section 8; the restore test needs the **full** hash of `fc4c274`:
  `fc4c274359088614b58c91af5daa541504cb50cf`). Update the project memory's stage 10 entry.
- If I am away: Azahar work, code, tests and docs only; write the console part up as a runbook and ask nothing mid-run.

## Afterwards

Report per defect: what it was (with the evidence), the fix and its cost, the gates, and what the console showed; then my choices:
the sky's depth wish, M7's kickoff (`KICKOFF_M7.md` is not written yet and must start from design 9.1's VRAM note and the M9 console fill
figures), the M8 and M6b console checks. Do not push.
