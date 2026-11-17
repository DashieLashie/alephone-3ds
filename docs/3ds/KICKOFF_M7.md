# Paste this into a fresh Claude Code session started in the stage10-c3d worktree

> **Superseded 2026-11-17: M7 was dropped by the user** (design Review log "M7 dropped"; the cheaper anti-aliasing options are in
> `NEXT-SPRINT.md` "Antialiasing, to explore later"). Do not run this prompt. Kept as a record.

(`/Users/dashielashie/Desktop/Aleph_3ds/.claude/worktrees/stage10-c3d`, branch `stage10-c3d`. Written 2026-11-17 after the M9 fix
(`9760e14d0` edge strip, `a0b9061bf` sky bleed; both seen right on the console 2026-11-15). Earlier kickoffs of the same shape:
`KICKOFF_M6B.md`, `KICKOFF_M9_FIX.md`.)

---

We are on Stage 10 of the Aleph One 3DS port (the optional citro3d GPU renderer behind `A1_RENDER_C3D`). **This session starts
M7, 2x2 supersampling (SSAA), the last milestone.** M7 has **no specification yet**: this session writes it first, has it
reviewed, brings me the decisions it needs, and only then builds. Be economical: I am short on usage (no subagents beyond one
`c3d-critic` at effort **medium** per review, at most 2 revise rounds for the spec and 2 for the code; implement inline; no Azahar
series that already passed on the same binary). Do **not push**.

## Where the project is

- M0-M6, M6b, M8 and M9 are done and tagged; the M9 fix is committed and console-confirmed, **untagged**: the tag
  `port-stage10-M9b` waits for the rest of `NEXT-SPRINT.md` "M9 fix console check" (edge control, bench @60, M9 blocks F/G/H/J),
  which needs me at the console. **M7 does not wait for it**: build on the current head; do not tag M9b yourself.
- Direct mode (M6) is what the Renderer preference ships (M8: "GPU (experimental)" = `renderer c3d`, `mode direct`,
  `fallback sw`). Console @60: direct 60.5 fps on L0/L6/L20, but **SW also holds 60.5**, so M7 is the fidelity payoff stage 10
  was started for ("Secondary: fidelity the CPU cannot afford", design 0.5; `STAGE7.md`:61-67: SW SSAA at 800x480 would cost
  ~+16 ms, below 30 fps).
- Stereo (M9) runs on direct frames whenever the 3D slider is up, with the right-eye objects (887 KiB VRAM) **always** allocated
  under the preference.

## What the design already says about M7 (read these first; none of it is a specification)

- Section 7, block "M7 — 2x2 supersampling": view at 800x480 in a 1024x512 texture target, halved by `GX_TRANSFER_SCALE_XY`
  (`gx.h`:33-38, readback) or a bilinear composite (direct); cache cap lowered (9.1). Accept: output vs a CPU 2x2 box of the same
  800x480 render read back, ≤ 1 per channel.
- **9.1 VRAM**, the M9 note at its end: SSAA alone fits (column "Direct + SSAA", pool 2 048 KiB), but **with the right-eye objects
  allocated the pool is at most ~1 490 KiB**, "unless M7 frees or reuses the right-eye view texture while SSAA is on"; a 1024x512
  view cannot exist twice, so **SSAA is off on stereo frames** (M9 block, 9.1).
