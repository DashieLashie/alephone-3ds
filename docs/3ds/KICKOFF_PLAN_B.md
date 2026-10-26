# Paste this into a fresh Claude Code session started in the stage10-c3d worktree

(`/Users/dashielashie/Desktop/Aleph_3ds/.claude/worktrees/stage10-c3d`, branch `stage10-c3d`.
Written 2026-10-26. The original stage 10 prompt is `KICKOFF_PROMPT.md`; this one continues it.)

---

We are on Stage 10 of the Aleph One 3DS port: the optional citro3d GPU renderer behind `A1_RENDER_C3D`. **Start developing
now: the next piece is plan B, the zero-copy present.** Nothing is paused and no decision is open. Do not wait for a G0
or G1 gate: both are behind us (G0-b ran on the console 2026-10-19; the user decided the same day to keep the GPU
present, build the renderer aiming at M7 supersampling and M9 stereo, and try the zero-copy present; that decision is
your go for B).

## Where the project is (all verified on a real New 3DS)

- P0 (software baseline) and P1 (GPU present, tag `port-stage10-P1`) are done.
- **Plan A, the fixes that had to come before anything builds on P1, is complete (2026-10-26):** the HOME > Close hang,
  the fatal-error hand-off to the main thread, the 3dslink mirror stall, Power Off in a game, and the fatal fallback's
  exit (A7: `aptExit` before the process ends; A7 is also on `3ds-port`). Results: `docs/3ds/NEXT-SPRINT.md` items 14-16,
  plan A items 0-7, `CITRO3D_DESIGN.md` 5.6.4. Never end the process with a bare `svcExitProcess`: it restarts the console.
- Branch state: `stage10-c3d` and `3ds-port` are both pushed to the fork (`alephone-3ds`). Git rules are in `CLAUDE.md`:
  never push to `origin`; push to the fork only when the user asks.

## Read first, in this order

1. `CLAUDE.md` (all 8 rules; "Platform traps"; "Testing in Azahar"; "Testing on the console"; "Stage 10").
2. `docs/3ds/NEXT-SPRINT.md`: the end of the file, plan A/B/C (about line 1124 for B), and the rules at its top.
3. `docs/3ds/CITRO3D_DESIGN.md`: 0.5 (goal, gates, decisions), 5.1 (the GPU present), 5.6 (release, gate, fatal
   paths: do not break them), 9.3 (where the time goes: "present + blit, ~5 ms"), 10 (needs console verification).
4. `docs/3ds/PERF.md`: "How to read these numbers", the profiler, the P0/P1 sections, "Console G0-b" (first and second
   session: the real present numbers), and the rejected experiments.
