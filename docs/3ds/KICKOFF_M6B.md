# Paste this into a fresh Claude Code session started in the stage10-c3d worktree

(`/Users/dashielashie/Desktop/Aleph_3ds/.claude/worktrees/stage10-c3d`, branch `stage10-c3d`. Written 2026-11-08 after M8 and
the M6b specification. Earlier prompts: `KICKOFF_PROMPT.md` (stage 10), `KICKOFF_M0.md` … `KICKOFF_M6.md`, `KICKOFF_M8.md`.)

---

We are on Stage 10 of the Aleph One 3DS port (the optional citro3d GPU renderer behind `A1_RENDER_C3D`).
**This session builds M6b: fades, flashes and tints in direct mode.** The specification is written and critic-approved:
`docs/3ds/CITRO3D_DESIGN.md`, section 7, the block "M6b — fades, flashes and tints in direct mode", sub-bullet
"**M6b specification (revision 3 …, APPROVED)**" (grep it). Read that block in full first; it rules over everything else, and it cites
the code it depends on. Its Review log entries are "M6b specification drafted", "M6b pre-implementation" rounds 1-4,
"M6b: every fade on the GPU". **Order after M6b: M9 (stereo), then M7 (supersampling) last.** Be economical: the user is
short on time and usage (no subagents beyond a `c3d-critic` at effort **medium** on the commit, at most 2 revise rounds;
implement inline; no Azahar series that already passed on the same binary).

## Where the project is

- M0-M6 and M8 are done (`CLAUDE.md` "State of the project"). M8 (2026-11-08): the Renderer preference; "GPU (experimental)"
  = `renderer c3d`, `mode direct`, `fallback sw`. **Brightness was removed from the port** (`PATCHES.md` 56, on `3ds-port` and
  here), so `kDirectGamma` now comes only from fades, flashes and tints.
- Today every frame with a fade is not direct (`blocked=gamma`): readback, or SW under `fallback sw`. M6b makes them direct.
- Console checks (M8's and others) are **deferred by the user**: do not plan console work; write the M6b console items into
  the spec's console list and a short runbook in `NEXT-SPRINT.md`, not to run.

## The design in one paragraph (the spec has the details and citations)

In a 32-bit build every fade is three per-channel 256-entry curves (`current_gamma_r/g/b`, engine globals) over a grey ramp.
The port reads them at hook 3a (`direct_policy`, `front/frame.cpp`), classifies them and, when they fit, keeps the frame
direct and adds passes to the composite (`present_composite`, `back/present.cpp`) **after the overlay bands** (SW fades
the overlays too). **Part 1** (tint and soft_tint: damage/pickup/teleport flashes, cinematic fades, underwater and the other
liquids): `out = B + A·dst`, one quad, blend `ONE, CONSTANT_COLOR`. **Part 2** (negate, dodge, burn: exact; dodge's `uint16`
wrap is clamped, user decision): the TEV rebuilds the composited pixel from the view and overlay textures, then MIN and MAX
blend passes. **Part 3** (the compiler bolt's screen static): GPU-styled spatial noise from M5's noise texture, density from
the curves (user decision: it may look different from SW). **No engine edit.** Gate (user-accepted): ≥ 99.9 % of pixels within
Δ 2 per channel against SW, max Δ ≤ 4, through an extended `direct_check`; part 3 is functional. Pause darkening stays out.

## What to build (in the spec's order)

1. `core/fade_fit.{h,cpp}` with host tests (`scripts/3ds/tests/c3d-host/`): replicas of the six faders over the grey ramp,
   the part 1 / part 2 / part 3 classification, the GPU rounding model, the cache.
2. Part 1 end to end (front: fit + mask in `direct_policy` in the spec's order, one-frame hand-off; back: the fade quad and
   its restore; counters, the `[c3d] fade:` line, keys `fades`, `fade_tol`, `fade_parts`), then `direct_check`'s extension
   (`within_2`, max Δ, `fade=1`, the reference from the frame's curve copy).
3. Part 2 (texcoord1 via the `flip` uniform, the five-stage TEV plan, MIN/MAX passes, the per-row-range split for the
   overlay texture, the state restore), then part 3 (detection last, `noise_get()` in `direct_policy`). **Three commits**,
   parts 1, 2, 3, each gated before the next (spec "Work split"); the tag goes on the last.
4. The plugins "Fade Test" and "Liquid Test" (`scripts/3ds/tests/plugins/`, wired into `run-azahar.sh` like the others).
5. Acceptance in Azahar (both backends where M6 ran both), the bench (`c3d_composite` on fade frames), the regression runs
   the spec lists (M6's `direct_check` on L0/L6/L20; `fades 0` against the L01 film).

## Rules that bite here

- `CLAUDE.md` rules; rule 7: no guessing about PICA/citro3d: cite headers; mark the rest **UNVERIFIED** in the spec.
- Never break the OFF build or the host build (rule 6: `nm` 0 on the OFF `alephone.elf`); 0 warnings in all four builds.
- Azahar's emulated card is shared: take `~/Applications/Azahar-2126.2/user/.run-lock` (mkdir/rmdir) around every run; in a
  worktree pass `SCENARIO_SRC="/Users/dashielashie/Desktop/Aleph_3ds/data/Scenarios/Marathon 2"`. Under Azahar's software
  backend run films with `gxq_budget 3` (Review log "M3 acceptance, software-backend films").
- Commit `stage10: M6b <summary>`, tag `port-stage10-M6b`, `PERF.md` section "Stage 10 M6b" (Azahar — non-authoritative),
  design status line + Review log entry, `CLAUDE.md` one sentence, backup bundle refresh (recipe `STAGE5-PREREQS.md`
  section 8; `git fetch --depth 1` needs the full `fc4c274359088614b58c91af5daa541504cb50cf`), the project memory's stage 10
  entry. **Do not push.**
- If something in the spec proves impossible or breaks an earlier gate, stop and ask the user ("within reason").
- Then stop and report. Next is **M9** (stereo): its spec must redo the VRAM budget first (design section 6, M9 block).
