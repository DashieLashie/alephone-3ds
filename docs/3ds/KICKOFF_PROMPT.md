# Paste this into Claude Code (start in PLAN MODE: Shift+Tab until "plan mode")

We're doing Stage 10 of the Aleph One 3DS port: an OPTIONAL citro3d hardware renderer, behind A1_RENDER_C3D,
with the software renderer kept as default and fallback. I have NO console access right now, so everything must be
verifiable in Azahar and on the host; anything only hardware can prove goes on a "Needs console verification" list.

Three project subagents exist in .claude/agents/: c3d-architect (designs), c3d-critic (adversarial review, read-only),
c3d-implementer (one milestone at a time). Subagents don't see this conversation — pass them everything they need
in each task description.

## Phase A — Plan (stay in plan mode, no code changes)
1. Read CLAUDE.md, docs/3ds/{ENVIRONMENT,AUDIT,PATCHES,PERF}.md, and confirm where the software renderer and the
   OpenGL renderer live. Summarise in ≤15 lines.
2. Delegate to c3d-architect: write docs/3ds/CITRO3D_DESIGN.md (it knows the required sections).
3. Delegate to c3d-critic: review the design.
4. Loop: architect answers every finding in the doc's "Review log" → critic re-reviews. Max 3 rounds.
   If still BLOCK after 3 rounds, stop and show me the unresolved findings — don't force it.
5. Present the final plan to me: milestones, what each verifies in Azahar, the VRAM/perf budget, the list of
   effects that will be approximated, and the "Needs console verification" list. Wait for my approval.

## Phase B — Implement (after I approve and switch out of plan mode)
For each milestone M1..Mn in order:
1. Delegate to c3d-implementer with the milestone name and its acceptance criteria copied from the design.
2. Delegate to c3d-critic to review that milestone's commit (code + test evidence).
3. BLOCK/REVISE → back to implementer with the findings (max 2 rounds), then escalate to me.
4. APPROVE → tag port-stage10-M<n>, give me a 5-line status, continue.
Stop after M2 for a checkpoint with me, regardless of progress.

Ground rules: no guessing about citro3d/PICA200 behaviour — check $DEVKITPRO headers and examples and cite them.
Don't widen scope beyond the current milestone. Never break the host or software-renderer builds.