- M6's G3-a block (grep "*G3-a, how it is measured*" and "*M7's landscape budget*"): the `g3a 1` test key already draws the view at
  800x480 into a 1024x512 target (nearest decimation, not M7's filter); **Marathon 2's landscape part 0 is 2 048 KiB RGBA8 = the
  whole M7 pool**, so M7 converts landscape parts to `GPU_RGB8` (1 536 KiB) if a host test shows their alpha is constant, else
  leaves them to FCRAM sampling (M5's `promotions_blocked` path). RGB565 is rejected.
- Fill figures (console): G3-b `gpu_ms view` 1.3-1.5 ms mono (M6); M9: 1.09 / 2.43 / 3.84 ms for 1 / 2 / 4 passes at 400x240,
  "about 0.7 ms per extra pass". 4x the pixels is an **unverified** extrapolation of ~5-6 ms (Review log "M6 console check"):
  fill is not the only cost. @60 the frame has ~16.7 ms; direct busy was ~13.3 ms.
- Open questions (section 8): **6 "SSAA default on or off?"**, **10 "800 px wide mode (`gfx.h`:101) instead of SSAA?"**;
  section 10 item 13 ("Stereo comfort/ghosting; SSAA look") and the two VRAM questions M6's console check did not reach
  (VRAM contents across HOME for `tex_view`; contiguous 512 KiB / 2 MiB blocks; step 3's `alloc-fail 5` is an unexamined lead).

## What the specification must settle (write it into the M7 block, cite code and headers for every claim)

1. **Scope:** direct mode only (my expectation: the preference is direct; readback's `GX_TRANSFER_SCALE_XY` path only if it is
   nearly free), HUD + Map only or Classic too (480x240 → 960x480 still fits 1024x512).
2. **The filter:** how the composite turns 800x480 into 400x240. A bilinear sample at the shared corner of four texels is their
   2x2 average in exact arithmetic; the PICA's filter weights and rounding are **UNVERIFIED** (rule 7: cite `gpu.h`/`texture.h`,
   mark the rest). Alternatives: `GX_TRANSFER_SCALE_XY` into a 512x256 texture before the composite (a copy per frame, cost to
   measure), or four nearest samples combined in the TEV. Pick one with a host model of its error against the ≤ 1 gate.
3. **Stereo against SSAA (the main decision, mine to make):** (a) SSAA off on any stereo frame (simplest; the picture changes
   when the slider moves: does it need M9's ramp or a fade?), (b) free or reuse the right-eye view texture while SSAA is on, (c)
   SSAA and stereo mutually exclusive by preference. Give the VRAM arithmetic and the switch cost (allocations out of frame,
   M0's rules) for each.
4. **VRAM and the pool cap** in every combination (mono, SSAA, stereo, SSAA allocated + stereo frame), and the landscape:
   RGB8 (host test of the alpha first) or FCRAM; a new texture kind touches `texcache`, the converter and its host tests.
5. **Everything that assumes a 512x256 view at 1x:** `Mtx_Ortho`, viewport/scissor, the M9 edge strips and parallax (in 1x
   pixels or target pixels), the seed/sync copies and `dest_view`, readback staging, `direct_check` / `seed_check` / the captures
   (they compare to SW at 1x: what each one checks under SSAA), M4's `rect_bias` and M5's landscape δ (sub-texel positions defined
   at 1x), the fades of M6b (applied in the composite, after the halving?), the overlay bands (stay 1x).
6. **The `w` trap:** every constant-`w` primitive stays at `kWMax` (32 767) or below (the M9 sky bleed: the console's PICA draws
   nothing of a polygon whose vertices all carry `w = 2^24`; Azahar does; the real limit is unknown).
7. **Switching:** a `debug-c3d` key (`ssaa 0|1`), and how a player turns it on (a third Renderer choice? a separate Graphics
   option, `PATCHES.md` 57+ if it touches `preferences.cpp`?), default on or off (question 6). Ask me; do not decide.
8. **Gates** (measured and reported, not deciding: design 0.5, "within reason"): the filter gate (≤ 1 per channel vs the CPU box
   of the same 800x480 render), the M1-M6b gates at 1x unchanged with `ssaa 0`, G3-a (VRAM, queues, command buffer) with SSAA,
   the bench @60 and @30 on L0/L6/L20 (Azahar only indicative: it does not model cache misses or fill), and a console runbook
   whose first item is `gpu_ms view` at 800x480.
9. **Not in M7** (say so): mipmaps/linear filtering of the world textures (question 8), the wide 800 px mode unless I choose it
   instead (question 10), SSAA on stereo frames under option (a).

## Order of the session

1. Read: `CLAUDE.md` (this worktree), design sections 0.5, 6, 7 (M6, M6b, M9 blocks and M7), 9.1, 10, the Review log entries
   "M7 moved to the end", "M6 console check", "M9 done", "M9 console check, partial", "M9 fix: the sky bleed"; `PERF.md` "Stage 10
   M6 — console", "Stage 10 M9 — console", "Stage 10 M9 fix — console".
2. A cheap Azahar probe if it helps the spec (the existing `g3a 1` key at 800x480: VRAM free, `promotions_blocked`, on L0 with the
   preference's stereo objects allocated). One run per question, recorded.
3. Write the spec into the M7 block; `c3d-critic` (medium) on it; apply; **then stop and ask me** the decisions in items 3, 4
   (if the host test leaves RGB8 open) and 7, in plain language with the trade-offs (I asked for plain explanations of gate
   issues before). Commit the spec (`stage10: docs: M7 spec ...`).
4. Build in the spec's commits, each gated in Azahar before the next (both backends where M6 ran both; under the software backend
   films with `gxq_budget 3`); `c3d-critic` (medium) on the code; bench; records.

## Rules that bite here

- `CLAUDE.md` rules; rule 7: no guessing about PICA/citro3d: cite `$DEVKITPRO/libctru/include`, `citro3d.h`, `c3d/*` and the
  examples; mark the rest **UNVERIFIED** in the spec.
- Never break the OFF build or the host build (rule 6: `nm` 0 on the OFF `alephone.elf`); 0 warnings in all four builds. An engine
  edit (unlikely) is fenced, numbered 57+ in `PATCHES.md`, and passes `ppcheck.sh` (Docker Desktop must be running: check with
  plain `docker ps`, macOS has no `timeout`).
- Azahar's emulated card is shared: take `~/Applications/Azahar-2126.2/user/.run-lock` (mkdir/rmdir) around every run; in a worktree
  pass `SCENARIO_SRC="/Users/dashielashie/Desktop/Aleph_3ds/data/Scenarios/Marathon 2"`; always pass `--timeout` (no default; a run
  without one sat 2.5 h); `perf-bench.sh` has a 240 s wall limit a slow Azahar can hit (rerun). Leave the card's prefs at
  fps_target 60 + HUD + Map.
- Quote variables in helper scripts (an implementer once `rm -rf`'d files through an unquoted zsh variable); keep helpers in
  `build/m7/tools/`, not the scratchpad (it was wiped once).
- Commits `stage10: M7 <summary>`, tag `port-stage10-M7` on the last; `PERF.md` section "Stage 10 M7" (Azahar — non-authoritative),
  design status line + Review log entry, `CLAUDE.md` one sentence, the console runbook in `NEXT-SPRINT.md` "M7 console check" (not
  to run), backup bundle refresh (recipe `STAGE5-PREREQS.md` section 8; `git fetch --depth 1` needs the full
  `fc4c274359088614b58c91af5daa541504cb50cf`), the project memory's stage 10 entry. **Do not push.**
- If something proves impossible on the hardware or breaks an earlier gate, stop and ask me.
- Then stop and report. After M7 the plan has no milestone left: the open console runbooks (M9 fix check and tag M9b, M6b, M8,
  B-b, M7) and the backlog are what remains.