5. `ports/3ds/render_c3d/back/present.cpp` (its header comment is the present's 5 steps) and `back/context.cpp`;
   `Source_Files/RenderOther/screen.cpp` (`main_surface`, the present seam); `docs/3ds/PATCHES.md` (next free number
   for B: **55**; 45-48 are reserved for M0/M2/M6/M8).

## The task: plan B, the zero-copy present

Today the present copies the 400x240 (or halves 640x480) `main_surface` into a linear **staging image** with the CPU
(`<< 8`, UHADD8 2x2 box for Classic), flushes the cache, does one display transfer (linear -> tiled) into a VRAM
texture, then draws one quad. The idea (critic P1 finding 3, summarised in `NEXT-SPRINT.md` plan B): put
`main_surface`'s pixels in **linear memory the GPU can read directly**, and let the GPU/TEV reorder the channels, so the
CPU copy and its cache cost disappear. It needs **one engine edit** (where `main_surface` is allocated), so rule 2
applies in full (`#ifdef A1_PLATFORM_3DS` fence, a numbered `PATCHES.md` entry, `scripts/3ds/host-baseline/ppcheck.sh`
IDENTICAL or DIFFER with 0 normalised lines).

What I want from you, in this order:

1. **Establish the baseline and the goal.** From `PERF.md`, the console numbers of the current GPU present (HUD + Map
   and Classic, @30 and @60: `present`, `bottom`, `max`). State what B could save and what it cannot (Classic still
   halves 640x480 -> 400x240; byte-identical Classic output is a standing requirement: `present_diff` must stay at 0
   differences). If the expected saving is within the noise (the console measurements are ~1 ms precise), say so
   **before** writing code and propose stopping at the design. Rule 1: only measured gains are kept.
2. **Design B** as a short section in `CITRO3D_DESIGN.md` (new 5.7 or an addendum to 5.1): where the surface is
   allocated (`linearAlloc`, alignment, pitch), the format and channel order the GPU needs (RGBA8 vs the surface's
   ARGB/ABGR: cite `$DEVKITPRO/libctru/include`, `citro3d.h`, `c3d/*` and the examples under
   `/opt/devkitpro/examples/3ds/graphics/gpu/`; mark every unverified claim **UNVERIFIED**, rule 7), what the GPU reads
   (texture in linear memory vs a display transfer), the cache flush (the ranged flush from piece 5 stays), the
   interaction with the present gate and the release paths (5.6), the 640x480 / odd-surface / menu cases, and how it
   is switched off (the default build, `A1_RENDER_C3D=OFF`, must stay byte-for-byte unchanged: `arm-none-eabi-nm
   build-3ds/alephone.elf | grep -ciE 'citro|C3D_|a1_c3d'` prints 0).
3. **Review the design**: `c3d-critic` (effort medium, the user's standing choice), at most 2 revise rounds.
4. **Implement** with `c3d-implementer` (one piece), then `c3d-critic` on the commit. Commit as
   `stage10: B <summary>`, tag `port-stage10-B`.
5. **Azahar first** (`scripts/3ds/run-azahar.sh`, `scripts/3ds/tests/perf-bench.sh`; in this worktree pass
   `SCENARIO_SRC="/Users/dashielashie/Desktop/Aleph_3ds/data/Scenarios/Marathon 2"`): `present_diff` 0 differences,
   scripts 07/08/13/14 and the HOME/Close scripts (22) still pass, the profile build shows the `present` zone change.
   Label results "Azahar — non-authoritative". Record them in `PERF.md` in the style of the P1 sections.
6. **Console last, with the user** (they are at the console only when they say so): write the session up as a
   runbook (as `CONSOLE-SESSION-A6.md` and the "Fallback exit runbook" do) and wait for them. Until then, work that
   needs neither the console nor a decision continues (rule 8).

## Standing rules and traps (the short list; `CLAUDE.md` has the rest)

- Subagents (`c3d-architect`, `c3d-critic`, `c3d-implementer`) do not see this conversation: give each everything it
  needs. The critic and architect run at effort medium.
- Zero compiler/linker warnings; devkitARM `-O2`; no LTO; `-DNDEBUG` compiles out `assert()`.
- Build dirs: `build-3ds` (software), `build-3ds-c3d` (GPU present), `build-3ds-prof`, `build-3ds-c3d-prof`. Both
  must keep building. Engine TUs never include `<3ds.h>`.
- citro3d owns the top screen when it is up: only `GX_*` through its queue, and release it before `gfxExit`
  (`--wrap=gfxExit`). Nothing in B may touch citro3d off the main thread.
- Memory: 96 MiB heap area, linear heap ~19.9 MiB (must fit inside the heap area), VRAM 6 MiB, citro3d about
  1 MiB of linear heap. Account for the new linear allocation.
- Anything that needs `sudo`: give the user the command to run in **Terminal.app**.
- The console is a New 3DS at 192.168.100.72 (it was; ask). Tell the user each time whether you need ftpd or the
  Homebrew Launcher. Raw logs and captures stay in `build-3ds/hw/` (ignored by git).

## Deliverables and reporting

- Code and docs committed on `stage10-c3d` (one commit per finished piece), the backup bundle refreshed and
  restore-tested (`STAGE5-PREREQS.md` section 8) after approved commits. **Do not push** unless the user asks.
- Finish with a report: what you did, where it lives (branch, commit, tag), the measurements with their labels, and
  the next command or the console runbook the user needs to run.
- After B: plan C, the renderer, M0 first (`CITRO3D_DESIGN.md` section 7). Do not start M0 inside this task; report
  and let the user say when.
