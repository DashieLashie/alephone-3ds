# Aleph One 3DS port: stage 10 — citro3d renderer (design)

Status: design approved (critic round 3: APPROVE); P0/P1 done, G0-b run on the
console (2026-10-19); section 5.6 (the console fixes) added 2026-10-19, revised
after critic round 1 (REVISE) on 2026-10-23, approved in round 2 (8 minors
folded). Branch `stage10-c3d` (from `3ds-port` `6c3e077`). Written for the c3d-implementer agent, which builds **one
milestone at a time** from this document; anything unclear here is a
stop-and-ask, not an improvisation.

Conventions: `file:line` is the code as of `6c3e077`. **UNVERIFIED** marks a
citro3d/PICA200 claim that neither the local headers/examples
(`/opt/devkitpro/libctru/include/{citro3d.h,c3d/*,3ds/*}`,
`/opt/devkitpro/examples/3ds/graphics/gpu/*`) nor the critic's disassembly of
`libcitro3d.a`/`libctru.a` settles; each has a test in the milestone that
first depends on it. "per critic" = the critic's disassembly of the named
object, not read by the architect. Azahar timings are **emulator,
non-authoritative** ([PERF.md](PERF.md):434-454). The brief's "Stage 8
bottom-screen modes" are stage 7 ([STAGE7.md](STAGE7.md)).

## 0. Facts this design rests on

### 0.1 Project and engine

| Fact | Source |
| --- | --- |
| No `CLAUDE.md`; rules: measure before/after, keep only measured gains, `PATCHES.md` entry + `ppcheck.sh` per engine edit, Azahar first | [NEXT-SPRINT.md](NEXT-SPRINT.md):36-46 |
| Engine edits fenced by `#ifdef A1_PLATFORM_3DS`; port code in `ports/3ds/` | [PATCHES.md](PATCHES.md):1-21 |
| devkitARM r68 / gcc 16.1, libctru 2.7.0, citro3d 1.7.1, picasso 2.7.2; **no LTO** | [ENVIRONMENT.md](ENVIRONMENT.md):46-75, [PERF.md](PERF.md):273-297 |
| Main thread renders; audio on core 1 (7.8 ms/frame there) | [PERF.md](PERF.md):51-53, [STAGE7.md](STAGE7.md):220-222 |
| Engine TUs never include `<3ds.h>`; port TUs with engine headers stay apart from `<3ds.h>` | `a1_3ds_platform.h`:10-11, `a1_3ds_platform.cpp`:106 |
| 3DS engine pinned to software, `_no_acceleration`; bit depth 8, 16 or 32 allowed (32 default); `software_alpha_blending` off by default | `Misc/preferences.cpp`:4490-4515, :4263, :4271 |
| `main_surface` is always 32-bit ARGB8888, 640x480 (menus, dialogs, Classic) or 400x240 (HUD + Map game) | [AUDIT.md](AUDIT.md):416; `screen.cpp`:1076-1078, 128-131 |
| `HAVE_OPENGL` undefined on 3DS | `ports/3ds/CMakeLists.txt`:11-16 |
| 32-bit shading/tint table entries carry `Amask`: `SDL_MapRGB` ORs `Amask`; `pixel_format_32` is ARGB8888 → every entry `0xFFrrggbb` | `shapes.cpp`:1947-1954; `screen.cpp`:222-223; `SDL_pixels.c`:865 |
| SDL alpha blits produce premultiplied RGB onto a 0-cleared ARGB target | `SDL_blit_slow.c`:133-152 |

### 0.2 Who writes the top framebuffer today

| Writer | Source |
| --- | --- |
| SDL's N3DS driver: `gfxInit(GSP_RGBA8_OES, GSP_RGBA8_OES, false)` (linear, incl. right-eye buffers = the 2 100 KiB in use) | `SDL_n3dsvideo.c`:130; `a1_3ds_platform.cpp`:44-45 |
| The port's present (CPU rotate+convert, VBlank check, own swap) | `video_3ds.cpp`:119-138, 142-208 |
| SDL again whenever `present_screen()` returns false: `SDL_RenderPresent` → `gfxGetFramebuffer(GFX_TOP)` + swap | `screen.cpp`:2581-2591; `SDL_n3dsframebuffer.c`:80, 157 |
| The scenario chooser (`SDL_UpdateWindowSurface`, before the screen is set up) | `Misc/ScenarioChooser.cpp`:563 |
| The fatal error screen (`consoleInit(GFX_TOP)`, top swap) | `a1_3ds_platform.cpp`:944, 968, 995 |
| Teardown: `shutdown_application()` → `SDL_Quit()` → SDL's video quit → `gfxExit()`; also from `vhalt()` | `shell.cpp`:650-661; `CSeries/csalerts_sdl.cpp`:322-329; `SDL_n3dsvideo.c`:184 |

### 0.3 citro3d/PICA facts (headers, examples, disassembly per critic)

| Fact | Source |
| --- | --- |
| 6 TEV stages, no programmable fragment stage | `registers.h`:209-265; `c3d/texenv.h`:4-25 |
| No paletted texture format | `enums.h`:63-79 |
| Texture sides POT in [8, 1024] | `c3d/texture.h`:54; texture.o per critic |
| Texture unit 1 samples texcoord1 | `normal_mapping/source/vshader.v.pica`:16, `main.c`:149 |
| `GPU_UNSIGNED_BYTE` attributes arrive unnormalised | `loop_subdivision/source/main.c`:53, `program.g.pica`:34 |
| VRAM 6 MiB, banks A/B | `os.h`:59-61; `allocator/vram.h`:7-12 |
| `C3D_Init(size)`: one `linearAlloc(size)` command buffer, a 1 KiB malloc for the GX queue (**32 entries** × 32 B, `gx.h`:83-92), an APT hook, `GX_BindQueue`, GSP callbacks; `vramAlloc` only in `C3D_RenderTargetCreate` | `base.h`:13; base.o C3D_Init+0x58-0x6c per critic |
| `gxCmdQueueAdd` **svcBreaks** when the queue holds 32 entries; only `gxCmdQueueClear` (in `C3D_FrameBegin`) resets it | gxqueue.o +0x10, +0xdc-0xe4; renderqueue.o FrameBegin+0x64, per critic |
| Queue entries per frame: each in-frame `C3D_SyncTextureCopy`/`C3D_SyncDisplayTransfer` = 1, plus 1 more (a command-list split) **unless** `gpuCmdBufOffset == 0`; `C3D_FrameEnd` = 1 + 1 per drawn linked target | base.o C3Di_SplitFrame+0x0-0xc, renderqueue.o per critic |
| Command-buffer overflow **svcBreaks** (`GPUCMD_Add`); `gpuCmdBuf/Size/Offset` exported | `gpu.h`:13-15; gpu.o per critic |
| `C3D_FrameSplit`/`GPUCMD_Split` does **not** free space (advances the buffer start, shrinks the size); only `C3D_FrameBegin` rewinds to the full buffer | gpu.o +0x58-0x74; renderqueue.o FrameBegin+0x78-0x9c, per critic |
| `C3D_FrameBegin(0)` waits for the whole queue (`gxCmdQueueWait(-1)`), stops and clears it; **no VBlank wait** (only `C3D_FRAME_SYNCDRAW`); returns **false if already inside a frame** | `renderqueue.h`:20-25, 31; renderqueue.o FrameBegin+0x8-0x3c, +0xa4-0xc4, per critic |
| ⇒ **"frame retired" = `C3D_FrameBegin(0)` returned**: every earlier C3D frame's draws, copies and linked transfers are done. The only port-visible signal; every deferred action hangs off it | per critic (settled) |
| GPU work starts only at `C3D_FrameEnd` (`gxCmdQueueRun`); in-frame queueing does not start it | gxqueue.o +0x9c-0xd4; renderqueue.o FrameEnd+0x130, per critic |
| Inside a frame, `C3D_SyncDisplayTransfer`/`C3D_SyncTextureCopy` queue (no wait: `C3Di_SplitFrame` + `GX_ProcessCommandList` of the part so far, then the transfer behind the draws); outside a frame they `gxCmdQueueWait(-1)`, issue, run the queue and `gspWaitForEvent(PPF, false)` = wait *then clear* of a **sticky** event nobody clears after the present's in-frame transfers, so the wait can return **before the transfer is done** (Azahar completes transfers synchronously and cannot show it). **Never used for a result that is read afterwards**; the readback queues the transfer in the frame and confirms it with the next `C3D_FrameBegin(0)` (M0 revise 1, 7) | `renderqueue.h`:77-78; `renderqueue.o` `C3D_SyncDisplayTransfer` disassembled (main session, libcitro3d.a 1.7.1), `svc.h`:107 RESET_STICKY, `gspgpu.h`:155, 183; same reason as 5.7.4 "Rejected" |
| An in-frame `C3D_SyncDisplayTransfer`/`C3D_SyncTextureCopy` submits the command list so far with flags **0** (no GSP cache flush), and `C3D_FrameEnd(GX_CMDLIST_FLUSH)` skips the whole-heap flush, so draws issued **before** an in-frame Sync* would run from RAM with their bytes possibly still in the dcache. **Rule: an in-frame Sync\* after draws is preceded by `C3D_FrameSplit(GX_CMDLIST_FLUSH)`** (one more queue entry); the Sync*'s own split then finds an empty buffer. The present's in-frame upload is unaffected (it runs on an empty command buffer) | `renderqueue.o` `C3D_SyncDisplayTransfer`+0xcc-0xdc, `C3D_FrameEnd`+0x7c-0x90/+0x178-0x188, `C3D_FrameSplit`+0x24-0x48; `gx.o` `GX_ProcessCommandList`+0x1c-0x44 (flush = `(flags >> 1) & 1`); `renderqueue.h`:33, `gx.h`:66 (M0 critic round 2, 1) |
| `C3D_RenderTargetDelete` **inside a frame** svcBreaks (`USERBREAK_PANIC`: the first thing it does is test the in-frame flag); outside one, for an unlinked target, it waits for the queue itself and destroys. Targets are created and deleted out of frame | `renderqueue.o` `C3D_RenderTargetDelete`, disassembled (M0 revise 1) |
| `GX_DisplayTransfer`/`GX_TextureCopy` called directly go into citro3d's bound queue → **never call them directly** | `gx.h`:139-143, 183, 195; per critic |
| `C3D_TexLoadImage`: memcpy for linear textures, TextureCopy for VRAM | `texture.h`:55; texture.o per critic |
| Linked targets (`C3D_RenderTargetSetOutput`) are transferred only in frames that drew on them; the destination is taken from `gfxGetFramebuffer` **at `C3D_FrameEnd`**; the swap happens later in `onQueueFinish` on the GSP thread | `renderqueue.h`:11, 65; FrameEnd+0xb8-0xd4, framebuffer.o, per critic |
| citro3d does **not** guard against writing the displayed buffer: a swap flips `gfxCurBuf[scr]` and calls `gspPresentBuffer`; the old buffer stays on screen until the next VBlank | gfx.o per critic (settled) |
| Swaps keep per-screen state: citro3d swapping top on the GSP thread and the port swapping bottom on the main thread do not race | gfx.o per critic (settled) |
| `C3D_FrameEnd(flags)` without `GX_CMDLIST_FLUSH` flushes the whole linear heap (~19.5 MiB) | `gx.h`:66; per critic |
| `C3D_Init` defaults: cull back CCW, depth test GREATER + write, alpha blend SRC_ALPHA — must be overridden | `enums.h`:174, 231, 307; base.o per critic |
| `C3D_Init` zeroes the three texture-unit pointers and sets **all six TEV stages** to `C3D_TexEnvInit`'s values (sources `PREVIOUS`, operands 0, `REPLACE`, colour `0xFFFFFFFF`, scale 1: a pass-through) | base.o `C3D_Init`+0x14c-0x154, +0x15c-0x178 (M3 spec, main session); `texenv.h`:33-43 |
| The state flush enables texture unit *i* only if its pointer is non-null (a null unit is skipped, not dereferenced) and sets the unit mask + bit 16 (texture-cache clear) in the tex config | base.o `C3Di_UpdateContext`+0x1e0-0x230 (M3 spec) |
| `C3D_SetTexEnv(id, env)` copies the 20-byte stage into the context and marks it dirty; `env == NULL` writes `C3D_TexEnvInit`'s values | texenv.o `C3D_SetTexEnv`+0x14-0x58, +0x5c-0x90 (M3 spec) |
| **`C3D_TexBind(unit ≥ 1, NULL)` dereferences NULL + 12** (it reads `tex->param` to refuse non-2D types before storing; only unit 0 skips the test): through the API, units 1-2 can only be re-bound to another texture; M3 unbinds unit 1 with the store itself (`__C3D_Context` + 0x118 + 4·unit, dirty bit 23 + unit), checked at run time | texture.o `C3D_TexBind`+0x10-0x38 (M3 round 1, critic; M3 acceptance) |
| An enabled texture unit whose texcoord the shader does not output samples an undefined coordinate: **Azahar's software backend segfaults** (`Pica::Texture::LookupTexelInTile`); the host-GPU backend does not; PICA UNVERIFIED | macOS crash reports, M3 acceptance |

### 0.4 Current numbers

| | Source |
| --- | --- |
| Console, HUD + Map, L0 @30: busy **14.02** = render 5.78, blit 0.96, present 4.33, bottom 1.93; idle 18.67 | [STAGE7.md](STAGE7.md):220 |
| Console, Classic, L0 @30: busy 18.24, present 8.67-8.78; @60: **53.16 fps** | [STAGE7.md](STAGE7.md):466; [PERF.md](PERF.md):76 |
| HUD + Map at Framerate Target 60: **never measured** | — |

## 0.5 Goal, order and gates

At the 30 fps cap the software renderer idles more than half of each console
frame (18.67 of 32.69 ms), so "faster at 30" is not a goal. **Target
(confirmed by the user, 2026-10-16):** *HUD + Map at Framerate Target 60 sustains ≥ 55 fps
(busy ≤ 18 ms) on L0/L6/L20 on a New 3DS, no regression at 30; Classic at 60
not worse than today's 53.16 fps.* Secondary: fidelity the CPU cannot afford
(2x2 supersampling, `STAGE7.md`:61-67).

The console saving lives in **present + blit (~5 ms)** (9.3), and the GPU
present needs no 3D renderer, so it comes first. Every gate has an
**a-half** (provisional, Azahar, deterministic or relative measures — lets
work continue without a console) and a **b-half** (console confirmation,
section 10). A failed b-half reopens the decision taken on the a-half.

| Phase | Content | Gate after it |
| --- | --- | --- |
| **P0** | Software baseline @30/@60, both Bottom Screen settings (no code) | — |
| **P1** | citro3d bring-up + **GPU present** of the top screen | **G0-a**: P1 captures ≤ 1/channel (Classic byte-identical), scripts 07/08/13/14 pass, frame-indexed test passes @30 and @60, Azahar `present` zone (instruction-cost proxy) drops ≥ 50% in HUD + Map. **G0-b**: console `present` −2 ms in HUD + Map, HOME/sleep/close, no tearing |
| — | | **G1** (user): build the 3D renderer? Decided from P0/P1 Azahar data with the caveat that console numbers are unconfirmed (Azahar overstates render ~40%, understates copies ~40%, [PERF.md](PERF.md):90-110) and that M0-M5 bring **no speedup** |
| **M0-M5** | Harness, M1 = flat-shaded walls, ... full renderer in readback mode | **G2-a**: `R_est = (R_az / render_az) × 5.78 ms ≤ 3 ms` (R = M0 null-mode floor) and Azahar busy(readback) ≤ busy(SW) + 1 ms. **G2-b**: the same on the console |
| **M6** | Direct mode (HUD + Map) — the renderer's only performance payoff | **G3-a**: VRAM 9.1, command-buffer and queue budgets hold at 800x480 in Azahar. **G3-b**: console `C3D_GetDrawingTime` leaves room for 4x fill |
| M8, M9, M7 | Preference, stereo, supersampling — **all to be built, in this order: M7 goes last** (user, 2026-11-08; before that M7 → M8 → M9, 2026-11-07; optional before that) | — |

**User decisions (2026-10-16):** stage 10 **pauses at G0** until a console
session confirms G0-b; it does not proceed on G0-a alone. G1 (build the 3D
renderer?) is decided by the user after G0-b. The current session implements
**P0 and P1 only**.

**User decisions (2026-10-19, after the G0-b console sessions, `PERF.md`
"Console G0-b"):** **G0-b: keep the GPU present**, once the console findings are
fixed (HOME > Close in a game hangs; `error_screen()` on a second thread draws
nothing; and, outside citro3d, `NEXT-SPRINT.md` backlog items 14 and 15). **G1:
build the 3D renderer**, aiming at **M7 (2x2 supersampling) and M9 (stereo 3D)**:
they are wanted as features, not for speed (the software renderer already meets
the target on the console). **Try the zero-copy present** (critic P1 finding 3).
Order: the fixes, then the zero-copy present, then M0-M6, M7, M9 (open questions
3 and 9).

**User decisions (2026-11-07, during M5, before G2-a is measured):** **continue until stereoscopic 3D (M9)**
whatever G2-a's result: M5 → M6 (direct mode, which M9 needs: section 6) → M9. **Amended the same day: M7 and M8 are
not optional either** ("I want to try all possibilities"): the order is section 7's, **M5 → M6 → M7 → M8 → M9**. **Re-ordered 2026-11-08 (user): M7 goes last, M6 → M8 → M9 → M7** (Review log "M7 moved to the end"). Every gate
(G2-a, G2-b, G3-a, G3-b) is still measured and recorded, but none decides whether the next milestone is built. Also: play checks leave SW's static coverage out (Review log "M5, play check and
static, user decision").

**Status, 2026-10-26:** G0 and G1 are behind us. The fixes (plan A, 5.6.0 pieces 0-5 and
A6/A7) are done and verified on the console; the next piece is the zero-copy present (plan B),
then M0 onwards. The user's go for B is the 2026-10-19 decision above.

## 1. Frame flow and the seam

### 1.1 Software path per frame (reference)

1. `render_screen()` (`screen.cpp`:1496) sizes `world_view` = `BufferRect`
   (:1610-1621; 400x240 HUD + Map, 480x240 Classic) and calls
   `render_view(world_view, software_render_dest.get())` (:1684-1697, zone
   `Render`).
2. `render_view()` (`render.cpp`:420-515): vis tree, sort, objects
   (:449-464), `Rasterizer_SW` (:466-479), `Render_Classic.render_tree()`
   (:488-497), weapons `render_viewer_sprite_layer()` (:501-503), `End()`.
3. `Render_Classic` (`RenderRasterize.cpp`) clips in eye space against the
   clipping windows and hands over screen-space integer-vertex convex
   polygons, back to front: `texture_horizontal_polygon` (:469),
   `texture_vertical_polygon` (:608), `texture_rectangle` (:652; weapons
   `render.cpp`:1083). Interface `Rasterizer.h`:35-60.
4. CPU overlays into `world_pixels` (`screen.cpp`:1727-1750), blit + gamma into
   `main_surface` (:1786-1797, :1992-1998), HUD routing (:1805-1822),
   `MainScreenUpdateRects()` → `a1_3ds::present_screen()` (:2570-2583),
   `a1_3ds::bottom_frame()` (:1847-1855).

### 1.2 The seam (hook 1, the only place the render path diverges)

`render.cpp`:466-506 becomes (original kept verbatim in `#else`):

```cpp
#if defined(A1_PLATFORM_3DS) && defined(A1_RENDER_C3D)
#ifdef HAVE_OPENGL
#error "A1_RENDER_C3D assumes the 3DS build (no OpenGL)"
#endif
			// docs/3ds/CITRO3D_DESIGN.md, section 1.2
			struct C3dDrawCtx { view_data* view; } c3d_ctx{view};
			auto c3d_draw = [](void* c, RasterizerClass* ras) {
				view_data* v = static_cast<C3dDrawCtx*>(c)->view;
				ras->SetView(*v);
				ras->Begin();
				Render_Classic.view = v;
				Render_Classic.RasPtr = ras;
				Render_Classic.render_tree();
				if (!Render_Classic.renders_viewer_sprites_in_tree())
					render_viewer_sprite_layer(v, ras);
				ras->End();
			};
			assert(software_render_dest);
			Rasterizer_SW.screen = software_render_dest;
			if (!a1_c3d::render_frame(*view, software_render_dest, &Rasterizer_SW, c3d_draw, &c3d_ctx))
				c3d_draw(&c3d_ctx, &Rasterizer_SW);
#else
			// original lines 466-506, unchanged
#endif
```

plus `#include "a1_c3d.h"` under the same fence. `Render_Classic` and
`render_viewer_sprite_layer` are file statics (`render.cpp`:290, :324).
Both macros are tested (port rule `A1_PLATFORM_3DS`, PATCHES.md:4-5;
`A1_RENDER_C3D` only defined with it by `ports/3ds/CMakeLists.txt`), so
non-3DS preprocessing is byte-identical.

**Why here:** the C3D rasteriser gets exactly the software rasteriser's
polygons, order and clipping; visibility, sorting, clipping and transfer-mode
animation (`render.cpp`:771-979) stay engine code. The in-frame pixel diff
(section 8) is then meaningful and the upstream surface is one block. The
classic OpenGL path did the same (`Rasterizer_OGL.h`:33-67 → `OGL_RenderWall`,
rebuilding eye coordinates from screen vertices, `OGL_Render.cpp`:1238-1403).
The shader renderer re-implements traversal with GL user clip planes and a
depth buffer (`RenderRasterize_Shader.cpp`:247-273, :1236); PICA's clip
registers are undocumented locally (`registers.h`:84-88 "Unknown"), so it is
not followed.

**Re-running `c3d_draw`** for one view is safe: `render_node_object()` resets
the rectangle's clip fields from the window before each call
(`RenderRasterize.cpp`:625-648), so SW's in-place clip edits
(`scottish_textures.cpp`:664-673) do not leak; transfer modes are
re-instantiated from `view->tick_count` (`render.cpp`:869). Only the
static-noise seed (`low_level_textures.h`:40-44) carries over; the harness
saves/restores it. M0 proves it (SW-vs-SW diff = 0).
*Superseded (M0 pre-implementation, round 1):* the weapon layer calls `rand()`
(`weapons.cpp`:2922-2924), so a second traversal is not side-effect free; the
harness records one traversal and replays it (section 7, M0 specification).

### 1.3 Support hooks

Engine hooks (fenced `#if defined(A1_PLATFORM_3DS) && defined(A1_RENDER_C3D)`,
`PATCHES.md` entry + `ppcheck.sh` each):

| Hook | Where | What | Milestone |
| --- | --- | --- | --- |
| 2a | `shapes.cpp`:1494-1496 (after `update_color_environment()`, inside the existing 3DS fence) | `a1_c3d::collections_changed()` — `load_collections()` unloads and reloads every collection (:1429-1437) and rebuilds tables (:1493) | M2 |
| 2b | end of `unload_all_collections()` (`shapes.cpp`:1179-1193; callers `shell_misc.cpp`:86, `screen.cpp`:327, `interface.cpp`:3009) | same call | M2 |
| 3 | `screen.cpp`:1727-1750, :1786-1797, plus one static helper in `screen.cpp` | direct frames only: overlays into `a1_c3d::overlay_surface()`, conservative overlay rectangles (5.3) passed to `a1_c3d::overlay_dirty()`, world-view blit skipped | M6 |
| 4 | `Misc/preferences.cpp/.h` | "Renderer" preference | M8 (optional) |

Port-only hooks (no engine edit): `video_3ds.cpp` `present_screen()` calls
`a1_c3d_present()` first (P1); `error_screen()` calls `a1_c3d_release_top()`
first (P1); `-Wl,--wrap=gfxExit` (pattern `CMakeLists.txt`:335-338) runs
`a1_c3d_release_top()` + `C3D_Fini()` before the real `gfxExit()` on **every**
teardown path, `vhalt()` included (0.2). The wrapper and the release are
**idempotent**, guarded by a `s_c3d_up` flag (set after a successful init,
cleared by `release_and_fini()`):

```cpp
extern "C" void __wrap_gfxExit(void)
{
	if (s_c3d_up) release_and_fini();   // a1_c3d_release_top() + C3D_Fini(); s_c3d_up = false
	__real_gfxExit();
}
```

On the `vhalt()` path `SDL_Quit()` → `gfxExit()` (wrapped: release + fini)
runs first; then `system_alert_user()` → `error_screen()`
(`csalerts_sdl.cpp`:175-182, 327-328) → `gfxInitDefault()`/`gfxExit()` enters
the wrapper again, and with the flag down it only calls the real `gfxExit()`,
never touching citro3d's freed queue entries or command buffer (freed in
`C3D_Fini`, which itself returns at once when inactive: base.o
C3D_Fini+0x8-0x10, per critic). `a1_c3d_release_top()` also returns at once
when `!s_c3d_up`. `gfxExit` is defined only in libctru's gfx.o and reached
from SDL's video quit (`SDL_n3dsvideo.c`:184) via `SDL_Quit()`
(`shell.cpp`:661: normal exit, HOME > Close, `vhalt`); SDL's message-box path
is unreachable on 3DS (per critic).

### 1.4 `a1_c3d` API

Engine-facing (`ports/3ds/render_c3d/include/a1_c3d.h`, no `<3ds.h>`):

```cpp
class RasterizerClass; struct view_data; struct bitmap_definition; struct SDL_Surface;
namespace a1_c3d {
using DrawFn = void (*)(void* ctx, RasterizerClass* ras);
// One world view. true: frame done (in `dest`, or on the GPU in direct mode).
// false: caller draws it with `sw`; nothing was written to `dest`.
bool render_frame(view_data& view, bitmap_definition* dest, RasterizerClass* sw,
                  DrawFn draw, void* ctx);
void collections_changed();                      // hooks 2a, 2b
SDL_Surface* overlay_surface();                  // hook 3; nullptr unless this frame is direct
void overlay_dirty(int x, int y, int w, int h);  // hook 3
bool direct_frame();                             // hook 3
}
```

Port-internal: `bool a1_c3d_present(const void* px, int pitch, int w, int h, uint32_t fmt);`
`void a1_c3d_release_top();`

### 1.5 Runtime selection and fallback (per frame, in `render_frame`)

| Check (in order) | Result |
| --- | --- |
| renderer not requested (`debug-c3d` absent or `renderer sw`; M8: preference) | `false` |
| citro3d not up | `false` |
| `bit_depth != 32`, `software_alpha_blending != off`, view side > 1024 | `false`, logged once per change |
| milestone < M4 | `false` in play (C3D only in diff frames) |
| frame has a class the milestone does not support | **discard** (below), `false` |
| ring full, texture pool full after eviction, command-buffer reserve or GX-queue budget reached (2.4) | **discard**, `false`; 10 in a row → off until next level |
| direct requested, frame not eligible (5.4) | readback for this frame |

**Discard** = stop issuing draws, transfers and promotions; `C3D_FrameEnd`
the frame normally (the reserves in 2.4 guarantee it can); skip the readback;
the caller redraws in SW. A frame cannot be abandoned: queued work still runs
and `C3D_FrameEnd` needs command words. In direct mode a discarded frame
becomes a readback-style frame (SW output in `world_pixels`, normal blit,
P1 present). One extra `render_tree()` on that frame; counters in
`[c3d] stats` once a second.

**Risk:** citro3d's APT hook, GSP callbacks and bound GX queue interfere with
SDL, the port's APT hook, the bottom console or `present_bottom()`.
**How we test without hardware:** P1 runs scripts `07-suspend-menus.txt`,
`08-demo-close.txt`, `13-bottom-screen.txt`, `14-terminal.txt` with citro3d
up; logs `linearSpaceFree()`/`vramSpaceFree()` (`linear.h`:49, `vram.h`:71)
around init; exit via HOME > Close and via a forced `vhalt` (`debug-c3d
test_vhalt`) logs `C3D_Fini` before `gfxExit`; the `test_vhalt` run reaches
the error screen and logs **one** release and **two** `gfxExit` calls.
Hardware → section 10.

## 2. Geometry

### 2.1 Input and vertex format

Polygons: 3..16 vertices (`scottish_textures.h`:52-53, :181-211), integer,
pinned (`RenderRasterize.cpp`:413, 430, 563, 579). The front end rejects
polygons with a vertex off screen or degenerate, as SW does
(`scottish_textures.cpp`:231-241, :449-459); does **not** draw
`_solid_transfer` (SW has no case for it: :308-322, :415-417;
`render.cpp`:959-961); fans into a u16 index ring (`C3D_DrawElements`,
`base.h`:24; `GPU_TRIANGLES`, `enums.h`:494-498). Consecutive polygons with
identical state merge into one draw.

Vertex 32 B: `v0` float3 `(sx, sy, w)`, `v1` float2 texcoord0, `v2` float2
texcoord1, `v3` ubyte4 colour (setup as `textured_cube/source/main.c`:130-146;
`attribs.h`:11-13, `buffers.h`:17-21). One `C3D_BufInfo` per frame (u16
indices: ≤ 65 536 vertices/frame).

Vertex shader `ports/3ds/render_c3d/shaders/world.v.pica`:

```
.fvec proj[4]            ; Mtx_Ortho/Mtx_OrthoTilt of the target, pixels -> clip
.fvec stereo             ; M9 only (x = parallax scale), else 0
.constf k(0.5, 1.0, 0.00392156862, 0.0)   ; 1/255
.out opos position
.out otc0 texcoord0
.out otc1 texcoord1
.out oclr color
.proc main
	mov r0.xy, v0.xy
	mov r0.z, k.x            ; z = 0.5: inside the ortho volume by construction
	mov r0.w, k.y
	dp4 r1.x, proj[0], r0
	dp4 r1.y, proj[1], r0
	dp4 r1.z, proj[2], r0
	dp4 r1.w, proj[3], r0
	mul opos, r1, v0.zzzz    ; clip *= w: same NDC, perspective-correct interpolation
	mov otc0, v1
	mov otc1, v2
	mul oclr, k.zzzz, v3     ; ubyte attributes are 0..255 (0.3)
	end
.end
```

`proj` from `Mtx_Ortho`/`Mtx_OrthoTilt` (`maths.h`:492, :542), y down as
`proctex/source/main.c`:43. Scaling clip by w keeps vertices at their integer
positions and makes interpolation perspective-correct (**UNVERIFIED**; M2's
floor diff fails visibly otherwise). No depth buffer (painter's order):
`C3D_RenderTargetCreate(..., C3D_DEPTHTYPE(-1))` (`renderqueue.h`:48-60, :62).

### 2.2 Per-surface vertex data

`hw, hh` = half screen size, `wx, wy` = `world_to_screen_x/y`, `dtp` =
`dtanpitch` (`render.cpp`:621). Eye space: x forward, y right, z up;
`sx = hw + y·wx/x`, `sy = hh − z·wy/x + dtp` (`RenderRasterize.cpp`:412, 429,
562, 578). Formulas are transliterations of the SW mapper code evaluated in
float **at the vertex**; the mappings are projective, so perspective-correct
interpolation reproduces them. Texcoords are evaluated at
`(sx − 0.5, sy − 0.5)` (SW samples pixel corners, the GPU centres); M2 tests
0 and −0.5 and records the winner. Per polygon, `floor(min u/v)` is
subtracted for repeating textures (float24 precision).

| Class | Recognised by | w | texcoord0 | texcoord1 |
| --- | --- | --- | --- | --- |
| Wall | `texture_vertical_polygon`, `_textured_transfer` | `x = o.x + t·v.i`, `t = (o.y − ry·o.x)/(ry·v.i − v.j)`, `ry = (sx−hw)/wx` (`scottish_textures.cpp`:916-919, 954) | column `t`, row `(z − o.z)/v.k`, `z = x·(hh + dtp − sy)/wy` (:958-965, 999-1001) | `(M − x/8192, a)` |
| Floor/ceiling/liquid | `texture_horizontal_polygon`, `_textured_transfer` | `|o.z·wy/(hh − sy + dtp)|`, `sy' = 0 → 1` (:1053, 1084) | SW `source_x/source_y` (:1051-1080) incl. `_SCALE_2X/4X` (:1075; `RenderRasterize.cpp`:443-451) | `(M − D/8192, a)` |
| Landscape | `_big_landscaped_transfer` (:222, :436-439) | 1 | SW per-line `source_x` (affine in sx), `source_y` (affine in sy, PIN or `VertRepeat`) (:1102-1170) | `(1, 1)` |
| Static | `_static_transfer` (:222-225) | wall/floor w if `_TRANSPARENT_BIT`, else 1 | as wall/floor | noise, 4.3 |
| Sprite | `texture_rectangle` before `SetForeground()` | 1 | SW `texture_x/dx`, `texture_y0/dy`, `flip_horizontal` (:690-732); `flip_vertical` ignored as SW | constant from `depth`, `ambient_shade` (:738-758) |
| Weapon | `texture_rectangle` after `SetForeground()` (`render.cpp`:994) | 1 | as sprite | depth 0 (`render.cpp`:1069-1072) |
| Tinted | rectangle `_tinted_transfer` (`render.cpp`:782-791) | 1 | as sprite (mask texture, 3.2) | — |

`o`, `v` = `polygon->origin/vector` (walls `RenderRasterize.cpp`:544-549;
floors :452-454). Sprites are clipped on the CPU as SW does (:661-732), one
quad each.

### 2.3 Clipping and the near plane

No near plane exists: `Render_Classic` clips against window planes through
the eye, then projects and pins (`RenderRasterize.cpp`:391-440, 502-589). The
GPU never clips (z/w constant, w > 0). w clamps to `[16, 32767]` (`SHRT_MAX`
as SW, `scottish_textures.cpp`:991, 1091); denominators guarded as SW
(`RenderRasterize.cpp`:411; `scottish_textures.cpp`:943, 950, 964, 1053) and
OGL (`OGL_Render.cpp`:1343-1376). A clamped w only bends texture perspective
near the eye plane, never coverage.

### 2.4 GPU state, command-buffer reserve, GX-queue budget

State set once per C3D frame, then only on change (citro3d's defaults are
wrong for us, 0.3):

| State | World draws | Tinted | P1 present / M6 composite |
| --- | --- | --- | --- |
| `C3D_CullFace` (`effect.h`:5) | `GPU_CULL_NONE` | same | same |
| `C3D_DepthTest` (:10) | `false, GPU_ALWAYS, GPU_WRITE_COLOR` | same | same |
| `C3D_StencilTest` (:6) | off | off | off |
| `C3D_AlphaTest` (:11) | `true, GPU_GEQUAL, 128` (static: threshold, 4.3) | `true, GPU_GEQUAL, 128` | off |
| `C3D_AlphaBlend` (:12) | `ADD, ADD, ONE, ZERO, ONE, ZERO` | `ADD, ADD, ZERO, CONSTANT_COLOR, ZERO, ONE` + `C3D_BlendingColor` (:8) | view/present pass `ONE, ZERO`; overlay pass `ONE, ONE_MINUS_SRC_ALPHA` (premultiplied) |

Frames: `C3D_FrameBegin(0)` (never `C3D_FRAME_SYNCDRAW`, whose VBlank pacing
fights the engine's), **its return value checked** (false = already in a
frame = bug: log, discard, SW). `C3D_FrameEnd(GX_CMDLIST_FLUSH)` (`gx.h`:66)
after ranged `GSPGPU_FlushDataCache` (`gspgpu.h`:239) of exactly the ring,
texture and staging bytes written this frame. Never more than one C3D frame
open; nesting is impossible by construction (5.1, 5.3).

**Command buffer** (overflow = crash, 0.3). Before every draw, bind, copy or
transfer the back end checks `gpuCmdBufSize − gpuCmdBufOffset` (`gpu.h`:13-15)
against `kDrawReserveWords + kEndReserveWords`; `kEndReserveWords` covers
`C3D_FrameEnd` (split + finalize) plus one transfer per linked target. Both
start at 1 024 words; M1 measures the worst case of each (offset before/after)
and sets them to 2x. Reserve reached → discard (1.5). `C3D_FrameSplit` is
**not** used for recovery (it frees nothing, 0.3). Size `C3D_Init(512 KiB)`
unless M1's max `C3D_GetCmdBufUsage()` (`base.h`:16; deterministic, so
authoritative in Azahar) asks for more.

**GX queue** (32 entries, overflow = crash, 0.3). Every GX operation goes
through one back-end wrapper `gx_op()` that counts entries per C3D frame
(+1 per op, +1 more if `gpuCmdBufOffset != 0` at the call) against a hard
budget of **24**, leaving room for `C3D_FrameEnd` (1) and the linked target
(1) plus margin. Budget reached → the op is refused and the frame discarded
(or, for promotions, deferred to a later frame, 3.3). Ops are grouped right
after `C3D_FrameBegin(0)`, before the first draw, while `gpuCmdBufOffset == 0`,
so they cost 1 entry each. Per frame: present upload 1, promotions ≤ 8 →
≤ 11 + 2 for `C3D_FrameEnd`. The **readback** (5.2) is an in-frame
`C3D_SyncDisplayTransfer` (revise 1; it was out of frame in the first M0 version),
counted through `gx_op` like every other op (1 entry, 2 with a non-empty command
buffer). Worst case 24 (budget, readback included) + 1 `C3D_FrameEnd` = **25 < 32**. The wrapper
logs the per-frame high-water mark.

**Risk:** coverage rules differ (±1 px on slanted edges, SW Bresenham
`scottish_textures.cpp`:1173-1219; SW "whitespace" seams `render.cpp`:263-264
not reproduced); texel choice at texel boundaries; draws ≈ polygons.
**How we test without hardware:** host tests vs transliterated SW code;
in-frame SW-vs-C3D diff per class (section 8); `cmdbuf_kib 32` and
`gxq_budget 4` runs must discard and fall back, never crash (M1, M2).

## 3. Textures

### 3.1 Source data

8-bit indices in `row_addresses[]` rows; column-order bitmaps store columns as
rows; RLE sprites have `bytes_per_row == NONE` with per-column `first/last`
(`textures.h`:33-51; `scottish_textures.cpp`:762-767). Colour of index `i` at
level `L` (0..255, `shapes.cpp`:1568-1570) = `shading_tables[256·L + i]` =
`0xFF000000 | rgb` (0.1); multiplier `L` for normal colours, `128 + L/2` for
self-luminous (`shapes.cpp`:1946). Alternate CLUTs have their own tables
(:1694-1740); infravision uses the collection's single tint table
(:1304-1306, 2279-2301) with `_SHADELESS_BIT` (`RenderRasterize.cpp`:463, 602;
`RenderPlaceObjs.cpp`:391; `render.cpp`:1072). Tinted rectangles carry the
**global** tint table (`render.cpp`:787), not a colour table.

### 3.2 Conversion (CPU, core code, host-tested)

GPU texel `(s, t)` = byte `row_addresses[t][s]` (memory order; each class maps
its coordinates onto `s/t`, 2.2). RLE: decode per column, outside
`[first, last]` transparent. Per index `i` (`m = 0x00FFFFFF`):

| Kind (part of the cache key) | RGB | A |
| --- | --- | --- |
| normal | `table[256·255 + i] & m` | 0 if transparent; **128 if `(table[i] & m) != 0`** (self-luminous); else 255 |
| shadeless (infravision) | `table[i] & m` | 0 if transparent, else 255 |
| mask (tinted) | — (A8, `enums.h`:73) | 0 if transparent, else 255 |

Transparent = index 0 for RLE or `_TRANSPARENT_BIT` bitmaps (`textures.h`:36;
`scottish_textures.cpp`:538-541). Black self-luminous colours read as normal:
harmless. Format **RGBA8** (`enums.h`:65): RGBA5551 loses colour bits and the
self-luminous level. Tiled output (8x8 Morton) by `core/tiling.cpp`, checked
**byte for byte against `/opt/devkitpro/tools/bin/tex3ds -f rgba8 -z none`**
on a unique-value-per-texel image. POT padding, UVs scaled by `w/Wpot`;
sides > 1024 → class unsupported. `GPU_NEAREST`, no mipmaps (SW
point-samples). `GPU_REPEAT` for walls/floors (128² POT,
`low_level_textures.h`:58-69), clamp for sprites (`enums.h`:35-41,
`texture.h`:157-173).

**Landscapes** (Marathon 2 collections 27-30 are 270x512, per critic): the
270 dimension is padded to 512 by **repeating the last real line**, so
`GPU_CLAMP_TO_EDGE` equals SW's `PIN(…, 0, texture_height−1)`
(`scottish_textures.cpp`:1113-1114, 1159-1162). `VertRepeat` wraps per line
(:1155-1160): the front end splits the polygon at the wrap rows (v affine in
sy), each piece clamped. Horizontal 512 is POT: `GPU_REPEAT`.

### 3.3 Upload model, cache, pools, eviction

- **Conversion** happens only inside a C3D frame, after `C3D_FrameBegin(0)`
  returned: a new texture is converted **directly into a linear-pool
  texture** (`C3D_TexInit`, `texture.h`:94-98; `C3D_Tex2DGetImagePtr`, :142;
  ranged flush) — no shared staging, nothing in flight overwritten, no GX op.
  It is sampled from linear in this frame.
- **Promotion to VRAM** (`C3D_TexInitVRAM`, :112-116) happens in a **later**
  frame, in one batch right after `C3D_FrameBegin(0)` and before any draw
  (2.4): ≤ 8 promotions per frame via `gx_op(C3D_SyncTextureCopy)`
  (`renderqueue.h`:78), oldest first; the rest stay linear and wait. The
  linear copy goes on the deferred-free list, freed after the next
  `C3D_FrameBegin(0)` (retired, 0.3); the VRAM copy is bound from the frame
  after the promotion.
- **Never** call `GX_*` directly; never transfer outside a frame except the
  readback (5.2), which is meant to wait.
- Key: `(bitmap_definition*, shading_tables*, kind)`. Value: `C3D_Tex`
  (`texture.h`:4-42), POT dims, UV scale, bytes, pool, `last_used_frame`.
- **Flush on `collections_changed()`** (hooks 2a/2b) is deferred: it marks
  the cache "flush pending" (lookups miss from now on, nothing is freed);
  the frees run right after the next `C3D_FrameBegin(0)` returns, like the
  deferred-free list, so a frame still in flight (M6) never samples freed
  memory.
- Pools: VRAM cap 3 584 KiB (`debug-c3d vram_cap`) in **size classes** (16,
  64, 256, 1 024 KiB slabs; a 128² RGBA8 wall = one 64 KiB slab); linear pool
  cap 4 096 KiB. Log `alloc failed with free >= request` if it happens.
- Eviction: LRU on `last_used_frame`; textures of the current or in-flight
  frame never. Both pools full after eviction → discard (1.5).
- Budget: soft 512 KiB converted per frame; over-budget conversions still
  happen and are counted. The level-start burst is measured by deterministic
  counts (KiB/textures converted, promotions pending, in the first 10 frames);
  pre-warming at level load is decided on the console (section 10).
- "CLUT change" re-uploads: infravision on/off (different table ⇒ key; both
  sets coexist), alternate CLUTs (separate keys), level loads (flush). Gamma
  and fades never touch textures (`screen.cpp`:1992-1998, 2121-2128).

**Risk:** working set > VRAM cap → linear sampling (speed **UNVERIFIED**);
level-start hitch; TextureCopy throughput.
**How we test without hardware:** host conversion tests with **real tables**
(normal, black normal, black/dark self-luminous, alternate CLUT, tint, global
tint) and tex3ds tiling; Azahar diffs with `vram_cap 512`, with a big cap at
level start (≥ 16 new textures in one frame; queue high-water logged ≤ 24),
and with `gxq_budget 4` (promotions defer, frames still correct).

## 4. Lighting, shading and transfer modes on TEV

### 4.1 Software model (32-bit)

Depth shade `s = PIN(M − (d << 3), 0, FIXED_ONE)` in 16.16, i.e.
`M/65536 − d/8192` (`scottish_textures.cpp`:131-132, 157-158),
`M = maximum_depth_intensity` (weapon flash, `screen.cpp`:1466); with ambient
`a` (`RenderRasterize.cpp`:460, 598-599): `max(a,s) + min(a,s)/2`, quantised
to 256 levels, clamped (:159-170); `a < 0` ⇒ level `−a`, no depth (:151-154).

### 4.2 GPU mapping: shade LUT + alpha trick, 2 TEV stages

- **Shade LUT**: 256x256 L8 (64 KiB, VRAM, nearest, clamp),
  `LUT(u=s, v=a) = SW level/255`, built once. texcoord1 =
  `(M/65536 − d/8192, a/65536)` — affine in d, interpolated exactly; the
  texture clamp is SW's PIN. `a < 0` → `(0, −a)`. Shadeless, landscapes
  (`calculate_shading_table(…, 0, FIXED_ONE)`, :1137) → `(1, 1)`. Sprites,
  weapons: constant. No polygon splitting (OGL needs up to 6,
  `OGL_Render.cpp`:1426-1559). The fog unit is unsuitable: one global
  128-entry LUT (`fog.h`:5-8, 21-26) while `a` changes per polygon.
- **Self-luminous**: texture A = 128; with `x = A·(1−L)`, `c·(1−x)` gives `c·L`
  (A = 1) or `c·(0.5 + 0.5·L)` (A = 0.5) = SW's `128 + L/2` to ±1 level.
- TEV (`texenv.h`:49-96, `enums.h`:331-383); the LUT is read through the R
  channel operands (`GPU_TEVOP_RGB_SRC_R`/`ONE_MINUS_SRC_R`, `enums.h`:335-336):

| Stage | RGB | Alpha |
| --- | --- | --- |
| 0 | `MODULATE(TEXTURE0 op SRC_ALPHA, TEXTURE1 op ONE_MINUS_SRC_R)` = x | `REPLACE(TEXTURE0 SRC_ALPHA)`, scale x2 |
| 1 | `MODULATE(TEXTURE0 SRC_COLOR, PREVIOUS ONE_MINUS_SRC_COLOR)` | `REPLACE(PREVIOUS)` |

  Alpha test `GEQUAL 128`. **UNVERIFIED:** TEV outputs clamp to [0,1] (the x2
  alpha relies on it); the M3 diff catches it.

### 4.3 Transfer modes

Animated modes are folded into geometry by the engine (`render.cpp`:860-979);
the rasteriser sees `_textured`, `_solid`, `_static`, `_big_landscaped`
(polygons) and `_textured`, `_tinted`, `_static` (rectangles),
`scottish_textures.h`:55-70.

| Effect (`map.h`:391-418) | Reaches the rasteriser as | GPU | Fidelity |
| --- | --- | --- | --- |
| normal, slides, wander, wobble, pulsate, 2x/4x | `_textured` + moved origin/scale | 4.2 | ±1 level, texel boundaries |
| smear | `_solid` | not drawn | as SW |
| landscape | `_big_landscaped` | affine UVs, full bright, 3.2 padding | texel boundaries |
| static, 50%, fade-out, pulsating, fold in/out | `_static`, `transfer_data` = drop threshold (`render.cpp`:796-831) | unit 1 = 128x128 RGBA8 noise generated with SW's LFSR/colour formula (`low_level_textures.h`:590-645); texcoord1 = screen position/128 + per-draw random offset; non-transparent bitmaps drawn with w = 1 (screen-locked noise); A = `(TEX0.A x2)·noise.A`; alpha test `GEQUAL drop>>8` | **approximated**: same density/colour statistics, different pattern; noise follows the surface on `_TRANSPARENT_BIT` bitmaps |
| invisibility / subtle | `_tinted`, index `0x0f`/`0x18` (`render.cpp`:786-788) → dst × `8·idx/255` (`low_level_textures.h`:501-505, 530-539; `shapes.cpp`:2010-2021) | blend 2.4 with the mask texture | ±1 |
| fade out to black | disabled in the engine (`render.cpp`:833-849) | — | as SW |
| infravision | tint tables + shadeless | shadeless conversion | exact |
| self-luminous | — | alpha 128 | ±1 level |
| weapon flash | `M` | texcoord1 offset | ±1 |
| fades, damage flash, underwater tint, gamma | `apply_gamma` on `world_pixels` (`screen.cpp`:1926-1998) | readback: unchanged; direct: frame goes readback (5.4) | exact |
| see-through liquids / SW alpha | `software_alpha_blending` (`RenderRasterize.cpp`:80) | not supported → SW frame | as SW |
| per-column/line lighting | — | per pixel | ≤ 1 level |

**Approximated effects (complete):** static/teleport noise pattern (and its
surface-following on transparent bitmaps); slanted-edge coverage ±1 px; texel
choice at texel boundaries; lighting ±1 of 255, per pixel instead of per
column/line; (M9) stereo parallax at window edges.

**Risk:** TEV clamp; TEV precision Azahar vs hardware.
**How we test without hardware:** host LUT vs transliterated
`calculate_shading_table` on every `(d, a)` grid point (≤ 1 level); per-class
Azahar diffs; a solo Lua plugin (pattern
`scripts/3ds/tests/plugins/Terminal Test/`) setting `infravision_duration`,
`invisibility_duration` (`Lua/lua_player.cpp`:2412-2414).

## 5. Present, overlays, bottom screen

### 5.1 P1: GPU present; citro3d owns the top screen

**Ownership.** citro3d is initialised lazily at the **first
`present_screen()` call** — after the scenario chooser, which writes the top
screen through SDL before the screen is set up (0.2). From then on there is
exactly one writer of the top framebuffer: citro3d, through one 240x400
RGBA8 screen target **linked** with `C3D_RenderTargetSetOutput(target,
GFX_TOP, GFX_LEFT, flags)` (`renderqueue.h`:65; flags as
`simple_tri/source/main.c`:8-11, :98 but
`GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGBA8)`, the framebuffers being RGBA8,
`SDL_n3dsvideo.c`:130). citro3d transfers and swaps (0.3). The port's CPU top
paths (`present_native`, the 640x480 path) run only while citro3d is not up.
`a1_c3d_present()` returns **true for every surface it is given**:
`main_surface` is always 32-bit (0.1), 640x480 or 400x240; any other size or
format (none expected) is converted with `SDL_ConvertSurface` and
centre-cropped/letterboxed into 400x240, and logged once — so
`present_screen()` never returns false and SDL's fallback (`screen.cpp`:
2585-2591) never writes the top framebuffer while citro3d owns it.
`error_screen()` and the `gfxExit` wrapper call `a1_c3d_release_top()`:
end any open frame, `C3D_FrameBegin(0)` + `C3D_FrameEnd(0)` (an empty frame:
everything earlier has retired), detach the output
(`C3D_RenderTargetDetachOutput`, `renderqueue.h`:67-70), `C3D_Fini()`; then
the console/SDL may use the top screen. The bottom screen stays port-owned
(`present_bottom()`, `video_3ds.cpp`:210-225); concurrent per-screen swaps
are safe (0.3).

**Per present** (`a1_c3d_present`, first call in `present_screen()`,
`video_3ds.cpp`:142):
1. `C3D_FrameBegin(0)`; on false: log, release ownership, CPU present.
2. CPU fills a linear staging image (pitch 512 px, 400x240 used): 400x240 →
   copy with `<< 8` (XRGB8888 → RGBA8, `to_fb`, `video_3ds.cpp`:61-64);
   **640x480 → halved on the CPU with the existing UHADD8 2x2 box and
   converted in the same pass**: each staging pixel is `to_fb(box(r0, r1))`
   (`video_3ds.cpp`:61-77; `half_blit` :227-239 shows the box loop but writes
   XRGB without `to_fb`, so it is not reused as is) into 320x240 centred in
   400x240 with black bars — the same arithmetic as today's CPU present
   (:198-201), so Classic and menus are byte-identical, and the present
   texture is **always 512x256** (9.1). Staging is written only after step 1
   (retired).
3. `gx_op(C3D_SyncDisplayTransfer)` linear → tiled into the 512x256 present
   texture (first GX op of the frame, 1 queue entry).
4. One quad into the screen target (`Mtx_OrthoTilt`, `maths.h`:542).
5. Ranged flush of the staging; **`if (gspIsPresentPending(GSP_SCREEN_TOP))
   gspWaitForVBlank();` immediately before `C3D_FrameEnd`** — the transfer's
   destination is taken at `C3D_FrameEnd`, the previous queue (and its swap
   request) finished in step 1, so this is the point where the displayed
   buffer is known (0.3; citro3d does not guard). Then
   `C3D_FrameEnd(GX_CMDLIST_FLUSH)`; citro3d transfers and swaps
   asynchronously; the CPU continues with the next frame.

**Test:** `calibrate 1` clears the screen target to a frame-indexed colour;
`dump_screen()` (`a1_3ds_platform.cpp`:425-480) on several frames shows frame
N or N−1, never older or mixed — at Framerate Target 30 and 60, with
`debug-c3d alt_load 1` alternating light and heavy (busy-wait 20 ms) frames.
GPU vs CPU present captures ≤ 1/channel (Classic and menus identical).
Ownership: a counter of `present_screen()` **false returns while citro3d is
up** (logged in `[c3d] stats` and at exit) must stay **0** over every P1
script; `debug-c3d odd_surface 1` hands `a1_c3d_present()` an odd-size
surface (e.g. 300x200) once a second, which must be converted/letterboxed and
presented by C3D, counter still 0. (`main_surface` is always ARGB8888,
[AUDIT.md](AUDIT.md):416; bit depth changes only `world_pixels`, so a
bit-depth run does not exercise this.)

**P1 calibration record (Azahar 2126.2, 2026-10-16; `render_c3d/back/present.cpp`).**
The upload transfer must cover the **whole texture**: with the staging image and both
transfer dimensions 512x240 into the 512x256 texture, the picture came out 16 rows
too low (Azahar). Staging is therefore 512x256 (512 KiB; 9.2 said 480) and the
transfer is `GX_BUFFER_DIM(512, 256)` on both sides. Flags:
`GX_TRANSFER_FLIP_VERT(1) | OUT_TILED(1) | RAW_COPY(0) | IN/OUT_FORMAT(RGBA8) |
SCALING(NO)`; without the flip the picture is upside down. The quad is
`Mtx_OrthoTilt(0, 400, 240, 0, 0, 1, true)` (y down) with texcoord `(x/512, y/256)`,
nearest filter, clamp; the linked target's flags are `simple_tri`'s with
`OUT_FORMAT(RGBA8)`. With these the 400x240 and 640x480 captures are byte-identical
to the CPU present. The orientation flags are the part most likely to differ on
hardware (section 10, item 8). `vramSpaceFree()` reports 0 before the first VRAM
allocation, so "VRAM free before" is only meaningful as 6 144 KiB (known total);
after init it is 5 257 KiB = 6 144 - 887 (9.1).

### 5.2 Readback mode (M0-M5): downstream unchanged

`render_frame()` opens its own C3D frame (view frame) and closes it before
returning, so it never overlaps the present frame: `C3D_FrameBegin(0)`,
promotions/uploads, draws into an **unlinked** view target,
`C3D_FrameSplit(GX_CMDLIST_FLUSH)` (submits the draws with GSP's cache flush, 0.3), then
the in-frame `C3D_SyncDisplayTransfer` into linear staging (queued behind the draws, 0.3),
`C3D_FrameEnd(GX_CMDLIST_FLUSH)`, and an empty `C3D_FrameBegin(0)`/`C3D_FrameEnd` pair, which
returns only when the transfer has finished (M0 revise 1: the out-of-frame form is rejected for the reason of 5.7.4),
`GSPGPU_InvalidateDataCache` (`gspgpu.h`:246), one pass
`(rgba >> 8) | 0xFF000000` (same form as SW pixels, 0.1) into `world_pixels`
(`screen.cpp`:673). Overlays, gamma/fades, HUD, Classic, the P1 present and
the stage 7 bottom screen work as today in both Bottom Screen settings
(`STAGE7.md`:40-43). Readback orientation for this unlinked target is found
by M0's calibration (**UNVERIFIED** until then).

### 5.3 Direct mode (M6, HUD + Map only): two C3D frames per game frame

1. **View frame** inside `render_frame()`: `C3D_FrameBegin(0)`, draws into a
   texture-backed target (`C3D_RenderTargetCreateFromTex`, `renderqueue.h`:63),
   `C3D_FrameEnd(GX_CMDLIST_FLUSH)` — the GPU starts now (work only starts at
   `C3D_FrameEnd`, 0.3), while the CPU draws overlays and the bottom screen (M6 specification, hook 3c: on a
   direct frame the bottom screen is drawn before the present, which comes first in `render_screen`).
2. Hook 3 sends CPU overlays to `overlay_surface()` (400x240 ARGB8888 **with
   alpha**; SDL blending yields premultiplied colour, 0.1) and skips the
   world-view blit.
3. **Composite frame** in `a1_c3d_present()`: `C3D_FrameBegin(0)` (the sync
   point: the view has rendered), upload of the overlay's dirty bands (below)
   via `gx_op` (only if any), composite into the linked target (view quad
   `ONE, ZERO`; overlay `ONE, ONE_MINUS_SRC_ALPHA`), VBlank guard (5.1 step
   5), `C3D_FrameEnd`; citro3d swaps.

**Overlay rectangles** are conservative and computed in the hook-3 helper in
`screen.cpp`, which sees the overlay state (`screen_shared.h` is included in
that TU): crosshair → centred 64x64 box when `Crosshairs_IsActive()`
(`RenderMain/Crosshairs.h`:67); FPS → bottom band `text_margins.bottom` +
2 `LineSpacing` when `displaying_fps` (`screen_shared.h`:550-593); input line
→ same band when `Console::input_active()` (:646-667); position + messages +
script HUD → top band from `text_margins.top`, `(6 if ShowPosition) +
NumScreenMessages + 2` lines when any applies (`DisplayPosition`,
`DisplayMessages`, :603-777; the static `Messages` array is visible there);
anything else (scores in net games, Lua) → the whole surface.

**Bands, not boxes.** The overlay upload is a display transfer into a tiled
texture, which writes whole rows of the texture width in 8-line tiles (3.2).
So every region above becomes a **full-width band** of rows: `y0` rounded
down and `y1` rounded up to multiples of 8 (e.g. the 64x64 crosshair box is a
64-row, 400-wide band, 72 rows if unaligned); overlapping bands are merged.
Each band is transferred as its own linear sub-image (staging rows
`y0..y1`) into the matching tile rows of the texture, taking the tiled row
order into account (which end of the texture holds row 0 depends on the
transfer's flip flag and the ortho orientation, **UNVERIFIED**; the band's
destination offset is computed in tile rows from that end, and M6's
calibration checks it against a frame-indexed band pattern). Each band is one `gx_op` (≤ 4 per frame: top,
bottom, crosshair, plus one merged spill). Only last frame's bands are
cleared each frame. No overlay → no upload, no overlay pass.

### 5.4 Direct-frame eligibility (else readback for that frame)

`native_screen()` (400-wide surface, `screen.cpp`:128-131: HUD + Map, no Lua
HUD, :115-120); overhead map inactive; no terminal; `using_default_gamma`
(`screen.cpp`:152, non-static); `get_keyboard_controller_status()`
(`Misc/interface.h`:368; else `darken_world_window()` edits pixels,
:1833-1836). Classic: readback + P1 present.

### 5.5 Bottom screen and risks

The bottom screen stays CPU in every phase (`STAGE7.md`:82-97); no target is
linked to `GFX_BOTTOM`.

**Risk:** readback waits for the GPU every frame; a missed eligibility
condition in direct mode shows an un-faded view; conservative rectangles
missing an overlay type (e.g. a Lua HUD element) → full-surface fallback.
**How we test without hardware:** 5.1 tests; M0 readback calibration;
direct vs readback captures with FPS, position, messages, input line and
crosshair on: ≤ 1/channel; damage-flash/underwater frames logged as readback;
GPU wait and frame overlap → console.

### 5.6 P1 fixes from the console: suspend, and `error_screen()` off the main thread

Status: design, not built; critic round 1 REVISE, round 2 APPROVE with 8 minors,
folded (Review log "P1 follow-ups 2, round 1" and "round 2"). Citations are the code as of `0d92621`. Evidence:
[PERF.md](PERF.md) "Console G0-b, second session" (:1045-1115) and the card logs
and host transcripts in `build-3ds/hw/g0b/` (not in git; cited below as
`g0b/<file>:<line>`). libctru's and citro3d's sources are not installed locally;
"per critic (c3drev)" = the critic's disassembly of `libctrud.a`/`libcitro3dd.a`
(release `libctru.a`/`libcitro3d.a` re-checked) and of newlib's `libc.a`, objects
and listings in `/tmp/c3drev/` (`apt.dis`, `gsp.dis`, `ctru/*.o`, `rel/*.o`,
`nl/*.o`; scratch, not in git). Anything neither a header nor that disassembly
settles is UNVERIFIED (5.6.4).

#### 5.6.0 Branches and order (for the implementer)

Everything under `ports/3ds/source/`, `ports/3ds/include/`, `ports/3ds/CMakeLists.txt`
and `scripts/3ds/tests/input/` is shared with `3ds-port` and is fixed **there**
(`3DS port: ...` commits, Azahar then console), then `3ds-port` is merged into
`stage10-c3d`. Only `ports/3ds/render_c3d/`, `scripts/3ds/tests/c3d/` and this doc
change on `stage10-c3d` alone.

| # | Branch | Piece | Section |
| --- | --- | --- | --- |
| 0 | `3ds-port` | **done** (`5cd45ce` + fix-up): SDL2 2.32.10 patch `scripts/3ds/patches/SDL2-release-2.32.10/0001`, `N3DS_JoystickQuit` releases HID once (`SDL_Quit` quit the joystick subsystem twice and took libctru's own HID reference: dumps 3/4, `hidScanInput`+0x28) | item 14 (b) |
| 1 | `3ds-port` | `error_screen` draws through the console device and holds its own HID reference; d6 | 5.6.2 a, item 14 (a) |
| 2 | `3ds-port` | the fatal hand-off to the main thread: request/serve, `--wrap=abort`, the `vhalt()` hook (**engine edit, `PATCHES.md` 54**), the audio-thread exit variant, the fallback; `debug-fatal` tests | 5.6.3, item 14 (c) |
| 3 | `3ds-port` | the A1 port side: `app_suspended()`, d1, `exit_requested()` with `aptShouldClose()`, script command `home-close`, script 22 | 5.6.1 a, c |
| 4 | — | merge `3ds-port` into `stage10-c3d` (`error_screen`'s `#ifdef A1_RENDER_C3D` release call, `a1_3ds_platform.cpp`:1081-1090, survives the merge) | — |
| 5 | `stage10-c3d` | **built, Azahar passes, console check pending** (`NEXT-SPRINT.md` plan A item 5): the gate, bounded waits, abandon, d2-d5, `debug-c3d` keys, configs; `test_vhalt` 1-4 re-run against pieces 1-2 | 5.6.1 a, b |
| A7 | `stage10-c3d` (**exception**, user's instruction 2026-10-26: built here, no merge) | the fallback's exit through `aptExit` and `APT_CloseApplication` behind a watchdog; `apt_hook` off main during the fallback; d7. Shared files (`ports/3ds/source/`): `3ds-port` keeps the old fallback until the user decides how to carry A7 there (cherry-pick or the next merge) | 5.6.3 step 3a |

Order 1 → 2 → 3 → 4 → 5; 3 may go before 1-2 (independent). Each piece: Azahar
first, then the console runbook items of its section.

#### 5.6.1 A1: HOME > Close in a game hangs

**What the logs show.**

| Fact | Source |
| --- | --- |
| `aptMainLoop()` = `aptHandleSleep(); aptHandleJumpToHome(); return !aptShouldClose();`. Every hook runs on the **calling** thread, i.e. the main thread (SDL's `N3DS_PumpEvents`): ONSUSPEND/ONRESTORE inside `aptJumpToHomeMenu`/`aptWaitForWakeUp`, ONSLEEP → `LightEvent_Wait` → ONWAKEUP inside `aptHandleSleep`. The main thread is blocked there for the whole HOME visit or sleep, so **no present runs while suspended** except after a Close | per critic (c3drev, `apt.dis`); `SDL_n3dsevents.c`:31-41; logs agree: no `[c3d] stats` line while in HOME, `g0b/fn/fn-main-log.txt`:244-246 (3.9 s), `g0b/home-close-1-sd/log.txt`:165-185 (one a second before, one in the 10.9 s after) |
| `aptJumpToHomeMenu` order: hooks (ONSUSPEND) → `GSPGPU_SaveVramSysArea` → `GSPGPU_ReleaseRight`. In `aptWaitForWakeUp`, `GSPGPU_AcquireRight`, `RestoreVramSysArea` and the ONRESTORE hook call are **one basic block**, skipped for `APTCMD_WAKEUP_CANCEL` (12, which also sets flag 0x400; `aptShouldClose()` tests `& 0x600`) and `APTCMD_WAKEUP` (1) | per critic (c3drev): `aptWaitForWakeUp` debug +0x124..0x134, release +0x84..0xa8 |
| `gspHasGpuRight()` (`gspgpu.h`:122) returns a cached flag: `GSPGPU_ReleaseRight` clears it, `GSPGPU_AcquireRight` sets it, `gspInit` acquires at boot. So it reads **1 at boot and at ONSUSPEND** (the hook runs before the release), **0 after a Close** (no ONRESTORE, no reacquire) | per critic (c3drev, `gsp.dis`) |
| citro3d has its own APT hook: `C3D_Init` registers `C3Di_AptEventHook`; ONSUSPEND → `C3Di_RenderQueueWaitDone` (unbounded) then VBlank callbacks off; ONRESTORE → on again, all state dirty; `C3D_Fini` unhooks. `aptHook` inserts at the head, so citro3d's hook (registered at the first present) runs **before** the port's. Frames in flight when HOME is pressed are drained while the rights are still held: that is why the post-close present's `frame_begin` returned in `home-close-1` | per critic (c3drev, `base.o`); `fn-main-log.txt`:246 (`cmdbuf_usage` 0.0023 after a resume: state re-emitted) |
| On Close, `aptMainLoop()` returns false **without** `APTHOOK_ONRESTORE` (no "resumed" line), and SDL pushes `SDL_QUIT` on every poll | `home-close-1-sd/log.txt`:183-187; `g0b/odd-sd/log.txt`:208-209; `SDL_n3dsevents.c`:36-40; [STAGE5.md](STAGE5.md):261 |
| `APTHOOK_ONEXIT` can never reach `log.txt`: it is called only from `aptExit()` when `aptShouldClose()`, inside `__appExit`, after the `atexit` handler `close_outputs` (`a1_3ds_platform.cpp`:272-298) closed the log. Its line (`input_3ds.cpp`:698-702) is indeed in no card log or transcript under `build-3ds/hw/` | per critic (c3drev); grep, 2026-10-19 |
| So `exit_requested()` is false (`input_3ds.cpp`:1119-1122) and `SDL_QUIT` in a game takes the upstream branch: `do_menu_item_command(mGame, iQuitGame, false)` opens the quit dialog (invisible), which draws, then ends with `exit(0)` on its next `SDL_QUIT`: hence "a dialog's exit" | `shell.cpp`:1518-1530; `a1_3ds_platform.cpp`:308-316; `home-close-1-sd/log.txt`:185 |
| That draw is a **GPU present after the close**: the stats line is printed only at the end of a present (`present.cpp`:593, after `frame_end` :580), ≥ 1 s after the previous one (`context.cpp`:322-327) | `home-close-1-sd/log.txt`:184; `odd-sd/log.txt`:209 |
| In a dialog (first session) the dialog's poll sees `SDL_QUIT` first and exits without a draw; the release then completes after the close | `fn-main-log.txt`:446-453 |
| `home-close-1`: `release #1` logged, `release done` never; called from the `atexit` hook, i.e. outside a present (`s_in_frame` false): blocked in `C3D_FrameBegin(0)`/`C3D_FrameEnd(0)` (`context.cpp`:175-176), `present_detach()` (:177) or `C3D_Fini()` (:178) | `home-close-1-sd/log.txt`:187 |
| `odd`: nothing after the post-close present: blocked in the **next present** or before `exit()` (`PERF.md`:1102 put both hangs in `release()`; for `odd` that is not established) | `odd-sd/log.txt`:209 (last line) |
| Not the log mirror (backlog item 15): the host received every line the card has, and nothing more (`tee_write` writes the card first, then `send`s, `a1_3ds_platform.cpp`:202-219), so no thread was blocked inside a later `tee_write` | `g0b/home-close-1.txt`:187-191; `g0b/odd.txt`:212-213 |

**Conclusion.** A C3D frame ended after the close never retires, and the next
`C3D_FrameBegin(0)` — which waits for the whole queue without a bound (0.3,
`renderqueue.h`:31) — blocks for good: in `home-close-1` the release's empty
frame (`context.cpp`:175), in `odd` most likely the next present's
`frame_begin()` (`present.cpp`:488 → `context.cpp`:273). `C3D_Fini()` also
waits for the queue (comment `context.cpp`:167-169, per critic). The software
build makes the same post-close present on the CPU, VBlank wait included
(`video_3ds.cpp`:123-142, :130-131), and does not hang ([NEXT-SPRINT.md](NEXT-SPRINT.md):395),
which points at GPU-queue completion rather than at VBlank events. The post-close
frame is GX work from a process **without GPU rights** (settled above: Close skips
the reacquire). Only the last step is UNVERIFIED: that GSP then never runs or never
signals that work (5.6.4).

**(1) Diagnostics** (they stay in; none prints in a normal frame):

- **d1, APT hook** (`apt_hook`, `input_3ds.cpp`:681): every hook logs
  `[3ds] apt hook <ONSUSPEND|ONRESTORE|ONSLEEP|ONWAKEUP> on thread <id> (main <id>): gpu right <0|1>, should close <0|1>`
  (`svcGetThreadId` as `context.cpp`:103-109; the main thread's id recorded in
  `boot_begin`, `a1_3ds_platform.cpp`:894; `aptShouldClose()`, `apt.h`:155). A
  confirmation of the binary facts above, nothing more: expected main thread every
  time, gpu right 1 at ONSUSPEND and at ONRESTORE, and **no hook at all after a
  Close** (ONEXIT comes after the log is closed). The post-close rights show only
  in d3/d5/d2. The Azahar script commands `home`, `sleep` and `home-close` call
  `apt_hook` directly (`input_3ds.cpp`:863-871), bypassing libctru and citro3d's
  hook, so in Azahar d1 shows the script, not libctru.
- **d2, release** (`release_and_fini`, `context.cpp`:156-189): a state line first
  (`in_frame`, gate state and its four inputs below, frames ended while the gate
  was closed, ms since the last `frame_end`), then one line after each step:
  "open frame ended", "waiting for the queue (up to N ms)", "queue retired after
  X ms" or "did not retire", "empty frame ended", "detached", "calling C3D_Fini",
  then the existing "release done".
- **d3, present breadcrumbs**, only while the gate condition holds (so only with
  `suspend_gate 0`, below): before and after `frame_begin`, before the VBlank
  wait (`present.cpp`:557), before and after `frame_end` (:580); the line just
  before `frame_end` also logs `gspHasGpuRight()` and `s_in_frame`.
- **d4**: `__wrap_gfxExit` logs "real gfxExit returned" after
  `__real_gfxExit()` (`context.cpp`:359-360).
- **d5**: gate transitions (below).

**(2) Fix.**

a. **Gate: no GPU work without the GPU rights.** `back/` gets
   `gate_closed() = !gspHasGpuRight() || aptShouldClose() || a1_3ds::app_suspended()`.
   The inputs have different jobs:
   - `!gspHasGpuRight()` is the **hardware-authoritative** one: false exactly from
     `GSPGPU_ReleaseRight` until a reacquire, i.e. for good after a Close (binary
     facts above);
   - `aptShouldClose()` (`apt.h`:155) is set by the same `APTCMD_WAKEUP_CANCEL`;
   - `a1_3ds::app_suspended()` (new, piece 3: `s_in_home || s_asleep`,
     `input_3ds.cpp`:623, implemented there, declared in `a1_3ds_platform.h`,
     engine-safe) is the input **Azahar tests can flip** (the script commands set
     it; Azahar's libctru state is untouched by them). On hardware it is true during
     a main-thread present only after a Close. Plain `bool`s suffice: every hook
     runs on the main thread (binary facts).
   `back/` already includes `a1_3ds_platform.h` and `<3ds.h>` (`context.cpp`:20-24,
   `present.cpp`:27-32); `core/` and `front/` untouched. In `a1_c3d_present()`
   (`present.cpp`:410), after the `test_vhalt` block, **in this order**:
   1. `s_up && s_release_requested` → serve the release (today `ensure_up()`
      :209-221; with b. it is bounded) and return `top_surrendered()`;
   2. the gate: closed and citro3d up or top surrendered → count a skipped present,
      return **true** with no citro3d call (the HOME menu owns the screens); closed
      and citro3d not up → return false (CPU present, as without citro3d);
   3. `ensure_up()` (init only ever happens with the gate open, :222-261).
   - d5: `[c3d] gate closed at present N (gpu_right d, should_close c, in_home a, asleep b)`
     once per closing; `[c3d] gate open again after K skipped presents`; the skip
     total in `[c3d] final`. The transition/count logic is pure and goes in `core/`
     with a host test (8.4).
   - `debug-c3d suspend_gate 0|1` (default 1); 0 restores today's behaviour for the
     console confirmation run only.
b. **Bounded waits where the queue may not retire.**
   - `retire_wait(ms)`: poll `C3D_FrameBegin(C3D_FRAME_NONBLOCK)`
     (`renderqueue.h`:24, "Return false instead of waiting if the GPU is busy") with
     `svcSleepThread(1 ms)` between tries (never spin), up to `retire_wait_ms`
     (default **500**; a normal frame retires within one frame: `c3d_wait` 0.00-0.40 ms
     on the console, `PERF.md`:995-996, :1056-1060). True = retired, now inside a frame.
     NONBLOCK is settled: `C3D_FrameBegin(C3D_FRAME_NONBLOCK)` → `gxCmdQueueWait(q, 0)`:
     deadline = now; a queue that is not running → true; otherwise false **without**
     reaching `gspWaitForAnyEvent`; on success `gxCmdQueueStop`/`Clear` and "in frame"
     set (per critic, c3drev `renderqueue.o`, `gxqueue.o`). **Never "improve" this to
     `gxCmdQueueWait(q, N)` with N > 0** (`gx.h`:125): any positive timeout blocks in
     `gspWaitForAnyEvent`, in the release build until the next GSP event, which
     after a Close may never come.
   - `release_and_fini()`: the open-frame end stays (`context.cpp`:170-174);
     `retire_wait` replaces `C3D_FrameBegin(0)` (:175); on success `C3D_FrameEnd(0)`
     ends the empty frame. It submits no GPU work (`C3Di_SplitFrame` finds no
     commands, no linked target was drawn, `gxCmdQueueRun` on the empty queue clears
     its running flag; per critic) but makes one `GSPGPU_FlushDataCache` IPC over
     the whole linear heap (flags 0, 0.3); `fn-main-log.txt`:447-452 shows exactly
     this sequence completing after a Close. Then detach, `C3D_Fini`,
     `present_free` as today. On timeout: **abandon**: log `release abandoned: the
     GPU queue did not retire in N ms (...)`, skip `C3D_Fini` and `present_free`
     (`C3D_Fini` would wait unbounded; the GPU might still read staging/texture if it
     ever ran the work), `s_off = true`, `s_surrendered = true`, `s_abandoned = true`,
     `s_up = false` last (as :187). `__wrap_gfxExit` then calls the real `gfxExit()`
     (`context.cpp`:357-366): it must, since without `gspExit` the GSP event thread
     outlives the heap and faults ([STAGE5.md](STAGE5.md):270-282).
   - `frame_begin()` (`context.cpp`:264-280) returns a tri-state
     `FrameStart { Ok, AlreadyInFrame, Abandoned }`. When `gate_closed()` (reachable
     only with `suspend_gate 0`) it uses `retire_wait`; on timeout it abandons and
     returns `Abandoned`, and `a1_c3d_present()` returns **true** without drawing and
     **without** the `present.cpp`:488-492 branch (that branch, "already inside a
     frame", stays for `AlreadyInFrame` only). Every later present then reaches
     `ensure_up()` false (`s_off`) and returns `top_surrendered()` = true (`s_surrendered`
     set by the abandon), so no CPU present writes into the framebuffer the abandoned
     transfer targets. With the gate open it stays `C3D_FrameBegin(0)`: no change on
     the measured path (rule 1).
   - **After an abandon, citro3d's APT hook is still registered** (`C3D_Fini`, which
     unhooks, is skipped): a later ONSUSPEND would block unbounded in
     `C3Di_RenderQueueWaitDone` on the stuck queue. Reachable only if something runs
     `aptMainLoop()` after an abandon with `aptShouldClose()` false (an error screen
     after an abandon outside a Close, i.e. only with `suspend_gate 0` or
     `force_abandon`, where Azahar's queue is in fact retired). Accepted and
     documented; not fixed (an `aptUnhook` needs citro3d's private cookie). What GSP
     does with commands still pending at `gspExit` is UNVERIFIED (5.6.4).
   - `debug-c3d retire_wait_ms N` (0 = unbounded, today's behaviour, for a
     deliberate reproduction only), `close_frame 0|1` (one forced GPU frame in the
     release while the gate condition holds, console run 1) and `force_abandon 0|1`
     (release takes the
     abandon path without polling; Azahar cannot hang a queue, it completes GPU
     frames synchronously, `PERF.md`:790-791).
c. **`3ds-port`, piece 3:** `exit_requested()` (`input_3ds.cpp`:1119-1122) also
   returns `aptShouldClose()` (`apt.h`:155). Reason: the flag it reads today is set
   only by `APTHOOK_ONEXIT`, which libctru calls from `aptExit()` inside `__appExit`,
   i.e. after the engine has already handled the close (binary facts above), so
   `PATCHES.md` 16/22's fast path (`shell.cpp`:1522-1523, `interface.cpp`:2417,
   :2990, :3056) has never run on hardware; HOME > Close in a game opens the
   invisible quit dialog instead. With the fix it takes the fast path. It shortens
   the close; it does not replace the gate (a game frame may still be drawn before
   `SDL_QUIT` is handled). Proposed as backlog item 16.

**(3) Tests.**

Azahar (non-authoritative; it cannot show the hang: GPU frames retire
synchronously, and the script commands bypass libctru, so `gspHasGpuRight()` and
`aptShouldClose()` keep their running values there; the gate closes through
`in_home`):
- New script command `home-close` (`run_script_line`, `input_3ds.cpp`:800, next to
  `home` :863-871): `apt_hook(APTHOOK_ONSUSPEND)`, then `s.quit = true` **without**
  ONRESTORE and **without** `s_exit_requested` (`quit` sets it, :891-895; the
  console never does), i.e. the console's post-close path including the invisible
  quit dialog. New script `22-c3d-home-close.txt`: start a game (as
  `06-gameplay.txt`), play 5 s, `home-close`.
- `p1-gpu.cfg`: `gate closed ... in_home 1`, ≥ 1 skipped present, every d2 line,
  `release done`, `real gfxExit returned`, `exit`; no guest fault in
  `azahar_log.txt`.
- New `p1-home-close-diag.cfg` (`suspend_gate 0`): presents run with `in_home 1`,
  d3 breadcrumbs, `queue retired after 0 ms`, clean exit.
- New `p1-abandon.cfg` (`force_abandon 1`): `release abandoned`, `real gfxExit
  returned`, `exit`, no guest fault.
- Regression, `p1-gpu.cfg` and `p1-diff.cfg`: scripts 07, 08, 13, 14, 19 pass;
  **0 skipped presents** (the scripted `home`/`sleep` hold the main thread inside the
  hook, :868-870, as libctru does on hardware, so nothing presents meanwhile);
  `present_diff` 0; captures byte-identical to P1. Four builds, 0 warnings; `nm`
  OFF: no citro3d.

Console (runbook for the user's return; `p1-gpu.cfg` unless noted). **Pass
criterion for every close below: "Closing software..." ends, the Homebrew Launcher
comes back, and Aleph One launches again from it** (dump 5 showed that a broken exit
can kill the next program).
1. **Diagnostic, no hang expected:** `p1-home-close-diag.cfg` (`suspend_gate 0`,
   `retire_wait_ms 5000`: a long window tells "never retires" from "slow"; and
   `close_frame 1`, below), HOME > Close in a Classic @30 game (as `home-close-1`).
   Piece 3 lands before piece 5 and removes the invisible quit dialog whose draw was
   the post-close present, so the run **forces one**: with `close_frame 1`,
   `release_and_fini()`, when `gate_closed()` holds, first ends one frame that draws
   the present quad from the present texture into the linked target (the same GPU
   work as a present: a draw plus the display transfer to the framebuffer), with d3
   breadcrumbs, then goes on to `retire_wait`. Chosen over disabling the
   `aptShouldClose()` term of `exit_requested()`: it stays inside `render_c3d/`
   (piece 5), does not depend on the engine's quit path, and runs exactly once.
   Expect d1 `ONSUSPEND on thread <main> ... gpu right 1` and **no hook line after
   it**; d3 for the forced frame (and for a natural post-close present, if one
   happens) with `gpu right 0`; then `did not retire in 5000 ms ... abandoned`,
   `real gfxExit returned` and a normal close. That
   confirms the last UNVERIFIED step (GSP does not complete work from a process
   without rights). `queue retired after X ms` with a large X instead = slow, not
   never: report X. A hang: the last breadcrumb names the wait → back to this design.
2. **The fix:** HOME > Close in a game ×3 (HUD + Map @60 twice, Classic @30 once);
   log `gate closed ... gpu_right 0`, ≥ 1 skipped present (or none, once piece 3's
   fast path quits before a draw), `queue retired after 0 ms`, `release done`,
   `real gfxExit returned`, `exit`.
3. `p1-diff.cfg`: HOME and back in a game ×2, sleep (lid) and wake in a game ×2: the
   game continues with a correct picture, `present_diff` 0 differences, and
   **0 skipped presents** (predicted: the main thread is blocked in `aptMainLoop()`
   for both); d1 shows each hook on the main thread with gpu right 1.
4. No regression: HOME > Close in a dialog and at the main menu. Luma dumps: none new.

**Built (plan A piece 5, Azahar — non-authoritative; deviations in `NEXT-SPRINT.md`
plan A item 5).** Code: pure state `render_c3d/core/gate.h` (`GateInputs`, `GateTracker`,
`retire_wait`, `FrameStart`) with the host test `scripts/3ds/tests/c3d-host/build.sh`;
`back/context.cpp` (`gate_present()`, `serve_release()`, bounded `release_and_fini()`,
`abandon()`, tri-state `frame_begin()`, d2, d4, d5), `back/present.cpp` (the order 1-2-3 of
5.6.1 a, d3, `present_close_frame()`); configs `p1-home-close-diag.cfg`, `p1-abandon.cfg`,
`p1-unbounded.cfg`.

**Risk:** GSP running the abandoned work after `gfxExit` freed the framebuffers, or
leaving the system unhappy at `gspExit` (the process is exiting; the launcher /
relaunch criterion checks it); citro3d's APT hook after an abandon (above; only
outside a Close).
**How we test without hardware:** the Azahar runs above (gate logic, logs, the
abandon path's clean exit); the post-close GSP behaviour and the relaunch only on
the console.

#### 5.6.2 A2: `error_screen()` on a second thread draws nothing

**What the logs show.**

| Fact | Source |
| --- | --- |
| The citro3d hand-off works: requested, released on the main thread, `release done`, `released by the main thread` | `g0b/vhalt-thread-screen-sd/log.txt`:151-156 |
| The error text went to **`log.txt`**: it follows the hand-off, and its unterminated last line ("Press START or A to exit.") is joined to the main thread's next log line | `vhalt-thread-screen-sd/log.txt`:157-162 |
| `consoleInit(GFX_BOTTOM)` ran — the displayed bottom is RGB565, its format (`input_3ds.cpp`:1131) — and cleared it; nothing was drawn afterwards | capture "format 2", :166; 0 of 76 800 pixels lit (`PERF.md`:1084) |
| stdout's device stayed the log tee for every thread: the main thread kept logging to the card after the second thread's `consoleInit` (`devoptab_list` is one process-wide table, `iosupport.h`:78) | `vhalt-thread-screen-sd/log.txt`:162-172 |
| `printf` goes to `devoptab_list[STD_OUT]`; the port puts the tee there (`a1_3ds_platform.cpp`:224-236); at boot `bottom_init` → `console_attach` makes the process's first `consoleInit` and puts the tee back (`input_3ds.cpp`:1101, 240-243, 224-235). `consoleInit` installs its stdout device (`dotab_stdout`) and the `setvbuf` calls **only** while its static `firstConsoleInit` is set. So `error_screen`'s `consoleInit` calls (`a1_3ds_platform.cpp`:1111, 1116) do not route stdout to the console and its `printf`s (:1113-1123) write to the log | per critic (c3drev, `ctru/console.o`): **verified**; the port's comment `input_3ds.cpp`:231 says the same |
| `consoleInit` also calls `gfxSetScreenFormat`, `gfxSetDoubleBuffering`, `gfxSwapBuffersGpu()` (= `gfxScreenSwapBuffers(GFX_TOP, true)` + `(GFX_BOTTOM, true)`) and `gspWaitForEvent(VBlank0)`: a bottom console init **swaps the top screen too** | per critic (c3drev, `console.o`, `gfx.o`) |
| Azahar's captures were black for `test_vhalt 4` **and** for the main-thread `test_vhalt 1`, with and without citro3d: the same cause, not Azahar | `PERF.md`:812-814, :952 |
| The "readable" main-thread `vhalt` screen on the console was **Luma's exception screen** ("An exception occurred"), not the port's (the user, 2026-10-23). So the port's error screen has never been seen after boot, on any device. Positive control: the boot-time error screen (missing scenario, `STAGE4.md`:134-147), which makes the process's first `consoleInit`, does draw | the user; [STAGE4.md](STAGE4.md):134-147 |

**Conclusion.** A2 is a port bug, not citro3d's and not specific to the second
thread: after boot, `error_screen()` draws through stdout, and stdout is the log.
A second thread adds problems of its own, which the user's decision for backlog
item 14 removes by never drawing there (5.6.3; only its fallback does): the main thread keeps running and,
in a game, redraws and swaps the bottom screen every frame (`bottom_frame` →
`present_bottom`, `bottom_3ds.cpp`:210-247, `video_3ds.cpp`:267-282, also
`show_saving` :183-196), draws the legend console into the same framebuffer
(`console_write`, `input_3ds.cpp`:140-147), and scans HID too (`PERF.md`:886-887).

**(1) Diagnostics.** **d6** (a confirmation now): `error_screen` logs, through the
log and before drawing, the thread (main or not) and `stdout device after
consoleInit: console|log|other` (`devoptab_list[STD_OUT]` against the console
device, `input_3ds.cpp`:84, and the tee, `a1_3ds_platform.cpp`:192). Expected after
fix a: `log` (stdout is never moved; drawing does not use it).

**(2) Fix** (`3ds-port`, piece 1 of 5.6.0; merged into `stage10-c3d`):

a. **Draw through the console device, never through stdout.** `input_3ds.cpp`:
   `PrintConsole* console_init_keep_log(gfxScreen_t, PrintConsole*)` (the sequence of
   `console_attach`, :224-235: save `STD_OUT`/`STD_ERR`, `consoleInit`, capture the
   console device on the first call, restore), used by `console_attach` and
   `error_screen`; and `void console_write_on(PrintConsole*, const char*)`
   (`console_write`, :140-147, with the console as a parameter and no
   `s_bottom_game` check). Declared locally in `a1_3ds_platform.cpp` (as
   `present.cpp`:46-52 does), so `a1_3ds_platform.h` stays free of libctru types.
   `error_screen` (`a1_3ds_platform.cpp`:1075-1154): both `consoleInit`s become
   `console_init_keep_log`, every on-screen `printf` becomes `console_write_on(&top
   or &bottom, ...)` (the `wrap()` lines joined with `\n`), and the `devoptab_list`
   swaps go (:1105-1106, :1118, :1143-1146, :1150-1151): stdout stays the log for
   every thread. The log keeps the message through the existing `fprintf(stderr)`
   (:1077). An error before `input_init` (the boot errors :959, :997) makes the
   first `consoleInit`, which captures the device.
   - **Never `consoleInit` while citro3d is up.** Because `consoleInit` swaps the
     top screen too (table above), `console_init_keep_log` would break the rule
     "keep off the top while citro3d owns it" (`context.cpp`:131-137) and race
     citro3d's swap from `onQueueFinish`. On the main thread (every error screen
     after 5.6.3) the release comes first (`a1_3ds_platform.cpp`:1087, a direct
     `release_and_fini()` there), so this never arises. If `a1_c3d_release_top()`
     returns false (only in 5.6.3's fallback, citro3d left up by a blocked main
     thread), `error_screen_here` makes no `consoleInit`, draws nothing, logs only,
     and exits at once without waiting for a button (5.6.3 step 3).
b. **Its own HID reference (item 14 (a), the user's decision).** `hidInit()` before
   the loop (:1126), `hidExit()` after it (:1148): libctru's HID is reference-counted,
   so the error screen's `hidScanInput` stays valid whatever `SDL_Quit` did before
   it (with piece 0, `SDL_Quit` now releases only SDL's reference, 3 → 1 in Azahar;
   b. covers the error screens that follow `SDL_Quit` on the `vhalt` path, dump 3).
c. The second-thread problems (main thread drawing, two HID scanners, teardown under
   a live main thread) are handled by never drawing off the main thread: 5.6.3. The
   park of the previous revision is withdrawn (user decision, item 14 (c)).

**(3) Tests** (piece 1; the off-main-thread cases are 5.6.3's).
Azahar (non-authoritative), `debug-screens` on:
- **Positive control first:** the boot-time error screen (rename the scenario
  folder, as `STAGE4.md`:276), because `console_init_keep_log` changes that path
  too: it must still draw, as in `STAGE4.md`:147.
- `debug-fatal mode 1` (`vhalt()` on the main thread, 5.6.3) and, on `stage10-c3d`
  after the merge, script 17 with `p1-vhalt.cfg`: the bottom captures show the text
  (lit pixels > 0; Azahar does capture the console, [STAGE4.md](STAGE4.md):147); d6
  says `log`; the `ERROR:` lines are still in the log; the screen is left with A
  and the process exits with **0 null reads** in `azahar_log.txt` (b.). Again with
  `present cpu` and the OFF build.
Console: the boot-time control, then `vhalt()` on the main thread: by eye the port's
error screen (red title, wrapped text, "Press START or A to exit."), **not** Luma's
exception screen; A ends it; the launcher returns and Aleph One launches again.

**Risk:** the console device captured from `STD_OUT` at the first `consoleInit`
(verified behaviour, but it makes the boot path the one place that installs it:
the positive control checks it); the fallback's "no console while citro3d is up"
leaves that rare case without a screen (log only).
**How we test without hardware:** the Azahar captures and the clean exit above;
readability by eye on the console.

#### 5.6.3 Fatal errors on other threads: the whole fatal path goes to the main thread

The user's decision for backlog item 14 (2026-10-23): (a) the error screen holds
its own HID reference (5.6.2 b), (b) done (piece 0), (c) **an error on any other
thread hands the whole fatal path to the main thread**. `3ds-port`, piece 2.

**Why today's second-thread paths fail** (beyond A2):

| Path | What happens | Source |
| --- | --- | --- |
| `vhalt()` on thread T | `stop_recording()` and `shutdown_application()` (→ `SDL_Quit`: HID, video, audio) run on T **before any port code**, under a running main thread → main faults in `hidScanInput` (dump 4) | `csalerts_sdl.cpp`:322-330; `shell.cpp`:650; `PERF.md`:1083 |
| `abort()` on thread T | never reaches `on_abort`: newlib's signal handlers are per `_reent` (`signal()`/`_raise_r` use `__getreent()->_sig_func`, offset 504), libctru gives each thread a zeroed `_reent` (`threadCreate` clears 512 bytes at +24; `__syscall_getreent` reads TLS), so `signal(SIGABRT, on_abort)` (`a1_3ds_platform.cpp`:920) covers only the main thread. On T: `_raise_r` sees `SIG_DFL` → `_kill_r` → `_exit(1)` → `__ctru_exit` → `__appExit` (`userAppExit`, `archiveUnmountAll`, `fsExit`, `hidExit`, `aptExit`, `srvExit`) → `__libctru_exit` (heap freed, `svcExitProcess`), all on T while main runs: main's next `fseek` hits the unmounted `sdmc` (dump 6, `_lseek_r` 0x18; Azahar's `_write_r` null read, `PERF.md`:890). The log ends at `test_vhalt 2: calling abort()` with no `[3ds] ERROR` line | per critic (c3drev, `nl/libc_a-signal.o`, `nl/libc_a-abort.o`, `ctru/thread.o`); `PERF.md`:1085 |
| `std::terminate` on T | `std::set_terminate` (`a1_3ds_platform.cpp`:919) is process-wide: `on_terminate` runs on T, then `die_with_message` → `error_screen` on T (today: A2 plus the main thread running) | `a1_3ds_platform.cpp`:592-630 |
| fatal `alert_user()` on T | `error_screen` on T | `csalerts_sdl.cpp`:175-183 |
| SPECULATIVE: dump 5 (the launcher's `svcBreak` after `test_vhalt 4`'s `_Exit(1)` on a second thread, `PERF.md`:1084) may be the same class: a process teardown run from T under a live main thread | — |

**The hand-off.**

State (all `std::atomic`, in `a1_3ds_platform.cpp`): `s_fatal_from` (thread id of
the request, 0 = none), `s_fatal_acked`, `s_audio_parked`, `s_fatal_active` (main
is in a fatal path of its own or serving one), the audio thread's id (recorded at
the first call of the port's callback, `timed_callback`, `audio_3ds.cpp`:157), and
`s_fatal_never`, an event created at boot and never signalled.

1. **Request (thread T ≠ main).** `a1_3ds::fatal_from_other_thread(title, body)`:
   if T is the audio thread, set `s_audio_parked` **first** (step 6; whether T wins
   or loses below). Copy title and body into **static fixed buffers** (`char[128]`,
   `char[1024]`, truncated; no heap: T may hold newlib's malloc lock). Compare-
   exchange `s_fatal_from` from 0 to T's id. **A loser** (another fatal error is
   already in progress) logs one raw line (step 3's log write) and goes straight to
   step 2: it never draws (a second `consoleInit` would also swap the top over the
   first screen, 5.6.2) and never exits the process. The winner logs `[3ds] fatal
   error on thread T: handed to the main thread` and waits for `s_fatal_acked` in
   `svcSleepThread(1 ms)` steps, bounded **N = 5 s** (a level load, 3-4 s, may pass
   no service point; assumed).
2. **Acknowledged (or lost) → T blocks forever in an untimed kernel wait**:
   `svcWaitSynchronization(s_fatal_never, U64_MAX)`. **Never a sleep loop:**
   `__libctru_exit` frees the heap (`svcControlMemory`) before `svcExitProcess`, and
   a thread that wakes with its stack in the heap faults
   ([STAGE5.md](STAGE5.md):270-282).
3. **Not acknowledged in N s → fallback on T** (main blocked: on SDL's mixer lock,
   the OpenAL device lock, `s_tee_lock`, newlib's malloc lock, ...). T calls
   `error_screen_here(title, body)`, the body of `error_screen` **without** its
   hand-off (calling `error_screen` would hand off again, lose the compare-exchange
   to itself and recurse). In the fallback every log line goes **straight to the log
   fd** (`write(s_log_fd, ...)`), never through `fprintf`/`fflush`: the tee lock or
   newlib's stdio lock may be what main holds. It shows the error (5.6.2 a + b) only
   if it can draw: with citro3d still up (`a1_c3d_release_top()` false, the hand-off
   to a blocked main thread times out) it makes no `consoleInit` (5.6.2 a) and then
   **does not wait for A/START** — nothing is on screen, so it exits at once (no
   frozen game needing a forced power-off). After A/START (or at once), it ends the
   process **without** `__appExit`'s teardown under a live main thread (the dump 6
   class): `svcExitProcess()` directly. This skips `gspExit`, `aptExit` and
   `ndspExit`; whether HOME, the launcher and the DSP survive that is UNVERIFIED
   (5.6.4 item 5). **Refuted on the console (A6, 2026-10-26): the system restarts the
   console. Replaced by step 3a.**
3a. **The fallback's exit (decided 2026-10-26, after A6; piece A7, `stage10-c3d`).**
   *Facts* (libctru 2.7.0; source lines from the 2.7.0 tree, which match `libctrud.a`'s
   DWARF line info and disassembly; research report of 2026-10-26):
   - libctru's normal end tells APT twice that the application is closing.
     `aptExit()` (`apt.c`:371-440), in its default branch (not crippled, no close order,
     no `RUNFLAG_APTREINIT`, no chainload), calls `APT_PrepareToCloseApplication(true)`,
     sets `__system_retAddr = aptExitProcess` and keeps srv up (`srvInit()`);
     `__libctru_exit` (`ctru_exit.c`:32-36) frees both heaps and then calls
     `__system_retAddr()`, i.e. `APT_CloseApplication(NULL, 0, 0)` (`apt.c`:366-369;
     libctru's comment: "APT_CloseApplication kills us"), and calls `svcExitProcess()`
     only when `__system_retAddr` is NULL. The other branches: a close order
     (`aptShouldClose()`) runs the ONEXIT hooks on the calling thread and then the same
     close; `RUNFLAG_APTREINIT` sends `APT_Finalize` and no close; a chainload jumps
     (`APT_DoApplicationJump`) and clears `__system_retAddr`. Every APT command takes the
     kernel mutex `aptLockHandle` (`aptSendCommand`), held only for one IPC; `aptExit`
     then joins the APT event thread (released by its quit event). `aptExit` frees no
     heap. Nothing in `aptExit` checks for the main thread (TLS command buffer only).
   - The fallback's bare `svcExitProcess()` sends neither `APT_PrepareToCloseApplication`
     nor `APT_CloseApplication`. The three restarts (6b, 10, and 7 on the software build)
     share exactly this; the clean ends on the console (normal exit, mode 6's
     `gfxExit` + `_Exit`, the abandoned citro3d's real `gfxExit` + exit) all ran it.
     **That the missing APT close is the cause is UNVERIFIED** (the system's reaction is
     not in libctru); it is the best-supported candidate.
   - Which branch `aptExit` takes on this console (the launcher's run flags) is
     UNVERIFIED: the port never logged `envGetSystemRunFlags()`. d7 logs it at boot.
   - The main thread's stack and every `threadCreate` stack are in the application heap
     (`stack_adjust.o` `initSystem`; `thread.o` `threadCreate` memalign). `ndspExit` joins
     the ndsp thread, which waits untimed on the DSP interrupt (`ndsp.c`:31-47), so it can
     hang; `gspExit` joins the GSP event thread, which runs citro3d's callbacks;
     `gfxExit` waits for a VBlank when it holds the GPU right (`gfx.c`:165-190). citro3d's
     own APT hook ignores ONEXIT (`libcitro3d.a` `base.o` `C3Di_AptEventHook`: only
     types 0 and 1 are handled).
   *Decision (option A + watchdog):* the fallback ends the way libctru does, minus the
   heap frees and minus everything that touches citro3d, the GPU queue, the DSP or the
   heap's owners, all on thread T, every step logged raw first:
   1. **Watchdog.** Start a small thread (`threadCreate`, 4 KiB stack, priority 0x18,
      detached; the fallback already uses the heap, step 3) that sleeps 5 s and then logs
      `fallback exit: watchdog fired (stuck in <step>); svcExitProcess` raw and calls
      `svcExitProcess()`. A hang in the steps below therefore ends exactly as before this
      piece (the system's restart), never worse. If the thread cannot be created, log it
      and go on without it. *Built (A7):* the watchdog starts after `error_screen_here()`
      returns, not before: the screen waits for A/START, and a 5 s timer would end it.
      So the screen itself stays unguarded (a hang inside its `aptMainLoop()`, e.g. in a
      HOME transition), as before A7; only the exit steps are timed (critic round 1, 6).
   2. **Software build only (`A1_RENDER_C3D` off): `gfxExit()`**, as every clean exit did
      before its APT close (SDL's video quit reaches it there). The c3d build skips it:
      with citro3d up T must not touch the GPU state (5.6.2 a; `gspExit` joins the thread
      that runs citro3d's callbacks, and main may be between `C3D_FrameBegin` and
      `C3D_FrameEnd`), and with citro3d down the only reachable `gfxExit` is
      `__wrap_gfxExit`, which `printf`s through the tee lock that main may hold. This
      split is also the diagnosis: if mode 7 (software) passes and 6b/10 (GPU) do not,
      `gspExit` matters too; the GPU build then ends still holding the GPU right with the
      GSP event thread alive, and the next candidate is a bare `GSPGPU_ReleaseRight()` on T
      before `aptExit` (one IPC, no thread join; UNVERIFIED that it matters; critic round 1,
      5). Known window (software build): after `gspExit` the GSP shared memory is unmapped
      (`gspgpu.c`:218-220); a main thread that is not stuck for good and reaches a present
      (`gfxScreenSwapBuffers`) before APT ends the process would fault (a Luma data abort, not
      a clean close). Not reachable in the test modes (mode 10's main blocks at its next
      service point, `video_3ds.cpp`:233, before a present); the window is `aptExit` plus one
      IPC (critic round 1, 4).
   3. **`aptExit()`**, once. The port's `apt_hook`, when it runs on a thread other than
      main while the fallback is active, logs one raw line and returns: no `printf`, no
      SDL call (an ONEXIT under a close order would otherwise `printf` through the tee
      lock; this also closes deferred item (4) of piece 2's review, HOME during the
      fallback). Before the call the fallback logs the branch inputs (`should close`,
      run flags) and `__system_retAddr` (set or NULL).
   4. **End.** If `aptExit()` changed `__system_retAddr` (the close branch set it to
      `aptExitProcess`), call it: `APT_CloseApplication`, as `__libctru_exit` would,
      without the heap frees before it. Otherwise (reinit or chainload branch, or the
      loader's own return address, which expects a freed heap) `svcExitProcess()`. If the
      call returns, `svcExitProcess()`. The log line before it says which. **Only the
      close branch ends the way libctru does.** With `RUNFLAG_APTREINIT` libctru sends
      `APT_Finalize` and `__libctru_exit` then calls the loader's return address after
      freeing the heap; the fallback cannot (the heap is in use) and ends in
      `svcExitProcess()`: that is the A6 failure plus one IPC, so expect the restart there
      (critic round 1, 2). d7 says which branch the launcher selects.
   Not run: `ndspExit` (can hang; mode 6 ended silent without it), `hidExit`, `fsExit`,
   `archiveUnmountAll`, `srvExit` (the kernel closes their sessions; no evidence that they
   matter; UNVERIFIED), and the heap frees (they are what faulted in dump 6: threads
   waking on a freed heap).
   *Rejected:* (B) the full libctru exit (`_exit` → `__ctru_exit`) on T: it frees the heap
   under main, the SDL audio thread, the ndsp/GSP/APT threads, whose stacks are all in it
   (dump 6's class; mode 10's main wakes after 7 s); (C3) `APT_PrepareToCloseApplication` /
   `APT_CloseApplication` called directly: duplicates `aptExit`'s branch logic (reinit,
   chainload) and leaves the APT thread and lock state inconsistent; (C4) parking every
   thread first: threads in timed sleeps or in another library's lock cannot be parked, and
   there is no per-thread terminate (`svc.h` has only `svcExitProcess`/`svcTerminateProcess`);
   (C2) making main serve more often: does not remove the need for a clean fallback (a
   main thread stuck for good still needs one), so it is not this piece; a longer
   acknowledgement bound stays an option for later.
   **d7 (boot):** `[3ds] env: homebrew <0|1>, run flags 0x<hex> (aptworkaround <0|1>,
   aptreinit <0|1>, aptchainload <0|1>), return address <set|null>` (`env.h`:9-11, :18,
   :70; `__system_retAddr`).
4. **Serve (main thread)** only at **top-level** service points: `present_screen()`
   entry (`video_3ds.cpp`:229), `present_bottom()` (:267), and `__wrap_hidScanInput`
   on the main thread (:937; covers loops that pump events without drawing). Not in
   `console_write()`/`bottom_present()`: those run inside libctru's APT hook loop
   (`on_resume` → `bottom_redraw` → `console_write`, `input_3ds.cpp`:676-677,
   :184-219, inside `aptWaitForWakeUp`) and inside SDL's event filter under
   `SDL_event_watchers_lock` (`debug-input`: `event_filter` → `log_event` →
   `log_input` → `overlay_add` → `console_write`, :604-607, :367-376, :246-264;
   `SDL_events.c`:1160-1163), where serving would nest `aptMainLoop()`/exit in a hook
   or run the error screen under SDL's lock. As a guard, `apt_hook` and
   `event_filter` set `s_in_hook`/`s_in_filter` for their duration and the service
   point does nothing while either is set. None of the three points holds the log
   lock or a C3D frame. Serving: if `s_fatal_active` is already set (main is in its
   own fatal path or already serving: its error screen calls `hidScanInput`, a
   service point), **acknowledge without serving** (T then blocks in step 2; main's
   screen stays; no nested `error_screen`). Otherwise set `s_fatal_active`,
   acknowledge, log `fatal error from thread T, shown by the main thread`, run
   `error_screen(title, body)` on main (its `#ifdef A1_RENDER_C3D`
   `a1_c3d_release_top()` is then a direct release; it holds its own HID
   reference), then `fatal_exit()` (step 5).
5. **`fatal_exit()` (main), the one end of every fatal path on main** (served
   requests, `die_with_message` today `a1_3ds_platform.cpp`:592-598, the `vhalt`
   and `abort` cases in step 7). It picks the variant **at exit time from
   `s_audio_parked`**, not from who made the request:
   - not parked: if `SDL_WasInit(0)`, `SDL_Quit()` (stops the SDL audio thread,
     reaches `gfxExit` through the wrapper; as the console-proven dialog exit,
     [STAGE5.md](STAGE5.md):270-289, `fn-main-log.txt`:447-453); `close_outputs()`;
     `_Exit(1)` (`__appExit` then runs on main; no `atexit` handlers and no engine
     static destructors in a fatal state).
   - parked: step 6's variant.
   T, if any, is in a kernel wait and never wakes.
6. **The SDL audio thread is parked** (music decoding and OpenAL mixing run there:
   the most likely off-main assert). It holds `device->mixer_lock` inside
   `SDL_RunAudio`'s callback (`SDL_audio.c`:731-738; the N3DS driver uses the
   default `LockDevice` on that mutex), so `SDL_PauseAudio(1)` (`on_suspend`,
   `input_3ds.cpp`:645-648, reachable through HOME during the error screen's
   `aptMainLoop()`), `SDL_Quit` (`close_audio_device`: `LockDevice`, then
   `SDL_WaitThread`, `SDL_audio.c`:1199-1206) and any OpenAL call, including
   `shutdown_application()`, would deadlock on main — whether the audio thread won
   the request, lost it to another thread, or failed while main was already in a
   fatal path of its own. So while `s_audio_parked`: `on_suspend`/`on_resume` skip
   their SDL audio calls (`SDL_GetAudioStatus`, `SDL_PauseAudio`;
   `audio_set_suspended` is the port's own atomic and stays), `quit_sdl_at_exit`
   skips `SDL_Quit`, and `fatal_exit()` ends with `gfxExit()` directly (the wrapper
   releases citro3d first; `gspExit` joins the GSP event thread, the STAGE5 fault),
   `close_outputs()`, `_Exit(1)`. Main makes no OpenAL call on this path. Main can
   still block before it serves: the engine takes `SDL_LockAudio` through its
   backend lock (`OpenALManager::ResyncPlayers`/`StopAllPlayers`,
   `OpenALManager.cpp`:144, :221; backend `audio_3ds.cpp`:292) on volume changes, at
   level end and on stop; an audio-thread failure then gets no acknowledgement and
   ends in the fallback (test mode 6b).
- **Invariant:** no fatal error may be raised from inside `tee_write` or newlib's
  malloc (the fallback's raw log write and the static buffers assume neither lock
  is held by T). No entry point below is reachable from either today; keep it so.
7. **Entry points** on thread T:
   - `vhalt()`: a fenced engine edit at its top, **`PATCHES.md` 54**
     (`csalerts_sdl.cpp`:322, before `stop_recording()`):
     `#ifdef A1_PLATFORM_3DS a1_3ds::vhalt_hook(message); #endif`
     (declared in `a1_3ds_platform.h`, which the TU already includes, :39-41;
     `ppcheck.sh` IDENTICAL expected). `vhalt_hook`: off main →
     `fatal_from_other_thread("Aleph One stopped", message)` (never returns); on main
     with `s_audio_parked` → `s_fatal_active`, `error_screen`, `fatal_exit()` (never
     returns: upstream's `shutdown_application()` would deadlock on the audio lock,
     step 6); otherwise it sets `s_fatal_active` and returns, and upstream's `vhalt`
     goes on as today. Chosen over `--wrap` of the mangled `shutdown_application`
     because the wrap would still let `stop_recording()` and `logFatal` run on T
     first and depends on a mangled name.
   - `abort()`: `-Wl,--wrap=abort` (`ports/3ds/CMakeLists.txt`, the pattern of the
     existing wraps): `__wrap_abort` → elsewhere than main
     `fatal_from_other_thread("Aleph One stopped", "abort() was called on thread T
     ...")`; on main, **if an error screen has already been shown** (`s_error_shown`,
     set by `error_screen`; e.g. a screen shown on the `vhalt()` path before its
     trailing `abort()`, `csalerts_sdl.cpp`:328-329, which today yields a second
     screen, script 17's two As), straight to `fatal_exit()` with no second screen,
     so every fatal error shows exactly one; otherwise `__real_abort()` (unchanged:
     main's `_reent` has `on_abort`, whose `die_with_message` now ends in
     `fatal_exit()` and checks `s_audio_parked` like every main fatal path).
   - `on_terminate` and `alert_user(fatalError)`: through `error_screen`, whose first
     act off the main thread is `fatal_from_other_thread(title, body)` (never
     returns; every caller exits afterwards, `a1_3ds_platform.h`:70-73).
   - The citro3d off-main hand-off (`release_from_any_thread`, `context.cpp`:118-154)
     stays for a `gfxExit` from another thread; with `vhalt` intercepted, `SDL_Quit`
     no longer runs off the main thread on these paths.

**Tests** (`3ds-port`: a new flag file `debug-fatal` with `mode N`, fired from the
main thread's `present_screen()` 20 s after the first present, as `present.cpp`:415-438
does for `test_vhalt`; `stage10-c3d` keeps `test_vhalt` 1-4 for the citro3d interplay):

| Mode | What | Azahar expectation (non-authoritative) |
| --- | --- | --- |
| 1 | `vhalt()` on main | as 5.6.2 (3); **exactly one** error screen, left with **one** A (script 17 and its comment, `17-c3d-vhalt.txt`:7-8, change from two As to one) |
| 2 | `abort()` on a second thread | first, **before** the wrap: each thread logs the previous value of `prev = signal(SIGABRT, SIG_DFL)` and **restores it at once** with `signal(SIGABRT, prev)` (newlib's `signal()` swaps in place, `nl/libc_a-signal.o` +0x30-0x34 per critic, so on main the probe would otherwise uninstall `on_abort`; on the second thread it allocates a 128-byte table, harmless): `SIG_DFL` there, `on_abort` on main (MAJOR 2's mechanism). With the wrap: `[3ds] ERROR` line, `handed to the main thread`, `shown by the main thread`, error screen captured with text, A, clean exit, 0 null reads |
| 3 | `vhalt()` on a second thread | the same; no `SDL_Quit` before the hand-off; no `hidScanInput` faults |
| 4 | `error_screen()` on a second thread | the same |
| 5 | `error_screen()` on a second thread 10 s into a HUD + Map game (script 23) | the same; two captures 5 s apart, identical bottoms with the text |
| 6 | `vhalt()` **inside the SDL audio callback** at a quiet moment, a scripted `home 2000` during the error screen | `s_audio_parked`, fatal-exit variant logged; no deadlock (the HOME round trip and A both work); clean exit |
| 6b | the same, fired while main is inside the engine's audio lock: during a music-volume change (Preferences) or at a level transition (`OpenALManager.cpp`:144, :221) | either served with the variant or, main blocked on `SDL_LockAudio`, the fallback after 5 s; both accepted, as long as Azahar ends cleanly (game list back, no guest fault); in the fallback the exit lines of mode 7 |
| 7 | a second thread takes a `LightLock` that main takes every frame, then fails | no acknowledgement; after 5 s **one** fallback screen drawn on that thread by `error_screen_here` (no second `handed to the main thread` line: no recursion), fallback lines in the log via the raw fd; A; then (step 3a) raw lines `fallback exit: starting the 5 s watchdog`, `fallback exit: gfxExit` (software build only), `fallback exit: should close 0, run flags 0x0 (...), return address null; aptExit` (Azahar: not homebrew, flags 0, return address null at d7), `fallback exit: APT_CloseApplication via __system_retAddr` (the close branch changed the address; the other branch logs `svcExitProcess (aptExit left __system_retAddr unchanged: reinit, chainload, crippled, or not the last APT reference)`); the run ends (Azahar's game list returns), no `watchdog fired` line, 0 null reads. With the ON build and citro3d up: nothing drawn, no wait, immediate exit, the same lines minus `gfxExit`. The watchdog itself is tested by hanging `aptExit` on purpose (temporary build): `watchdog fired (stuck in aptExit); svcExitProcess` after 5 s |
| 8 | two fatal errors at once: the audio callback and a second thread, 1 ms apart, in either order | one screen, drawn by main; the loser logs one raw line and blocks; the exit takes the audio variant in both orders (no `SDL_Quit` deadlock) |

With the ON build, modes 2-5 also with `p1-gpu.cfg`: `release done` on the main
thread before the error screen, 0 false returns. Console: modes 1, 2, 3, 4 (the
three second-thread runs of G0-b, plus main), 6 and 6b; by eye the port's screen;
**after each, the Homebrew Launcher starts again and launches Aleph One** (dump 5).
Mode 7 on the console only if the user wants the `aptExit`-skipping exit tried
(5.6.4 item 5).

**Risk:** a main-thread loop with no service point for > 5 s (fallback, with main
still running); the fallback's `svcExitProcess` without `aptExit` upsetting the HOME
menu (UNVERIFIED); the audio-thread variant missing an SDL/OpenAL call on main
(mode 6 checks the HOME and exit paths).
**How we test without hardware:** modes 1-8 (and 6b) in Azahar; the launcher's survival and
by-eye readability on the console.

#### 5.6.4 UNVERIFIED here (each with its test)

Settled by the critic's disassembly since the first revision (no longer
UNVERIFIED): the threads of all APT hooks; the rights at each transition and after
Close; `C3D_FRAME_NONBLOCK`; `consoleInit`'s first-call-only stdout device; when
ONEXIT runs; citro3d's own APT hook; per-thread newlib signal handlers.

1. GSP never runs, or never signals, GX work from a process without GPU rights
   (console run 1 of 5.6.1). *Status: **VERIFIED** on a New 3DS, 2026-10-24 (A6, run 1):
   with `close_frame 1` a frame forced after the GPU right was lost (`gpu right 0` at `before
   frame_end`) did not retire in the 5000 ms wait (`release abandoned (release): the GPU queue
   did not retire after 5000 ms`), while the earlier `frame_begin` retired in 0 ms (citro3d's
   ONSUSPEND hook drained the queue). One run, the first ordering of 5.6.1 run 1; the second
   ordering (`abandoned (frame_begin)`) was not seen. "Never" is shown up to 5 s.*
2. What GSP does with commands still pending at `gspExit` after an abandon; the
   abandon path ends the process cleanly and the launcher survives (runs 1-2). *Status:
   **VERIFIED** for the one real abandon, on a New 3DS, 2026-10-24 (A6, run 1): `release
   abandoned` with a pending queue, `real gfxExit returned`, `exit`, the Homebrew Launcher
   returned and Aleph One launched again, no Luma dump. What GSP does with the pending commands
   after that stays unobserved (the process ended). `p1-abandon.cfg` was not run on the
   console.*
3. The 5 s acknowledgement bound and the 500 ms retire bound are assumptions. *Status:
   piece 5 implements the 500 ms bound (`retire_wait_ms`). Console (New 3DS, 2026-10-24,
   A6): **500 ms was never hit** in the 6 gate-on releases of runs 2-4 (`queue retired after
   0 ms` every time, no `release abandoned`), so it is not too tight for a HOME > Close
   release. Also not hit in the six fatal-error releases of part B (B1-B6, 11:
   `queue retired after 0 ms`, 96-270 ms after the last frame end; Console (New 3DS),
   2026-10-25/26). The 5 s window of run 1 is not needed to see the
   failure: the queue was still not retired at 5000 ms, so any wait is spent in full; a
   shorter diagnostic bound would have shown the same.*
4. SPECULATIVE: dump 5 is the teardown-on-another-thread class (5.6.3 tests 2-4 on
   the console, launcher check). *Status: **VERIFIED** on a New 3DS, 2026-10-25 (A6 B2-B4): modes 2,
   3 and 4 are handed to the main thread, one port screen, `exit`, launcher back, no new Luma
   dump (still 0-6); mode 4 shows that a non-main `error_screen()` never touches citro3d.*
5. The fallback's `svcExitProcess` skips `gspExit`, `aptExit` and `ndspExit`: the
   HOME menu and the launcher survive, and the DSP does not go on playing buffers
   from the freed heap (listen for a buzz or a looping fragment after the exit; mode
   7, console, optional). The **audio-thread variant** skips `ndspExit` too (`_Exit` ->
   `__appExit` does not stop the DSP, and SDL's audio close is skipped): the same
   silence check applies to mode 6, and to 6b when it ends in the fallback (critic,
   piece 2 review round 1, minor 8). *Status: **mode 6 VERIFIED** on a New 3DS, 2026-10-26: silent
   after the error and in the launcher. **REFUTED for the fallback**, 2026-10-26 (6b and mode
   10, citro3d up): `svcExitProcess` without `aptExit`/`gspExit` does not return to the
   launcher; the system shows its own fatal-error message and restarts the console. The DSP
   check could not be made after those restarts. Mode 7 on the shipping (software) build restarts the console
   too (2026-10-26): not citro3d-specific; the exit without `aptExit` is the cause (which
   skipped call exactly is UNVERIFIED).* *Fix (5.6.3 step 3a, piece A7, 2026-10-26):
   the fallback now ends through `aptExit()` and `APT_CloseApplication` (and `gfxExit` in the
   software build), behind a 5 s watchdog. Azahar only; **UNVERIFIED on the console**: that
   the missing APT close was the cause, that `aptExit` off main ends cleanly on hardware,
   and whether `gspExit` matters (mode 7 software vs 6b/10 GPU). Test: `NEXT-SPRINT.md`
   "Fallback exit runbook".*
   *Console (New 3DS), 2026-10-26 (A7 session, `NEXT-SPRINT.md` item 14): **VERIFIED that the
   fixed fallback ends cleanly** in all five cases: mode 7 software (F1), mode 6b (F2) and mode 10
   (F3) GPU, HOME round trips (F4) and HOME > Close (F5) on the fallback thread; the launcher
   returns, no restart message, no Luma screen, no new dump; the DSP is silent during the freeze
   and normal after relaunching (F2). **VERIFIED: the missing `aptExit` was the cause** of the A6
   restarts. **REFUTED: that `APT_CloseApplication` was the missing piece**: this console takes
   the chainload branch (item 6), where F1-F4 end in `svcExitProcess` after `aptExit` without
   calling it, and F5's call (close order) returns before `svcExitProcess`. **REFUTED: that
   `gspExit` matters**: the GPU build (F2, F3) ends cleanly without `gspExit`/`gfxExit`, so the
   `GSPGPU_ReleaseRight` follow-up is not needed. `aptExit` off the main thread ends cleanly
   on hardware (VERIFIED, with the chainload branch only).*
6. Which `aptExit` branch the Homebrew Launcher's run flags select on this console (d7 at
   boot), and whether `APT_Finalize` alone (the reinit branch) also ends cleanly if that is
   the branch (expected not: the fallback then skips the loader's return, step 3a.4).
   *Status: UNVERIFIED; read d7 in any console log after piece A7. Azahar (non-authoritative,
   2026-10-26): `homebrew 0, run flags 0x0`, return address null: the close branch.*
   *Console (New 3DS), 2026-10-26 (A7 session): **VERIFIED, and the Azahar prediction REFUTED.**
   d7 reads `homebrew 1, run flags 0x4 (aptworkaround 0, aptreinit 0, aptchainload 1), return
   address null` (all five logs). So the launcher's run flags select the chainload branch, not the
   close branch: `aptExit` leaves `__system_retAddr` unchanged and the fallback ends in
   `svcExitProcess` (`fallback exit: svcExitProcess (aptExit left __system_retAddr unchanged: ...)`),
   and that ends cleanly. The reinit-branch question (`APT_Finalize` alone) does not arise here
   (`aptreinit 0`); the close branch is reached only by a close order (F5, which also
   returns cleanly). Azahar's `homebrew 0, run flags 0x0` differs from the console.*

### 5.7 B: the zero-copy present (HUD + Map game surface)

Plan B of `NEXT-SPRINT.md` (critic P1 finding 3; the user's go is the 2026-10-19 decision, 0.5).
Written 2026-10-26. The idea: the 400x240 `main_surface` of a HUD + Map game lives in linear
memory, the GPU's display transfer reads it directly, and the channel order is fixed on the GPU,
so the present's CPU copy into the staging image (5.1 step 2) disappears.

#### 5.7.1 Baseline and goal (before any code)

Console (New 3DS), `PERF.md` "Console G0-b" (both sessions), GPU present (P1) per frame, L0 / L6 / L20:

| Series | `present` | `bottom` | `max` | fps |
| --- | --- | --- | --- | --- |
| HUD + Map @30 | 2.03 / 2.03 / 2.03 | 2.93 / 2.89 / 2.95 | 49.7-61.9 | 30.62-30.65 |
| HUD + Map @60 | 2.75 / 3.43 / 3.27 | 2.37 / 2.33 / 2.40 | 33.8-35.3 | 60.20-60.41 |
| Classic @30 | 6.35 / 6.22 / 6.31 | — | 54.8-63.0 | 30.82-30.86 |
| Classic @60 | 5.93 / 6.22 / 6.37 | — | 38.6-46.6 | 53.07-53.24 |

**What B can save:** in HUD + Map only, the staging fill (`fill_other`: 400x240 read, `<< 8`,
written into staging). Its console share of the 2.03 ms is **not measured** (the per-step timers
were Azahar only: fill 1.34, `C3D_FrameEnd` 0.31, flush 0.06, transfer + draw 0.02 of 1.74 ms,
`PERF.md` "Stage 10 P1"); estimate 1.0-1.5 ms. **What B adds:** a blocking CPU wait for the upload
(the GPU time of a 512 KiB linear -> tiled transfer plus the GSP interrupt and the main thread's
wake-up, **UNVERIFIED**, estimate 0.2-0.6 ms) and one more C3D frame (`C3D_FrameEnd` alone was 0.31
ms in Azahar's breakdown, so not negligible). **Expected net: somewhere between ~0.1 and ~1.3 ms per
HUD + Map frame.**

**Is it within the noise? The lower end is.** The console's `present` repeats within ~0.05 ms only
@30 (2.03 x3; software 4.27-4.33 across sessions); @60 it spreads 2.75 / 3.43 / 3.27 because it
holds the VBlank wait, and the kickoff takes the console as ~1 ms precise for a whole-frame figure.
A 0.1-0.4 ms gain could not be told from noise at @60 and would not justify an engine edit. So,
**as the kickoff asks: the engine edit is not proposed on this estimate.** B is split (5.7.11):
**B-a**, port code only and off by default, restructures today's staging present into the two-frame
form of 5.7.4 and adds profiler zones for the fill, the upload wait and the VBlank wait, so one
console session measures the fill and the extra cost directly; **B-b**, the engine edit and the
channel reorder, is built only if B-a shows **fill − (upload wait + extra frame) ≥ 0.5 ms per frame
@30 on L0, L6 and L20** (2 runs each). Otherwise B stops at the design (and B-a stays as an off-by-
default diagnostic, or is reverted: the user's choice).

**What B cannot save:** Classic and every menu/dialog (640x480 is still halved on the CPU into
staging: the UHADD8 box is what keeps them byte-identical, `present_diff` 0); the ranged cache
flush (the surface must be flushed instead of the staging: the same 480 KiB); `C3D_FrameEnd`; the
VBlank wait that @60 adds to `present` (2.75-3.43 against 2.03 @30, design 5.1 step 5); the
unexplained move of ~0.5-1 ms into `bottom` with the GPU present. The stage 10 target is already met
by the software renderer on the console (0.5), so B is an efficiency piece, not a target fix.

**How long B matters:** for the P1 present and readback mode (M0-M5). In M6 direct mode the world
view no longer passes through `main_surface` (5.3), so there B's saving is limited to HUD + Map
frames that are not direct-eligible (5.4); Classic is never zero-copy. The mechanism (a linear engine surface the GPU reads, an exact channel
reorder) is the one M6's overlay upload (5.3) can reuse; that is M6's decision.

#### 5.7.2 Where the surface is allocated (the engine edit, `PATCHES.md` 55)

`main_surface` has one creation site, `screen.cpp`:1140-1142 (`if (!main_surface)
main_surface = SDL_CreateRGBSurface(...)`); every other path frees it and sets it to NULL first
(:864-867, :975-978). The edit, fenced `#if defined(A1_PLATFORM_3DS) && defined(A1_RENDER_C3D)`:

```cpp
	if (!main_surface) {
#if defined(A1_PLATFORM_3DS) && defined(A1_RENDER_C3D)
		// Stage 10 B (docs/3ds/CITRO3D_DESIGN.md 5.7): the GPU present reads this surface.
		int a1_pitch = 0;
		if (void* a1_px = a1_3ds::main_surface_pixels(vmode_width, vmode_height, a1_pitch))
			main_surface = SDL_CreateRGBSurfaceFrom(a1_px, vmode_width, vmode_height, 32, a1_pitch,
				pixel_format_32.Rmask, pixel_format_32.Gmask, pixel_format_32.Bmask, 0);
		if (!main_surface)
#endif
		main_surface = SDL_CreateRGBSurface(...as today...);
	}
```

`a1_3ds::main_surface_pixels(int w, int h, int& pitch)` is declared in `a1_3ds_platform.h` (engine-safe,
already included by `screen.cpp` under `A1_PLATFORM_3DS`, :76-78) under `#ifdef A1_RENDER_C3D`, and
defined in `render_c3d/back/present.cpp`. It returns `nullptr` (the engine allocates as before)
unless `w == 400 && h == 240`, `debug-c3d` says `present gpu` and `zero_copy 1` (config() loads the
file lazily, so it can be asked at screen setup, before citro3d is up), and the allocation succeeds.
Otherwise it returns one buffer:

- `linearAlloc(512 * 256 * 4)` = 512 KiB, 0x80-aligned (`allocator/linear.h`:10-14), pitch
  **2048 bytes (512 px)**: exactly the staging's shape (5.1, "P1 calibration record"), so the
  transfer is unchanged: `GX_BUFFER_DIM(512, 256)` on both sides, same flags. Rows 240-255 and
  columns 400-511 are never written by the engine (SDL clips to `w`, `h`); they are zeroed and
  flushed once at allocation, and the quad never samples them (u < 400/512, v < 240/256).
- **Allocated once, never freed** (process lifetime). `SDL_CreateRGBSurfaceFrom` marks the surface
  `SDL_PREALLOC`, so `SDL_FreeSurface` leaves the pixels alone, and the next 400x240 `main_surface`
  gets the same buffer. This relies on the invariant above (one live `main_surface` at a time: one
  creation site, guarded by `!main_surface`). Never freeing it also means a queue abandoned by the
  release (5.6.1 b) can never read freed memory.
- Every engine user of the surface is pitch-aware: `screen.cpp` (`quadruple_surface`,
  `draw_pattern_rect`, the present seam :2581, all with `->pitch`), SDL blits and fills elsewhere
  (`MainScreenSurface()` users: `interface.cpp`:3884, `screen_drawing.cpp`, `sdl_dialogs.cpp`,
  `shell.cpp`:1610 screenshots), and the port's CPU present (`video_3ds.cpp` `rotate_copy`,
  `half_rotate`, `present_reference`: all take `pitch`). So after a release (CPU present) the same
  linear surface is presented by the CPU unchanged.

#### 5.7.3 Format and the channel reorder

`main_surface` is ARGB8888 (`Amask` 0: `SDL_PIXELFORMAT_RGB888`; 0.1): the u32 `0xXXRRGGBB`, bytes
B, G, R, X in memory. The PICA RGBA8 texel is the u32 `0xRRGGBBAA` (P1: staging pixels `xrgb << 8`
display correctly and byte-identically, `PERF.md` "Console G0-b" `present_diff` 153 checks 0
differences). The display transfer with `IN/OUT_FORMAT(RGBA8)` moves whole 32-bit pixels, so the
surface's pixel reaches the texture as **R = X, G = surface R, B = surface G, A = surface B**.

**Reorder: three draws of the present quad, each with TEV stage 0 `GPU_REPLACE` and a one-channel
colour write mask** (`C3D_DepthTest(false, GPU_ALWAYS, mask)`, `effect.h`:10; masks
`enums.h`:224-230; RGB operands `enums.h`:331-346):

| Draw | RGB operand of `GPU_TEXTURE0` | write mask | result |
| --- | --- | --- | --- |
| 1 | `GPU_TEVOP_RGB_SRC_G` | `GPU_WRITE_RED \| GPU_WRITE_ALPHA` | red = surface R; alpha = 0 (alpha source `GPU_CONSTANT`, `C3D_TexEnvColor(env, 0)`) |
| 2 | `GPU_TEVOP_RGB_SRC_B` | `GPU_WRITE_GREEN` | green = surface G |
| 3 | `GPU_TEVOP_RGB_SRC_ALPHA` | `GPU_WRITE_BLUE` | blue = surface B |

Blending stays `ONE, ZERO`. **TEV stage 0 and the write mask are set in full before every draw**
(critic round 1, 1): one helper `set_present_draw(Swizzle)` sets source, both operands, function,
constant colour (through `C3D_GetTexEnv(0)`, which marks the stage dirty, `texenv.h`:24) and the
`C3D_DepthTest` mask, for each reorder draw, for the staging draw (`SRC_COLOR`/`SRC_ALPHA`, REPLACE,
`GPU_WRITE_COLOR`, as `present_init` sets it today, `back/present.cpp`:401-404) and for
`present_close_frame()` according to `s_tex_reordered`. Today stage 0 is set once at init; without
the helper a staging present after a zero-copy one would draw wrong colours (menus after a game,
Classic after the Bottom Screen switch, `odd_surface`). The framebuffer word then equals the CPU present's (`to_fb`: A = 0), so
`present_diff` should report 0 RGB and 0 alpha-only differences. **Exact by construction**: REPLACE
passes the operand unchanged, and nearest sampling at texel centres is exact (P1's byte-identical
result used the same quad, filter and REPLACE). A partial write mask is a read-modify-write of the
colour buffer, which needs colour-buffer reads enabled: citro3d writes the target's `colorMask`
(0xF for a colour target, `framebuffer.h`:13, :47) to `GPUREG_COLORBUFFER_READ` and `_WRITE`
(`registers.h`:295; `framebuffer.o` `C3Di_FrameBufBind`+0x100-0x13c, one 4-word write to 0x112).
That the PICA honours a one-channel mask this way is **UNVERIFIED** on hardware: `present_diff`
settles it on the console (5.7.9).

GPU cost: three 400x240 fills instead of one, asynchronous (the CPU does not wait for them; they
delay the framebuffer transfer and swap of the same frame). Size **UNVERIFIED**.
`C3D_GetDrawingTime()` (`renderqueue.h`:38) holds the last *finished* queue (the timer starts in
`C3D_FrameEnd`+0x104-0x124 and stops in `onQueueFinish`+0x8c-0xb8), so it is sampled right after
each `frame_begin()`: after frame U's it is the previous frame D (fills + framebuffer transfer),
after frame D's it is frame U (the upload); both are averaged into the `[c3d] stats` line (critic
round 1, 3).

**Rejected:**
- One draw, three TEV stages (`MODULATE` by constant (255,0,0), then `MULTIPLY_ADD`...): one fill,
  but the hardware's rounding of `x * 255 / 255` is **UNVERIFIED** and Classic/menus' byte identity
  is a standing requirement; the masked draws need no arithmetic. A candidate only if the console
  shows the three fills cost real time.
- `main_surface` as `SDL_PIXELFORMAT_RGBX8888` (bytes matching RGBA8): the world blit
  (`world_pixels`, `pixel_format_32`, 0.96 ms on the console) becomes a converting blit.
- A reorder in the transfer: it converts formats, it does not swizzle; `RGBA8 -> RGB8` drops byte 0
  (here surface B).
- Sampling the surface as a linear texture: PICA textures are tiled whatever memory they are in
  (`C3D_TexLoadImage` uses a TextureCopy for VRAM and a memcpy of already-tiled data for linear, 0.3);
  a display transfer is the only linear -> tiled step.

#### 5.7.4 Synchronisation: the present waits for the upload

The engine writes `main_surface` again after the present returns (the next frame's world blit;
in loops such as `screen_drawing.cpp`'s per-draw `MainScreenUpdateRect`, immediately). A transfer
queued inside a frame starts only at `C3D_FrameEnd` (0.3) and runs asynchronously, so the present
must not return before the upload has read the surface. **A zero-copy present is two C3D frames:**

1. **Frame U** (upload): `frame_begin()` (as today: every earlier frame has retired), the
   `present_diff` reference (it reads the surface), the ranged flush
   `GSPGPU_FlushDataCache(surface, 240 * 2048)` (480 KiB, the same range as today's
   `kStagingWrittenBytes`), `gx_op(C3D_SyncDisplayTransfer(surface -> present texture))`,
   `frame_end(0)` (no linked target drawn: no framebuffer transfer, no swap). `C3D_FrameEnd` runs
   the queue unconditionally (`renderqueue.o` `C3D_FrameEnd`+0x130, `gxCmdQueueRun`; the linked-target
   transfer only for targets drawn on, +0xa8-0xd4).
2. **Frame D** (draw): `frame_begin()` = `C3D_FrameBegin(0)`, which waits for frame U's whole queue
   (0.3): when it returns the upload has read the surface, and the engine may write it. Zone
   `c3d_wait` therefore now also holds the upload's GPU time (the console figure we want). Then the
   three draws (or the `calibrate` clear), the VBlank guard, the `present_diff` framebuffer note,
   `frame_end(1)`, exactly as 5.1 steps 4-5.

In B-a the same two frames carry today's staging (filled before frame U's flush), so the console
measures exactly the cost structure of B-b with the fill still in place (5.7.11). Zones (profile
builds): `c3d_fill` (the staging fill), `c3d_upload` (frame D's `frame_begin()`: the upload wait),
`c3d_vblank` (the VBlank guard's `gspWaitForVBlank()`), `c3d_wait` stays frame U's (or the one-frame
present's) `frame_begin()`. `frame_begin()` opens its own zone (`back/context.cpp`:420-422), so it
takes the zone as a parameter (`frame_begin(Zone z = C3dWait)`) and frame D passes `C3dUpload`;
wrapping the call instead would hand the time to the nested `c3d_wait` (critic round 2, 1). The new
zones sit inside the same `#ifdef A1_RENDER_C3D` fence as `C3dWait` (`a1_3ds_profile.h`:38-40,
`profile_3ds.cpp`:56-58), so the OFF-prof build and its BENCH line do not change (round 2, 3).

Queue entries: frame U 1 (transfer; `C3Di_SplitFrame` adds nothing when `gpuCmdBufOffset == 0`,
base.o +0x0-0xc, which `C3D_FrameBegin` resets) + 1 (`C3D_FrameEnd`) ≤ 3;
frame D 1 + 1 linked target = 2; each frame within the budget of 4 (2.4), counted per frame as today.

**Rejected:**
- An out-of-frame `C3D_SyncDisplayTransfer` (the first M0 readback used it; replaced in revise 1 for this reason): it waits for the queue, issues the transfer, runs the
  queue, then `gspWaitForEvent(GSPGPU_EVENT_PPF, false)` (`renderqueue.o`+0x60-0xb4), i.e. *wait,
  then clear* (`gspgpu.o` `gspWaitForEvent`+0x18-0x34). The GSP event thread signals that light event
  for **every** PPF (`gspEventThreadMain`+0xd0-0x108: `gxCmdQueueInterrupt`, the callback, then
  `LightEvent_Signal`), and nobody clears the one left by the previous frame's framebuffer transfer,
  so the wait can return at once, before this transfer has run.
- Waiting for PPF after `frame_end`: `gspWaitForEvent(PPF, true)` clears then waits; if both of the
  frame's PPFs (upload, framebuffer transfer) fired before the clear, it waits forever (no timeout
  in that API).
- Deferring the wait (into the bottom present, to overlap the upload with the bottom screen's CPU
  work): it needs a proof that nothing writes `main_surface` between the top and the bottom
  present, on every path. A follow-up only if the console's `c3d_wait` shows the upload wait is large.

#### 5.7.5 Which presents are zero-copy

Zero-copy iff `zero_copy` is on, `pixels` is the buffer of 5.7.2, 400x240, pitch 2048, format
`SDL_PIXELFORMAT_RGB888` or `ARGB8888`, and not an `odd_surface` present. Everything else (640x480
menus, dialogs and Classic, odd surfaces, a 400x240 surface that did not get the buffer) takes the
staging path of 5.1 unchanged, so **Classic and menus are untouched** (same code, same bytes). Each
upload rewrites the whole 512x256 texture, so switching between the paths needs nothing but a flag
saying what the texture holds (`s_tex_reordered`): `present_close_frame()` (5.6.1, a diagnostic
frame that redraws the texture) draws it the matching way. The staging's `s_layout` logic is
unchanged (only the staging path writes the staging).

#### 5.7.6 The gate, the release and the fatal paths (5.6: not to be broken)

- The gate is evaluated at the top of `a1_c3d_present()`, as now (with `suspend_gate 1` a closed
  gate returns Skip there, before any `frame_begin()`). HOME and sleep cannot arrive between frame U
  and frame D: the APT hooks run only inside `aptMainLoop()` on the main thread (`aptHandleSleep`,
  `aptJumpToHomeMenu`, `aptWaitForWakeUp`; `aptEventHandler` calls no hook, apt.o, critic round 1, 4).
  **`aptShouldClose()` can flip in between** (`aptEventHandler` sets the close flags on its own
  thread), so **`gate_closed()` is checked again after frame U** (only with `suspend_gate 1`; with
`suspend_gate 0`, the deliberate-reproduction mode, frame D draws as today: round 2, 2): if it now holds, frame D is not
  drawn; the present finishes with a bounded retire of frame U (`frame_begin()` takes the bounded
  path because the gate condition holds; `Abandoned` -> return true) and ends that frame empty
  (`frame_end(0)`), so nothing is drawn while the gate condition holds (5.6.1 a). Otherwise both
  `frame_begin()` calls behave as today's one: `Abandoned` -> return true, `AlreadyInFrame` -> the
  existing release path.
- All citro3d calls stay in `a1_c3d_present()` on the main thread; nothing new runs elsewhere.
- `present_free()` does not free the surface buffer (the engine's surface uses it; 5.7.2). After a
  release or abandon the engine keeps drawing into it and the CPU present (if any) reads it.
- `a1_3ds::main_surface_pixels()` makes no citro3d call (`linearAlloc`, `memset`,
  `GSPGPU_FlushDataCache` only), so it is safe at screen setup whether citro3d is up or not.

#### 5.7.7 How it is switched off

- `debug-c3d zero_copy 0` (new key, default 1): `main_surface_pixels()` returns `nullptr`, the
  engine allocates with `SDL_CreateRGBSurface` as before, and every present takes the staging path:
  exactly the P1 present. The surface is set up at the start of a game, so the key acts from the next
  game screen setup (put the file on the card before launching). One build serves both sides of
  the measurement.
- `present cpu`: the same (`nullptr`).
- The default build (`A1_RENDER_C3D=OFF`): the engine edit and the declaration are fenced with
  `A1_RENDER_C3D`, so `screen.cpp` and every other TU compile as before; the OFF `alephone.elf` must
  give `arm-none-eabi-nm build-3ds/alephone.elf | grep -ciE 'citro|C3D_|a1_c3d|main_surface_pixels'`
  = 0, with 0 warnings, and its code must not change: `arm-none-eabi-objdump -d` of the OFF
  `alephone.elf` (`build-3ds` and `build-3ds-prof`) before and after, identical (`ppcheck.sh` runs on
  the host, where `A1_PLATFORM_3DS` is undefined, so it says nothing about the 3DS OFF build).
  The host build defines neither macro: `ppcheck.sh` IDENTICAL expected (or DIFFER with 0
  normalised lines).

#### 5.7.8 Memory

+512 KiB of linear heap from the first HUD + Map game on, for the rest of the run (the staging stays:
640x480 still needs it). 9.2's total grows by 512 KiB, still far inside the ~17.8 MiB free in game.
The `[c3d] init ok` line and a one-time `[c3d] zero_copy: surface buffer ...` line log linear free
before/after.

#### 5.7.9 Tests

Azahar (non-authoritative; it completes GPU work synchronously, so the upload wait is ~0 there and
the gain is overstated):
1. `present_diff` (`p1-diff.cfg` + `zero_copy 1`), HUD + Map @30 L0/L6/L20 (`perf-bench.sh`):
   **0 checks with RGB differences and 0 alpha-only pixels**; script 07 (640x480 path, unchanged) too.
2. Negative control (temporary build, not committed): draws 2 and 3 swapped -> `present_diff`
   reports G/B differences at every check (proves the check sees the reorder).
3. Scripts 07, 08, 13, 14 (13 and 14 in both Bottom Screen settings: the HUD + Map <-> Classic
   switch moves the present between the two paths) and 22 (HOME > Close) pass; 0
   `present_screen()` false returns; GX high-water ≤ 4. **Scripts 13 and 14 and a game -> main
   menu run with `b-diff.cfg`: 0 differences after every path switch** (critic round 1, 1); negative
   control: a temporary build that skips the stage-0 restore must show differences there. Exactly one
   `[c3d] zero_copy: surface buffer` line across HUD + Map -> Classic -> HUD + Map, and the same
   linear free after (the buffer is reused, 5.7.2).
4. Profile build, same build both ways (`zero_copy 0` / `1`), HUD + Map @30 and @60, L0/L6/L20,
   plus Classic @60 L0 (must be unchanged): `present`, `c3d_wait`, `c3d_fill`, `c3d_upload`,
   `c3d_vblank`, **`blit` and `bottom`** (the world blit now writes into lines the per-present flush
   just cleaned; `bottom` already moved with P1: critic round 1, 6), busy, `max`.
5. `nm` and the `objdump -d` comparison on the OFF builds (5.7.7); `ppcheck.sh` on `screen.cpp`.

Console (runbook): `present_diff` with `zero_copy 1` (settles the write mask and the orientation,
section 10 item 16), the four series A/B in one build (`zero_copy 0` / `1`), `c3d_wait` (upload time) and
the logged drawing time; HOME, sleep, HOME > Close in a game; by eye. **Keep B-b only if, @30,
HUD + Map `present` (all its zones) and busy drop on L0/L6/L20 (2 runs each)**; @60 HUD + Map and
Classic are no-regression checks only (fps, `max`, `gaps`), because @60 a CPU saving can turn into
VBlank wait (zone `c3d_vblank` shows it) without changing busy (rule 1).

#### 5.7.10 UNVERIFIED here

1. The console's fill share of the 2.03 ms `present` (estimate 1.0-1.5 ms) and the upload's GPU
   time (estimate 0.2-0.6 ms): the `zero_copy` A/B series and `c3d_wait`.
   *Console (New 3DS), 2026-10-26 (B-a series, `PERF.md` "Stage 10 B-a"): **VERIFIED**: fill 1.11-1.15 ms
   (inside the estimate), upload wait 0.61-0.62 ms every run (above it). The rule's saving is
   0.435 / 0.350 / 0.355 ms on L0 / L6 / L20: **below 0.5 ms, no-go for B-b.** Not in the rule: `bottom`
   falls ~0.4 ms with `two_frame 1`, which would raise the saving to ~0.7-0.9 ms if it carried to B-b
   (UNVERIFIED). The decision rests with the user.*
2. One-channel colour write masks on the PICA (read-modify-write with `COLORBUFFER_READ` 0xF):
   `present_diff` 0 on the console.
3. The GPU time of three fills, and whether they push the swap late @60: `C3D_GetDrawingTime()`
   sampled after frame U's begin (5.7.3).
4. The TEV RGB operands `GPU_TEVOP_RGB_SRC_G` / `_SRC_B` / `_SRC_ALPHA` copy that one channel into
   R, G and B (`enums.h`:339-346 says only "Source green" etc.): `present_diff` 0 on the console
   settles it together with item 2.
5. Whether `blit` or `bottom` move with the surface in linear memory at a 2048-byte pitch.

*B-b built 2026-10-26 (branch `stage10-bb`, `PERF.md` "Stage 10 B-b"). Azahar — non-authoritative:
items 2 and 4 hold in the emulator (`present_diff` 0 RGB and 0 alpha-only pixels on every zero-copy
present, L0/L6/L20 and scripts 13/14/26; the swapped-draws control reports G/B differences, so the
check sees the reorder); items 1, 3 and 5 cannot be read there (GPU work completes synchronously).
All five stay open for the console ("B-b console runbook", `NEXT-SPRINT.md`).*

#### 5.7.11 Order: B-a, then B-b on the console's go

- **B-a (port only, no engine edit, no `PATCHES.md` entry):** `debug-c3d two_frame 0|1` (default
  0 = today's present, unchanged). With 1, a staging present runs as frames U and D of 5.7.4 with
  the staging filled before frame U's flush, the gate re-check of 5.7.6, the stage-0 helper of
  5.7.3 (one swizzle only: plain), drawing time sampled per 5.7.3; profile zones `c3d_fill`,
  `c3d_upload`, `c3d_vblank` (also with `two_frame 0`, so both sides are measured the same way).
  Azahar: `present_diff` 0, scripts 07/13/14/22 with `two_frame 1`; Azahar's `c3d_upload` is ~0 (it
  completes GPU work synchronously), so the go/no-go is the console's. Console: HUD + Map @30
  L0/L6/L20, `two_frame 0` and `1`, 2 runs each; **go for B-b iff `c3d_fill` − (`present` total with
  `two_frame 1` − `present` total with `two_frame 0`) ≥ 0.5 ms on every level** (present total =
  `present` + `c3d_wait` + `c3d_fill` + `c3d_upload` + `c3d_vblank`, the zones being exclusive).
  `c3d_fill` is the mean of the `two_frame 0` runs per level; both sides' `c3d_fill` are recorded
  and must agree (a sanity check). Against G0-b's 2.03 ms only the present total compares (`present`
  alone drops by the fill once it is split out). `scripts/3ds/tests/perf-table.py` gets the columns
  `c3d_wait`, `c3d_fill`, `c3d_upload`, `c3d_vblank` and "present total" (round 2, 4, 5).
  The user decides on the result.
- **Status (2026-10-26):** B-a built (`a35aa79f4`, critic APPROVE, minors folded), Azahar passes
  (`PERF.md` "Stage 10 B-a"); the console session is `NEXT-SPRINT.md` "B-a console runbook".
- **Status B-b (2026-10-26):** built on the user's decision despite the no-go, on branch `stage10-bb`
  only (critic round 1 REVISE: `PATCHES.md` 55 missing, 3 minors; round 2 APPROVE); Azahar passes
  5.7.9 items 1-5 (`PERF.md` "Stage 10 B-b"); console: `NEXT-SPRINT.md` "B-b console runbook".
- **B-b (on the go):** the engine edit (5.7.2, `PATCHES.md` 55), the reorder draws (5.7.3), the
  zero-copy path selection (5.7.5), `zero_copy` (5.7.7); B-a's `two_frame` becomes the zero-copy
  present's structure (zero-copy presents are always two-frame).

## 6. Stereoscopic 3D (M9, last; direct mode only)

Two view targets, two linked screen targets (`GFX_LEFT`/`GFX_RIGHT`),
`gfxSet3D(true)` (`gfx.h`:79; `particles/source/main.c`:175-183), slider
`osGet3DSliderState()` (`os.h`:313). One `render_tree()` (centre-eye
visibility); each eye redraws the same ring with `stereo` shifting
`clip.x += k·(w − w0)`; landscapes at max shift; overlays at zero parallax.
Right-eye framebuffers already exist (0.2). GX budget: +1 linked target.

**Non-stereo frames and the ramp-up (user decision 2026-11-08; M9's spec details it).** Only direct frames (5.4) have two
eye views. Every other frame (map, terminal, pause darkening, fades, gamma and damage/underwater flashes, a software
frame, Classic) writes **one image to both eyes, parallax 0**, so the right-eye buffer is never stale. The depth effect
**eases back in with a ramp** when direct frames resume: the parallax scale goes from 0 to the slider's value over a few
frames instead of jumping, so a hit flash, the map or a pause does not make the depth pop. Going flat has no ramp (a
fallback frame has no second view to ramp with: my reasoning, to be confirmed in the spec); only the return is eased. The ramp
length is a console tuning value (spec: a config key, `stereo_ramp N` frames, default to be set from the console, a first
guess of a quarter to half a second); it restarts from 0 whenever a non-stereo frame intervenes, and a slider change while
direct frames run is applied through the same ramp.

**Risk:** side-eye gaps at portal edges (**approximated**); 2x GPU fill; depth popping on non-stereo frames (the ramp).
**How we test without hardware:** Azahar 3D render mode (`render_3d`,
`qt-config.ini`:290); slider 0 ⇒ eyes pixel-identical; non-stereo frames ⇒ eyes pixel-identical; after a non-stereo frame the
parallax scale rises monotonically to the slider's value in exactly `stereo_ramp` frames (a logged per-frame scale, host-testable
in `core/`: the ramp is pure state). Comfort and ghosting stay a console item (section 10 item 13).

## 7. Milestones

Each: one commit `stage10: <id> <summary>`, tag `port-stage10-<id>`, a
`PERF.md` section "Azahar — non-authoritative", `PATCHES.md` + `ppcheck.sh`
for engine touches; the `-DA1_RENDER_C3D=OFF` build unchanged (`nm`: no
citro3d symbols). Code: `ports/3ds/render_c3d/{include,core,front,back,shaders}`
— `core/` plain C++ (host-tested), `front/` engine headers only, `back/`
`<3ds.h>`/`<citro3d.h>` only, talking through `core/` POD types.

**CMake** (`ports/3ds/CMakeLists.txt`, after :78 and :324):
`option(A1_RENDER_C3D "citro3d renderer (docs/3ds/CITRO3D_DESIGN.md)" OFF)`;
when ON: `add_compile_definitions(A1_RENDER_C3D)`, sources,
`ctr_add_shader_library(a1_c3d_shaders ...)` (`Nintendo3DS.cmake`:138-168),
`dkp_add_embedded_binary_library(a1_c3d_shbin a1_c3d_shaders)`
(`dkp-embedded-binary.cmake`:7-46; used as `textured_cube/source/main.c`:5,
116-119), link `citro3d` then `ctru` again, `LINKER:--wrap=gfxExit`.

**Runtime config** `sdmc:/3ds/alephone/debug-c3d` (parsed like `debug-perf`,
`profile_3ds.cpp`:80-114): `present cpu|gpu`, `renderer sw|c3d`,
`mode null|readback|direct`, `diff N`, `diff_frames N`, `classes ...`,
`fullbright 0|1`, `dump 0|1`, `calibrate 0|1`, `alt_load 0|1`,
`vram_cap KiB`, `cmdbuf_kib N`, `gxq_budget N`, `fail_every N`,
`film <path>`, `ssaa 0|1`, `test_vhalt 0-4`, `odd_surface 0|1`, `present_diff N`,
and (5.6) `suspend_gate 0|1`, `retire_wait_ms N`, `close_frame 0|1`, `force_abandon 0|1`, (5.7) `two_frame 0|1`, `zero_copy 0|1` (the
fatal-path tests are `3ds-port`'s `debug-fatal`, 5.6.3). `run-azahar.sh` gets
`--c3d FILE` (like `--script`, `run-azahar.sh`:201-206). Profiler zones (port
only, `a1_3ds_profile.h`:28-39, `profile_3ds.cpp`:55-57): `c3d_wait`,
`c3d_readback`, `c3d_upload` (B-a uses it for the upload wait; M0-M5's texture uploads would get
their own name), and (B-a) `c3d_fill`, `c3d_vblank`.

### P0 — software baseline (no code)
- `PREFS="fps_target=60 bottom_screen=0" RUNS=1 perf-bench.sh p0-hudmap60`,
  same with `bottom_screen=2`, and the two 30 fps series, L0/L6/L20, in
  Azahar; results in `PERF.md`. Console runs of the four series → section 10.

### P1 — citro3d bring-up + GPU present (5.1)
- Add: `back/context.cpp` (lazy init, state 2.4, `gx_op`, release/fini,
  `__wrap_gfxExit`), `back/present.cpp`, `shaders/present.v.pica`; touch
  `video_3ds.cpp`, `a1_3ds_platform.cpp` (`error_screen` release), CMake,
  `run-azahar.sh`.
- Accept (Azahar): builds ON/OFF; `[c3d] init ok` with linear/VRAM free
  before/after; 5.1 tests (frame-indexed @30/@60 with `alt_load`, captures,
  `present_screen()` false-return counter = 0 while citro3d is up,
  `odd_surface 1` presented by C3D); scripts 07, 08, 13, 14 pass; HOME > Close
  and `test_vhalt` log release before `gfxExit`, and `test_vhalt` reaches the
  error screen with one release and two `gfxExit` calls logged (idempotent
  wrapper, 1.3); 640x480 captures byte-identical to the CPU present (combined
  `to_fb(box())` pass); GX high-water ≤ 4 per frame; P0's four series
  repeated with `present gpu`. Gate **G0-a**; then **pause** for G0-b
  (0.5).
- **P1 follow-ups** (critic P1 minors 4, 5, 1): done, `PERF.md` "P1 follow-ups".
- **P1 follow-ups 2 (5.6; the console findings of 2026-10-19), before anything
  builds on P1.** Pieces, branches and order: **5.6.0** (piece 0 done).
  - `3ds-port` (`3DS port: ...` commits, then merged): piece 1, `error_screen`
    through the console device with its own HID reference, d6 (5.6.2); piece 2,
    the fatal hand-off with `--wrap=abort`, the audio-thread variant, the fallback,
    `debug-fatal` modes 1-8 and 6b, scripts 23 and the mode 6/6b scripts, and **one engine
    edit**: the `vhalt()` hook, **`PATCHES.md` 54**, fenced `#ifdef A1_PLATFORM_3DS`,
    `ppcheck.sh` on `csalerts_sdl.cpp` (5.6.3); piece 3, `app_suspended()`, d1,
    `exit_requested()` with `aptShouldClose()`, script command `home-close`, script 22
    (5.6.1 a, c).
  - `stage10-c3d` (`stage10: ...`, no tag), piece 5: the gate in the order of
    5.6.1 a (`back/present.cpp`, pure state in `core/` + host test), `retire_wait`,
    bounded release, tri-state `frame_begin`, abandon (`back/context.cpp`), d2-d5,
    `debug-c3d` `suspend_gate`, `retire_wait_ms`, `close_frame`, `force_abandon`, configs
    `p1-home-close-diag.cfg` (`suspend_gate 0`, `retire_wait_ms 5000`, `close_frame 1`),
    `p1-abandon.cfg`.
  - Accept (Azahar): the tests of 5.6.1 (3), 5.6.2 (3) and 5.6.3; P1's acceptance
    re-run with 0 skipped presents. Then the console runbook (5.6.1 runs 1-4, 5.6.2's
    control and main-thread run, 5.6.3 modes 1-4 and 6, the launcher criterion),
    written into `NEXT-SPRINT.md` for the user.

### B — the zero-copy present (5.7; plan B, after the P1 follow-ups 2)
- **B-a** (5.7.11; commit `stage10: B-a <summary>`, no tag): port only. `back/present.cpp`
  (two-frame staging present behind `two_frame`, `set_present_draw()`, gate re-check, drawing-time
  sampling), `core/config.*` + host test (`two_frame`), `a1_3ds_profile.h` / `profile_3ds.cpp`
  (zones `c3d_fill`, `c3d_upload`, `c3d_vblank`), configs `scripts/3ds/tests/c3d/b-two-frame.cfg`
  and `b-diff.cfg`. Then a console session (runbook) and the user's go.
- **B-b** (on the go; `stage10: B <summary>`, tag `port-stage10-B`): engine edit `PATCHES.md` 55
  (`screen.cpp` creation site, fenced `A1_PLATFORM_3DS && A1_RENDER_C3D`, `ppcheck.sh`); port:
  `a1_3ds::main_surface_pixels()` (`a1_3ds_platform.h` declaration, `back/present.cpp`
  definition), the reorder draws, path selection, `close_frame`'s matching draw, `zero_copy` +
  host test, `b-zero-copy.cfg`.
- Accept (Azahar): 5.7.9 items 1-5 (B-a: the subset in 5.7.11). Then the console (section 10
  item 16).

### M0 — renderer harness, calibration, null mode (after G1)
- **Status (2026-10-27): built and accepted in Azahar** (`PERF.md` "Stage 10 M0"; tag
  `port-stage10-M0`). (a) ppcheck IDENTICAL; (b) calibration 30/30 ok with the starting values
  unchanged (recorded in the block below); (c) SW-vs-SW 0 differing pixels in every check;
  (d) `R` 0.66 / 0.96 / 0.50 ms on L0 / L6 / L20, rings fixed in 9.2. The calibration's
  hardware half is section 10 item 8.
- Add: `include/a1_c3d.h`; `front/frame.cpp` (`render_frame`, config, null
  mode, diff harness, stats); `front/filter.cpp` (class/fullbright filter);
  `core/{diff,bmp}.cpp`; `scripts/3ds/tests/c3d-host/`; `c3d-diff.py`
  (stdlib, `python3 -I`); `scripts/3ds/tests/c3d/*.cfg`.
- Touch: `render.cpp` seam (hook 1), profiler zones, `PATCHES.md` 45.
- Accept: (a) `ppcheck.sh` on `render.cpp`: identical or `__LINE__` only;
  (b) readback calibration: 4 coloured corner quads on a frame-indexed clear,
  30 frames `orientation ok, current frame ok`; flags recorded here;
  (c) `renderer sw`, `diff 1`, all classes, 20 frames × L0/L6/L20 + menu
  demo: SW-vs-SW **0** differing pixels; (d) `mode null` bench L0/L6/L20:
  `R`, polygons/vertices/state changes max and p99 per frame; ring sizes in
  9.2 fixed at 2 × max.
- **M0 specification (revision of 2026-10-26, before code; Review log "M0
  pre-implementation").** The text above predates P1, plan A and B-a; where it and
  the code disagree, this block rules. Citations are the code as of `e2394b1`.
  - *Keys.* `calibrate` is taken: it is P1's present-target test (`core/config.h`
    "calibrate 0|1", `back/present.cpp` step 4). M0's readback calibration is the
    new key **`readback_cal 0|1`**. New keys, all in `core/config.*` + host test:
    `renderer sw|c3d` (default `sw`), `mode null|readback|direct` (default
    `readback`), `diff N` (0 = off, default; every N-th world frame is a diff
    frame), `diff_frames N` (default 20; diff frames in all, then diffing stops and
    a summary is logged; 0 = no limit), `classes LIST` (comma list, no spaces, of
    `walls floors landscape static sprites weapons tinted other frame`, or `all` =
    every one of them; default `all`; the `replay` check runs on every diff frame
    regardless), `fullbright 0|1`, `dump 0|1`, `readback_cal 0|1`. The parser stays `core/config.cpp`'s one-token `key value` (not
    `profile_3ds.cpp`'s), so `film <path>` (M1) will need a rest-of-line value.
  - *What runs in M0* (`render_frame`, in 1.5's order; everything else returns
    `false` and the caller draws in SW exactly as today):

    | Config | M0 behaviour |
    | --- | --- |
    | no `debug-c3d`, or `renderer sw` without `diff`/`readback_cal` | `false` at once (one flag test) |
    | `bit_depth != 32` or view side > 1024 | `false`, logged once per change (1.5) |
    | `diff N` (either renderer) | on diff frames: the harness below, which also draws the real frame into `dest` and returns `true`. With `renderer c3d` the B side is still SW in M0 (no C3D rasteriser before M1; logged once) |
    | `renderer c3d`, `mode null` | null mode (below), then `false` |
    | `renderer c3d`, `mode readback`/`direct` | logged once ("not before M1/M6"), `false` |
    | `readback_cal 1` | the first 30 world frames (after citro3d is up) run the calibration below, then `false` |

  - *One traversal per frame: record, then replay* (critic, M0 pre-implementation
    round 1, major 1). A second `c3d_draw` is **not** free of side effects:
    `render_viewer_sprite_layer` → `get_weapon_display_information()` →
    `add_random_flutter()` calls `rand()` (`weapons.cpp`:1242-1245, 1287-1289,
    1348-1350, 2922-2924), so two traversals can draw a fluttering weapon
    differently and shift the sequence Lua's `math.random` reads (`lmathlib.c`:201).
    1.2's "only the static-noise seed carries over" is therefore wrong; the harness
    never traverses twice. A diff frame runs `draw(ctx, &recorder)` **once**: the
    `front/` `Recorder` (a `RasterizerClass`) appends a **copy** of every primitive
    (`polygon_definition` holds its vertices inline, `scottish_textures.h`:181-211;
    `rectangle_definition` has no pointer SW writes through) with its entry point and
    foreground flag to a list of fixed capacity (8 192 events, a tagged union of the
    two structs, ≈ 1.9 MiB malloc (8192 × 240 B: `sizeof(polygon_definition)` 108, `sizeof(rectangle_definition)` 120, the Event's tag 12, ARM build) next to the scratch bitmaps, allocated at the first
    diff frame, diff runs only), **taking the copy before** forwarding the original to
    SW drawing into scratch A. The recording is an **event list**: `SetView(view)`,
    `Begin`, each primitive (entry point + copy), `SetForeground`, `End`, in traversal
    order; every replay re-emits the events in order to its target, handing each
    primitive over as a **temporary copy** of the stored entry (so SW's clip edits,
    the fullbright edit and later replays never see a modified entry). Then every comparison **replays** that list; the real frame is the
    full replay into `dest` (drawn over the previous frame's pixels, as SW does), and
    `render_frame` returns `true`. Recorder overflow: recording stops but forwarding
    to SW goes on to the end of the traversal; then the diff frame is dropped
    (logged, counted), the seed restored, `false` (the caller's SW draw is then a
    second traversal on that one frame). Null mode (below) keeps the caller's draw
    and so a second traversal per frame; its flutter is cosmetic, and null mode is
    **benchmark-only** (benchmarks run no Lua; with a Lua plugin its second traversal
    would shift `math.random`). M1+ (C3D frames, discards) should build on the recording; noted for M1.
  - *Diff frame* (8.1, made concrete). Two scratch bitmaps A and B of `dest`'s
    geometry (`bitmap_definition_buffer` + `precalculate_bitmap_row_addresses`,
    `textures.h`:55-81; 2 × 450 KiB malloc at the first diff frame, diff runs only);
    `dest` is written only by the final real-frame replay. SW draws into A or B by
    pointing `Rasterizer_SW_Class::screen` (`Rasterizer_SW.h`) at it and back. The
    noise seed (`texture_random_seed()`, `low_level_textures.h`:40-44, an inline
    function's static: one object program-wide; `front/frame.cpp` includes that
    header) is saved as `s0` before the traversal and set to `s0` before every draw
    and replay. Steps: (1) clear A, record while SW draws the full frame into A;
    (2) clear B, replay everything into B, compare as class **`replay`** (proves that
    replaying the copies equals the direct draw; must be 0 in every milestone);
    (3) per selected class: clear A (tinted: `0xFF808080`, else `0x00000000`),
    replay `filter(class)` to SW into A; clear B likewise, replay the same into B
    (M0: SW; M1+: C3D + readback keeping alpha); compare with `core/diff.cpp`;
    (4) replay everything into `dest`, `true`. Coverage: A by top byte ≠ 0 (B the
    same while B is SW; alpha ≠ 0 once B is C3D), tinted and static by "≠ clear".
    Edge band: a pixel whose 8-neighbourhood (inside the image) holds a different
    A-coverage. Metric over common covered non-edge pixels: max RGB channel |Δ|,
    mean of it, share ≤ the class tolerance (8.3; 0 in M0). One line per check:
    `[c3d] DIFF frame=F class=C covered=A/B cov_mismatch=E/N common=K within_tol=P% (tol T) mean=M max=X differ=D`
    (`differ` = pixels whose 32 bits differ, the M0 acceptance figure), and after
    `diff_frames` a `[c3d] DIFF summary frames=… checks=… differ_total=… dropped=…`.
    Zone **`c3d_diff`** around the whole harness (diff frames are not benchmark
    frames). `dump 1` writes `c3d-diff-<frame>-<class>-{sw,c3d}.bmp` (`core/bmp.cpp`,
    32-bit BMP, host-tested) for the first diff frame's checks and for every check
    with `differ > 0`, at most 32 files per run; `run-azahar.sh` copies `c3d-*.bmp`
    like `screen-*.bmp`. `c3d-diff.py` (stdlib, `python3 -I`): `log LOG [--expect-zero]` tabulates
    the DIFF lines per class (`--expect-zero` fails on any `differ`, a missing summary or
    dropped frames); `bmp A B [--class C]` recomputes the metric from a dump
    pair (a truncated or garbled BMP: clean message, exit 2).
  - *Classes* (`front/filter.cpp`; the pure mapping in `core/`, host-tested). The
    filter **wraps** a `RasterizerClass*` (composition; it does not derive from
    `Rasterizer_SW_Class`, so SW's internal punt of `_static` horizontal polygons to
    `texture_vertical_polygon`, `scottish_textures.cpp`:211-214, stays inside SW and
    the primitive is seen once). It forwards `SetView`, `Begin`, `End`,
    `SetForeground`, and per primitive either drops it or forwards a **copy** (the
    fullbright edit and SW's in-place clip edits never reach the recording):

    | Entry point | `transfer_mode` | class |
    | --- | --- | --- |
    | `texture_vertical_polygon` | `_textured` / `_big_landscaped` / `_static` | walls / landscape / static |
    | `texture_horizontal_polygon` | `_textured` / `_big_landscaped` / `_static` | floors / landscape / static |
    | `texture_rectangle` | `_tinted` / `_static` | tinted / static |
    | `texture_rectangle` | other, before / after `SetForeground()` (reset by `Begin()`) | sprites / weapons |
    | any | anything else (`_solid`: SW draws nothing) | other |

    `frame` = no filter (the full replay, under the class's own clear and
    `fullbright`). `fullbright 1`: every forwarded `_textured` primitive without
    `_SHADELESS_BIT` gets `ambient_shade = FIXED_ONE`, which selects the top shading
    table at any depth (`scottish_textures.cpp`:157-159: `a + s/2 ≥ FIXED_ONE`,
    `CEILING` to the last table, :161-170), i.e. 3.2's `table[256·255 + i]`. The
    `replay` check never uses `fullbright`.
  - *Null mode* (defines `R`, 0.5 and 9.3): `draw(ctx, &null)` with a `front/`
    `NullRasterizer` that receives every primitive the SW rasteriser would,
    classifies it, counts and draws nothing; then `false` and the caller's SW draw.
    `R` = its time, zone **`c3d_null`** (nested in `render`, so `render` stays the SW
    cost). Per frame it counts: polygons, rectangles, vertices `V = Σ n + 4·rects`,
    fan indices `I = Σ 3(n−2) + 6·rects`, **state changes** (consecutive primitives
    whose key `(class, texture, shading_tables, transfer_mode, transfer_data,
    _SHADELESS_BIT)` differs: the merges 2.1 allows; draws = changes + 1; this key
    comparison is M1's batching test, so it belongs in `R`) and distinct `(texture,
    shading_tables)` pairs. The bookkeeping inside `c3d_null` is bounded (critic
    minor 5): the distinct pairs in a fixed 1 024-slot open-addressing table with a
    per-frame generation tag (no clearing; a full table counts as 1 024 and is
    flagged), the per-frame samples in fixed arrays of 16 384 frames (beyond that only
    the max is kept), allocated once at the first null frame. A `[c3d] null` line at release or exit (revise 1; it was every 300 null frames) gives max and p99 of each over all stored null
    frames (p99 by a host-tested nearest-rank function in `core/`). Ring sizes (9.2) =
    2 × the bench max of `V` and `I`, rounded up to 1 024; 9.2's own "2 ×" (two
    halves, one per frame in flight in M6) is kept on top.
  - *The front/back interface.* `front/` includes two plain headers from `back/`
    (no `<3ds.h>`): `context.h` (as now: `config()`, `is_up()`) and the new `view.h`,
    which exports exactly: `enum class ViewCal` / `struct CalResult` (POD from
    `core/`), `bool view_calibrate(unsigned k, int w, int h, CalResult& out)` (false =
    skipped: citro3d not up, a release pending or the gate closed, 5.6.1 a; it applies
    those rules itself) and `void view_free()` (called by `release_and_fini()`).
  - *Readback calibration* (5.2's orientation, UNVERIFIED until it passes),
    `back/view.cpp`: an **unlinked** view target of the view's size
    (`C3D_RenderTargetCreate(w, h, GPU_RB_RGBA8, C3D_DEPTHTYPE(-1))`,
    `renderqueue.h`:62; 400x240, Classic 480x240) and a linear readback staging of
    `w·h·4` bytes, both created at the first calibration frame and freed by
    `view_free()` after `C3D_Fini` (the abandon path skips it, as it skips
    `present_free()`). A calibration frame runs inside `render_frame`, i.e. between
    two presents, through `back::frame_begin()` / `frame_end(0, …, FrameKind::View)`
    (the gate, the abandon and `s_in_frame` rules hold for it). It clears the view
    target to a colour encoding the calibration frame index `k`, draws four 16x16
    corner quads (top-left red, top-right green, bottom-left blue, bottom-right
    white; screen pixels, y down) with the present's shader and attribute layout
    (`present.v.pica`, float3 + float2) and TEV stage 0 = `GPU_CONSTANT`
    (`texenv.h`:85), queues the readback **in the frame** after the draws and the BufInfo
    restore: `C3D_SyncDisplayTransfer` tiled → linear (counted by `gx_op`; in a frame it
    splits the command list and queues the transfer, no wait, 0.3), ends the frame, then
    an empty `frame_begin()`/`frame_end()` pair confirms retirement (`C3D_FrameBegin(0)` =
    `gxCmdQueueWait(-1)`, the one settled signal, 0.3; a non-Ok begin skips the check,
    logged once), then `GSPGPU_InvalidateDataCache` (`gspgpu.h`:246), converts with 5.2's
    `(rgba >> 8) | 0xFF000000`. **Starting values** (no local example uses an
    unlinked, untilted target: every target in `examples/3ds/graphics/gpu/` is a
    240x400 screen with `Mtx_OrthoTilt`): projection `Mtx_Ortho(0, w, h, 0, 0, 1,
    true)` (`maths.h`:492); transfer `GX_BUFFER_DIM(w, h)` in and out,
    `GX_TRANSFER_FLIP_VERT(0) | OUT_TILED(0) | RAW_COPY(0) | IN_FORMAT(RGBA8) |
    OUT_FORMAT(RGBA8) | SCALING(NO)`. Checks: each corner holds its own colour →
    `orientation ok`; else the observed mapping is logged and fixed by **one** change
    and one re-run: rows flipped → swap bottom and top of the projection
    (`Mtx_Ortho(0, w, 0, h, …)`; the transfer flags stay); columns flipped → swap
    left and right; transposed → **not** a flag change (swapping `GX_BUFFER_DIM` on a
    non-square tiled buffer scrambles it): log it and stop; the fix is a design change
    to the screen-style arrangement every local example uses (target `(h, w)`,
    `Mtx_OrthoTilt`, `GX_BUFFER_DIM(h, w)`, `both_screens/source/main.c`:52, 102) with
    a transposing CPU conversion; the centre
    holds frame `k`'s colour (else which `k` it is) → `current frame ok`; the
    converted red is `0xFFFF0000` → `channels ok` (else the conversion is changed).
    **Revise 1, full-image check:** the staging is filled with a per-`k` sentinel (neither
    the clear of `k` nor a corner colour) and `GSPGPU_FlushDataCache`d before each
    transfer, so no dirty line is written back over the GPU's data and a pixel the
    transfer never wrote is recognisable; `core/calibrate` then checks **every** pixel:
    clear(`k`), or inside a 16x16 corner quad the colour the detected orientation puts
    there; `current frame ok` needs the centre and **0 mismatches**. Orientation and
    channel order still come from the five samples. `key readback_cal_resize 1` makes
    odd frames (w-8)x(h-8), so the target is deleted and created every frame (out of
    frame; `m0-readback-cal-resize.cfg`).
    One `[c3d] readback_cal k=… orientation … current frame … mismatched N (sentinel S) channels …` line per
    frame and a summary after 30 (`… channels ok 30, mismatched pixels M (sentinel S), skipped K`); with `dump 1` the readback of frames 0 and 29 as
    `c3d-rbcal-<k>.bmp`. The values that pass are written into this block.
    **Passed (Azahar 2126.2, 2026-10-27, 30/30 frames, 400x240):** exactly the starting values,
    no change needed: `Mtx_Ortho(0, w, h, 0, 0, 1, true)`; `GX_BUFFER_DIM(w, h)` in and out,
    `FLIP_VERT(0) | OUT_TILED(0) | RAW_COPY(0) | IN/OUT_FORMAT(RGBA8) | SCALING(NO)`;
    conversion `(rgba >> 8) | 0xFF000000`; TEV constant colour `0xAABBGGRR` (R in the low byte,
    UNVERIFIED locally; the corners came out right). Classic's 480x240 was not run (the
    calibration runs on the game's view, HUD + Map here). Console: section 10 item 8. **State
    hygiene with the present** (`back/present.cpp` is not edited in M0: plan B-b
    edits it): `view.cpp` copies `*C3D_GetBufInfo()` into a local, binds its own
    buffer, and after its draws restores the copy with `C3D_SetBufInfo(&saved)`
    (`buffers.h`:20-21; `C3D_SetBufInfo` **copies** its argument, VERIFIED by disassembling
    libcitro3d.a 1.7.1: a 152-byte memcpy into the context when it is active; the structs
    are static in `view.cpp` all the same); sets the projection uniform for its own draws (the present sets its
    own before each draw, `present.cpp`:342); sets TEV stage 0 (the present resets
    it in full before every draw, `set_present_draw`) and the cull/depth/stencil/
    alpha/blend state it needs (the present sets them all before every draw,
    `draw_present_quad`); and does **not** touch the program (`C3D_BindProgram` at
    init only, `present.cpp`:432), `AttrInfo` (:435-438), `C3D_TexBind(0)` (:453) or
    TEV stages 1-5, none of which the present sets again. **GPU timing:** a view
    frame sets a flag in `context.cpp` so that the present's next
    `sample_drawing_time(Draw)` is skipped (its "last finished queue" would be the
    view frame and readback, critic minor 7); `present.cpp` is unchanged.
  - *GX queue* (2.4). The readback transfer is in-frame since revise 1, preceded since revise 2
    by `C3D_FrameSplit(GX_CMDLIST_FLUSH)`, each counted by its own `gx_op`
    (`note_out_of_frame()` was removed); host test of 2.4's worst case (24 + 1 = 25 < 32).
    `frame_end` gets a frame kind (`FrameKind::Present` default, `View`): presents
    keep the "> 4 entries" warning, view frames warn at > 27 (2.4).
  - *Zones* (`a1_3ds_profile.h`, inside the `A1_RENDER_C3D` fence after B-a's;
    `profile_3ds.cpp` names; `perf-table.py` columns if it lists zones by name):
    `c3d_null`, `c3d_diff`, `c3d_readback` (the flushing split, the in-frame transfer, the frame end, the retirement wait pair, the invalidate
    + conversion + the full-image check; the wait pair's `C3D_FrameBegin(0)` is timed in the nested
    `c3d_wait`). A view frame's `C3D_FrameBegin(0)` is timed in `c3d_wait`, which
    then also appears inside `render`.
  - *Stats.* `[c3d] frame stats` once a second while world frames run: frames,
    diff frames (and dropped), null frames, calibration frames, skipped calibration
    frames.
  - *Revise 1 (critic round 1, 2026-10-27).* (a) `C3D_RenderTargetDelete` svcBreaks inside a
    frame: the target is created/deleted before `frame_begin()` (0.3). (b) The out-of-frame
    `C3D_SyncDisplayTransfer` could return before its own transfer (sticky PPF, 0.3, 5.7.4
    "Rejected"): the transfer is queued in the frame and retirement is confirmed by an empty
    `C3D_FrameBegin(0)`/`FrameEnd` pair. (c) Sentinel + full-image check, so that a race cannot
    pass on the console; Azahar completes transfers synchronously and cannot show one. (d)
    `readback_cal_resize`. (e) The recorder is ≈ 1.9 MiB, not 1.1 (240 B per event). (f) The final
    null/diff summaries also print on the `atexit` path; the null summary is no longer logged
    every 300 frames (its sort ran inside the `render` zone). **Carried to M1:** the 9.2 ring
    sizes come from three fixed bench views; M1 re-measures V/I max with null mode over the menu
    demo and a scripted walk before sizing its rings, and logs the ring high-water. The
    calibration does not check alpha (the conversion forces 0xFF); M1 adds an alpha check with
    the alpha-keeping readback (diff step 3).
  - *Revise 2 (critic round 2, 2026-10-27).* Revise 1's in-frame transfer made its own split
    submit the four draws with flags 0 (no GSP cache flush) while `C3D_FrameEnd(GX_CMDLIST_FLUSH)`
    skips the whole-heap flush: the console could run the draw segment from stale RAM (0.3, new
    row). `C3D_FrameSplit(GX_CMDLIST_FLUSH)` now comes first; the rule holds for every readback
    frame of M1-M5 (5.2). Azahar does not model the cache and cannot show it. The `c3d_readback`
    zone excludes the retirement wait (timed in `c3d_wait`): the readback's cost is
    `c3d_readback + c3d_wait`. Applied by the main session as the critic prescribed; a
    narrow critic round on that commit (`e971b1198`, user's request): **APPROVE**, one nit carried
    to M1: `GxQueue::take` charges the flushing split 2 entries (1 + 1 for a non-empty command
    buffer) where the queue holds 1, so `gx_high_water` (5 in Azahar) is a conservative upper
    bound; if M1's per-frame readback nears the budget, charge the split exactly 1.
  - *Not in M0:* any C3D rasteriser, readback of a real frame into `world_pixels`,
    the discard path, texture code. Hook 1's code (1.2) is used unchanged; its
    "re-running `c3d_draw` is safe" paragraph is superseded by the record/replay
    bullet above.

### M1 — flat-shaded walls only
- **Status (2026-10-27): built and accepted in Azahar** (`PERF.md` "Stage 10 M1"; commits `54d1e03c4`,
  `ac9a0241a`; tag `port-stage10-M1`; critic: APPROVE, six minors, three fixed in revise 1, the others
  recorded). Walls coverage: 0 non-edge mismatches on L0/L6/L20 × 20 and 200 film frames, every edge
  mismatch a tie pixel (0.11-0.32 % of the band). **Reserves:** `kDrawReserveWords` 508,
  `kEndReserveWords` 20 (2 × 254 and 2 × 10 words). **Rings:** 1 024 vertices and 2 048 indices per
  half, unchanged (re-measured: max V 387, I 597 over five views). **`C3D_Init`:** 512 KiB (worst usage
  0.82 %). Console: section 10 items 6 and 8 (`NEXT-SPRINT.md` "M1 console check", which carries
  M0's).
- Add: `core/geometry.cpp`, `front/rasterizer.cpp` (walls → draw items; others
  `unsupported`), `back/draw.cpp` (rings, batching, reserves 2.4, constant
  colour per `ShapeDesc`); `debug-c3d film` via `handle_open_document()`
  (`shell.h`:161, as `tests/replay_film_test.cpp`:68).
- Accept: diff `walls`, coverage only (8.1): 0 mismatches outside the 1-px
  edge band, ≤ 2% inside, 20 frames × L0/L6/L20 + 200 frames of a film;
  0 ring overflows; max `C3D_GetCmdBufUsage()`, words per draw and per
  `C3D_FrameEnd` logged, reserves set; `cmdbuf_kib 32` run discards and
  falls back without a crash; host: w vs SW `world_x`
  (`scottish_textures.cpp`:916-954) rel. error ≤ 1e-3.
- **M1 specification (revision of 2026-10-27, before code; Review log "M1
  pre-implementation").** The five lines above predate M0. Where they and the code disagree,
  this block rules. Citations are the code as of `fc4e1e493`. M1 runs **only on diff frames**
  (the C3D side B of a check); play frames stay SW until M4 (1.5). No engine edit.
  - *Keys* (`core/config.*` + host test; the `[c3d] debug-c3d:` line prints every one):
    `renderer c3d` with `diff N` now draws side B of each check in C3D (M0: SW, `frame.cpp`:213-217
    logged it); `renderer c3d` without `diff`, `mode readback` → logged once "play frames are
    software before M4", `false`; `mode null` unchanged. New:
    `cmdbuf_kib N` (16..4096, default 512: the `C3D_Init` size, today the constant
    `context.cpp`:43; below 16 KiB the present's own frames, which check no reserve, are not
    guaranteed); `gxq_budget N` (1..24, default 24: `GxQueue::kBudget` becomes a runtime value,
    `gx_queue.h`:22); `ring_vertices N`, `ring_indices N` (vertices / indices **per half**, 64..32768
    / 64..65536; defaults set by the measurement below, until then M0's 1 024 / 2 048); `film PATH`
    (**rest of the line**, trimmed: `config_parse` reads one token today, `config.cpp`:153, and film
    names have spaces; a `#` still starts a comment, so a path with `#` is not supported; relative to
    `a1_3ds::user_dir()` unless it starts with `sdmc:/` or `/`); `reserve_extra N` (0..1 000 000
    words, default 0: added to the command-buffer reserve test, to force a `cmdbuf` discard; revise 1).
  - *Side B in the harness* (record/replay, `frame.cpp`:209-283, unchanged in shape). Step (1) and
    the `replay` check stay SW-vs-SW. Step (3), per selected class with `renderer c3d`: side A is
    M0's SW replay of `filter(class)` into scratch A; side B is **one C3D view frame**: the same
    recording replayed through `FilterRasterizer(&c3d, class, fullbright)` into the new `front/`
    `C3dRasterizer`, which hands draw items to `back/draw.cpp`; the frame is read back keeping alpha
    into scratch B; `core/diff.cpp` compares. Step (4), the real frame into `dest`, stays SW. The
    noise seed is set to `s0` before each side as in M0. One view frame per C3D check, so a diff frame
    with `classes walls` costs one GPU frame plus its retirement wait (not a benchmark frame; zone
    `c3d_diff` as M0).
  - *What C3D draws in M1.* `texture_vertical_polygon` with `_textured_transfer` (class walls) whose
    texture is opaque. Everything else reaching `C3dRasterizer` is **unsupported**: the view frame is
    **discarded** (1.5: no more draws, `C3D_FrameEnd` normally, no readback) and the check is logged
    `[c3d] DIFF frame=F class=C discarded (unsupported|ring|cmdbuf|gx|begin)` and not compared (a check
    that could not start is `skipped (down|gate|size)` instead, see *Back end*).
    So with `classes all` every check but `walls` (and `replay`) is discarded in M1; that is
    expected. **Transparent walls** (`texture->flags & _TRANSPARENT_BIT`: SW skips texel 0,
    `low_level_textures.h` `write_pixel<…, check_transparent>`) are drawn with holes by SW and would
    be solid in flat C3D, a non-edge coverage difference that M1 cannot fix (M2 brings textures).
    They are therefore **excluded on both sides** of a C3D check: `FilterRasterizer` gets a flag
    `c3d_only` (set only for C3D checks) that drops the primitives a pure `c3d_supports()` (M1:
    walls, opaque) rejects, so a level with a grate keeps its walls check; the DIFF line counts them
    (`excluded=N`). Excluded is not unsupported: only primitives of a class the filter passes are
    tested, and only C3D checks exclude.
  - *Polygon acceptance* (2.1, mirrored from SW so that C3D never draws what SW skips): a vertex
    outside `[0, w] × [0, h]` (`scottish_textures.cpp`:449-456), `min x == max x` (:459), or a walk
    that `build_y_table` refuses (negative dx on the clockwise walk from the leftmost vertex or on the
    counter-clockwise one, :479-505, `build_y_table` returns NULL at :1288) → the polygon is skipped
    (not unsupported); a non-positive signed area in y-down screen space is skipped too (SW then
    has `y1 < y0` in every column and writes nothing, `low_level_textures.h`:322-326). The pure test
    is in `core/geometry.cpp`, host-tested against a transliteration of those lines.
  - *Coverage rule check (why 8.3's "0 outside the band" is reachable).* `build_y_table`'s
    x-dominant walk (`scottish_textures.cpp`:1262-1270) records, for column `k`, `y0 + ceil(s·(k+½) −
    ½)` (`d` starts at `adx − ady` and gains `−2·ady` per column, `+2·adx` per step down): the
    line sampled at the **column centre**, and rows `[y0(x), y1(x))` are written
    (`low_level_textures.h`:322-326). That is the GPU's pixel-centre rule; the two differ only on
    exact ties (the line through a pixel centre; PICA's tie rule is UNVERIFIED) and by the
    rasteriser's sub-pixel precision, both inside the edge band. The y-dominant walk (:1274-1281) is
    the same midpoint form. No half-pixel offset is applied to positions.
  - *Vertex data and the shader.* 2.1's 32-byte vertex: `(sx, sy, w)` float3, texcoord0/1 float2
    each (constant 0 in M1), colour ubyte4. `w` per 2.2's wall row, evaluated at the vertex's
    integer `sx` (SW evaluates `world_x` at the column `x0 − hw`, `scottish_textures.cpp`:906, 916-954;
    the ½-pixel question is M2's, with the texcoords), `|w|` as SW (:954), clamped to `[16, 32767]`
    (2.3), denominator 0 → SW's guard (:943, 950). `shaders/world.v.pica` exactly as 2.1, assembled
    into **its own** shader library and embedded binary (`a1_c3d_world_shaders`,
    `a1_c3d_world_shbin.h`), so the present's shbin and its `DVLE[0]` (`present.cpp`:431, `view.cpp`
    `find_projection_location`) do not change. Colour: a hash of `ShapeDesc` to RGB, alpha 0xFF,
    via TEV stage 0 = `GPU_PRIMARY_COLOR`, `GPU_REPLACE`, RGB and alpha (`texenv.h`). GPU state per
    2.4's "World draws" column (cull none, depth off, stencil off, alpha test `GEQUAL 128`, blend
    `ONE, ZERO`), projection `Mtx_Ortho(0, w, h, 0, 0, 1, true)` (M0's calibrated value).
  - *State save and restore around a view frame* (the present sets only its own state again before
    each draw, M0 block "State hygiene"). M0 restored the BufInfo; M1 also changes the **program** and
    the **AttrInfo**, which the present sets once at init (`present.cpp`:432, 435-438) and never
    again. AttrInfo: copy `*C3D_GetAttrInfo()`, restore with `C3D_SetAttrInfo(&saved)` — copy
    semantics VERIFIED (libcitro3d.a 1.7.1 `attribs.o` `C3D_SetAttrInfo`+0x14-0x48: 24 bytes copied
    into the context at +0x28, dirty bit 4 set; a no-op when the context is inactive). Program:
    citro3d has no getter and `present.cpp`'s `s_program` is in an anonymous namespace (:65, :100),
    and `present.cpp` is not edited (plan B-b), so `-Wl,--wrap=C3D_BindProgram` (the
    `--wrap=gfxExit` pattern, `CMakeLists.txt`:363-365) records the last program bound in
    `back/context.cpp` and calls the real one (`base.o` `C3D_BindProgram`: stores the pointer at
    context +0x24 and sets dirty bits, +0x10-0x44; with different code it also sets the re-upload
    flags, +0x7c-0xa8, and reloads the bound shader's constants, `C3Di_LoadShaderUniforms`, so the
    world shader's `.constf` cannot leak into the present's registers). **`back/draw.cpp` binds
    through `__real_C3D_BindProgram` only** (both the world program and the restore), so the record
    always holds the program bound by everyone else, i.e. the present's (revise 1, critic finding
    2: through the wrapper the "recorded" program would be the world program, and nothing would show
    it: the present's z = 0.5 makes the world shader give the same NDC, and its TEV reads only texture
    0). The save happens **before** any bind of the view frame. Testable: the wrapper logs the first
    pointer it records (`present_init`) and counts later binds; after every restore `draw.cpp` checks
    that the recorded pointer is the one it re-bound and counts `prog_restore_mismatch` (in the
    `[c3d] cmdbuf` line; must be 0). The re-bind makes citro3d upload the present's shader at the
    present's next draw (dirty flags): more command words in that present frame, measured below.
    The world program is created at the first C3D view frame and freed by `draw_free()` after
    `C3D_Fini` (the abandon path skips it, as it skips `present_free()`). TEV stages 1-5,
    `C3D_TexBind(0)` and the viewport (set by `C3D_FrameDrawOn`) are not touched.
  - *Back end* (`back/draw.cpp` behind a plain `back/draw.h`; the view target and readback move from
    `view.cpp`'s calibration into shared `view.cpp` functions used by both). Per C3D view frame:
    `is_up()`, `release_pending()` and the gate checked first as M0, and a view size the readback
    does not support (`view.cpp`:40-41, multiples of 8, ≤ 1 024 per side, ≤ 262 144 pixels) → the
    check is **skipped** (`skipped (down|gate|size)`, not a discard: nothing was drawn; revise 1,
    critic finding 8); target create/resize **out of frame** (M0 `ensure_resources`); the staging is
    filled with a per-frame sentinel and flushed before the frame, as M0's calibration does
    (`view.cpp`:254-262), so a pixel the transfer never wrote is recognisable (counted as
    `sentinel=N` on the DIFF line, must be 0; the colour hash never produces the sentinel's RGB)
    and no dirty line is written back over the GPU's output; `frame_begin()` (return checked: not
    `Ok` → discard `begin`); `gx_op` clear to `0x00000000` (`C3D_RenderTargetClear`,
    `renderqueue.h`:72) — **only before the first draw**: `C3D_FrameBufClear` calls
    `GX_MemoryFill` directly without a split (`framebuffer.o`), so a clear after draws would run before
    them; `C3D_FrameDrawOn`; **save** BufInfo/AttrInfo/the recorded program; then bind the world
    program (`__real_C3D_BindProgram`), AttrInfo, BufInfo and state; per primitive: append its vertices to
    the **current ring half** and its fan indices (`3(n−2)`) to the index half; consecutive polygons
    with an equal key (M0's null-mode key: class, texture, shading tables, transfer mode and data,
    `_SHADELESS_BIT`; so M1's draw count is what M2 will see) merge into one
    `C3D_DrawElements(GPU_TRIANGLES, count, C3D_UNSIGNED_SHORT, idx)` (`base.h`:24); a half that
    cannot take the polygon → discard `ring`. Before **every** draw and before the readback, the
    command-buffer reserve (2.4): `gpuCmdBufSize − gpuCmdBufOffset ≥ kDrawReserveWords +
    kEndReserveWords` (`gpu.h`:13-15; a split shrinks the size and zeroes the offset, so the
    difference is the space left), else discard `cmdbuf`. After the last draw: restore program,
    AttrInfo, BufInfo; **one ranged `GSPGPU_FlushDataCache` each of exactly the vertex and index
    bytes written** this frame (before the split below; the GPU runs nothing before `C3D_FrameEnd`,
    0.3) — on **every** path that issued at least one draw, the discard path included: the draws
    issued before a discard still run at `C3D_FrameEnd(GX_CMDLIST_FLUSH)`, which skips the whole-heap
    flush (0.3; revise 1, critic finding 4); then M0's readback sequence unchanged:
    `gx_op(C3D_FrameSplit(GX_CMDLIST_FLUSH))`, `gx_op`
    in-frame `C3D_SyncDisplayTransfer`, `frame_end(0, false, FrameKind::View)`, the empty
    `frame_begin()`/`frame_end()` retirement pair, `GSPGPU_InvalidateDataCache`, conversion. A refused
    `gx_op` → discard `gx`. A discarded frame still restores the state, flushes the ring bytes it
    wrote and ends normally; no readback. Vertex/index addressing (revise 1, critic finding 10):
    `BufInfo_Init` (base `0x18000000`) and `BufInfo_Add` of the **current half's** vertex start, so
    fan indices are relative to that half (0-based per half); the index ring is linear memory, whose
    physical address must lie above the BufInfo base and within the 28-bit offset `C3D_DrawElements`
    writes unchecked (`drawElements.o` +0x18-0x30: below the base it silently draws nothing):
    checked once at allocation (logged; failure = the C3D side off for the run) — and the same
    physical-range check for **both halves of the vertex ring** (the attribute offset register is 28
    bits too; New 3DS FCRAM above `0x28000000` lies ≥ `0x10000000` past the base), and
    `BufInfo_Add`'s return checked every frame (`buffers.o` +0x18-0x38: below the base it returns −2
    with a half-built slot; < 0 → the C3D side off for the run; round 2, critic finding 3).
    Halves alternate per view frame (M1 retires every view frame before returning, so one half would
    do; the second is M6's frame in flight, 9.2). Rings, the world program and the staging are
    allocated at the first C3D view frame (diff runs only) and freed by `draw_free()` / `view_free()`
    in `release_and_fini()` after `C3D_Fini`.
  - *The alpha-keeping readback* (8.1; carried from M0). Side B is converted with
    `rgba_to_argb_keep(rgba) = (rgba >> 8) | (rgba << 24)` (`core/calibrate.h`, beside M0's forcing
    `rgba_to_argb`), so B's coverage is alpha ≠ 0 and `Coverage::TopByte` applies to both sides. The
    calibration (`readback_cal`) gains an **alpha check**: a fifth 16x16 quad at the top centre
    (`x ∈ [w/2 − 8, w/2 + 8)`, `y ∈ [0, 16)`), ARGB `0x80206090`, drawn with blend `ONE, ZERO`; the
    calibration converts with the keeping form; the full-image check expects alpha 0xFF everywhere
    except that quad (alpha 0x80, RGB as drawn), placed by the detected orientation (rows flipped →
    bottom centre). The calibration line and summary gain `alpha ok|NO` / `alpha ok N`; all of M0's
    other figures are unchanged, so the M0 configs re-run as they are.
  - *GX queue* (2.4, the M0 nit). `GxQueue` charges the flushing split **exactly 1** (it is one queue
    entry; the following transfer then finds an empty buffer and costs 1): a C3D view frame takes
    clear 1 + split 1 + transfer 1 = **3** entries; `C3D_FrameEnd` adds **none** when the buffer is
    empty, as it is after the in-frame transfer (`renderqueue.o` `C3D_FrameEnd`+0x134-0x144:
    `C3Di_SplitFrame` returns 0), so `GxQueue::end_frame`'s `kFrameEndEntries` 1 is margin there
    (kept: conservative); the retirement pair 0-1. Host test updated. Hence `gxq_budget 4` must pass
    with **no** discard and `gxq_budget 2` must refuse the readback (discard `gx`, SW for that check)
    without a crash; both are run.
  - *Reserves and the command buffer, measured* (2.4; revise 1, critic finding 1). `C3D_FrameEnd`
    zeroes `gpuCmdBuf`, `gpuCmdBufSize` and `gpuCmdBufOffset` (`renderqueue.o`
    `C3D_FrameEnd`+0x44-0x5c), and `C3D_GetCmdBufUsage()` returns a context field written only in
    `C3Di_SplitFrame` (+0x3c-0x6c: the end of the last split relative to the buffer start), so every
    measurement is taken **inside the frame**, where the globals are valid. The write position `P =
    gpuCmdBuf + gpuCmdBufOffset` (a split advances `gpuCmdBuf`, 0.3, so `P` only grows within a frame)
    is read: right after `frame_begin()` returned (`P0`, the rewound buffer start); before and after
    every draw call (words per draw, citro3d's lazy state flush included); after the last draw and
    after the flushing `C3D_FrameSplit` + the transfer (the readback's words); and **right before**
    `C3D_FrameEnd` in `frame_end()` (`P1`; `P1 − P0` = the frame's words before its end, per frame
    kind: view frames and present frames, the latter including the shader re-upload after a view
    frame). `C3D_FrameEnd`'s own words matter only where the buffer is not empty at it (the discard
    path, and presents that drew): they are `lroundf(C3D_GetCmdBufUsage() × size) − (P1 − P0)` read
    **after** `C3D_FrameEnd`, **only if `gpuCmdBufOffset != 0` at `P1`** (round 2, critic finding 2:
    with an empty buffer `C3D_FrameEnd` does not split, `renderqueue.o` +0x13c-0x144, and the usage is
    a stale value from an earlier frame; then 0 is recorded: two_frame's frame U, gate-closed and
    `close_frame` presents, calibrate presents, a view frame discarded before its first draw); `size`
    = the `C3D_Init` size in words; the validity flag is an input of the host-tested `core/` function. End reserve = max(readback words, `C3D_FrameEnd`'s words on a discard path).
    The arithmetic is a pure `core/` function, host-tested. Max of each, plus the max
    `C3D_GetCmdBufUsage()` read after each `C3D_FrameEnd`, in a `[c3d] cmdbuf …` line once a second
    (with draws, polygons, vertices, indices, ring high-water per half, discards and skips by reason,
    `prog_restore_mismatch`) and at release/exit. `kDrawReserveWords` / `kEndReserveWords` start at
    1 024 and are set to **2 × the measured worst case** over the acceptance runs; `C3D_Init` stays
    512 KiB unless the max usage asks for more (deterministic: authoritative in Azahar).
    **Discard tests** (critic finding 7): `cmdbuf_kib 32` must run without a crash (recorded whether
    or not it discards: a walls-only view frame may need well under 8 192 words); the `cmdbuf` discard
    itself is forced deterministically by a test key **`reserve_extra N`** (0..1 000 000 words, default
    0, added to the reserve test), run at a value that refuses the first draw of every view frame and
    at one that refuses mid-frame (after some draws: exercises the discard-path ring flush).
  - *Ring sizes, measured first* (carried from M0). Before the defaults are fixed: `mode null` over
    the menu demo (script 12; done 2026-10-27 on the M0 build: 4 580 frames, V max 374, I max 582,
    below the bench's 387 / 597) and over the film (below), added to M0's three bench views; defaults =
    2 × the max `V` / `I` over all five, rounded up to 1 024, per half. Ring high-water logged as
    above; a `ring_vertices 64` run must discard `ring` and fall back without a crash.
  - *Diff line and tolerance* (8.3, walls flat). Every DIFF line gains ` band=N` (A's edge-band
    pixels, `core/diff.cpp`) and, on C3D checks, ` b=c3d excluded=N`. Walls flat (8.3): **coverage
    only**, the colour figures of a C3D walls check are meaningless (flat colour vs texture) and are
    not judged. Pass: `cov_mismatch` non-edge **0** in every check, and edge mismatches **≤ 2 % of
    the band** summed over the run's walls checks (per-check worst ratio logged, informative).
    `c3d-diff.py log LOG --coverage CLASS[,CLASS] --band-pct 2 --min-checks N` checks exactly that,
    also failing on a missing summary, a dropped frame, a discarded check of a listed class, a
    `sentinel` > 0, or fewer than N **compared** checks of a listed class (so skipped checks, e.g.
    citro3d not up yet when a film reaches its first diff frames, can neither fail nor pass a run
    silently; `skipped` lines are tabulated by reason); `--expect-zero` (M0's SW-vs-SW test) keeps
    its meaning and skips `b=c3d` lines.
  - *Film* (`debug-c3d film PATH`). `a1_3ds::boot_begin(int&, char**&)` (`a1_3ds_platform.cpp`:1034,
    port code, called before `shell_options.parse`, `main.cpp`:15, 45) asks a new
    `a1_c3d_boot_film()` (`a1_c3d_port.h`, defined in `back/context.cpp`; it loads `debug-c3d`, which
    so happens earlier than today, after `boot_begin` registers its `atexit` handlers, so the summary
    handler still runs before `close_outputs`) and, if the file exists, appends its absolute path to
    argv (static storage), under `#ifdef A1_RENDER_C3D`. `shell_options.parse` puts an existing
    non-option path into `shell_options.files` (`shell_options.cpp`:176-189) and `main.cpp`:60-66
    calls `handle_open_document()` on it (`shell.cpp`:190, `_typecode_film` by the `.filA`
    extension, `FileHandler.cpp`:584, 606-617), i.e. the upstream command-line film path, as
    `replay_film_test.cpp`:68. A missing file is logged (`[c3d] film: … not found`) and **not**
    appended (`shell_options` would `exit(1)` on an unknown argument). **To the card:**
    `run-azahar.sh --film FILE` copies FILE to `sdmc:/3ds/alephone/c3d-film.filA` (removed
    otherwise) and the configs say `film c3d-film.filA`; on the console `sd-ftp.sh IP put`. Films
    must match the scenario's maps (the Tooncinator films are Marathon 2's). First film:
    `M2 L01 Waterloo Waterpark.46857.filA`; 200 diff frames at `diff 2`.
  - *Zones* (`a1_3ds_profile.h`, `profile_3ds.cpp`, `perf-table.py`): new **`c3d_draw`** (the C3D
    side's replay through `C3dRasterizer` and the back end's draw submission, ring writes and flush;
    nested in `c3d_diff`); `c3d_readback` and `c3d_wait` as M0. Diff frames are not benchmark frames.
  - *Configs* (`scripts/3ds/tests/c3d/`): `m1-diff-walls.cfg` (`renderer c3d`, `diff 1`,
    `diff_frames 20`, `classes walls`, `dump 1`), `m1-film.cfg` (the same with `diff 2`,
    `diff_frames 200`, `film c3d-film.filA`), `m1-null-ring.cfg` (`mode null`, with `--script` 12 or
    `film`), `m1-cmdbuf32.cfg`, `m1-gxq4.cfg`, `m1-gxq2.cfg`, `m1-ring-small.cfg`,
    `m1-reserve-first.cfg` and `m1-reserve-mid.cfg` (`reserve_extra`, values set from the measured
    words: the first refuses the first draw, the second a later one).
  - *Host tests* (8.4). **w** (revise 1, critic finding 3; a tolerance change, so recorded in the
    Review log and reported to the user): SW's own `world_x` is not exact. After the shifts at
    `scottish_textures.cpp`:933-939 the numerator is cut to [16 384, 32 767] and the denominator
    truncated with it, so `tx` carries a relative error up to about `t/16 384` before cancellation;
    the critic's transliteration put 90 of 46 267 random samples outside `max(1e-3·sw, 2)` (worst 9 %)
    at `|o| ≤ 12 000`, `t ≤ 8`, and 23 % at wider ranges. So three gates over generated walls and
    columns (`wx` 160-320 for a 400-px view, `|o| ≤ 40 000`, `t ≤ 64`), **restricted to SW's domain**
    (round 2, critic finding 1): `scottish_textures.cpp`:954 computes `int32(1LL·tx·v.i) >> 16`, which
    wraps once `|tx·v.i| ≥ 2³¹`, so samples with `|tx·v.i| ≥ 2³¹` or `|o.x + t·v.i| > 32 767` are
    excluded and **counted** (printed). The gates compare the **unclamped** `w`; 2.3's clamp to
    `[16, 32767]` is tested on its own. (a) the **accuracy gate**: the float `w` vs a double-precision
    evaluation of 2.2's formula, `|w_f − w_d| ≤ 1e-4·|w_d| + 0.05` everywhere (the absolute term
    covers cancellation near `w ≈ 0`, below the clamp); (b) the kickoff's **rel ≤ 1e-3 vs SW `world_x`** on every sample where SW's own
    quantisation bound `q = 1 + |v.i|·(|t|·(1/|n_r| + 1/|d_r|) + 2⁻¹⁶)` (`n_r`, `d_r` the shifted
    numerator and denominator of :933-944) is ≤ 0.5e-3 · sw, i.e. where SW is precise enough to judge;
    (c) `|w − sw| ≤ q + |w_f − w_d|` on every other sample (the float error budgeted). The test prints how many samples each gate judged.
    **Coverage model** (critic finding 9): on generated convex polygons (3-8 vertices, integer, in a
    400x240 view, both windings, degenerate ones included) the SW coverage transliterated in full
    (acceptance tests :449-505, both `build_y_table` walks, rows `[y0(x), y1(x))` per column of
    `low_level_textures.h`:322-326) vs `core/geometry`'s acceptance test plus an ideal pixel-centre
    rasterisation of the fan: identical except pixels whose centre lies exactly on an edge (counted
    apart; the PICA tie rule is UNVERIFIED). Also: fan indices; the batching key; `rgba_to_argb_keep`;
    the calibration with the alpha quad (all four orientations); `GxQueue` with the split at 1 and a
    runtime budget; the new keys including `film` with spaces and a `#`, `reserve_extra`; the
    command-buffer arithmetic of the measurement bullet.
  - *Accept* (Azahar, non-authoritative except the deterministic counts): `m1-diff-walls.cfg` on
    L0/L6/L20 (perf runs, `--min-checks 20`) and `m1-film.cfg` (`--min-checks 200`): 0 non-edge,
    ≤ 2 % band, 0 discards, `sentinel` 0, 0 ring overflows (`c3d-diff.py --coverage walls`); the
    `[c3d] cmdbuf` maxima recorded and the reserves set to 2 ×; `prog_restore_mismatch` 0;
    `cmdbuf_kib 32` (no crash; discards recorded), `gxq_budget 4` (no discard), `gxq_budget 2`,
    `ring_vertices 64` and both `reserve_extra` configs (discards of the expected reason, SW fallback,
    no crash, no guest fault); M0's two calibration configs
    30/30 with `alpha ok 30`; P1 scripts 07, 08, 13, 14, 22 as in M0; the OFF builds' `objdump -d`
    unchanged and `nm` clean.
  - *Implementation notes (2026-10-27, during the build).* (a) **Tie pixels join the band**
    (a tolerance-definition change, Review log "M1 implementation, tie pixels"; reported to the
    user): the coverage-rule bullet above said exact ties lie "inside the edge band". That holds on
    the outline of A's coverage but not on an edge two drawn polygons share: the film run's 1-px
    sliver walls put a pixel centre exactly on a top edge with neighbouring walls all around it, SW
    covered it (its walk includes top-edge ties) and Azahar's GPU did not, so it counted as
    non-edge. A sub-pixel bias would encode Azahar's tie rule, unverified on PICA, and would not
    survive the rasteriser's sub-pixel snap (Azahar keeps 1/16 px). So the C3D front end marks the
    **tie pixels** of every polygon it draws (centre exactly on an edge: an exact integer test,
    `core/geometry`, host-tested against the coverage model's tie count) in a per-check mask;
    `core/diff.cpp` counts a tie pixel as band, and the C3D DIFF line reports ` ties=N
    tie_mismatch=M` (not a failure condition; the console run reads PICA's rule from it).
    (b) The front end is `front/c3d_rasterizer.{h,cpp}`, not `rasterizer.*`: on macOS's
    case-insensitive file system `rasterizer.h` shadowed the engine's `Rasterizer.h` through the
    `front/` include path. (c) The embedded shader header is `a1_c3d_world_shaders_shbin.h` (the
    toolchain names it after the shader library, as the present's `a1_c3d_shaders_shbin.h`).
    (d) `c3d_only` drops only the passed class's primitives that `c3d_supports` rejects (transparent
    walls); other classes still reach the C3D side and discard as `unsupported`.
  - *Not in M1:* textures, floors, sprites, play-mode C3D frames, the "10 discards in a row" rule
    (play mode, M4), any engine edit.

### M2 — textured walls and floors (full bright), texture cache v1
- **Status (2026-11-02): M2 done** (tag `port-stage10-M2`; `PERF.md` "Stage 10 M2", Azahar — non-authoritative;
  commits `1c3b9c6f5` parts 6-7, `38ca4868e` review fixes). The tiler's row flip is confirmed (orientation
  run: walls/floors within 99.68 % / 99.85 % with the flip, 55.75 % / 59.79 % without); `uv_half 1`
  (`δ = −½`) wins (`uv_half 0`: 45-82 % within) and stays the default; L0/L6/L20 and the film pass under both
  Azahar backends (0 non-edge coverage mismatches, within 99.1-99.85 %); the film's sliver was a non-convex
  polygon (logged: `(200,158) (290,139) (399,142) (400,142) (400,154)`) and is drawn as SW's spans; draw
  reserve 536 (first textured draw 268 words). **One recorded exception (user decision 2026-11-02, Review
  log "M2 acceptance, load test colour"):** the load test's level-1 floors reach 97.78 % within (a minified
  ceiling, host-predicted). Not reached: ≥ 16 conversions in one view frame (at most 10 on these levels).
  **Console short check passed (2026-11-03,** `PERF.md` "Stage 10 M2 — console, short check"): L0 textured
  diff and deferred promotions (`gxq_budget 4`, `promo 11/10`) exit 0, 0 non-edge, `sentinel` 0, 0 discards;
  every check at a given coverage identical whatever the texture residency; command words identical to
  Azahar. The conversion flush and the TextureCopy promotion are verified on hardware.
- *Earlier:* **Status (2026-11-02): session 1 done** (parts 1-5; `PERF.md` "Stage 10 M2 session 1"; commits
  `c28261369`..`686e5cd01`; critic on the code: APPROVE, six minors, five fixed, the sixth = the OFF-build
  record in `PATCHES.md` 46). Interim run (flat, both Azahar backends) passes except 13 film floor pixels:
  a non-convex 1-row sliver (fan vs SW's chain fill; UNVERIFIED cause) — session 2 draws non-convex
  polygons as SW's row spans. **The "M1 console check, short version" passed on the console 2026-11-02**,
  so session 2 (parts 6-7 and the acceptance) may write the upload code. Tiling: tex3ds does not flip;
  source row j → memory row W−1−j (verified byte-for-byte; the sampling orientation is part 7's first run).
- *Earlier:* **Next (user decision 2026-10-27):** kickoff `KICKOFF_M2.md`. The upload code is written only after
  `NEXT-SPRINT.md` "M1 console check, short version" has passed on the console.
- Add: `core/tex_convert.cpp`, `core/tiling.cpp`, `back/texcache.cpp` (3.3);
  floors/ceilings/liquids, transparent walls, `_SCALE_2X/4X`; hooks 2a/2b
  (`PATCHES.md` 46).
- Accept: host tests 3.3 incl. tex3ds; diff `walls,floors` `fullbright 1`:
  ≥ 98% of non-edge pixels Δ ≤ 2, mean ≤ 1.0, under both Azahar backends
  (8.2); UV offset recorded; load test (`15-load-test.txt`) logs deferred
  flushes and diffs after the level change pass; level start with ≥ 16 new
  textures in one frame passes with queue high-water ≤ 24; `gxq_budget 4`
  run passes (promotions deferred).
- **M2 specification (revision of 2026-10-31, before code; Review log "M2 pre-implementation").**
  Where the lines above, 2.2-2.3, 3.2-3.3 and the code disagree, this block rules. Citations are the
  code as of `d4dd81eb3`. M2 runs **only on diff frames** (side B of a C3D check), as M1; play frames stay
  SW until M4. **Two sessions** (kickoff `KICKOFF_M2.md`): the "M1 console check, short version" has not
  run, so session 1 builds parts 1-5 below and stops; **part 6 (the upload code: the GPU side of
  `back/texcache.cpp`, linear textures, their flushes, the VRAM promotions) is written only after that
  check passes**, then part 7 and the acceptance. The M2 commit and tag come at the end of session 2;
  session 1 commits its parts as `stage10: M2 part N <summary>` (no tag).
  - *Parts and order.* 1 `core/tex_convert.*` + `core/tiling.*` (+ tex3ds host test); 2 `core/texcache.*`
    (the pure cache: keys, generations, pools, LRU, promotion queue, deferred frees); 3 hooks 2a/2b
    (`PATCHES.md` 46) and `a1_c3d::collections_changed()`; 4 the front end (floors, texcoords, `_SCALE`,
    `uv_half`, the M2 support predicate) and its host tests; 5 the harness (keys, `diff_tol`, the
    sentinel alpha, `c3d-diff.py --colour`, configs) and an **interim Azahar run with `textures 0`**
    (flat colour, coverage only: floors' coverage and M1's walls, both Azahar backends); 6 the upload code;
    7 textured view frames (bind/restore, TEV, alpha test) and the acceptance.
  - *What C3D draws in M2.* Class **walls** (`texture_vertical_polygon`, `_textured_transfer`) and class
    **floors** (`texture_horizontal_polygon`, `_textured_transfer`: floors, ceilings, liquids), with a
    **supported bitmap**: `bytes_per_row != NONE` (no RLE: sprites are M4), `width == height`, a power of
    two in [128, **512**] (`TEXBITS_DISPATCH`, `low_level_textures.h`:58-63, picks 7 for any non-square
    bitmap and would read a 128-texel square out of it; 3.2's POT padding is never needed for these; a
    1024² RGBA8 texture is 4 096 KiB, above `vram_cap` and all of `linear_cap`, so 1024 is unsupported in
    M2, round 1 finding 5), and **without `_SHADELESS_BIT`** (infravision is M3). With
    `software_alpha_blending` ≠ off SW blends walls and floors (`scottish_textures.cpp`:386-399, 610-627):
    1.5 already refuses the renderer then; on diff frames a C3D check is **skipped** with a new reason
    `pref` (round 1 finding 6). `c3d_supports()` becomes this predicate, pure in
    `core/geometry.h` (inputs: entry, transfer, width, height, RLE, shadeless; plus, with `textures 0`,
    the M1 rule "no `_TRANSPARENT_BIT` wall"). `FilterRasterizer`'s `c3d_only` keeps its meaning
    (`filter.cpp`:42-46: drops, on both sides, the passed class's primitives that `c3d_supports` rejects,
    counted as `excluded`), so with textures on a grate wall is drawn, and a shadeless or odd-sized
    primitive is excluded instead of discarding the check. Everything else reaching `C3dRasterizer` is
    unsupported (discard), as M1.
  - *Transparent texels: per use, not per bitmap* (a 3.2 correction). SW skips texel 0 **only on walls**
    whose bitmap has `_TRANSPARENT_BIT` (`scottish_textures.cpp` vertical path → `write_pixel<…,
    check_transparent>`, `low_level_textures.h`:158-160, 329); the floor mapper always writes
    (`write_pixel<T, sw_alpha_blend, false>`, :222), so the same bitmap on a floor is opaque. The
    conversion kind therefore carries the keying: **`normal`** (A 255, or 128 self-luminous) and
    **`normal_keyed`** (the same, A 0 for index 0), used for a `_TRANSPARENT_BIT` bitmap on a wall only;
    a floor always uses `normal`. Both can coexist in the cache (key below). `shadeless`/`shadeless_keyed`
    and `mask` (3.2) are implemented and host-tested in `core/tex_convert` now but not drawn before M3/M4.
    The alpha test (2.4: `GEQUAL 128`, set by M1's `view_begin`, `draw.cpp`:381) drops A 0 and keeps 128
    and 255, so M1's exclusion of transparent walls ends **when textures are on**.
  - *Conversion* (`core/tex_convert.*`, 3.2). Input: a bitmap view (POD: width, height, `row_addresses`
    as `const uint8_t* const*`, the RLE flag), the 32-bit shading tables (`const uint32_t*`, at least
    256 × 256 entries: `number_of_shading_tables` is 256 at 32 bits, `shapes.cpp`:1569) and the kind.
    Output: `W × W` RGBA8 texels **already tiled** into a caller buffer (the linear texture in part 6, a
    vector on the host). Texel `(s, j)` (GPU column `s`, GPU row `j`) = byte `row_addresses[j][s]`
    (memory order: for a column-order wall bitmap `j` is the column, `s` the row in it; for the floor
    mapper's linear read `base[row·W + col]`, `low_level_textures.h`:222, `j` = row, `s` = col — the same
    bytes when the rows are contiguous, which the loader guarantees for these bitmaps; the converter reads
    through `row_addresses` as the wall mapper does). Colour of index `i`, `m = 0x00FFFFFF`:
    RGB = `table[256·255 + i] & m` (the top table, which `fullbright` selects: `calculate_shading_table`
    with `ambient = FIXED_ONE` gives `table_index = CEILING(…, 255) = 255`, `scottish_textures.cpp`:142-165;
    self-luminous colours reach the same top entry). A per 3.2. **Texel word:** the GPU's RGBA8 texel as
    a little-endian `u32` = `R<<24 | G<<16 | B<<8 | A` (bytes A, B, G, R): confirmed against tex3ds's
    output by the critic (round 1 finding 4); the host test keeps checking it.
  - *Tiling* (`core/tiling.*`). 8 × 8 tiles, Morton (Z) order inside a tile, tiles in row order; the
    function maps a memory row `m` and column `s` → texel index. **Orientation (round 1 finding 1):**
    tex3ds does **not** flip — its output stores PNG row 0 in the first memory row, tile rows top-down —
    and its `.t3x` subtexture record gives that row `t = 1.0` (top 0x400, bottom 0), consistent with
    `textured_cube/source/main.c`:21-23 (bottom vertices `t = 0`, image upright). So **`t = 0` samples the
    last memory row**: the tiler writes **source row `j` into memory row `W − 1 − j`**, and texcoord
    `t = j/W` still addresses `row_addresses[j]` (the formulas below are unchanged). **Host test:** a
    `W × W` image (W = 8, 16, 128) with a unique RGBA value per texel and **A ≠ 0 everywhere** (tex3ds
    zeroes the RGB of A = 0 texels), written as a PNG by a minimal stored-deflate writer in the test (no
    library); `/opt/devkitpro/tools/bin/tex3ds -f rgba8 -z none -r -o out.bin in.png` (`-r`: raw data,
    no `.t3x` header; `-z none` still writes a 4-byte compression header, type 0x00 + 24-bit size, which
    the test parses and checks); the PNG's row `r` is source row `W − 1 − r`, and our tiled output must
    equal tex3ds's **byte for byte**. This fixes the tiling and the byte order only, **never the sampling
    orientation**, which part 7 checks first in Azahar (below). The test is skipped (printed, not failed)
    when tex3ds is absent; on this machine it must run.
  - *Cache key and value* (`core/texcache.*`, pure; 3.3). Key `(bitmap pointer, shading-tables pointer,
    kind, generation)`. Value: a fixed-capacity slot array (**stable addresses**: citro3d keeps the
    `C3D_Tex*` given to `C3D_TexBind` and reads the struct at the next draw's state flush —
    `texture.o` `C3D_TexBind`+0x24-0x38 stores the pointer at context +0x118 + 4·unit and sets dirty
    bit 23 + unit; so slots never move): W, bytes, pool (`Linear`, `Vram`), `last_used_frame`,
    `promote_pending`, an opaque back-end handle (the `C3D_Tex`, part 6). 256 slots (a level uses tens of
    textures; full → evict, then discard `tex`). The core part is host-tested with a fake allocator.
  - *Generations and the deferred flush* (hooks 2a/2b). `collections_changed()` only increments the
    cache's generation and counts the call (logged `[c3d] tex: collections changed (gen N, M entries
    retire)`); lookups match only the current generation, so a bitmap reallocated at the same address can
    never hit an old entry; the old generation's entries are freed **after the next `frame_begin()`
    returned** (retired, 0.3), with the deferred-free list. At `release_and_fini()` (after `C3D_Fini`)
    every slot is freed. Main thread only (`load_collections` and `unload_all_collections` run on it).
  - *Hooks 2a/2b* (`shapes.cpp`; `PATCHES.md` 46; `#if defined(A1_PLATFORM_3DS) &&
    defined(A1_RENDER_C3D)`; `ppcheck.sh` on `shapes.cpp`, IDENTICAL expected). 2a: **at the end of
    `load_collections()`** (after the last `time_mark`, `shapes.cpp`:1508-1510), not right after
    `update_color_environment()` (1.3's place): `SW_Texture_Extras::Load` (:1500-1506) runs after it, and
    nothing draws in between, so the end covers every change the function makes. 2b: the end of
    `unload_all_collections()` (:1179-1193). Both call `a1_c3d::collections_changed()` (`a1_c3d.h`, the
    header hook 1 already includes in `render.cpp`, `PATCHES.md` 45). Callers: `unload_all_collections`
    from `shell_misc.cpp`:86, `interface.cpp`:3009, `screen.cpp`:327; `load_collections` from the level
    loader, `Misc/PlayerImage_sdl.cpp`:375 and `RenderOther/images.cpp`:1308 (round 1 finding 9); the hooks
    sit inside the functions, so every caller is covered.
  - *Pools, caps, allocation* (9.1/9.2; a 3.3 simplification). Each texture is its own citro3d
    allocation: `C3D_TexInit` (linear) / `C3D_TexInitVRAM` (`texture.h`:83-86, 98-101), freed with
    `C3D_TexDelete`. The pools are **accounting caps**: linear 4 096 KiB (`linear_cap`), VRAM 3 584 KiB
    (`vram_cap`). 3.3's size-class slabs are **deferred to M5** (eviction): every M2 texture is 64 KiB
    (128² RGBA8) or a multiple, so fragmentation is measured first — a failed allocation with
    `cap − used ≥ request` is logged `alloc failed with free >= request` and counted. Before an allocation
    that would pass the cap, LRU eviction of slots with `last_used_frame` < the current view frame (M6's
    in-flight rule: never the current or an unretired frame), **never a slot whose `C3D_SyncTextureCopy` was issued
    in this frame** (its linear source and VRAM destination are in use; round 1 finding 7, narrowed in
    round 2): a slot merely *waiting* for promotion may be evicted and is dropped from the promotion
    queue, and with `promote_max 0` no slot is ever marked `promote_pending`; nothing evictable →
    discard `tex`.
  - *Upload model* (3.3, part 6). Miss → convert **inside the frame** (after `frame_begin()` returned,
    during the replay, before the first draw that uses it) directly into a new linear texture
    (`C3D_Tex2DGetImagePtr`, `texture.h`:118-121), then **one ranged `GSPGPU_FlushDataCache` of exactly
    its bytes** right after the conversion (before any split; the readback's flushing split, M1, comes
    later in the frame), sampled from linear in this frame. **Promotion** to VRAM in a **later** view
    frame, in one batch after `frame_begin()` and **before the clear** (both need an empty command buffer
    to cost 1 GX entry each, 2.4): ≤ `promote_max` (default 8, 0 = never: linear sampling only, for 10
    item 5) per frame, oldest first, each a `gx_op` in-frame `C3D_SyncTextureCopy` raw copy of the tiled
    bytes (its exact arguments read off `texture.o` `C3D_TexLoadImage`'s VRAM branch when written, rule 7);
    the VRAM copy is bound from the **next** view frame; the linear copy goes on the deferred-free list.
    A promotion refused by the GX budget is **deferred, not a discard**: promotions take an entry only if
    `used + 1 + kViewReserveEntries ≤ budget`, `kViewReserveEntries = 3` (clear, flushing split, readback
    transfer: the frame's own entries, M1 "GX queue"), so `gxq_budget 4` promotes one per frame and never
    refuses the readback. Soft budget 512 KiB converted per view frame, counted, not enforced.
  - *Texcoords* (2.2, `core/geometry`, host-tested). Every vertex keeps its integer position `(X, Y)`
    (coverage, M1). Its attributes are evaluated at **`(ex, ey) = (X + δ, Y + δ)`**, `δ = −½` (`uv_half 1`)
    or 0 (`uv_half 0`): SW samples the texel at the pixel's integer corner (walls: `screen_x = x0 − hw`
    per column and `screen_y0 = hh − y0 + dtp` per row, `scottish_textures.cpp`:902, 926; floors:
    `screen_x = x0 − hw`, `screen_y = hh − y0 + dtp`, :1043-1044), the GPU at its centre. **Both the
    texcoords and the perspective weight `w` are evaluated at `(ex, ey)`** (a 2.2 correction): clip × w
    leaves NDC unchanged, so `w` only weights the interpolation, and only a pair `(u, w)` taken from the
    same projective map at the same point keeps `u/w` affine — shifting `u` alone would bend it.
    *Walls* (`polygon_definition` `origin o`, `vector v`, `RenderRasterize.cpp`:540-548): `s = ex − hw`,
    `t = (wx·o.y − s·o.x)/(s·v.i − wx·v.j)` (denominator 0 → 1, as M1's `wall_w`), `x = |o.x + t·v.i|`,
    `w = x`, GPU texcoord **`(s_gpu, t_gpu) = ((x·(hh + dtp − ey) − wy·o.z)/(wy·v.k), t)`** (row in the
    column, column; `scottish_textures.cpp`:921-929, 958-965). *Floors* (`o` = view origin + surface
    origin, z = adjusted height, `RenderRasterize.cpp`:450-452; yaw `θ` with `cos_t, sin_t` =
    `cosine_table/sine_table[view->yaw]`, 1 024 = 1.0): `sx' = ex − hw`, `sy' = hh − ey + dtp` (0 → 1, :1044),
    **sign guard (round 1 finding 3):** a vertex on or past the horizon row would give `sy'` the wrong
    sign for its polygon (eye near the surface's height), flipping its `u/w`; so if `sy'·o.z ≤ 0`, `sy' =
    ½·sign(o.z)` (`o.z = 0`: the polygon projects onto the horizon row, so `accept_polygon(Horizontal)`
    already skips it as `Degenerate`; the front end checks acceptance before the texcoords, and the
    texcoord code treats `o.z = 0` as a skip counted `degenerate`, never as unsupported; round 2);
    `S = 1 024 · 2^scale`, `scale = flags & _SCALE_BITS` (:1075), **`s_gpu = (o.z·(wy·cos_t − sx'·sin_t)/sy'
    + 1 024·o.x)/(1 024·S)`**, **`t_gpu = (o.z·(sx'·cos_t + wy·sin_t)/sy' + 1 024·o.y)/(1 024·S)`** (the
    reals of :1051-1080 with the `<< bits` wrap at 2³²), `w = |o.z·wy/sy'|`. Per polygon `floor(min)` of
    each texcoord over its vertices is subtracted (`GPU_REPEAT`, integer shifts are exact). **Clamps**
    (a 2.3 correction): the perspective weight is clamped to **[16, 2²⁴]**, not `[16, 32767]` —
    `SHRT_MAX` is SW's clamp of the *shading* depth (:991, 1091; M3's texcoord1 keeps it), and a floor
    vertex near the horizon (`sy'` small) has `w` far above 32 767, where the old clamp would bend the
    texture. M1's walls are unaffected (`|w| ≤ 32 767` in practice; coverage does not depend on `w`).
    **`uv_half` default 1** until the acceptance measures both (the winner becomes the default and is
    recorded); the host test predicts it.
  - *Floor coverage rule* (the M1 question for rows). The horizontal mapper walks rows from the highest
    vertex (`scottish_textures.cpp`:226-300; left table counter-clockwise, right table clockwise) with
    `build_x_table` (:1173-1223), which records, for row `r`, the x of the edge sampled at the **row
    centre** with pixel-centre rounding (x-dominant: `k_r` = smallest integer `> s(r+½) − ½`; y-dominant:
    the same midpoint form), for both signs of dx without a reversed walk (worked out: `dx < 0` gives the
    same pixel-centre rule); rows write `[x0(y), x1(y))` (:222, `count = x1 − x0`, nothing if negative).
    So floors follow the pixel-centre rule as walls do, and the tie set (a centre exactly on an edge) is
    the same geometric set: **`tie_pixels` serves floors unchanged**. Acceptance mirrored from SW: a vertex
    outside `[0, w] × [0, h]` (:229-235); `min y == max y` (:240, `highest == lowest`); a negative dy on
    either walk (`build_x_table` returns NULL, :1222); non-positive area (no row written). `accept_polygon`
    gains an orientation argument (`Vertical` = M1's, `Horizontal` = this); the host coverage model gains
    the transliterated horizontal walk and must match `accept_polygon(Horizontal)` + pixel-centre
    rasterisation except ties, as M1's.
  - *Texture binding and its restore* (the M1 lesson). `present.cpp`:453 binds `C3D_TexBind(0, &s_tex)`
    once at init and never again; citro3d has no getter. So `-Wl,--wrap=C3D_TexBind` (beside the
    `C3D_BindProgram` wrap, `CMakeLists.txt`:373) records the last `(unit 0, pointer)` bound by anyone
    else in `back/context.cpp`; `back/draw.cpp` binds only through `__real_C3D_TexBind` (its textures and
    the restore), so the record keeps the present's; after the restore it checks the record against what
    it re-bound and counts `tex_restore_mismatch` (in the `[c3d] cmdbuf` line; must be 0). The restore
    sits beside M1's program/AttrInfo/BufInfo restore (`draw.cpp` `restore_state`, :251-259). Proof:
    `m1-present-diff.cfg`-style runs with textures on, `present_diff` 0 (an `m2-present-diff.cfg`).
  - *View frame with textures* (part 7). TEV stage 0: `GPU_TEXTURE0`, `GPU_REPLACE`, RGB and alpha
    (`texenv.h`; the present sets its own stage 0 before each draw, `present.cpp`:311-326). Per merged draw
    (the key already contains texture and tables, `DrawKey`): look up/convert, `__real_C3D_TexBind(0,
    tex)` when it changes; `C3D_TexSetFilter(GPU_NEAREST, GPU_NEAREST)`, `C3D_TexSetWrap(GPU_REPEAT,
    GPU_REPEAT)` at creation. `textures 0` keeps M1's flat colour path (no bind, coverage only).
    **Orientation check first** (round 1 finding 1): before any colour-metric run, one `m2-orient.cfg` run
    (`classes walls,floors`, `fullbright 1`, `dump 1`, 3 checks on L0) whose dumps are compared by eye and by
    the metric with the tiler's row order flipped as a test build define (`A1_C3D_TILE_NOFLIP`): the right
    order must win clearly (mean Δ far apart); recorded.
  - *Sentinel* (M1's, `draw.cpp`:349-351). Textured pixels can have any RGB, so a texel could equal
    `cal_sentinel_rgba(k)` and count as `sentinel`. View frames therefore fill the staging with the
    sentinel's RGB and **alpha 0x01**, a value no M2/M3 draw writes (texture A ∈ {0, 128, 255}, blend
    `ONE, ZERO`, clear 0). M0's calibration keeps its own sentinel unchanged.
  - *GX queue and command buffer.* Per view frame: promotions ≤ 8 + clear 1 + split 1 + readback 1 =
    ≤ 11, + `C3D_FrameEnd` margin 1 (2.4's 24 budget holds; logged high-water). Texture binds add state
    words to the draw after a bind: the reserves (`kDrawReserveWords` 508, `kEndReserveWords` 20,
    `geometry.h`) are re-measured over the M2 acceptance runs and set to 2 × the worst (deterministic, so
    Azahar is authoritative); the conversion and the flush issue no command words.
  - *Colour metric and tolerance* (8.3, walls/floors full bright). `diff_images` gets `tol = diff_tol`
    (key, 0..255, default 2; today `frame.cpp`:274 passes 0). Over the run, per class (walls, floors):
    coverage mismatches **outside the band ≤ 0.1 % of A's covered pixels** (summed), **≥ 98 % of common
    non-edge pixels Δ ≤ 2** and **mean ≤ 1.0** (sums over the run's checks), plus every M1 failure rule
    (missing summary, dropped frame, discard of a listed class, `sentinel` > 0, fewer than N compared
    checks). Per-check worst figures are printed (informative). **Reachability (round 1 finding 2):** SW
    is not exact (wall `ty_delta` divides by `den >> 8`, `scottish_textures.cpp`:971-979; the `tx`
    shift-downs :938-951; the floor branch truncates then multiplies, :1069-1072), so a correct GPU picks
    a neighbouring texel on some pixels, and dithered textures turn most such picks into Δ > 2. **Part 4's
    host prediction decides the gate:** if its best texel agreement (walls and floors separately) is
    under 98 %, session 1 stops and records a revised gate in the Review log (the host figure minus 1
    point, or Δ judged with a ±1-texel neighbourhood) and reports it to the user before session 2; the gate
    is never tuned against Azahar. The same holds for the 0.1 % non-edge coverage rule on grates.
    **Revised gate (user decision 2026-10-31, Review log "M2 implementation, colour gate"):** the host
    prediction came out at walls 98.30 %, floors 89.62 % exact texel agreement (`δ = −½`; 100 % within one
    texel), so colour is judged by the **neighbourhood rule**: a common non-edge pixel is *within* if its Δ
    to A's pixel **or to any of A's 8 neighbours that are covered in A** is ≤ 2 (`diff_tol`); gate **≥ 98 %
    within (neighbourhood)** per class over the run; the strict figures (Δ to A's own pixel: within % and
    mean) are still computed and printed, **mean ≤ 1.0 is judged on the neighbourhood Δ** (the minimum over
    the 3×3), the strict mean is informative. The host texcoord test gates **within one texel ≥ 99.5 %**
    (walls and floors separately) and prints the exact agreement. `c3d-diff.py log LOG --colour
    CLASS[,CLASS] --nonedge-pct 0.1 --within 98 --tol 2 --mean 1.0 --min-checks N`. Only with
    `fullbright 1` are the colour figures meaningful (M2 draws full bright); `--colour` fails a log whose
    `[c3d] debug-c3d:` line lacks `fullbright 1` or has `textures 0`.
  - *Keys* (`core/config.*` + host test): `textures 0|1` (default 1 once part 7 exists; **0 until then**:
    flat colour, coverage only, transparent walls excluded as M1), `uv_half 0|1` (default 1), `diff_tol N`
    (0..255, default 2), `vram_cap KiB` (0..4 096, default 3 584), `linear_cap KiB` (64..16 384, default
    4 096), `promote_max N` (0..8, default 8).
  - *Zones* (`a1_3ds_profile.h`): **`c3d_tex`** (conversion, its flush and the promotion issue; nested in
    `c3d_draw`) — the name 7 reserves for M0-M5's uploads, distinct from B-a's `c3d_upload`.
  - *Stats* (`[c3d] tex …` once a second and at exit): entries per pool, KiB per pool, conversions and
    KiB converted (total, max per frame, first 10 view frames after a generation change), promotions
    done/deferred, evictions, deferred frees, generation, `alloc failed with free >= request`, discards
    `tex`.
  - *Host tests* (8.4): conversion with **real-format tables** built like `shapes.cpp`'s 32-bit tables
    (normal, black normal, self-luminous incl. black/dark, alternate CLUT = another table pointer,
    tint/shadeless, global tint for `mask`), every kind incl. `normal_keyed` vs `normal` on the same
    bitmap; tiling vs tex3ds (above); the cache with a fake allocator (generation miss after
    `collections_changed`, frees only after a retire, LRU never evicting the current frame, caps,
    promotion order and the deferral at `gxq_budget 4`; the linear cap filled under `promote_max 0` and
    under `gxq_budget 4`, asserting LRU eviction and no discard); **texcoords vs the transliterated SW mappers**
    (`scottish_textures.cpp`:891-1001 walls, 1003-1097 floors incl. `_SCALE_2X/4X`): generated views and
    polygons, every covered pixel's SW texel `(col, row)` from the integer code vs the texel the GPU
    would sample at the pixel centre (perspective-correct barycentric interpolation of the vertex
    attributes in double over the fan triangle, `floor(u·W) mod W`); agreement printed for `δ = 0` and
    `δ = −½`, walls and floors separately (it predicts `uv_half`); gate (revised, see *Colour metric*):
    the better δ's agreement **within one texel ≥ 99.5 %**, exact agreement printed; the horizontal coverage model and
    tie set; the new keys.
  - *Interim run (session 1, part 5; Azahar, non-authoritative, not M2's acceptance).* With `textures 0`:
    `m2-diff-flat.cfg` (`renderer c3d`, `diff 1`, `diff_frames 20`, `classes walls,floors`) on L0/L6/L20
    and `m2-film-flat.cfg` (200 frames), under the host-GPU backend **and** the software backend:
    floors and walls coverage by M1's rule (0 non-edge outside ties, `--band-pct 2`), 0 discards; M1's
    configs unharmed.
  - *Accept* (session 2, Azahar, both backends): the kickoff's list — `m2-diff.cfg` (`classes
    walls,floors`, `fullbright 1`, `textures 1`) on L0/L6/L20 (`--min-checks 20`) and the film (200)
    pass `--colour`; `uv_half 0` vs 1 measured on the same views, winner recorded; `15-load-test.txt` with
    `diff 30` logs ≥ 1 generation change and its deferred frees, and the checks after the level change
    pass; a level start with ≥ 16 conversions in one view frame passes with KiB converted per frame
    logged, the cmdbuf words within the re-measured reserves and the linear cap respected (GX entries
    cannot exceed 8 + 3 + 1 = 12 by construction, so the high-water is logged, not a gate; round 1
    finding 8);
    `gxq_budget 4` passes with promotions deferred and 0 discards; `promote_max 0` passes (linear only);
    `vram_cap 512` runs and evicts without a discard on L0; `m2-present-diff.cfg` `present_diff` 0 and
    `tex_restore_mismatch` 0; M1's walls coverage, M0's calibration configs and P1 scripts 07, 08, 13, 14,
    22 unharmed; the OFF builds' `objdump -d` unchanged and `nm` clean.
  - *Not in M2:* lighting (M3), shadeless/infravision drawing (M3), sprites and RLE (M4), landscapes and
    static (M5), size-class slabs (M5), play-mode C3D frames (M4), conversion on another thread.

### M3 — lighting
- Add: `core/shade_lut.cpp`, texcoord1, TEV 4.2, infravision, negative
  ambient, weapon flash.
- Accept: host LUT ≤ 1 level everywhere; diff `walls,floors` lit ≥ 98% Δ ≤ 4,
  mean ≤ 1.5 on L0/L6/L20 + 2 films; infravision via the Lua plugin.
- **Console (2026-11-07): short check passed** (`PERF.md` "Stage 10 M3-M5 — console, short checks": lit walls /
  floors 99.56 % / 99.80 %, `mean_nb` 0.23 / 0.72 against Azahar's 0.21 / 0.71, command words 1304 / 282 / 10
  identical, the ×2 alpha clamps, unit 1 unbound after each lit frame with `unit1_left_bound=0`).
- **Status (2026-11-05): M3 implemented and accepted** (`PERF.md` "Stage 10 M3", Azahar —
  non-authoritative; commits `c3b4349c4`..`dd955459a`; critic: spec APPROVE round 2, code APPROVE, post-review
  change APPROVE). Host LUT worst 1 level (90.6 % exact); L0/L6/L20, both films (200 checks each), infravision
  and M2's full-bright gate pass on the host GPU, and L0/L6/L20, infravision and M2's gate pass on the software
  backend too; command words 1304 / 282 / 10, draw reserve **564**, rings **2 048 / 4 096**; unit 1 is
  **unbound after each lit frame** (Review log "M3 acceptance, unit 1 after the frame"); `alpha_x2` clamps on
  both Azahar backends. Under Azahar's **software** backend the films need `gxq_budget 3` (all linear): that
  rasteriser faults when one draw samples textures from FCRAM and VRAM together (an emulator bug, bisected;
  Review log "M3 acceptance, software-backend films"); so run, both films pass. Negative ambient was not reached in
  either film; the Azahar test forcing it (Lua plugin `Negative Light Test`, `m3-neglight.cfg`, L0, both backends)
  **passes** (2026-11-06: 604 negatively lit polygons drawn, 0 wraps; `PERF.md` "Stage 10 M3"). Console: `NEXT-SPRINT.md` "M3 console check, short version" (TEV rounding, the ×2 clamp, the unit-1
  unbinding on PICA).
- **M3 specification (revision of 2026-11-03, before code; Review log "M3 pre-implementation").**
  Where the lines above, 2.1-2.2, 4.1-4.3 and the code disagree, this block rules. Citations are the code
  as of `6533f8b01`. M3 runs **only on diff frames** (side B of a C3D check), as M1/M2; play frames stay SW
  until M4. **No engine edit.** Kickoff `KICKOFF_M3.md`.
  - *Parts and order.* 1 `core/shade_lut.*` + host tests (LUT vs SW, L8 tiling vs tex3ds, the lit colour
    prediction); 2 texcoord1 in the front end (the flash, `a < 0`, shadeless) + host tests; 3 the LUT's
    lifecycle in a new `back/shade.cpp`, TEV 4.2, unit 1, the state restore; 4 infravision (shadeless kinds
    drawn), the counters; 5 configs, the plugin, `c3d-diff.py --shading`, the second film; then the
    acceptance. One commit per piece where it builds alone (`stage10: M3 part N …`), the M3 commit + tag at
    the end.
  - *What exists already* (checked at HEAD). The vertex already carries texcoord1 (`core/geometry.h`
    `Vertex::u1, v1`, set to 0 by `front/c3d_rasterizer.cpp` `make`); the AttrInfo already loads it
    (`back/draw.cpp` `world_init`: loader 2 = `GPU_FLOAT, 2`); `shaders/world.v.pica` already outputs it
    (`mov otc1, v2`). **The vertex format and the shader do not change**; texcoord1 is a front-end change.
    The conversion kinds `Shadeless`/`ShadelessKeyed` exist and are host-tested (`core/tex_convert.h`:36);
    `normal` already writes A 128 for self-luminous colours (3.2).
  - *SW model, exactly* (`scottish_textures.cpp`:131-170, 32-bit: `shading_table_fractional_bits` 8,
    `shapes.cpp`:1579; 256 tables). `s = PIN(M − (d << 3), 0, 65536)` (`DEPTH_TO_SHADE`, :132, with
    `WORLD_FRACTIONAL_BITS` 10), `M = view->maximum_depth_intensity` = `weapon_intensity`
    (`screen.cpp`:1466; `NATURAL_LIGHT_INTENSITY` = `FIXED_ONE_HALF` by default, `player.cpp`:338, plus the
    firing intensity while it decays, `weapons.cpp`:641-654). Index = `(a > s ? a + (s >> 1) : s + (a >> 1))
    >> 8`, `a < 0` → `(−a) >> 8`; level = `CEILING(index, 255)` (no lower clamp is needed: both are ≥ 0).
    Depth `d`: walls `(short)MIN(world_x, SHRT_MAX)` per column (:991), floors `(short)MIN(|hworld_to_screen
    / screen_y|, SHRT_MAX)` per row (:1084-1091); shadeless → table 0 = `polygon->shading_tables` (:984-986,
    :1085-1087). Ambient `a`: floors `get_light_intensity(…)` **unpinned** (`RenderRasterize.cpp`:460: may be
    negative or above 1), walls `+ ambient_delta` pinned to `[0, 65536]` (:598-599): **negative ambient
    reaches floors/ceilings only**. Table entries: channel = `((c16 · m) / 255) >> 8`, `m = L` normal,
    `128 + (L >> 1)` self-luminous (`shapes.cpp`:1952-1960).
  - *Shade LUT* (`core/shade_lut.*`, plain C++). 256 × 256 **L8** (`GPU_L8`, `enums.h`; 64 KiB), column `i` =
    s bin, row `j` = a bin; value = SW's level at a **representative** `(s_i, a_j)`: `s_0 = 0`, `s_255 =
    65536`, else `256·i + 128` (the bin centre); `a_j = 256·j + 128`. So the two clamped cases that cover
    most pixels are exact: far pixels (`s` pinned to 0, texel 0) and `a < 0` (row `(−a) >> 8`, `s = 0`:
    index `a_j >> 8 = j`, exact); full bright (`a ≥ 65280`, row 255) gives 255 everywhere. Inside a bin the
    true index spreads over ≤ 1.5 levels (slope 1 in the larger of `s`, `a`, ½ in the other), so the bin
    representative is within 1 level of SW everywhere — the host test proves it on the grid below. Tiled with
    `core/tiling` (`tile_image(…, texel_bytes 1, …)`, the source row `j` into memory row `255 − j` as every
    texture, M2 "Tiling"), checked byte for byte against `tex3ds -f l8 -z none -r` as M2's RGBA8 test.
  - *texcoord1* (front end, `core/shade_lut` `shade_texcoord1()`, host-tested; the vertex's other attributes
    unchanged). Per polygon: `shadeless` (`_SHADELESS_BIT`) → `(u, v) = (255.5/256, 255.5/256)` (LUT 255:
    the texture's own colour, exact); `a < 0` → `u = 0.5/256` (bin 0), `v = (min(k, 255) + 0.5)/256` with
    `k` = SW's own index (below); else per vertex **`u = M/65536 − w/8192`** and per polygon **`v = (min(a >> 8,
    255) + 0.5)/256`** (a row centre: `a` is constant on the polygon, so its row is chosen exactly, never at a
    texel boundary). **SW's index wraps** (round 1 finding 2): `table_index` is a `short` (:151) assigned a
    `_fixed >> 8` (:156, :162) and `CEILING` is `MIN` (`csmacros.h`:42), with no lower clamp. So for `a < 0`
    the front end computes SW's index exactly, `k = (int16_t)((int32_t)(0u − (uint32_t)a) >> 8)` (32-bit
    negation wrap, arithmetic shift, truncation): `a = −2³¹` gives `k = 0` (black, as SW), `a = −2³¹ + r`
    (r ≥ 1) gives `k = −1` and SW reads the table **before** `shading_tables` (out of bounds). A polygon with
    `k < 0`, or with `a ≥ 2²³ − 32 768` (where `a + (s >> 1)` can wrap the short for some pixel: floors' `a`
    is unpinned, `RenderRasterize.cpp`:460), is **excluded on both sides** by the filter (`c3d_only`, as
    M2's unsupported bitmaps; counted `c3d_ambient_wrap`, printed apart from `excluded` in the DIFF
    summary), never drawn and never a discard: SW's output is undefined there. The test reads the ambient SW
    will see (after the filter's `fullbright` rewrite, i.e. skipped when `fullbright` is on) and skips
    shadeless polygons (SW ignores their ambient, :984, :1085; round 2 minor 1). `w` is the vertex's perspective
    weight (already evaluated at `(X + δ, Y + δ)` and clamped to `[16, 2²⁴]`, M2 "Texcoords"): it is the eye
    depth SW uses (walls `world_x`, floors `hworld_to_screen/screen_y`) at the pixel corner SW samples, and
    perspective-correct interpolation of an attribute equal to `w` yields `1/(interpolated 1/w)` = the depth
    the texcoords use, so `u` is exact per pixel (SW: per column / per row; walls and floors have constant
    depth along those, so there is no per-pixel-vs-per-line difference for M3's classes). **The `SHRT_MAX`
    depth clamp needs no vertex clamp** (a correction of M2's "M3's texcoord1 keeps it"): for `d ≥ 32 767` SW's
    `s = PIN(M − 262 136, …) = 0` whenever `M ≤ 262 136`, and the GPU's `u < M/65536 − 3.9999 < 0` samples
    texel 0 (`GPU_CLAMP_TO_EDGE`); clamping the vertex value would bend the interpolation. `M > 262 136`
    (never in Marathon 2's definitions: `firing_light_intensity` ≤ 1.0) → the polygon is unsupported, counted
    `flash_over` (discard). The `u` range at the far clamp (−2 048) is within float24 (UNVERIFIED precision
    of PICA's interpolation, section 10 item 6; the diff catches it). The weapon flash is just `M`: read per
    polygon from the view, no uniform. Known inexactness (round 1 finding 7, named in the gate report): a
    floor vertex on or past the horizon gets M2's sign-guarded `sy' = ±½` (`geometry.cpp` `floor_vertex`), a
    fictitious `w`, so texcoord1 interpolates a wrong depth across such a polygon (as texcoord0 does since
    M2); `w < 16` is clamped to 16 (M2), so very near pixels are within 1 level, not exact.
  - *TEV* (design 4.2's table, `texenv.h`, `enums.h`:331-383). Stage 0: RGB `MODULATE(TEXTURE0
    RGB_SRC_ALPHA, TEXTURE1 RGB_ONE_MINUS_SRC_R)` = `x = A·(1 − L)`; alpha `REPLACE(TEXTURE0 A_SRC_ALPHA)`,
    scale 1 by default. Stage 1 (built from `C3D_TexEnvInit`, scale 1): RGB `MODULATE(TEXTURE0 RGB_SRC_COLOR,
    PREVIOUS RGB_ONE_MINUS_SRC_COLOR)` = `c·(1 − x)`; alpha `REPLACE(PREVIOUS A_SRC_ALPHA)`. Alpha test
    `GEQUAL 128` (M1's, unchanged). **The alpha ×2 is not the default** (round 1 finding 3; a change from
    4.2): stage 0's RGB reads the *texel's* alpha, so the scale only changes the alpha written to the target,
    and M3 needs none of that: with scale 1 a written pixel has alpha 255 or 128 (self-luminous), as in M2,
    both pass the test and neither is the sentinel's 0x01. Key `alpha_x2 1` sets `C3D_TexEnvScale(env,
    C3D_Alpha, GPU_TEVSCALE_2)` on stage 0 (alpha 255 everywhere if PICA clamps, **UNVERIFIED**); it is run as
    an A/B (`m3-alpha2.cfg`) in Azahar and on the console because M5's static (4.3: `(TEX0.A ×2)·noise.A`)
    will depend on the clamp. **Should the ×2 wrap**, A 255 → 254 still passes, A 128 → 0 is dropped by the
    alpha test: self-luminous texels go missing as non-edge coverage mismatches, which that run's coverage
    gate shows. Normal texel (A 255): `x = 1 − L`, out = `c·L`;
    self-luminous (A 128): out = `c·(0.5 + 0.5·L)` ≈ SW's `128 + L/2`; shadeless (A 255, LUT 255): out = `c`.
    `lit 0` keeps M2's single stage (`REPLACE TEXTURE0`; stage 1 untouched, unit 1 unbound); `textures 0`
    keeps M1's flat colour (no LUT, no stage 1).
  - *Lit colour prediction* (host, informative; the gate's support). SW channel `((c16·m)/255) >> 8` vs the
    two stages on `c8 = c16 >> 8` (texture = top table, M2), integer TEV `(a·b)/255` (Azahar's software
    backend) or rounded float (the host-GPU backend), over every `c16` (step 7) and `L`: normal max Δ 1,
    mean 0.24-0.30; self-luminous max 1-2, mean 0.33-0.36; **with the LUT off by ±1 level: max Δ 2, mean ≤
    0.47** in all four cases (main session, 2026-11-03; the host test reprints it). So lighting adds at most
    2 to M2's texel-choice error.
  - *The LUT's lifecycle* (`back/shade.cpp`, new; `<3ds.h>`/`<citro3d.h>`; main thread). States `None →
    Linear → Copying → Vram`. **None**, at a lit view frame's start (after `frame_begin()` returned, before
    the clear): `C3D_TexInit(256, 256, GPU_L8)` linear, `C3D_TexSetFilter(NEAREST, NEAREST)`,
    `C3D_TexSetWrap(CLAMP_TO_EDGE, CLAMP_TO_EDGE)` (`texture.h`:157-173), build + tile straight into it,
    **one ranged `GSPGPU_FlushDataCache` of its 65 536 bytes**, sampled from linear in this frame; allocation
    failure → the check is discarded `tex` (retried at the next lit frame). **Linear**, at a later lit view
    frame's start, **before** the texture cache's promotions: if `gx.used() + 1 + kViewReserveEntries ≤
    budget` (M2's rule, `core/texcache.cpp`:228) and `C3D_TexInitVRAM(256, 256, GPU_L8)` succeeds, one
    `gx_op`-counted in-frame `C3D_SyncTextureCopy(lin, 0, vram, 0, 65536, GX_TRANSFER_RAW_COPY(1))` (M2's
    verified promotion, `back/texcache.cpp` `texcache_frame_start`); still sampled from linear this frame
    (the copy runs before the draws, but the binding switches only after it retired). A refusal or a failed
    VRAM allocation leaves it **Linear** (retried; the failure logged once). **Copying**, at the next lit view
    frame's start (`frame_begin()` returned: the copy retired, 0.3): unit 1 re-bound to the VRAM texture
    **first** (unit 1 keeps the LUT between frames, below), then `C3D_TexDelete` of the linear one, state
    **Vram**, sampled from VRAM. `shade_free()` after `C3D_Fini` (`release_and_fini`, beside `draw_free()`
    and `texcache_release_all()`): both deleted, state None (a re-init rebuilds it). Not touched by
    `collections_changed()` (the LUT depends on no collection). VRAM 64 KiB of 9.1's "shade LUT + noise"
    128 KiB, outside `vram_cap`; linear 64 KiB transiently. Log lines `[c3d] shade: LUT built (linear,
    65536 B)` and `[c3d] shade: LUT promoted to VRAM`.
  - *Binding and the state the present relies on* (`back/draw.cpp`; `present.cpp` is not edited). The
    present binds its program, AttrInfo and `C3D_TexBind(0, …)` once at init (`present.cpp`:432-453) and sets
    **TEV stage 0 in full** (`C3D_TexEnvInit` first, so its scale is reset) and the depth/write mask before
    every draw (`set_present_draw`, :314-330); it never touches stage 1 or unit 1. A lit view frame adds:
    Both are configured **only after M2's SAVE block** (`draw.cpp`:413-415), at stage 0's place (:431), so the
    early returns of `view_begin` (:400-410, before the SAVE) leave nothing to restore; `lit 0` and
    `textures 0` frames neither touch nor save stage 1 or unit 1 (round 1 finding 4).
    (1) **Unit 1 = the LUT during the frame, and no texture after it** (round 1 finding 1; revised after the
    acceptance, Review log "M3 acceptance, unit 1 after the frame"). `C3D_TexBind(unit ≥ 1, NULL)` reads
    `tex->param` before storing (texture.o `C3D_TexBind`+0x10-0x20: `ldr ip, [r1, #12]`): a data abort on
    hardware (Azahar returns 0 for the unmapped read and would hide it). Leaving the LUT bound was the first
    answer, but then unit 1 is **enabled during the present** while the present's shader has no texcoord1
    output, so it samples an undefined coordinate: **Azahar's software backend segfaults on it**
    (`Pica::Texture::LookupTexelInTile`, macOS crash reports of every software-backend run, right after the
    first lit view frame; the host-GPU backend shows nothing), and on PICA the effect is unknown. So
    `view_begin` binds the LUT through `__real_C3D_TexBind(1, …)` on every lit frame and `restore_state`
    **unbinds it** with `shade_unbind_unit1()` (`back/shade.cpp`): the store `C3D_TexBind` makes for unit 0
    (no type check: +0x24-0x38, the pointer at `__C3D_Context` + 0x118 + 4·unit, dirty bit 23 + unit), done
    for unit 1 with NULL; the state flush then disables the unit (0.3), which is the state the present had
    before M3. `__C3D_Context` is a global object of citro3d 1.7.1 (base.o `.bss`, 0x2d0 bytes), so the write
    depends on that layout: it is **checked at run time** the first time — right after the real
    `C3D_TexBind(1, lut)` the slot must hold `lut` with bit 0 and dirty bit 24 set, logged `[c3d] shade:
    citro3d context layout confirmed` — and if the check fails unit 1 keeps the LUT (counted
    `unit1_left_bound`, must be 0). The Copying → Vram step re-binds before deleting **only in that
    fallback** (with the layout confirmed unit 1 is already NULL, and a bind there would reach the present if
    `view_begin` discarded before its lit block: critic, post-review minor 1). The `C3D_TexBind` wrap's record is extended from unit 0 to units 0-2
    (`back/context.cpp` `__wrap_C3D_TexBind`): a foreign unit-1 bind ever, or during a frame, counts
    `tex1_restore_mismatch` (must be 0: only `back/` binds unit 1, through the real function).
    (2) **TEV stage 1**, saved by copy in `view_begin` (`*C3D_GetTexEnv(1)`) and put back with
    `C3D_SetTexEnv(1, &saved)` in `restore_state` (copy semantics, 0.3); the saved value is compared with
    the init value (0.3) and a difference counted `tev1_not_init` (must be 0: nothing else configures stage
    1; `view.cpp`'s calibration re-inits stage 0 only and needs stages 1-5 pass-through, :431-435, which this
    gives). (3) **Stage 0** is left as the view frame set it, as in M2: the present re-sets it in full before
    each draw. (4) The **AttrInfo** is M2's save/restore (copy; the present's has two loaders, the view
    frame's four); texcoord1 never reaches the present: its program has no texcoord1 output and the restored
    AttrInfo has no third loader. Proof: `m3-present-diff.cfg` (`m2-present-diff.cfg` with lit view frames:
    `fullbright 0`) → `present_diff` 0, `tex_restore_mismatch` 0, `tex1_restore_mismatch` 0, `tev1_not_init`
    0, `prog_restore_mismatch` 0.
  - *Infravision* (3.1, 4.3). `_SHADELESS_BIT` on walls/floors comes only from `view->shading_mode ==
    _shading_infravision` (`RenderRasterize.cpp`:463, 602); `shading_tables` is then the collection's tint
    table (one table, 256 entries). `c3d_supports` stops refusing `shadeless` **when `textures` is on** (with
    `textures 0` the flat path has no kind to draw it with: still refused, so `c3d_only` excludes it); the
    kind is `Shadeless`, or `ShadelessKeyed` for a `_TRANSPARENT_BIT` bitmap on a wall (M2's keying rule);
    texcoord1 `(1, 1)` as above. The cache key already holds the tables pointer and the kind, so tinted and
    normal copies of a bitmap coexist (3.3 "CLUT change"). The filter's `fullbright` already leaves shadeless
    primitives alone (`filter.cpp`:49, 64).
  - *Counters* (the DIFF summary): `c3d_lit_polys` (drawn with depth/ambient shading), `c3d_shadeless_polys`,
    `c3d_neg_ambient_polys`, `c3d_flash_checks` (checks whose `M` > `NATURAL_LIGHT_INTENSITY`), `c3d_m_max`,
    `c3d_flash_over`, `c3d_ambient_wrap` (excluded polygons). The `[c3d] cmdbuf` line gains `lut=<state> lut_builds=N tex1_restore_mismatch=N
    tev1_not_init=N`.
  - *GX queue and command buffer.* One more GX entry, once (the LUT promotion), inside M2's per-frame bound
    (promotions ≤ 8 + LUT 1 + clear 1 + split 1 + readback 1 = 12, + `C3D_FrameEnd` 1; the budget rule keeps it
    ≤ 24). The first lit draw adds TEV stage 1 and texture unit 1 state words: the reserve is re-measured over
    the acceptance runs (`[c3d] cmdbuf final`, deterministic: Azahar authoritative) and `kDrawReserveWords`
    set to 2 × the worst first-draw words (536 today = 2 × 268); `view_frame_words` and `draw_words` recorded.
  - *Zones:* the LUT build, its flush and its promotion issue run in **`c3d_tex`** (M2's zone, nested in
    `c3d_draw`). No new zone.
  - *Colour gate (lit).* **M2's neighbourhood rule** (Review log "M2 implementation, colour gate", a user
    decision) **with 8.3's lit numbers unchanged**: per class (walls, floors) over the run, non-edge coverage
    mismatches ≤ 0.1 % of A's covered pixels, **≥ 98 % of common non-edge pixels within Δ ≤ 4 (`diff_tol 4`)
    to A's pixel or one of its covered 8 neighbours, neighbourhood mean ≤ 1.5**; strict figures printed.
    Lighting does not change which texel is sampled (texcoord0 is M2's), and neighbours along a wall column
    or a floor row share SW's shade, so the neighbourhood rule absorbs exactly what it absorbed in M2; the
    prediction above bounds lighting's own share at 2. **No tolerance changes** (8.3's row as written, M2's
    approved metric), so the user is not asked; the report names the gate used. Infravision: 8.3's row
    "full bright (M2), infravision", `diff_tol 2`, mean ≤ 1.0, neighbourhood. `fullbright 1` runs keep M2's
    gate (Δ ≤ 2, mean ≤ 1.0) on the M3 binary (`lit 1` at LUT 255 is exact: `x = 0`).
  - *`c3d-diff.py`.* `--shading full|lit|infra` (default `full` = today's check: the debug line shows
    `fullbright 1` and not `textures 0`). `lit`: the debug line shows `fullbright 0`, `lit 1` and not
    `textures 0`, and `c3d_lit_polys` > 0. `infra`: as `lit`, plus `c3d_shadeless_polys` > 0 and
    `c3d_lit_polys` == 0 (every compared polygon was shadeless). The lit counters are printed in every mode.
  - *Films* (8.2). Film 1: M2's "M2 L01 Waterloo Waterpark" (`m3-film.cfg`; combat, so weapon flashes).
    **Film 2: "M2 L08 Nuke and Pave.682.filA"** (`m3-film-l08.cfg`): an offline parse of `Map.sceA` (light
    definitions × polygon floor/ceiling lights, by area; main session) finds **one** Marathon 2 level with a
    light that can go negative, L08 (its light 5: `secondary_active` intensity −32 768.0, i.e. `0x80000000`,
    on 13 % of the floor/ceiling area; SW's short index then wraps: black for exactly −2³¹, an out-of-bounds
    table otherwise, so such polygons are excluded and counted `c3d_ambient_wrap`, above), with
    25 % of the floor/ceiling area under ambient 0.35. Darkness is also covered by the bench spots: L6
    (Ex Cathedra, 52 % under 0.35) and L20 (Thermopylae, 65 %); depth shading appears on every level (`M =
    0.5` by default: `s > 0` only for `d < 4 096`). Whether the film looks at the negative light while it is
    negative is printed (`c3d_neg_ambient_polys`, `c3d_ambient_wrap`), not gated: 0 is recorded as "not
    reached", as M2's ≥ 16 conversions. (A moderately negative `a`, `k` in [0, 255], is the drawn case; the
    host test covers it.)
  - *The infravision plugin.* `scripts/3ds/tests/plugins/Infravision Test/` (after `Terminal Test/`): solo
    Lua, `Triggers.init` sets `Players[0].infravision_duration = 3000` and `Triggers.idle` keeps it above
    1 500 (`lua_player.cpp`:2412); installed by a new `run-azahar.sh --infravision-test` (removed otherwise,
    as the other test plugins). `m3-infra.cfg`: `diff 15` (so the first check comes after init and the first
    idle ticks: the run-wide `c3d_lit_polys == 0` rule needs every check in infravision, round 1 finding 6),
    `diff_frames 20`, `classes walls,floors`, `diff_tol 2`; gate `--shading infra --tol 2 --within 98 --mean
    1.0`.
  - *Keys* (`core/config.*` + host test): `lit 0|1` (default 1), `alpha_x2 0|1` (default 0). Both printed in
    the `[c3d] debug-c3d:` line.
  - *Configs.* `m3-diff.cfg` (`diff 1`, `diff_frames 20`, `classes walls,floors`, `fullbright 0`,
    `textures 1`, `lit 1`, `diff_tol 4`), `m3-film.cfg` and `m3-film-l08.cfg` (`diff 2`, 200 checks),
    `m3-infra.cfg`, `m3-present-diff.cfg`, `m3-alpha2.cfg` (`m3-diff.cfg` + `alpha_x2 1`: the clamp A/B).
  - *Host tests* (8.4; round 1 finding 5: a grid that runs in seconds and models the GPU's continuous
    depth). The LUT vs a transliterated `calculate_shading_table` (short index, `CEILING`) for `M` ∈ {32 768,
    65 536, 98 304, 131 072, 262 136}: every integer `d` in [0, M/8 + 64] (beyond, `s` is pinned to 0; one far
    `d` per `M` checks the pinning and one `d ≥ 32 767` the `SHRT_MAX` case), each also at `d ± ½` for the
    GPU's `u` (SW keeps the integer), × `a` at every bin edge `256·j + {0, 1, 127, 128, 255}` for j in 0..256
    and the negative mirror, plus `a` ∈ {−2³¹, −2³¹ + 1, −2³¹ + 255, ±(2²³ ± 1), 2²³ − 32 768 ± 1}; sampling as
    the GPU does (texel `clamp(floor(u·256), 0, 255)` of the front-end texcoord1, row from `v`), and with `u`
    nudged by ±1/65 536 to model a one-bin miss at a boundary: **|LUT − SW| ≤ 1 everywhere** (asserted; the
    gate), exact-agreement % printed; exactness asserted for `s` pinned to 0, `0 ≤ k ≤ 255` with `a < 0`,
    `a ≥ 65 280` and shadeless; the wrap cases excluded exactly where SW's index is < 0 or can wrap; L8 tiling vs tex3ds
    (byte for byte; skipped, printed, if tex3ds is absent); the lit colour prediction (printed); the keys.
  - *Accept* (Azahar, non-authoritative, both backends unless said): `m3-diff.cfg` on L0/L6/L20
    (`--min-checks 20`) and both films (`--min-checks 200`) pass `--colour walls,floors --shading lit
    --nonedge-pct 0.1 --within 98 --tol 4 --mean 1.5`, `sentinel` 0, 0 discards; `m3-infra.cfg` on L0
    passes `--shading infra`; `m2-diff.cfg` (`fullbright 1`) on L0 still passes M2's gate on the M3 binary;
    `m3-present-diff.cfg` (host GPU) `present_diff` 0 and the five restore counters 0 (`tex_restore_mismatch`, `tex1_restore_mismatch`, `tev1_not_init`,
    `unit1_left_bound`, `prog_restore_mismatch`), with `[c3d] shade: citro3d context layout confirmed` in every
    lit run (both backends); the LUT log lines
    (built once, promoted once, VRAM from then on); the re-measured reserve; M1's walls coverage
    (`m1-diff-walls.cfg` L0), M0's calibration (`m0-readback-cal.cfg`) and P1 scripts 07, 08, 13, 14, 22
    unharmed (host GPU); the OFF builds' `objdump -d` unchanged and `nm` clean. The lit counters of the
    films are recorded (flash checks > 0 expected on both; negative ambient on L08 if reached).
    `m3-alpha2.cfg` (L0, both backends) is **recorded, not gated** (the clamp's Azahar answer; the console's
    in the short check).
  - *Not in M3:* sprites/weapons and their constant texcoord1 (M4), landscapes `(1, 1)` and static noise on
    unit 1 (M5), play-mode C3D frames (M4).

### M4 — sprites, weapons, invisibility; play mode on (readback)
- Add: rectangles (RLE, flip, clip), mask textures, tinted blend; play mode on
  (frames with landscape/static discard per frame).
- Accept: diff `sprites,weapons,tinted` (tinted over a grey clear, 8.1):
  coverage outside band ≤ 0.5%, colour as M3; full-frame ≥ 97% Δ ≤ 4;
  `12-sprite-check.txt`, `06-gameplay.txt`, 07, 13, 14 pass in play mode;
  bench C3D vs SW recorded.
- **M4 specification (revision 1 of 2026-11-06, before code; Review log "M4 pre-implementation", round 1).**
  Where the lines above, 1.5, 2.2, 3.2-3.3, 4.3, 8.1 and 8.3 and the code disagree, this block rules. Citations are
  the code as of `198eb38e3`. **No engine edit.** Kickoff `KICKOFF_M4.md`. Two sessions: (1) the spec, rectangles,
  mask and tinted on diff frames; (2) play mode, the configs, the plugin, the films and the acceptance.
  - *Corrections to the kickoff and to 3.2-3.3, checked at HEAD.* (a) The RLE path is **not written**: `tex_convert`
    refuses RLE and non-square bitmaps (`core/tex_convert.cpp` `tex_supported`), so RLE decoding, non-square POT
    padding and the rectangle kinds are new M4 code; the A8 `Mask` kind exists for square bitmaps only and the
    allocator refuses it (`back/texcache.cpp` `C3dAllocator::alloc`: RGBA8 only). (b) The cache **has** LRU
    eviction since M2 (`core/texcache.cpp` `lru_victim`, `alloc_with_eviction`; 3.3's bullet is implemented, only
    the size-class slabs were deferred): "a full pool" in M4 is a create() that fails after eviction, already
    counted `discards_tex` and returned as `TexGet::Discard`; M5 keeps the hardening runs (`vram_cap 512`,
    `fail_every`). (c) The core cache and the tiling are square: `TexCache::create(key, W)`, `tex_bytes(W, kind)`,
    `TexAllocator::alloc(pool, W, …)`; `tile_index(W, m, s)` already takes the row width only and
    `tile_memory_row(H, j)` the row count, so non-square tiling needs no new formula.
  - *SW's rectangle, exactly* (`scottish_textures.cpp` `Rasterizer_SW_Class::texture_rectangle`, :656-886). Draws
    only if `x0 < x1 && y0 < y1`; the clip fields are pinned to the screen, then to the rectangle (:664-673); nothing
    if the result is empty or off screen (:676-678). `texture_dx = (W << 16) / (x1 − x0)`, `texture_x = dx >> 1`;
    `texture_dy = (H << 16) / (y1 − y0)`, `texture_y0 = 0` (integer `_fixed` divisions; **rows are not centred,
    columns are**); nothing if either is 0 (:693). `flip_horizontal`: `dx = −dx`, `texture_x = (W << 16) + (dx >> 1)`
    (arithmetic shift) (:696-700); `flip_vertical` is ignored (as 2.2 says). Left clip `texture_x += δ·dx`, top clip
    `texture_y0 += δ·dy` (:703-723). `W`, `H` = `texture->width`, `->height`; column `c` = `row_addresses[c]`
    (every rectangle bitmap is read as column-ordered RLE: 4 header bytes, `first`, `last` big-endian, then the
    bytes of rows `[first, last)` — `last` is exclusive, `shapes.cpp`:705-717 and `OGL_Textures.cpp`:1082-1085).
    Per screen column: texel column `texture_x >> 16`; per screen row `j` (from the clip top) texel row
    `(texture_y0 + j·dy) >> 16`; index 0 is not written (`texture_vertical_polygon_lines<…, true>`, :858; the tinted
    loop's `if (read[…])`, `low_level_textures.h`:567). **SW's per-column trim** (:763-783): rows before
    `delta = (first·65536 − ty0)/dy + 1` and after `y1 − ((ty1 − last·65536)/dy + 1)` are not drawn. It equals "the
    texel lies in `[first, last)`" except (i) a row whose position is exactly `first` (skipped by SW); (ii) when the
    bottom trim runs (`ty1 >> 16 > last`), **a row is kept only if the next row's position is still below `last`**
    (`v + dy < last`): SW drops the last row of the run; (iii) when `ty1 >> 16 == last` (no bottom trim; round 1
    finding 3) rows whose position lies in `[last, ty1)` read `data[last − first]`, i.e. the next column's header
    (`shapes.cpp`:715-719: the high byte of its `first`, 0 for any `first` < 256, so normally transparent) or memory
    past the bitmap for the last column, where the GPU samples transparent. (i)-(iii) are 1 pixel at the end of a
    column's run, inside A's edge band, **except** where a run covers only 1-2 screen rows (minified sprites,
    projectiles): (i)+(ii) can then remove the whole run in SW while the GPU draws a pixel, outside the band. The
    host test transliterates SW's column loop with (iii)'s read and bounds the non-band share (below). Shading (:738-758): `_textured` without `_SHADELESS_BIT` → `calculate_shading_table(…,
    (short)MIN(depth, SHRT_MAX), ambient_shade)`, one table for the whole rectangle; shadeless, `_tinted`, `_static`
    → `shading_tables` itself (table 0).
  - *Rectangles on the GPU* (new `core/rect.*`, plain C++, host-tested; the front end calls it). `rect_setup(r, w,
    h, W, H, out)` transliterates the code above on `int16`/`int32` exactly as SW (shorts for the screen values,
    `_fixed` products) and returns `None` (SW draws nothing: counted `rect_skipped`) or the clipped screen box
    `[cl, cr) × [ct, cb)` with `TX0`, `DX`, `TY0`, `DY` (fixed). **One quad**, corners on pixel boundaries (so its
    coverage is exactly the box and it has no tie pixels), `w = 1` (affine), texcoord0 in texels: `t(x) = (TX0 +
    (x − cl − ½)·DX) / 65536` (the bitmap column) and `s(y) = (TY0 + (y − ct − ½)·DY) / 65536` (the row in the
    column), evaluated in double at the four corners; at a pixel centre `X + ½` the interpolation gives SW's own
    `texture_x + (X − cl)·dx` exactly in real arithmetic. GPU coordinates `u0 = (s + ε)/S`, `v0 = (t + ε)/T` (S, T
    the POT sizes below; texel `(s, t)` = byte `s` of column `t`, as walls). **ε** (`rect_bias`, in **1/131072**
    texel, **default 1**, measured): SW's positions are multiples of 1/65536 texel; they are often exact integers
    (rows start at 0; 1:1 weapons have `dy = 65536`), where a value a hair below samples the texel before, and,
    because `dx = (W << 16)/w` truncates, often **a few 1/65536 below** an integer (e.g. 3.99998), so any bias of
    1/65536 or more flips those. Host test (W ∈ {8, 31, 64, 128, 200}, H ∈ {8, 81, 128, 200}, screen sizes 1..400,
    centred, clipped and flipped; 9.8 M pixels): ε = 0, 1, 2, 8 /131072 give 99.68 / **100.000** / 99.92 / 99.83 %
    of SW's texels in double arithmetic (the same with the corners stored as float32, as `Vertex` does and Azahar's
    host-GPU backend then interpolates: round 2 minor 1), 99.59 / **99.85** / 99.83 / 99.78 % in a float24 model (vertex attributes
    rounded to a 17-bit significand, interpolated, floored). **UNVERIFIED** (round 1 finding 5): the float24 model is
    not PICA's documented behaviour and Azahar's host-GPU backend interpolates in float32, so only the console tells
    how close PICA comes (section 10); the neighbourhood gate absorbs the residual off-by-one texel. The
    clamp-to-edge sampler (`GPU_CLAMP_TO_EDGE`, `enums.h`:37) plus a transparent border (below) make texels
    outside the bitmap invisible, as SW never reads them.
  - *texcoord1 of a rectangle* (constant per rectangle: one table, as SW). `L = sw_shade_index(M, (int16)min(depth,
    32767), ambient_shade)` (`core/shade_lut.cpp`:39, M3's transliteration of `calculate_shading_table`) and
    texcoord1 = `(0.5/256, (L + 0.5)/256)`: column 0 of the LUT is `index_of(256j + 128, 0) = j` for every row
    (`shade_lut_value(0, j) = j`, host-asserted), so the level is **exact**. Weapons: `depth` 0 and their ambient
    (`render.cpp`:1094-1097) take the same path. Shadeless → `(255.5/256, 255.5/256)` (M3's constant). `L < 0` (SW's
    short index wraps: SW reads before the tables) → excluded on both sides of a check (`c3d_ambient_wrap`, M3's
    rule) and a scan reason in play (below). No `flash_over` for rectangles: `L` comes from the pinned shade, so it is
    exact for any `M` (round 1 minor 11). Tinted: `(0.5/256, 0.5/256)`
    (a defined coordinate for unit 1 in a lit frame; the blend ignores the source colour).
  - *Conversion of rectangle bitmaps* (`core/tex_convert.*`). New predicate `rect_tex_supported(W, H, rle)`: RLE
    (`bytes_per_row == NONE`), `1 ≤ W, H ≤ 1024`; anything else → the rectangle is unsupported (a non-RLE rectangle
    never occurs in Marathon 2's shapes; counted `rect_unsupported`). POT sizes `S = pot(max(H, 8))` (texel row
    length = the column's height), `T = pot(max(W, 8))` (rows = columns), 8 the PICA minimum. `tex_convert_rle(bitmap,
    tables, kind, S, T, out)`: memory row `tile_memory_row(T, c)` holds column `c`; texel `s` of it is `data[s −
    first]` for `first ≤ s < last` (clamped to `H`), else transparent; padding (`s ≥ H`, `c ≥ W`) transparent. Kinds:
    **`NormalKeyed`** (RGBA8, 3.2's normal with A 0 for index 0 and for every texel outside the run), **`ShadelessKeyed`**
    (infravision's tint table), **`Mask`** (A8 = `GPU_A8`, `enums.h`:73: 0 transparent, 255 else; tinted). A
    malformed column (`first > last`, or the bytes beyond the bitmap's storage) cannot be detected from a
    `bitmap_definition` and is trusted as SW trusts it. The wrap is **part of the cache key** (`TexKey::clamp`, true
    for RLE rectangles) and passed to `TexAllocator::alloc` for **both** pools, so a promotion's VRAM copy keeps it
    (round 1 finding 4: `NormalKeyed` serves transparent walls, `GPU_REPEAT`, and sprites, **`GPU_CLAMP_TO_EDGE`**,
    `texture.h`:169).
  - *Cache* (`core/texcache.*`, `back/texcache.*`). `create(key, S, T)` and `TexSlot::{S, T}`; bytes `S·T·texel_bytes`;
    the allocator takes `(pool, S, T, kind, bytes)` and the GPU format from the kind (`GPU_RGBA8` / `GPU_A8`); the
    promotion copies `bytes` (RAW copy, any size: M2's form). `TexSource` gains `rect` (the RLE path and the clamp).
    The key is `(bitmap, tables, kind, clamp)`: a tinted rectangle's key holds the global table pointer and
    `Mask`, so mask and colour copies of one sprite frame coexist. Slots `kTexSlots` 256 → **512** (sprite frames
    are many and small; L0's bench spot already has 26 (texture, table) pairs, M0's null table) — 512 slots of 64 B
    of bookkeeping. Pool caps unchanged (VRAM 3 584 KiB, linear 4 096 KiB): a 128×64 sprite frame is 32 KiB RGBA8,
    the working set of a frame is far below the caps; the films log the high-water `used` bytes, evictions and
    `discards_tex` and the defaults are revised only if a film discards on `tex` (recorded).
  - *Tinted* (4.3; `low_level_textures.h`:543-579, `scottish_textures.h`:90-95, `shapes.cpp`:2006-2021). SW: index
    `k = transfer_data & 0xff`, tables `(tint_table32*)shading_tables + (k << 3)`, each channel `v ↦ (v·8k)/255`
    (integer; `number_of_shading_tables` 256 at 32 bits; `render.cpp`:808-814 gives k 15 or 24) for every pixel whose
    texel index is not 0; **the written
    pixel has alpha byte 0** (`get_pixel_tint<pixel32>` ORs only the three channel tables). GPU: the `Mask` texture on
    unit 0, the alpha test `GEQUAL 128` keeps the masked texels, and the blend `C3D_AlphaBlend(ADD, ADD, ZERO,
    CONSTANT_COLOR, ZERO, ONE)` with `C3D_BlendingColor(0xFF000000 | c·0x010101)`, `c = 8k`; `k ≥ 32` (SW would read past the 256 tables; never in
    Marathon 2) is unsupported (`effect.h`:8,
    :12; `enums.h`:247-257): RGB = dst·c/255, alpha kept. The three channels get the same factor, so the register's
    byte order does not matter (`effect.o` `C3D_BlendingColor`: the raw u32 into the context at +0x100, dirty bit 4).
    **TEV does not change**: stage 0's alpha is `REPLACE(TEXTURE0 alpha)` in every textured frame (lit: `draw.cpp`
    :477-479; unlit: :502-503) and stage 1 passes it on (`REPLACE(PREVIOUS)`), so an A8 texel reaches the alpha test
    as it is; the RGB the stages compute is multiplied by ZERO. A tinted draw needs `textures 1` (else unsupported).
    Rounding: SW floors `v·c/255`; PICA's blend rounding is UNVERIFIED (Azahar: integer); predicted Δ ≤ 1.
  - *State and its restore* (2.4; the M3 rule "every unit and stage you enable is disabled before the present").
    A draw's blend is a property of its key's class: `kTinted` → the tinted blend with its constant, else 2.4's world
    blend. `view_polygon` sets it **between draws** when the class kind changes (`s_f.blend` = Normal | Tinted(c)),
    after `issue_pending()`. `restore_state` re-issues the world blend `C3D_AlphaBlend(ADD, ADD, ONE, ZERO, ONE, ZERO)`
    and `C3D_BlendingColor(0)` (C3D_Init's value: `base.o` `C3D_Init` never stores +0x100, the context is zeroed)
    whenever the frame set the tinted blend (counted `blend_restores`; `blend_sets` counts the switches); the present
    sets its own alpha test and blend before every draw anyway (`present.cpp`:339-340) and never reads the blend
    colour. citro3d has no getter for the blend state, so there is no "left tinted" counter: the proof is the
    present diff. Unit 0 holds an A8 texture during the frame; `restore_state` puts the present's texture back (M2).
    Proof: `m4-present-diff.cfg`
    (play mode on, the Invisibility Test plugin, so play frames end with tinted draws) → `present_diff` 0 and the
    restore counters 0.
  - *Front end* (`front/c3d_rasterizer.cpp` `texture_rectangle`). Class from `classify(Rectangle, transfer,
    foreground_)`; `_static` → unsupported (M5); `_textured` and `_tinted` drawn: `rect_setup`, the kind (normal /
    shadeless → `NormalKeyed` / `ShadelessKeyed`; tinted → `Mask`), the DrawKey (as polygons: class, texture, tables,
    transfer mode/data, shadeless), the four vertices (colour white, `a` 255), `view_polygon(v, 4, key, &ts)`.
    Counters (`DIFF summary`): `c3d_rects`, `c3d_rect_skipped`, `c3d_tinted_rects`, `c3d_rect_unsupported`.
    `FilterRasterizer::c3d_excluded` gains rectangles (unsupported bitmap; `L < 0`), so a diff check of
    `sprites`/`weapons`/`tinted` excludes them on both sides as M2 does for walls.
  - *Diff checks* (8.1; `front/frame.cpp` `c3d_check`). `sprites`, `weapons`: as walls (clear 0, `Coverage::TopByte`).
    **`tinted`**: A cleared to `0xFF808080`, the C3D target cleared to `(128, 128, 128, 255)` (`view_begin(w, h,
    clear_rgba)`, a new argument; M1-M3 pass 0), `Coverage::NotClear` with clear `0xFF808080` on both (A's tinted
    pixels are `0x00RRGGBB`, B's `0xFFRRGGBB`: both differ from the clear; the colour delta reads RGB only,
    `core/diff.h`). **`frame`**: every class through the filter with landscapes and static **excluded on both sides**
    (`c3d_only`, counted `excluded`: M5's classes; otherwise a sky in view would discard every frame check) and the new
    **`Coverage::All`**: there is no band and no coverage mismatch (A's tinted pixels have alpha 0, so `TopByte` would
    call them uncovered); the compared (`common`) pixels are those where A **or** B differs from the clear in RGB, and
    the pixels both leave at the clear are counted apart (`empty`, printed; round 1 finding 2: otherwise an excluded
    sky would pass as Δ 0). The neighbourhood minimum runs over A's 8 neighbours, but over A's **non-empty**
    neighbours when only one side drew the pixel (round 2 minor 2: else a pixel C3D dropped next to an empty A pixel
    would score Δ 0).
  - *Play mode* (1.5's row "milestone < M4 → `false` in play" goes; `front/frame.cpp` `render_frame`, after the
    diff-frame branch). A **play frame** is a world frame with `renderer c3d`, `mode readback`, `play 1` (new key,
    default 1), `textures 1` and `lit 1` (round 1 minor 7: no flat or unlit picture for the player) and the renderer
    not "off until the next level". In order:
    (0) *Cheap refusals first* (round 1 minor 8): `software_alpha_blending` not off (`pref`), citro3d not up or a
    release pending (`down`), the gate closed (`gate`), a view size `view_size_supported` refuses or `view_ensure`
    failed for before (`size`) → SW draws the frame directly (`draw(ctx, sw)`, no recording), counted
    `skip:<reason>`. Classic's world view (640 × 320, `PERF.md` "Resolution", 204 800 pixels) **is** supported (a
    correction, code review minor 3): play runs there too, and the acceptance has a Classic `play_check` run.
    (1) *Record*: `s_recorder.clear()`, `forward = nullptr`, `draw(ctx, &s_recorder)` — the one traversal. On
    overflow (8 192 events; M0's null maxima are 387 vertices, 89 polygons, 18 rectangles) the frame is drawn by a
    second traversal `draw(ctx, sw)`, counted `overflow`: the only side effect of a traversal is the weapon
    flutter's C `rand()` (`weapons.cpp`:2922: "we MUST use rand() here, and not global_random()"), which no game
    state reads.
    (2) *Scan*: the recording is replayed into a counting `PlayScan` rasterizer (no drawing) that finds the first
    reason C3D cannot draw the frame: `landscape`, `static` (polygon or rectangle), `other` (a polygon class C3D does
    not draw: `_solid`, tinted polygon; a tinted `k ≥ 32`), `bitmap` (`c3d_supports` / `rect_tex_size` false), `wrap`
    (M3's `shade_ambient_wraps` on a polygon, `L < 0` on a rectangle), `flash` (`M > kFlashMax` on a polygon). A
    reason → the recording is replayed into SW (`dest`), counted
    `scan:<reason>`; **no C3D frame is opened** (a sky in view costs the scan, not a GPU frame).
    (3) *Draw*: `view_begin(w, h, 0)` (skip → SW replay, counted `skip:<reason>`); the recording replayed into the
    `C3dRasterizer` (unfiltered); `view_end_play(rows)`: M1's readback (`view_readback(true)`: the staging converted
    to ARGB keeping the target's alpha, `back/view.cpp`), then one pass into `dest`'s rows (`row_addresses`, its own
    pitch): the sentinel first (its alpha 0x01 would pass the next test: counted `sentinel`, not copied), then
    **only the pixels C3D covered** (alpha ≠ 0) are written, with alpha forced to 0xFF (5.2's form): a pixel no
    primitive writes keeps the previous frame, as SW leaves it (SW never clears `world_pixels`). Known difference,
    holes only (round 1 minor 10): a tinted pixel over a C3D hole keeps alpha 0 (the blend keeps dst alpha) and is
    not copied, where SW would tint the previous frame's pixel.
    (4) *Discard* (any `DiscardReason`, 2.4) → the recording is replayed into SW (`dest`, `texture_random_seed()`
    restored as the diff harness does), counted `discard:<reason>`. **Ten resource discards in a row** (`ring`,
    `cmdbuf`, `gx`, `begin`, `tex`; not `unsupported`, not scan reasons, which are content, not trouble; scan, skip
    and `unsupported` frames leave the run unchanged, a C3D frame resets it: round 1 minor 9) → play is
    **off until the next level**: logged `[c3d] play: off until the next level (10 discards in a row, last <reason>)`;
    `collections_changed()` (hook 2a at the end of `load_collections`, which `entering_map` calls for every level,
    `marathon2.cpp`:689) turns it on again, logged `[c3d] play: on again (collections changed)`, and resets the run.
    Diff frames keep M0's form (the real frame is SW's replay) and come first. Counters: `[c3d] play` once a second
    beside `[c3d] frame stats` and a final `[c3d] play final` line (`frames c3d sw overflow scan=landscape/static/other/
    bitmap/wrap/flash discard=unsup/ring/cmdbuf/gx/begin/tex skip=down/gate/size/pref/config offs ons max_run sentinel
    play_checks`). **`play_check N`** (new key, default 0): every N-th C3D play frame, `dest` is copied to scratch A
    before the readback, the recording is replayed in SW over it afterwards, and A vs `dest` is logged as a `DIFF …
    class=play` line (`Coverage::All` with clear 0; both start from the previous frame, so `empty` is only black): the
    play path's own pixels, copy and pitch included. The engine judges nothing: `c3d-diff.py` gates the `play` lines
    (round 1 finding 2).
  - *GX queue and command buffer.* A play frame is a view frame (promotions ≤ 8 + LUT 1 + clear 1 + split 1 + readback
    1 = 12, + `C3D_FrameEnd` 1 ≤ 24: unchanged). More draws per frame; the reserve is per draw. A tinted draw adds the
    blend words; `view_frame_words`, `draw_words` and the draw reserve are re-measured over the acceptance
    (`kDrawReserveWords` = 2 × the worst first-draw words) and recorded; rings' high-water over the films (2 048 /
    4 096 kept unless a film refuses, then doubled and recorded).
  - *Zones.* Play frames: the record + scan + replay + readback run in **`c3d_draw`**; a SW redraw after a discard or a
    scan reason runs where SW always runs (`render`, the caller's zone). No new zone.
  - *Colour gates* (**user decision 2026-11-06**, Review log "M4 pre-implementation, gates"). 8.3's rows predate
    M2's neighbourhood rule. **sprites, weapons**: non-edge coverage mismatches ≤ **0.5 %** of A's covered
    pixels; M2's neighbourhood rule with M3's lit numbers (≥ 98 % within Δ ≤ 4, `mean_nb` ≤ 1.5). A rectangle's texel
    choice is SW's affine step reproduced to float precision (host test above), so the neighbourhood rule absorbs
    the residual off-by-one texel as for walls. **tinted**: 8.3 as written, strict (≥ 98 % Δ ≤ 3, coverage ≤ 0.5 %
    `NotClear`): the colour depends only on the destination and the factor. **frame**: `Coverage::All`, M2's
    neighbourhood rule at 8.3's numbers (≥ 97 % within Δ ≤ 4, no mean gate; strict printed); 8.3's "coverage ≤ 0.5 %"
    is carried by the per-class checks (a frame check has no coverage notion once tinted pixels carry alpha 0).
    **play** (`play_check`): as frame.
  - *The Invisibility Test plugin* (4.3; `scripts/3ds/tests/plugins/Invisibility Test/`, `run-azahar.sh
    `--invisibility-test`). `Triggers.init` and `Triggers.idle` keep `Players[0].invisibility_duration` in **[1 500,
    2 000] ticks** (`lua_player.cpp`:2293): at most `kINVISIBILITY_DURATION` (70 s = 2 100, `player.cpp`:191-198),
    so the transfer mode is `_xfer_invisibility` (k 15), and never under 10 s, where the weapon flickers to `NONE`
    (`player.cpp`:1993-2000; round 1 minor 6). The weapon takes the player's transfer mode (`render.cpp`:1104-1105,
    `player.cpp`:1985-1991), so **every frame has a tinted weapon** (the gate's content); plus, best effort, one `"minor invisible compiler"` and one `"major invisible
    compiler"` (`_monster_is_invisible` / `_subtly_invisible`, `monster_definitions.h`:572-630) created 2 WU ahead of
    the player (`Polygon:find_polygon`, `Monsters.new`, `lua_monsters.cpp`:1092) for tinted **sprites** (recorded,
    not gated: `c3d_tinted_rects` by class in the log). Why: the player's own invisibility is deterministic and puts a
    tinted rectangle on every frame; no bench spot has an invisible monster.
  - *Keys* (`core/config.*` + host test): `play 0|1` (default 1), `play_check N` (0..100000, default 0), `rect_bias
    N` (0..65536, 1/131072 texel; default 1). Printed in the `[c3d] debug-c3d:` line. **The M1-M3 configs get `play
    0`** (round 1 minor 7), so their re-runs on the M4 binary are the regressions they were; play is exercised by the
    `m4-*` configs.
  - *The diff configs* (`m4-diff`, `m4-tinted`, `m4-film*`) carry **`play 0`** (code review minor 6): their
    non-diff frames stay SW, so a diff log shows only checks; play mode has its own runs (`m4-play*.cfg`, incl.
    `m4-play-film.cfg` over the L01 film). **Sprites are gated on L0 and the films only**: the L6 and L20 bench
    spots have no sprite on screen (`--min-covered` refuses an empty class there), so those gate weapons and frame.
  - *Configs* (every software-backend run with VRAM promotions needs `gxq_budget 3`, the Azahar fault of M3; each
    `m4-*.cfg` says so): `m4-diff.cfg` (`diff 1`, 20 checks, `classes sprites,weapons,frame`, lit, `diff_tol 4`),
    `m4-tinted.cfg` (`diff 15`, `classes tinted,frame`, `diff_tol 3`: with the player invisible every weapon is
    `kTinted`, so `weapons` would be empty), `m4-film.cfg` / `m4-film-l08.cfg`
    (`diff 2`, 200 checks, `classes sprites,weapons,tinted,frame`), `m4-play.cfg` (`diff 0`, `play 1`,
    `play_check 30`), `m4-present-diff.cfg`, `m4-off.cfg` (`reserve_extra 1000000`: every play frame discards
    `cmdbuf`; with `--load-test`, which changes level at 10 s, the off/on lines must appear).
  - *`c3d-diff.py`.* `--colour` accepts `sprites,weapons,tinted,frame,play`; `--strict CLASS[,CLASS]` gates those
    classes on the strict figures (tinted); `--nonedge-pct` per run as today; frame/play lines have no band.
    **`--min-covered N`** (round 1 finding 1): per gated class, A's covered pixels summed over the run ≥ N **and** at
    least `--min-covered-checks K` checks with `covered_A > 0`, so a run without sprites on screen fails instead of
    passing on `common == 0` (used for sprites, weapons, tinted: N = 2 000 and K = 10 on a bench spot, N = 20 000 and
    K = 100 on a film; K is not every check: a weapon is still being raised on the first frames of a level). New `play LOG`
    command: tabulates the `[c3d] play final` line; exit 1 on `sentinel` > 0, no C3D play frame, `--min-play-checks
    N` not met (so `play_check 30` producing no check fails), **any** `class=play` line under the frame gate (each
    check on its own; the pooled figure is printed: code review finding 1), or
    (with `--expect-offs N`) a different number of offs/ons.
  - *Host tests* (8.4): RLE conversion (every kind, padding, `first`/`last` at 0 and `H`, an empty column, tiling of
    a non-square image vs tex3ds byte for byte when present); `rect_setup` vs the transliterated SW clip on a grid
    (screen sizes, clips on each side, both flips, `dx`/`dy` of 0); the texel agreement at pixel centres vs SW per ε
    (printed; asserted for the default: 100 % in exact arithmetic, ≥ 99.8 % in the float24 model over the grid);
    SW's column loop transliterated (trim (i)-(iii), (iii)'s read modelled as the next header's high byte, 0 past the
    last column) vs the mask's coverage: band and non-band differences printed, non-band ≤ 0.1 % of the common
    pixels asserted (measured 0.057 %, all from runs of a few rows); `shade_lut_value(0, j) == j`; the
    tinted factor `(v·8k)/255` vs `round(v·c/255)` (Δ ≤ 1 asserted); the new keys; the cache with `S ≠ T` and A8.
  - *Accept* (Azahar, non-authoritative; both backends, the software one at `gxq_budget 3`). `m4-diff.cfg` on
    L0/L6/L20 and `m4-film*.cfg` pass the sprite/weapon/frame gates (`--min-checks` 20 / 200, `sentinel` 0);
    with `--min-covered` for sprites and weapons; `m4-tinted.cfg` (L0, plugin) passes the tinted gate with
    `--min-covered`; in play mode (`m4-play.cfg`) the
    scripts `12-sprite-check.txt`, `06-gameplay.txt`, `07-suspend-menus.txt`, `13-bottom-screen.txt`,
    `14-terminal.txt` run to the end with C3D play frames > 0, every `class=play` check within the frame gate,
    `sentinel` 0; `m4-off.cfg`
    shows the off and on lines; `m4-present-diff.cfg` `present_diff` 0, the restore counters 0 and `blend_restores`
    > 0; M3's lit gate (`m3-diff.cfg` L0), M2's full-bright gate (`m2-diff.cfg` L0), M1's walls
    coverage, M0's calibration and P1 scripts 08, 22 unharmed; the OFF builds' `objdump -d` unchanged and `nm` clean.
    **Recorded, not gated:** a C3D-vs-SW bench (`perf-bench.sh` L0/L6/L20 @30, `m4-play.cfg` vs `renderer sw`):
    readback mode is not meant to be faster (0.5); the play counters (frames C3D / SW by reason) per level; the
    films' cache and ring high-water marks; command words.
  - *Not in M4:* landscapes, static (rectangles and polygons), eviction hardening (M5); direct mode (M6).

- **Console (2026-11-07): short check passed** (`PERF.md` "Stage 10 M3-M5 — console, short checks": sprites 99.51 %
  with 78 non-edge (Azahar 79), weapons 100 %, tinted exact with `blend_sets` = `blend_restores` = 40, play mode on
  L20 780/780 C3D with 26 checks at 99.445 %, HOME / lid close / HOME > Close in play mode clean).
- **Status (2026-11-06): M4 implemented and accepted** (`PERF.md` "Stage 10 M4", Azahar — non-authoritative;
  commits `f389f85a5` code, `2de08146a` review fixes, then the record; critic: spec REVISE then APPROVE round 2,
  code REVISE then APPROVE round 2). Host tests 2 617 932 checks, 0 failures (`rect_setup` = SW; texel agreement
  100 % in double/float32, 99.85 % in the float24 model, **UNVERIFIED** on PICA). On both Azahar backends: sprites
  pass on L0 and both films (L6/L20 show none: Review log "M4 acceptance, sprite gate"), weapons 100 % within
  everywhere, the full frame ≥ 99.5 %, tinted **exact**; play mode passes scripts 06, 07, 12, 13, the L01 film and
  Classic L20, each play check on its own (worst 98.52 %); script 14 runs all landscape (0 C3D frames); the off
  test turns off twice and on once (`offs 2 ons 1`); present diff 0; M1-M3 gates unchanged (`view_frame_words`
  1304 on M3's diff: no word added without rectangles). Command words up to 3 432 per view frame, draw reserve
  564 holds, rings' high-water 1 440 / 2 154 of 2 048 / 4 096. Bench: L0/L6's bench spots are all landscape (SW
  frames); L20 busy **+22 %** in play mode (readback mode is no speedup, 0.5). Console: `NEXT-SPRINT.md` "M4
  console check, short version" (PICA's float24 texcoords on rectangles, the constant-colour blend, play mode
  across HOME/suspend, the readback cost).

### M5 — landscapes, static, eviction, fallback hardening
- Accept: diff `landscape` ≥ 98% Δ ≤ 2 (L6); `static` written-pixel density
  within ±2 points of SW; `vram_cap 512` and `fail_every 50` runs pass,
  evictions/discards counted; first-10-frames upload counts logged; 5 films ×
  3 min `diff 30` within tolerance. Gate **G2-a**.
- **M5 specification (revision 1 of 2026-11-07, before code; Review log "M5 pre-implementation", round 1).** Where the lines
  above, 1.5, 2.2, 3.2-3.3, 4.2-4.3, 8.1 and 8.3 and the code disagree, this block rules. Citations are the code as of
  `12550c9be`. **No engine edit.** Kickoff `KICKOFF_M5.md`. Two sessions: (1) the spec, landscapes and static on diff
  frames; (2) the play scan, `fail_every`, the level-start counts, the configs, the films, the acceptance and G2-a.
  - *Checked against the kickoff and the code at HEAD.* (a) `tex_convert` has no landscape path: `tex_supported`
    takes square POT bitmaps in [128, 512] only (`core/tex_convert.h`), and `texcache_get` refuses every non-rectangle
    kind whose texel is not 4 bytes (`back/texcache.cpp`), so the A8 `Mask` exists for RLE rectangles only in practice.
    (b) `fail_every` is parsed nowhere (`core/config.*`: only `vram_cap`, `linear_cap`, `promote_max`). (c) The cache
    already logs the first 10 frames after a generation change, as **KiB summed over every level** (`TexStats::
    gen_window_bytes`, `stats_line` "gen-window KiB"), not per level and without texture counts. (d) `vram_cap 512` has a
    config since M2 (`m2-vram512.cfg`, diff only). (e) The allocator takes one wrap for both axes (`C3dAllocator::alloc`:
    `C3D_TexSetWrap(t, wrap, wrap)`; `texture.h`:169 takes `wrapS, wrapT`). (f) **`_solid` polygons** (smear,
    `render.cpp`:985-987) are drawn by **no** SW mapper: both precalculation switches end in `VHALT_DEBUG` (a no-op in
    release, `scottish_textures.cpp`:138-142) and every bit-depth switch in `fc_assert(false)` (a no-op without
    `DEBUG_FAST_CODE`, `csalerts.h`:97-103). M4 sent such frames to SW (`PlayScan` `other`); M5 skips them (below).
  - *SW's landscape, exactly* (`scottish_textures.cpp`:1102-1170, lines :257-264 of `low_level_textures.h`). Every
    landscape goes through the **horizontal** mapper (`texture_vertical_polygon` punts `_big_landscaped` to it, :436-439),
    so its coverage is the floor rule (`accept_polygon(..., Orient::Horizontal)`, M2's fan/spans/ties). `R` =
    `texture->width` rows (`row_addresses`), each `texture->height` texels; `bits = NextLowerExponent(height)`. With
    `LandscapeOptions` `o = View_GetLandscapeOptions(ShapeDesc)` (`ViewControl.h`:81-106; defaults HorizExp 1, VertExp 1,
    OGL_AspRatExp 0, VertRepeat **false**, Azimuth 0): `first = (landscape_yaw + o.Azimuth) << (bits + HorizExp + 16 − 9)`,
    `hpd = (half_cone << (1 + bits + HorizExp + 16 − 9)) / standard_screen_width`, `vpd` the same with VertExp (`_fixed`,
    int32). Pixel `x` of a line reads texel `s(x) = floor(V/65536) mod 2^bits`, `V = first + x·hpd` (the `<< free_bits`
    and `>> (32 − bits)` of :1164-1165 and :245-262 keep bits 16..16+bits−1 of V; the 2^32 wrap is a multiple of
    2^(16+bits)). Line `y` reads row `r(y) = R − 1 − PIN(k′ + (R >> 1), 0, R − 1)`, `k = (y − hh − dtp)·vpd >> 16`
    (short `y0`, arithmetic shift: a floor), `k′ = k`, or with VertRepeat `k′ = ((k + h2) & (2h2 − 1)) − h2`, `h2 =
    (height >> OGL_AspRatExp) >> 1`. Shading: `_SHADELESS_BIT` → `shading_tables` itself (table 0), else
    `calculate_shading_table(…, depth 0, ambient FIXED_ONE)`, the top table for any `M` (host-asserted, below). Every
    pixel is written (no transparency test).
  - *Landscapes on the GPU* (new `core/landscape.*`, plain C++, host-tested; the front end calls it). Affine (w = 1),
    attributes evaluated at `(X − ½, Y − ½)` as M2's `uv_half` (the pixel centre then interpolates to SW's value at the
    integer pixel). **u** (texels along a row) `= V(X − ½)/65536 − 2^bits·m`, `m = floor(min over the polygon / 2^bits)`
    (whole repeats only, so `GPU_REPEAT` keeps the phase; the shift keeps float24's range small). **v** (the row) `=
    (R − (R >> 1)) − f(Y − ½) − ε + δ`, `f(y) = (y − hh − dtp)·vpd/65536` (round 1 finding 1: `R − (R >> 1)`, not
    `R >> 1`, which is wrong for odd R) — with δ = 0, `floor(v) = R − 1 − (k + (R >> 1))` whenever `frac(f) + ε ∈ (0, 1]`;
    f is a multiple of 1/65536, so **ε = 1/131072** (M4's `rect_bias` value) is exact for every
    line. **u** gets the same ε (`V/65536` is a multiple of 1/65536; an exact integer must not round down). GPU
    coordinates `u0 = (u + ε)/S`, `v0 = v/T`, `S = 2^bits`, `T = pot(max(R, 8))`. **Padding** (3.2, Review item 10): rows
    `R … T−1` repeat row `R − 1`, and the sampler is **`GPU_REPEAT` on s, `GPU_CLAMP_TO_EDGE` on t** (`texture.h`:169,
    `enums.h`:35-41): `v ≥ R` reads row R − 1 (SW's PIN at 0 in `k`), `v < 0` reads row 0 (the PIN at R − 1), so the
    clamp *is* SW's PIN and no split is needed without VertRepeat. **The row offset δ** (found by the host test before
    code): SW's row positions are often exact integers or a few 1/65536 from one (the horizon line has `f = 0`; "round"
    `vpd` such as 52 428 repeat fractions near 0 and 1), and float32 corners at ~256 rows carry ~1/65536 of error, so
    `ε` alone gives only **96.6 %** of lines right in float32 (86.8 % in the float24 model). Rows are constant per line,
    so any affine v whose value at each line's centre lies in `[r(y), r(y) + 1)` is exact: per polygon (per band with
    VertRepeat) `δ` = the middle of `[max(r − g), min(r + 1 − g))` over its unpinned lines (`g` = the δ = 0 value; pinned
    lines accept any `δ ∈ (−1, 1)`), computed on the CPU from the polygon's line range (`landscape_row_offset`, ≤ 240
    integer steps). Measured on the host grid: **100 % double, 99.996 % float32, 98.16 % float24** (bands of 16 lines
    gave 99.81 % in float24 in a first experiment; not done: the split costs vertices and the float24 figure is **UNVERIFIED** on PICA,
    section 10). Columns keep ε (a centred offset over a whole polygon's width gains nothing; ε: 99.992 % float32, 99.04 %
    float24 on the host grid). **VertRepeat** (M1's landscapes; not in Marathon 2):
    `k′` jumps where `k + h2` crosses a multiple of `2h2`; the polygon's line range is cut into **bands** of constant
    `n = (k + h2) div 2h2` (`landscape_bands`), each with `v = (R − (R >> 1)) − (f − 2h2·n) − ε + δ`; a polygon with one band is
    drawn as above, one with more is drawn as **SW's own spans** (M2's span path, `sw_spans`), its merged quads split at
    the band rows, so the coverage stays SW's exactly and no clip vertex is computed. Supported landscape bitmaps
    (`landscape_tex_size`): not RLE, `height` a power of two in [8, 1024], `width` in [1, 1024], `S·T·4 ≤ 1 MiB` (**corrected in the implementation**: `T` ≤ 512 per part, 2 MiB per part; Review log "M5 implementation, landscape size");
    anything else is unsupported (counted `c3d_land_unsupported`, a scan reason `landscape` in play).
  - *Landscape texture.* New `TexSource::landscape`; conversion `tex_convert_landscape(bitmap, tables, kind, S, T, out)`:
    memory row `t < R` = `row_addresses[t]`'s first `S` texels, rows `t ≥ R` = row `R − 1`, tiled by `tile_index` /
    `tile_memory_row` (M4: any S, T). Kinds **`Normal`** (top table RGB, A as 3.2) and **`Shadeless`** (table 0:
    infravision); both RGBA8. **Not RGB565**: 5-bit red/blue round by up to 4 (`255/31 ≈ 8.2` per step), which fails
    8.3's Δ ≤ 2 by itself. A Marathon 2 landscape (270 × 512 → 512 × 512) is **1 024 KiB** (**corrected**: 540 × 1 024, two parts of 2 048 + 128 KiB): one per level, plus its
    infravision copy while the player uses infravision. Beside ≤ 26 (texture, table) pairs at L0's spot (M0's null
    table: walls 64 KiB each, sprite frames up to ~32 KiB) the working set can pass the VRAM cap (3 584 KiB) with the
    infravision copy (round 1 minor 9: ~3.7 MiB), so evictions happen there, as designed (LRU; the LUT and the noise
    are outside the cache). Promoted like any texture (one 1 MiB `TextureCopy`). A 1 MiB conversion when infravision
    toggles is a mid-play burst: every frame converting ≥ 512 KiB outside the level-start window is logged (first 8,
    `[c3d] tex: burst …`). The wrap becomes **part of the key**: `TexKey::clamp` (bool) → `TexKey::wrap`
    (`Repeat`, `Clamp`, `Landscape` = repeat s, clamp t), passed to `TexAllocator::alloc` for both pools as M4's clamp.
  - *texcoord1 of a landscape.* `L = sw_shade_index(M, 0, FIXED_ONE)` (host-asserted 255 for `M` in
    [`FIXED_ONE`, 2^20]) → `(0.5/256, (L + 0.5)/256)` through the LUT's column 0 (M4: exact); shadeless →
    `(255.5/256, 255.5/256)`. Landscapes are never `wrap` or `flash` (depth 0): the scan's polygon checks skip them.
  - *SW's static, exactly* (`low_level_textures.h`:590-645; `scottish_textures.cpp`:221-225, :524-545, :738-755,
    :862-865). Polygons: `texture_horizontal_polygon` punts `_static` to the **vertical** mapper (:221-225), so every
    static polygon has the wall rule's coverage (`Orient::Vertical`) and, on a horizontal surface, wall texcoords
    from the floor's origin/vector; the texel is read only for `_TRANSPARENT_BIT` bitmaps (`check_transparent`).
    Rectangles: always `check_transparent` (RLE index 0), M4's coverage. Per written texel position: `if (seed >=
    drop) *write = seed ^ (seed << 8)` (32 bit: R = seed's high byte, G = high ^ low, B = low, **top byte 0**), then
    the Galois step `seed = (seed >> 1) ^ (seed & 1 ? 0xb400 : 0)`; `drop = (uint16) transfer_data` (`render.cpp`:796-
    831: 0, 0x8000, the fade phase, 0x8000 ± 0x6000·sin); the seed (`texture_random_seed()`) runs on across
    primitives and frames and advances only on non-transparent texels.
  - *Static on the GPU* (4.3, revised). **Noise texture**: **256 × 256** RGBA8 (256 KiB; round 1 finding 2: all 65 535
    states, so its density is SW's period density; 9.1's "shade LUT + noise" grows from 128 to 320 KiB, VRAM total ≤ 5 166
    KiB), laid out **down columns** like SW's walk (texel `(x, y)` = state `256x + y` after seed 1; minor 6), `RGB = (hi,
    hi ^ lo, lo)`, **`A = min(255, hi + 1)`** (minor 8: no floor of 2 is needed, the sentinel needs alpha 0x01 *and* its
    RGB; the shift by one keeps every byte level apart: a floor merges the lowest levels); nearest, **repeat**. Built on the first static draw of the run (linear, flushed: a CPU fill inside
    the frame, as a texture conversion) and promoted to VRAM in a later frame by the LUT's own state machine (`back/
    shade.cpp`, generalised to two resident textures; under `gxq_budget 3` neither leaves linear memory). **Alpha
    test** `GEQUAL ref`, `ref = min(255, (drop >> 8) + 1)`: a written texel needs `hi ≥ drop >> 8` (SW: `seed ≥ drop`;
    they differ only for `hi == drop >> 8`, `lo < drop & 0xff`: at most 255 of 65 535 states, **0.39 points**, and for
    `drop ≥ 0xFF00`, where `hi` 254 and 255 share A 255: ≤ 0.78 points of a nearly invisible fade; the whole-texture
    density, host-asserted); `ref ≥ 1` rejects transparent texels (A 0). A window of the texture vs a run of
    SW's states differs by sampling noise only (host simulation, below). **TEV** (`texenv.h`:49-96): stage 0 RGB `REPLACE(TEXTURE1)`;
    alpha `MODULATE(TEXTURE0 α, TEXTURE1 α)` for transparent bitmaps (unit 0 = the **`Mask`** kind, A8 0/255, so the
    product is the noise alpha or 0), `REPLACE(TEXTURE1 α)` for the others; stage 1 `REPLACE(PREVIOUS)` both.
    **texcoord1** = `((X + ox)/256, (Y + oy)/256)` at the vertex `(X, Y)`, `(ox, oy)` ∈ [0, 256)² from a front-end
    xorshift32 advanced once per static primitive (never `texture_random_seed()`: SW's replays restore it, M0).
    **w**: 1 for rectangles and for polygons whose bitmap is not transparent (no texcoord0 is sampled: they draw the
    coverage only: the noise is screen-locked, as SW's); transparent static **walls** keep the wall's w and texcoord0
    (`wall_attrs`) and the Mask (minor 5: `texcache_get` and the cache accept the A8 `Mask` kind for M2's square wall
    bitmaps, whose conversion `tex_convert` already has; host-tested): texcoord1 is then interpolated perspective-correctly (the noise follows the surface:
    4.3's listed approximation). A transparent static polygon from `texture_horizontal_polygon` (SW: wall texcoords
    from a floor's data) is **unsupported** (`c3d_static_unsupported`; none in Marathon 2's maps is expected, counted).
    Static requires `textures 1` and `lit 1` (unit 1 is set up and restored by the lit frame), else unsupported. If the
    noise texture cannot be allocated (it is not a cache entry) the frame is discarded `tex` (logged once, retried in
    the next frame that needs it).
  - *Unit 1 and TEV, the state machine* (`back/draw.cpp`; M3's rule "every unit and stage enabled is put back").
    A lit frame starts in mode **Lit** (M3: LUT on unit 1, stages 0-1 as 4.2, alpha test `GEQUAL 128`). A draw's mode is
    a property of its key: `kStatic` → **Static(ref, masked)** (`masked` = the Mask on unit 0), else Lit. `view_polygon`
    sets it **between draws** with M4's `set_blend`, after `issue_pending()`: Lit → Static binds the noise to unit 1
    (`__real_C3D_TexBind(1, noise)`, never through the wrapper: `tex1_foreign_binds` stays 0), writes stages 0 and 1 and
    the alpha test (`static_sets`); Static → Lit binds the LUT again, rewrites stages 0 and 1 as `view_begin` does
    (one shared function) and the alpha test `GEQUAL 128` (`static_restores`); Static → Static with another ref or mask
    rewrites stage 0's alpha and the test. The ref and the mask are in the key (`transfer_data`, the class and the
    bitmap's flag through the texture), so equal keys never need a switch. `restore_state` is unchanged in what it
    restores (stage 1 from the saved copy, `shade_unbind_unit1`, the present sets its own stage 0, alpha test and blend
    before every draw, `present.cpp`:339-340) and runs for every lit frame, so a frame **ending** in Static leaves unit
    1 unbound and stage 1 at C3D_Init's state exactly as a Lit one (`unit1_left_bound`, `tev1_not_init`,
    `tex1_restore_mismatch` stay 0). Counted `static_frames_ended` (frames whose last draw was static). Proof: the
    present diff with static on screen (`m5-present-diff.cfg`, a static film stretch or the Static Test plugin below)
    → `present_diff` 0 and the restore counters 0 with `static_sets` > 0.
  - *Front end* (`front/c3d_rasterizer.cpp`). `polygon()` dispatches on the transfer first: `_big_landscaped` →
    `landscape(p)` (Horizontal acceptance, `landscape_attrs`, the `Landscape` texture), `_static` → `static_polygon(p)`
    (Vertical acceptance), **`_solid` → skipped** (SW draws nothing; counted `c3d_solid_skipped`, not a discard),
    else M2-M4's path. `texture_rectangle`: `_static` → the Mask kind, M4's quad, texcoord1 = noise, key class
    `kStatic`. Counters (`DIFF summary`, `play final`): `c3d_land_polys`, `c3d_land_spans`, `c3d_land_unsupported`,
    `c3d_static_polys`, `c3d_static_rects`, `c3d_static_unsupported`, `c3d_solid_skipped`.
    `FilterRasterizer::c3d_excluded` gains the unsupported landscapes and statics (excluded on both sides of a
    check, as M2's unsupported walls).
  - *Diff checks* (8.1). **`landscape`**: as floors (clear 0, `Coverage::TopByte`: SW's table pixels carry 0xFF).
    **`static`**: three replays — A = SW, B = C3D (both over clear 0, `Coverage::NotClear`: SW's top byte is 0, its
    value never 0 since the LFSR state is never 0), and **C = SW with every static primitive's `transfer_data`
    forced to 0** (the filter's `static_cover` edit), i.e. SW's coverage: every texel position SW could write. New
    `diff_static(A, B, C)` (core, host-tested) prints `[c3d] DIFF frame=… class=static cover=|C| written=|A|/|B∩C|
    density=dA/dB out=e/n` (`out` = B pixels outside C, split by C's edge band). **`frame`**: landscapes are now
    **included** (8.3); static stays excluded on both sides (8.3; `frame_excluded` → `kStatic` only). **play**
    (`play_check`): a frame with static is compared with **C's pixels excluded** (computed by one more SW replay
    only on such frames; `diff_images` gains an optional exclude mask; counted `static_px` on the line): the
    noise differs per pixel by design. **This widens what the play gate excludes** (M4's play checks covered every
    pixel; their static frames were SW): it applies 8.3's "full frame: static excluded", which the user already set
    for the frame check, to the play check. **Decided by the user 2026-11-07** (round 1 minor 3; asked with the first
    L01 run's counts: 9 of 155 checks, 2 795 of 14.5 M pixels left out).
  - *Colour and density gates.* **landscape**: 8.3's row **as written** (coverage non-edge ≤ 0.1 % of A's covered;
    **strict** ≥ 98 % within Δ ≤ 2, no mean gate; the neighbourhood figure printed): the texel choice is SW's to the
    exact 1/65536 (host test: 100 % in double and float32), full bright, so the strict figure should hold; a new key
    **`landscape_tol N`** (default 0 = `diff_tol`) prints landscape lines at their own tolerance, so one run gates
    landscapes at 2 and the frame at 4 (`c3d-diff.py` compares `--tol` with the gated classes' lines only).
    **static** (8.3 "density ±2 points"; the measurement is new, the number is not): **per run, pooled** over the
    checks, `|ΣB∩C/ΣC − ΣA/ΣC| ≤ 2` points with **ΣC ≥ 40 000** (`--min-covered 40000`); per check printed, not gated
    (round 1 finding 2; host simulation of SW runs of n consecutive states against windows of the 256² texture: σ of
    the difference 2.2 / 1.1 / 0.5 / 0.25 points at n = 1 024 / 4 096 / 16 384 / 40 000, P(|Δ| > 2) 36 % / 5-10 % / ≤
    0.1 % / 0, mean ≤ 0.12); `out` non-edge ≤ 0.5 % of ΣC (M4's sprite coverage number, as rectangles carry most
    static). **frame**, **play**: unchanged (M4: ≥ 97 %
    within Δ ≤ 4, neighbourhood). If the strict landscape gate fails for a reason the design accepts (texel boundaries),
    the neighbourhood rule is the fallback **only on the user's decision** (a tolerance change).
  - *Play mode* (M4 (2)'s scan). `landscape` and `static` stop being reasons for drawable primitives: they remain the
    names of the **unsupported** cases (`landscape_tex_size` false; a transparent static from the horizontal entry;
    static without `lit`). `_solid` is no longer `other`. **Still SW, and why:** `other` (a tinted polygon, a tinted `k ≥
    32`, any other transfer mode: none in Marathon 2's content, kept as the guard), `bitmap` (sizes C3D refuses),
    `wrap` (SW's own output is undefined there: M3), `flash` (`M > kFlashMax` on a lit wall/floor: SW's `SHRT_MAX` clamp,
    never in Marathon 2). None is taken now; each stays a counted reason. Before the scan (M4 (0)), see-through liquids
    (`software_alpha_blending` on: SW blends, 4.3) still send every frame to SW (`skip:pref`; minor 9). Diff frames keep M0's form.
  - *`fail_every N`* (new key, 0..100 000, default 0; the "Runtime config" list). **Every N-th `TexCache::create()`
    fails before allocating** (counted `forced_fails`, and `discards_tex` as any create() failure), so the draw that
    needed the texture discards the frame (`tex`) and SW redraws it from the recording (M4's discard path); the next
    frame retries (the counter has moved on). It proves: (i) every forced failure is one counted discard and never a
    wrong picture (`sentinel` 0, every play check in gate); (ii) with `fail_every 1` every frame that needs a new texture
    discards, ten in a row turn play off and a level change turns it on (`--expect-offs` as M4's off test); (iii)
    nothing leaks across a discard: a new **cache audit** (`TexCache::audit()`, in the final `[c3d] tex` line and on
    every generation change) recomputes each pool's bytes from the live and retiring slots and the deferred-free list
    and compares them with `used_` (`audit ok` / `audit MISMATCH`), and a frame's other creates stay valid entries.
    Forcing the allocator instead (soft failure) would be absorbed by M2's evict-and-retry (`kEvictAfterAllocFail`) and
    prove nothing new; real allocation failures are already counted (`alloc-fail`, 3 on M4's L01 film). **Run**
    (minor 4): `fail_every 50` over the L01 film in play mode, whose M4 run made 1 463 creates (≈ 29 forced failures);
    the gate needs `forced_fails ≥ 10` and as many `tex` discards beyond the real ones.
  - *Level-start counts* (3.3 "the level-start burst"; section 10 item 10 decides pre-warming on the console). Per
    generation: the KiB and textures converted and the promotions pending at the start of each of the first 10 view
    frames after the change, and those frames' GX high-water, printed once as `[c3d] tex: level start gen G: KiB
    k0..k9 textures n0..n9 pending p0..p9 gx_hw H` (deterministic counts: authoritative in Azahar). The summed
    `gen-window KiB` stays.
  - *Eviction runs.* `vram_cap 512` (8 walls of VRAM; a landscape never fits and stays linear: `promote_pending`
    needs `bytes ≤ cap`, `core/texcache.cpp` `create`): a play run over a film with play checks (evictions,
    promotions and discards counted; every check in gate). **Host GPU only**: under the software backend `gxq_budget
    3` keeps every texture linear, which makes `vram_cap` inert (the run would be the plain `-swbe` run again).
  - *GX queue, command buffer, rings.* No new GX op per frame (the noise's one promotion uses the LUT's rule: it
    needs the frame's own entries free after it). A mode switch adds a texture bind, two TEV stages and the alpha test
    to the next draw's state flush: `draw_words`, `view_frame_words` and the first-draw words are re-measured over the
    acceptance, **`kDrawReserveWords` = 2 × the worst draw measured** (564 today; raised and recorded if a static draw
    exceeds 282). Frames without landscapes and static add no word: M3's diff stays at `view_frame_words` 1304 (the
    regression check of M4). Rings (2 048 / 4 096) unchanged unless a film refuses (then doubled and recorded); the
    high-water is logged on the films (landscape span quads only with VertRepeat).
  - *Zones.* No new zone: landscape and static draws are `c3d_draw`; the noise build and the landscape conversion
    are `c3d_tex` (M2).
  - *Keys* (`core/config.*` + host test): `fail_every N`, `landscape_tol N`. Printed in the `[c3d] debug-c3d:` line.
    The M1-M4 configs are unchanged (their runs are the regressions; `m4-*` play configs now draw landscapes and
    static, which is the point of their re-run).
  - *The Static Test plugin* (`scripts/3ds/tests/plugins/Static Test/`, `run-azahar.sh --static-test`; pattern of M4's
    Invisibility Test): best effort, it places a monster 2 WU ahead of the player that teleports in and out (fold
    in/out: `_static` rectangles with the fade drop) every few seconds, so a bench spot shows static rectangles for
    the `static` gate and the present diff; if the Lua API cannot produce it, the static gate runs on the films only
    (recorded).
  - *Films* (5 × 3 min, `diff 30`, classes `landscape,static,sprites,weapons,frame`). Chosen by a screening run of
    candidates in play mode on the M4 binary (its per-second `scan=landscape/static` counters show what each film puts
    on screen): **L01** (2 646 landscape and 269 static frames in M4) plus four others with landscapes, static or
    eviction pressure, named with their counts in the session's record; `--min-covered` per class is set from a first
    run of the M5 binary (a class a film never shows is not gated on that film, and that narrowing is recorded).
  - *Host tests* (8.4): `landscape_attrs` vs a literal transliteration of :1102-1170 + :245-262 on a grid (yaw 0..511,
    `dtp` ±60, `half_cone` from 40 to 80, screens 400 × 240 and 640 × 320, R ∈ {270, 135, 512}, height ∈ {256, 512},
    HorizExp/VertExp 0..2, Azimuth, VertRepeat on/off): the texel (row and column) at every pixel centre agrees 100 %
    in double and ≥ 99.99 % in float32 (asserted; rows with δ), the float24 model printed; `landscape_bands` vs the per-line `k′`; the
    padding conversion (rows past R = row R − 1, tiling vs `tex3ds` when present); `sw_shade_index(M, 0, FIXED_ONE) ==
    255`; the noise generator = SW's LFSR step, its texels' RGB through the readback's ARGB conversion = SW's
    `randomize_vertical_polygon_lines_write<pixel32>` (minor 6), the column layout, the whole-texture density for every
    `drop` vs SW's `(65536 − drop)/65535` (|Δ| ≤ 0.4 points asserted, ≤ 0.8 for `drop ≥ 0xFF00`), the window-density simulation (printed); the
    Mask conversion of a square wall bitmap; `diff_static`; `diff_images` with an exclude mask;
    the cache with `wrap Landscape`, `fail_every` and `audit()`; the keys.
  - *Configs* (each software-backend run `-swbe` with `gxq_budget 3`): `m5-diff.cfg` (`diff 1`, 20 checks, `classes
    landscape,static,frame`, lit, `diff_tol 4`, `landscape_tol 2`), `m5-static.cfg` (`classes static,frame` with the
    plugin), `m5-film.cfg` (`diff 30`, `diff_frames 0`: the run's length), `m5-play.cfg` (`play 1`, `play_check 30`),
    `m5-vram512.cfg` (play, `vram_cap 512`), `m5-fail50.cfg` (play over the L01 film) / `m5-fail1.cfg` (play, a level
    hop),
    `m5-present-diff.cfg`, `m5-bench-c3d.cfg` / `m5-bench-sw.cfg`.
  - *Accept* (Azahar, non-authoritative; both backends unless noted). `m5-diff.cfg` on L0/L6/L20: `landscape` passes
    the strict gate on every spot that shows one (**L6 required**, 8.3), the frame (landscapes included) passes M4's
    frame gate; `static` passes the density gate on the plugin run and the films that show it; the 5 films pass every
    gated class; M4's play runs (scripts 06, 07, 12, 13, 14, the L01 film, Classic L20) pass again with **landscape and
    static scan counts 0** (every such frame now C3D) and C3D play frames on L0 and L6; `m5-vram512` (host GPU) and
    `m5-fail50` pass with evictions / forced failures and discards counted and `audit ok`; `m5-fail1` + a level hop
    shows the off/on lines; `m5-present-diff` `present_diff` 0, the restore counters 0, `static_sets` > 0; M3's lit
    gate, M2's full-bright gate, M1's walls coverage, M0's calibration and P1 scripts 08, 22 unharmed
    (`view_frame_words` 1304 on M3's diff); the level-start lines logged; the OFF builds' `objdump -d` unchanged and
    `nm` clean. **Recorded, not gated:** the C3D-vs-SW bench on L0/L6/L20, the play counters per reason before (M4) and
    after, the films' cache/ring high-water marks, command words.
  - ***G2-a*** (0.5), measured at the end of the acceptance, **on the final binary, host-GPU backend**:
    (1) `R_est = (R_az / render_az) × 5.78 ms ≤ 3 ms` from M0's null-mode table (`PERF.md` "Stage 10 M0": L0 0.34,
    L6 0.62, L20 0.33 ms); null mode does not run the M1-M5 code, so it is not re-measured: **passes with a wide
    margin**. (2) `busy(readback) ≤ busy(SW) + 1 ms` on **each** of L0, L6, L20: `perf-bench.sh`, HUD + Map @30, 1 run,
    `m5-bench-c3d.cfg` (play 1, no checks) vs `m5-bench-sw.cfg` (`renderer sw`), `busy` = frame − idle; valid only if
    the C3D run's play frames are ≥ 95 % of its world frames on each spot (else the number does not measure the
    renderer and is reported as such). **What M4 already says:** L20 +3.49 ms (`c3d_draw` 8.37 + `c3d_readback` 2.18
    + `render` 1.08 against SW's `render` 8.64); L0/L6 were all landscape (SW). Expected after M5: L0 and L6 move to
    the C3D path; the readback and copy are per frame (about 2.2 ms), the draw path's share is unknown in advance (it
    holds the traversal, which SW also pays, and per-polygon work: L0 71, L6 89, L20 41 polygons at most, M0), so L0
    and L6 are **expected at or above L20's +3.49 ms** (round 1 minor 7); Azahar understates copies ~40 % (`PERF.md`
    "How to read these numbers"), so G2-b on the console is likely worse. **The prediction is a G2-a failure on its
    second half.** The gate itself is not changed. **What follows was decided by the user in advance (2026-11-07):
    continue to M6 and M9 whatever the result** (0.5); G2-a is measured and recorded all the same.
  - ***Implementation, session 1 (2026-11-07): corrections found in Azahar*** (Review log "M5 implementation, landscape
    size"). (1) **Marathon 2's landscape bitmaps are 540 rows × 1 024 texels** in this data (logged by the front end:
    `landscape bitmap 540 x 1024`), not 270 × 512 as 3.2, the kickoff and the bullets above say: padded to a power of two
    that is 1 024 × 1 024 RGBA8 = 4 MiB, over both caps. The texture is therefore cut at **row 512** into two parts:
    part 0 = rows 0-511 (1 024 × 512, 2 MiB, no padding), part 1 = rows 512 to R − 1 (1 024 × 32, 128 KiB, padded with
    row R − 1), each its own cache entry (`TexKey::part`) and draw (`DrawKey::part`); a line reads one part (rows are
    constant per line), so a polygon crossing the cut is drawn as SW's spans with its quads cut there (the VertRepeat
    machinery: a *piece* = band × 2 + part, `landscape_piece`), its own δ per piece; with Marathon 2's vertical scale
    (VertExp 1: 1.28 rows per line at 400 × 240) the cut is on screen only when the player looks up, so the fan path
    stays the common case. `landscape_tex_size` gives `S`, `T[2]`, the part count; any `S·T` up to 1 024 × 512 per part.
    Memory per level: 2 MiB (+ 128 KiB when part 1 is seen) — **twice the 1 MiB the bullets above assume**; RGB8 (3
    bytes) would make part 0 1.5 MiB and is the known option if VRAM pressure shows (not done: a new format and its
    tex3ds check). (2) The host grid gains R = 540 and 1 024: rows agree 100 % in double, **99.971 %** in float32 (the
    misses are on long pieces of R = 1 024, a size Marathon 2 does not have), 98.08 % float24; columns unchanged (the
    float32 assertion is ≥ 99.95 % for rows). (3) **Films** (screening on the M4 binary, `play 1`, 230 s each, frames
    sent to SW as landscape / static): L01 2 646 / 269 (M4's record), L02 535 / 28, **L06** 43 / 722, **L10** 3 439 / 95,
    L13 0 / 0, **L16** 234 / 196, **L21** 0 / 360: the five films are **L01, L06, L10, L16, L21**. A film at `diff 30`
    shows ΣC ≈ 11 000 (L06: 10 checks with static of 134), so the static gate runs on its own config
    (`m5-static-film.cfg`: L06, `diff 2`, classes `static,frame`); the Static Test plugin is not written (the film
    covers the class). Killed film runs print no DIFF summary: the film configs stop at `diff_frames 120`.
    (4) Smoke results (Azahar, host GPU): L6 `m5-diff`: landscapes 254 700 pixels over 20 checks, coverage exact,
    colour **exact** (max Δ 0), the frame with landscapes 99.58 % within; L06 film: static density 67.73 / 67.83 %
    (+0.10 points) over ΣC 10 818, B outside C only in the band, the noise promoted to VRAM, 10 frames ending on a static
    draw restored; L01 film in play mode: 4 650 frames, **all C3D** (M4: 2 915 to SW), 361 with static, 155 play checks,
    worst 98.63 %, 0 discards, `sentinel` 0; the level-start line logs (2 MiB in frame 0, `audit ok`, GX high-water 11).
  - *Not in M5:* direct mode (M6), the texture-cache hash (an M6 prerequisite), supersampling, stereo.
- **Console (2026-11-07): short check, one gate missed by 0.16 point** (`PERF.md` "Stage 10 M3-M5 — console, short
  checks": landscapes on L6 strict 100 %; the Look Up Test's cut path **97.84 % strict**, 0 non-edge, `within_nb`
  99.87 %; static and present counters pass, static not seen by eye; **G2-b +6.15 / +7.77 / +7.25 ms** busy on L0 /
  L6 / L20).
- **Status (2026-11-07): M5 implemented and accepted** (`PERF.md` "Stage 10 M5", Azahar — non-authoritative;
  commits `13910b169` session 1, `663d05820` and `9d6fde66f` review fixes, then the record; critic: spec REVISE then
  APPROVE round 2, code REVISE then APPROVE round 2). Host tests 2 654 258 checks, 0 failures. On both Azahar
  backends: landscapes **exact** on the host GPU wherever they are on screen (L0, L6, films L01, L10, L16; strict
  ≥ 98.99 % under the software backend), the cut between the two parts on screen with the test-only `land_cut 400`
  and the Look Up Test (exact on the host GPU; 97.72 % strict under the software backend, every miss one column late:
  Review log "M5 code review, round 1"); static density **+0.37 points** on the L06 film (host GPU, ΣC 81 730; software backend +0.49 points on ΣC 35 820, under the minimum, read by hand); sprites, weapons and the full frame (with
  landscapes) pass on all 5 films; play mode draws every frame with C3D (`scan` 0: no landscape or static frame to
  SW; the L01 film 13 431 C3D frames, M4 sent 2 915 to SW); `vram_cap 512`, `fail_every 50` (43 forced failures, audit
  ok) and `fail_every 1` (`offs 2 ons 1`) pass; present diff 0; M1-M4 gates unchanged. **G2-a fails** as predicted:
  `R_est` passes (≤ 0.62 ms), busy(readback) − busy(SW) = +2.04 / +4.88 / +3.51 ms on L0 / L6 / L20 against +1 ms;
  recorded, not deciding (user decision 2026-11-07): M6 next. Console: `NEXT-SPRINT.md` "M5 console check, short
  version" (the span path's float24 columns, `promotions_blocked` and FCRAM sampling, the level-start conversion).

### M6 — direct mode (HUD + Map)
- Touch: `screen.cpp` hook 3 (`PATCHES.md` 47).
- Accept: per-frame mode logged; direct vs readback captures with overlays on
  ≤ 1/channel; ineligible frames (5.4) readback; frame-indexed test in direct
  mode shows N or N−1 @30/@60 with `alt_load`; GX high-water ≤ 24; scripts
  07, 13, 14 pass. Gate **G3-a**.
- Prerequisite from M4 (commit review minor 4): the texture cache's lookup scans all 512 slots per new draw key
  (`core/texcache.cpp` `lookup`); a hash on `(bitmap, tables)` before direct mode, measured.
- **M6 specification (revision 1 of 2026-11-07, before code; critic round 1 applied: Review log "M6 pre-implementation", round 1).** Where the lines
  above, 1.3, 1.5, 5.3, 5.4, 9.1, 9.2 and the code disagree, this block rules. Citations are the code as of `02c6319d9`
  (the merge of `stage10-bb`, below). Kickoff `KICKOFF_M6.md`. **One engine edit, `PATCHES.md` 47** (`screen.cpp` and
  `screen_shared.h`). Two sessions: (1) the spec, the hash, the direct view frame and the composite on direct frames;
  (2) overlays, eligibility, calibration, configs, the acceptance, G3-a and the bench.
  - *B-b, decided by the user (2026-11-07): option (b).* `stage10-bb` is merged into `stage10-c3d` (`02c6319d9`;
    `PATCHES.md` 55; `zero_copy` defaults to 1): `main_surface` is a 512 KiB linear buffer that the present uploads
    without a fill (5.7). In M6 a direct frame never presents `main_surface` (below), so B serves the frames M6 leaves to
    readback (ineligible, discarded, Classic is not native and keeps the staging path). Option (a) (M6 without B) was
    rejected by the user; its cost would have been the conflict in `present.cpp` landing on whoever merged second.
  - *Checked against the kickoff and the code at HEAD.* (a) Every CPU overlay is drawn into **`world_pixels`**, not
    `main_surface`: the crosshair (`Crosshairs_Render(world_pixels)`, `screen.cpp`:1734-1741), then FPS, position,
    scores, messages and the input line into `disp_pixels = world_pixels` (:1743-1757), all **before** the world-view
    blit `update_screen()` (:1793-1804), which copies `world_pixels` (through the gamma table when
    `!using_default_gamma`, :2002-2005) into `main_surface`. So hook 3 redirects those two surfaces and skips the blit.
    (b) `DisplayText` maps its colours with **`world_pixels->format`** (`screen_shared.h`:509-510), and so does
    `DisplayTextCursor`'s fill (:541-542): drawn into a surface with alpha, a bitmap font (`sdl_font_info`, which stores
    the raw pixel, `screen_drawing.cpp`:441-445) and the cursor would get **alpha 0**. The TTF path converts back to
    RGB (`SDL_GetRGB`) and blends a glyph surface (`screen_drawing.cpp`:534-587), which over transparent black yields
    premultiplied colour (SDL's blend: `dstRGB = srcRGB·srcA + dstRGB·(1 − srcA)`, `dstA = srcA + dstA·(1 − srcA)`;
    from 0 that is "over" in premultiplied form, layer after layer). Patch 47 therefore also maps the colour with the
    overlay surface's format when the destination is the overlay surface (below); every other destination is unchanged.
    (c) The crosshair is **not a fixed 64x64 box**: its size comes from the preferences (`Crosshairs_SDL.cpp`:55-91:
    `FromCenter`, `Length`, `Thickness`, the circle bounded by `Length`); the band is computed from `GetCrosshairData()`.
    (d) Since M4 a play frame copies only covered pixels (`view_end_play`, `back/draw.cpp`); SW never clears
    `world_pixels`, so an uncovered pixel keeps the previous frame (the "smear"/HOM case). Measured on the merge's
    sanity run (L0 bench spot, 1 374 play frames): `copied` = 131 902 626 = exactly 96 000 = 400 × 240 per frame, so
    uncovered pixels are an edge case, but direct mode keeps the same semantics (below) rather than clear the view.
    (e) `render_frame` has no direct path (`front/frame.cpp`: `mode direct` logs "play frames are software (mode
    direct: M6)" and returns false). (f) 5.3's overlay surface "400x240 ARGB8888" becomes **RGBA8888 in linear memory,
    pitch 512 pixels**: its 32-bit word is the GPU's RGBA8 texel (`0xRRGGBBAA`, the present's staging word, 5.1), so the
    band upload reads the surface itself (no copy, no channel reorder) and SDL draws into it with alpha. (g) 9.1's
    direct column drops the unlinked 400x240 view target and adds the texture-backed one; both cannot coexist at the
    full pool cap (375 + 512 + 512 + 512 + 375 + 320 + 3 584 = 6 190 > 6 144 KiB), so in `mode direct` every 400x240
    view frame uses the texture-backed target (below). (h) There is no render-to-texture example under
    `/opt/devkitpro/examples/3ds/graphics/gpu/`; `C3D_RenderTargetCreateFromTex(tex, face, level, depthFmt)` is
    `renderqueue.h`:63 and `C3D_FrameBufTex` `framebuffer.h`:30. The orientations below are **UNVERIFIED** until the
    calibrations pass. (i) The present (`MainScreenUpdateRects`, :1852) comes **before** `a1_3ds::bottom_frame` (:1859),
    so in today's order only the crosshair and the text would run while the GPU draws the view (critic round 1, major 1):
    on a direct frame hook 3 calls `bottom_frame` before the present (below), so the bottom screen's CPU work (2-3 ms on
    the console, 5.7.1) hides the view frame's GPU time.
  - *The texture-cache hash (prerequisite, M4 commit review minor 4).* `TexCache::lookup` scans all 512 slots per new
    draw key (`core/texcache.cpp`:215-229). New: a chained index inside `TexCache`: `uint16_t head_[kTexBuckets]`
    (1 024 buckets, `kNone` = 0xFFFF) and `uint16_t next_[kTexSlots]`; hash = a mix of the bitmap and tables pointers,
    kind, wrap and part; a slot is inserted when `create()` makes it Live and removed wherever a Live slot stops being
    Live (`evict`, `forget`, and the whole index is reset by `bump_generation` and `release_all`). `lookup` walks the
    one chain and applies today's full test (state, generation, the five key fields). `audit()` also checks the index
    (every Live slot of the current generation is found from its bucket, every chained slot is Live; chain length max
    logged in `stats_line`). Host tests: a random sequence of create / lookup / evict / forget / bump against a linear
    scan reference (same answers, every step), and a micro-benchmark (lookups per second, full cache, old vs new) that
    prints its numbers. **Measured (rule 1)** on the M5 bench (`m5-bench-c3d.cfg`, L0/L6/L20 @30, `build-3ds-c3d-prof`)
    before and after, `c3d_draw` and `c3d_tex`; kept only with a gain in Azahar's instruction-cost proxy and the host
    benchmark and no change in any gate (it changes no pixel: the same slot is returned). The authoritative figure is a
    deterministic counter (critic round 1, minor 15): `probes` (slots examined by the index walk) against `probes_old`
    (what the scan would have examined: the matching slot's index + 1 for a hit, 512 for a miss, computed in O(1)), both
    in `stats_line`. *Measured before the spec's review* (`build/perf/m6-hash-{before,after}.txt`, Azahar): `c3d_draw`
    10.21 / 10.20 / 8.41 → 10.19 / 10.21 / 8.41 ms on L0 / L6 / L20, i.e. no change within the noise: the bench spots
    make ~71 lookups a frame (L0: 64 563 over ~909 frames). Host, full cache of 512 slots, half misses: 292 → 3.5 ns per
    lookup. The hash bounds the worst case (a full cache of sprite frames, every miss scanning 512 slots); it is kept as
    the prerequisite the M4 review asked for, with the counters as its record, not as a speedup.
  - *The view target in `mode direct`.* A 512x256 `GPU_RGBA8` texture in VRAM (`C3D_TexInitVRAM`) and a target on it
    (`C3D_RenderTargetCreateFromTex(&tex, GPU_TEXFACE_2D, 0, C3D_DEPTHTYPE(-1))`), never linked. Used for every
    view of at most 512x256 in the run (direct, readback, diff, calibration): HUD + Map's 400x240 and Classic's
    **480x240** (`view.cpp`:198; revision 0 said 640x320, wrong), so `mode direct` never needs M0's unlinked target next to
    it (critic round 1, major 4: Classic's 450 KiB target on top of the texture target and the overlay texture would be
    6 265 KiB > 6 144 at the full pool cap, and `view_ensure` never retries a failed size); `mode readback` is unchanged
    (the M1-M5 code path). Made and deleted **out of frame** as M0's
    (`C3D_RenderTargetDelete` panics inside one, 0.3); `C3D_Fini` destroys targets but not textures, so `view_free()`
    calls `C3D_TexDelete` after it. A failed VRAM allocation is logged once and `mode direct` falls back to `mode
    readback` for the run (`direct_off=vram`). The projection is `Mtx_Ortho(0, 512, 256, 0, …)` (the target's size;
    the view occupies pixels 0-399 × 0-239, i.e. the top-left corner in M0's convention). **Readback of the texture
    target**: the same transfer as M0's (`kReadbackFlags`, no flip) with both dimensions 512x256 into a 512x256 linear
    staging; rows 0-239, columns 0-399 are the view (M0's calibrated orientation, expected to hold because both are
    colour buffers: `readback_cal 1` with `mode direct` checks it, 30/30 orientation and current-frame ok; **Azahar,
    first probe on the implementation: 30/30 orientation, current frame, channels and alpha, 0 mismatched pixels**, and the
    `frame` diff of the same run 99.2 % within 2, as M5's). Staging 512 KiB linear instead of 375.
  - *No clear in direct mode, and what the texture holds.* `mode direct` never clears the view texture: a pixel no
    primitive covers keeps the last view, exactly SW's `world_pixels` behaviour (d). Two flags in the front end:
    `tex_view` (the texture holds the last view) and `dest_view` (`dest` = `world_pixels` holds it). Rules (host-tested
    as a transition table, `core/direct_state.*`):
    - a **direct** frame needs `tex_view`; draws, no clear, **no readback, no retirement wait, no sentinel fill**;
      leaves `tex_view` true, `dest_view` false;
    - a **readback** frame in `mode direct` draws without a clear and copies **all** 400 × 240 pixels into `dest`
      (alpha forced 0xFF, the form of SW's pixels, 5.2): `tex_view` and `dest_view` true;
    - a frame that will draw into the texture **without a clear** (direct, or readback in `mode direct`) while
      `!tex_view` is **seeded** first (diff and calibration frames clear the target and are never seeded: critic round
      1, minor 6): `dest` (then `dest_view`, by
      construction) is copied by the CPU into the staging as `(argb << 8) | 0xFF`, flushed, and transferred linear →
      tiled into the target's colour buffer as the frame's first GX operation (flags: `kReadbackFlags` with
      `OUT_TILED(1)`, the inverse of the readback; **UNVERIFIED**, checked by `seed_check` below); counted `seeds`.
      A seeded frame skips the staging's sentinel fill (the staging holds the seed);
    - a frame drawn by **SW** into `dest` while `!dest_view` is preceded by a **sync** — one readback of the texture
      into `dest` without draws (frame_begin, transfer, frame_end, retirement pair, full copy), counted `syncs` — **only
      when citro3d is up, the gate is open and the texture holds a complete view** (`tex_view`); otherwise no citro3d call
      is made (5.6.1 a) and the skip is counted `sync_skipped_gate` / `_off` (citro3d down or released, **or play off with citro3d up**: no C3D frame is
      wanted then) /
      `_discard` (the texture holds a partial frame): `dest` then stays stale where SW leaves pixels uncovered (critic
      round 1, major 3). After any SW frame `tex_view` is false and `dest_view` true;
    - a **discarded direct** frame (ring, cmdbuf, gx, tex) has left a partial frame in the texture: the frame is redrawn
      by SW as today (`dest` was not in sync if the previous frame was direct: counted `discard_stale`; only uncovered
      pixels can differ), `tex_view` false, `dest_view` true;
    - a new `dest` (`row_addresses[0]` or size changed: `reallocate_world_pixels`, `screen.cpp`:653-686), a diff or
      calibration frame (both clear the target), a target re-creation or a citro3d release: `tex_view` false.
    `seed_check` (test key): after the first seed of a run, the seeded texture is read back without draws and compared
    with `dest`: 0 differences expected (the seed and readback orientations agree).
  - *Hook 3 (`PATCHES.md` 47), exact text.* (Revision 1 adds 3c.) Fenced `#if defined(A1_PLATFORM_3DS) && defined(A1_RENDER_C3D)`;
    `screen.cpp` includes `a1_c3d.h` under the fence (as `render.cpp`, `PATCHES.md` 45). **3a**, just before
    `render_view` (:1701): `a1_c3d::direct_policy(a1_c3d_direct_blocks(ViewRect, BufferRect, HighResolution,
    DrawEveryOtherLine))`, a static helper in `screen.cpp` returning a bit mask of the eligibility conditions that fail
    (5.4, below; 0 = eligible). **3b**, after `render_view` at the crosshair (:1734): `SDL_Surface *a1_ov = NULL; if
    (a1_c3d::direct_frame()) a1_ov = a1_c3d::overlay_begin(a1_c3d_overlay_bands(bands), bands);` — the second static
    helper fills up to 4 `{y0, y1}` bands (below) from the overlay state this TU sees; the crosshair renders into `a1_ov
    ? a1_ov : world_pixels`; `disp_pixels` starts as `a1_ov ? a1_ov : world_pixels` (:1743; the map case is excluded by
    eligibility); the world-view `update_screen(...)` (:1793-1804) is skipped when `a1_ov`. In `screen_shared.h`,
    `DisplayText` and `DisplayTextCursor` map with `DisplayTextDest->format` / `s->format` **only when the destination
    is `a1_c3d::overlay_target()`** (else `world_pixels->format`, as today). **3c** (critic round 1, major 1): when
    `a1_ov`, `a1_3ds::bottom_frame(...)` (with its `Bottom` zone) is called right after the darken test (:1840-1843, a
    no-op on a direct frame) and **before** `MainScreenUpdateRect(s)` (:1846-1853), and the call at :1855-1861 is
    skipped; its arguments (`hud_to_bottom`, `update_full_screen`) are final by then (:1822-1828, :1640). Side effect
    (critic round 2, minor 1): `present_bottom`'s own VBlank wait when a bottom swap is still pending
    (`video_3ds.cpp`:277-278) now comes before the composite and its VBlank guard; at @60 it could push the composite
    past a VBlank. Counted: `bottom_pending` = direct frames whose bottom swap was still pending at hook 3b (an upper
    bound on those waits), in `[c3d] direct final`, reported with the @60 bench. `direct_frame()` is true only when this
    frame's world view is on the GPU (a direct view frame was drawn and not discarded); `overlay_begin` returns the
    surface ready to draw. No other line changes; the OFF objects must stay byte-identical (`objdump -d` of both OFF
    builds against the copies saved at the start, `build/m6/start/`), and `ppcheck.sh` IDENTICAL or 0 normalised lines.
  - *Engine-facing API* (`include/a1_c3d.h`, plain): `enum DirectBlock : unsigned` (the mask bits below);
    `void direct_policy(unsigned blocks)`; `bool direct_frame()`; `struct OverlayBand { short y0, y1; }`;
    `SDL_Surface* overlay_begin(int n, const OverlayBand* bands)` (forward-declared `struct SDL_Surface`);
    `const SDL_Surface* overlay_target()` (nullptr when none). `direct_policy` is called every frame; a frame
    without a call (none expected) is not direct. With a 0 mask, `direct_policy` also makes sure the overlay buffer,
    surface and texture exist (made there, out of frame), and the front end decides a direct frame only if they do, so
    `direct_frame()` implies `overlay_begin` returns non-null, with 0 bands too (critic round 1, minor 5: a null would
    make hook 3b blit a stale `world_pixels`).
  - *Eligibility (5.4), every condition with its line* (mask bit: condition that blocks; the first set bit is the
    logged reason). `not_native`: `!native_screen()` (:128-131: Classic, menus, dialogs, and a Lua HUD through
    `bottom_mode` :115-120). `view_rect`: `ViewRect` not `{0, 0, 400, 240}` or `BufferRect` smaller (`!HighResolution`
    halves it, :1596, :1618-1622) or `DrawEveryOtherLine` (:1611) — `update_screen` would scale or interlace. `accel`:
    `screen_mode.acceleration != _no_acceleration` (the blit is in the other branch, :1768; never on 3DS). `map`:
    `world_view->overhead_map_active` (:1528-1538; the map replaces or overlays the view, :1744-1745, :1806-1809).
    `terminal`: `world_view->terminal_mode_active` (:1540-1550, :1748, :1831-1838). `gamma`: `!using_default_gamma`
    (:152; set by `animate_screen_clut`, :2128-2135, i.e. every fade: damage and pickup flashes, teleport, liquid
    tints, and a non-default brightness; applied in the blit :2002-2005). `darken`: `!get_keyboard_controller_status()`
    (`interface.h`:368; `darken_world_window`, :1840-1843). `network`: `game_is_networked || is_network_pregame`
    (scores, `screen_shared.h`:786-788; the pregame screen, :1712-1731). `lua_hud`: `Screen::instance()->lua_hud()`
    (implied by `not_native`, kept as its own bit). `movie`: `Movie::instance()->IsRecording()` (the recorder reads
    `main_surface`; film export is stubbed, `STAGE3.md`). Nothing else writes the world view after `render_view`
    (checked: :1701-1863; the HUD goes to the bottom screen in native mode, :1822-1824; `clear_screen_margin` touches
    `main_surface`'s margins only). The state cannot change between 3a and the blit: nothing in between calls
    `animate_screen_clut` or changes the map, terminal or keyboard status. The front end decides a **direct** frame
    when the mask is 0, `mode direct`, play mode would draw the frame with C3D (5.2/M4 preconditions), the view is
    400x240 and the texture target exists; otherwise the frame takes the readback (or SW) path and the reason is
    counted. Per-frame mode: `direct=` / `readback=` with reasons `(not_native/view_rect/accel/map/terminal/gamma/darken/
    network/lua_hud/movie/play)` / `sw=` in the once-a-second `[c3d] direct ...` line and in `[c3d] direct final`;
    every change of mode is logged once with its reason (at most 20 lines, then counted).
  - *Overlay bands.* The helper (in `screen.cpp`, which sees the state) adds, in surface rows: **crosshair** when
    `!map && !terminal && NetAllowCrosshair() && Crosshairs_IsActive()`: rows `[ycen − r, ycen + r + 1)`, `ycen = h/2 −
    1`, `r = FromCenter + Length + Thickness + 1` (circle: `Length + Thickness + 1`); **top** when `ShowPosition`, any
    script HUD text, or any unexpired message (`Messages`, `screen_shared.h`:169-170): rows `[0, top + LineSpacing ·
    (lines + 1) + icon_drop + 2)`, `top = lua_text_margins.top`, `lines = (ShowPosition ? 6 : 0) + (script HUD ? 1 :
    0) + unexpired messages` (the layout of `DisplayPosition`/`DisplayMessages`, :603-777; `+ 2` covers the shadow and
    descent, the next baseline being a full `LineSpacing` below); **bottom** when FPS is shown (`displaying_fps &&
    !terminal && !chat_input_mode`, :550-552) or the input line (`input_active() && !displayBuffer().empty()`,
    :646-648): rows `[h − bottom − 2·LineSpacing − Ascent − 2, h)`. A message that expires between the helper and the
    drawing only removes rows. The back end (`core/overlay_bands.*`, host-tested) clamps to `[0, 240)`, rounds `y0`
    down and `y1` up to multiples of 8, sorts, merges overlapping or touching bands, and while more than 4 remain
    merges the pair with the smallest gap. **Dirty tiles:** a 30-bit mask of 8-row tiles written since they were last
    cleared; `overlay_begin` clears (memset of columns 0-399) **every dirty tile**, then `dirty = current` — 5.3's "last
    frame's bands are cleared" — so the surface is transparent outside the current bands on every frame and the audit
    and `direct_check` can be exact (critic round 1, major 2: clearing only `current & dirty` left an expired message's
    rows behind). Host test: a band that shrinks leaves 0 pixels outside the new bands. `overlay_audit N` (test): every N-th direct
    frame the present scans the surface for non-zero pixels outside the current bands: `overlay_leaks` must stay 0.
  - *Overlay surface and texture.* 512x256 RGBA8 linear buffer (512 KiB, zeroed and flushed at creation) wrapped by
    `SDL_CreateRGBSurfaceWithFormatFrom(buf, 400, 240, 32, 2048, SDL_PIXELFORMAT_RGBA8888)` (blend mode NONE: it is
    a destination); created by `direct_policy` before the first direct frame (above), freed at the release; it stays
    allocated in Classic (9.1's direct total holds with it, view target 512 included). Texture 512x256 RGBA8 in VRAM
    (`C3D_TexInitVRAM`, nearest, clamp); if VRAM refuses it, a linear texture (counted `overlay_linear`); if both fail,
    `direct_off=overlay`. **Band upload** in the composite frame, before any draw: per band, `GSPGPU_FlushDataCache`
    of its rows, then one `gx_op(C3D_SyncDisplayTransfer)` from `buf + y0·512` (`GX_BUFFER_DIM(512, y1 − y0)`) into
    `tex.data + d·512·4` with `d = 256 − y1` (**from the end**: the present's upload flips (`kUploadTransferFlags`), and
    P1 found a 240-row transfer 16 rows off, 5.1, which is what a flip within the transfer's own height predicts) and
    the same flags. `band_from_end 0|1` (default 1) selects `d = y0` instead; **UNVERIFIED**, settled by the
    calibration below. At most 4 transfers, each one GX entry (the command buffer is empty until the first draw, 0.3).
  - *The composite frame* (`a1_c3d_present`, when hook 3b handed out the overlay for this frame and the surface given
    is 400x240; otherwise the present as today, B's zero-copy included). (1) `frame_begin` (zone `c3d_wait`: the view
    frame's GPU time not hidden by the CPU's overlays and bottom screen); `sample_drawing_time(View)` = the view frame
    (G3-b's `C3D_GetDrawingTime`, `gpu_ms view=` in the stats line); (2) the band uploads; (3) the composite VBO (linear,
    5 quads × 4 vertices, written after (1), flushed): the view quad with the view texture on unit 0, `Swizzle::Plain`,
    blend `ONE, ZERO` (colour and alpha), positions 0-400 × 0-240 (`Mtx_OrthoTilt`, as the present), texcoords `u = x /
    512`, `v = 1 − y / 256` (`view_v_flip 1`, the default: the colour buffer's rows run opposite to an uploaded
    texture's, by the two transfers' flip flags; **UNVERIFIED**, `view_v_flip 0` gives `v = y / 256`); then, if any
    band, the overlay texture, blend `ONE, ONE_MINUS_SRC_ALPHA` (colour and alpha), one quad per band (`v` as the
    present's: `y / 256`); the view and overlay textures are bound through `__real_C3D_TexBind` (not the recording wrapper, so the record
    keeps the present's texture whatever path returns early: critic round 1, minor 10); (4) unit 0 back with
    `__real_C3D_TexBind(0, tex_recorded())`, blend back to `ONE, ZERO`; (5) the VBlank guard (5.1 step 5), `frame_end(1)`. No staging
    fill, no upload of `main_surface`. GX entries ≤ 4 + 2. The present's `present_diff` counts a direct present as
    `not checkable: direct`. `present_close_frame` and the gate are unchanged; two stale-picture cases follow and are accepted: the forced
    close frame draws the present texture (the last non-direct picture), and a composite whose `frame_begin` fails
    releases citro3d and the CPU present then shows the stale `main_surface` for that one frame.
  - *Calibration (the frame-indexed test in direct mode).* With `calibrate 1` and `mode direct`, a direct view frame
    draws no world: it clears the view texture to present 5.1's frame-indexed colour of the present that will show it
    (`n = present_counter() + 1`), draws M0's four 16x16 corner quads (`core/calibrate.h`, the same colours) at the
    view's corners and fills rows 240-255 with a fifth colour, so a mirrored or shifted view shows (critic round 1,
    minor 8), and `overlay_begin` writes one opaque white 8-row band at rows `8·(n mod 30)` …
    `+ 7` (all other overlay rows transparent; bands = that tile only). The captures of 5.1's bursts then show the
    colour of N or N − 1 and the band at the row of the same frame; a band at **any other row** means `band_from_end`
    is wrong (with `d = y0` under a flipping transfer it lands at `248 − 8k`, off screen for k = 0); swapped corner
    colours or the fifth colour on screen mean `view_v_flip` is wrong.
    Run at Framerate Target 30 and 60 with `alt_load 1` (5.1's light/heavy frames).
  - *`direct_check N` (in-frame equivalence, test).* Every N-th direct frame: the composite frame also reads the view
    texture back (gx_split + transfer behind the composite's draws, as M0's readback) and, after `frame_end`, waits
    for it (the empty retirement pair, as readback mode); the CPU builds the reference = the read-back view (400x240)
    with the overlay surface's **band rows** composited over it (`ref = ov + view·(255 − A)/255`, rounded to nearest;
    outside the bands the surface is transparent by the dirty-tile rule, so this equals the whole surface), converts it with
    `present_reference` (the CPU present's own code, 5.1) and compares the framebuffer (`gfxGetFramebuffer` at the
    composite's `frame_end`, as `present_diff`) with it: a `[c3d] direct_check:` line per check (pixels with RGB Δ > 1,
    max |Δ| per channel), summary in `direct final`. It proves the view orientation, the band placement and the blend;
    the reference's overlay arithmetic is the formula, so the engine's own drawing is checked by the captures below.
  - *Suspend, HOME, `error_screen()` with two frames in flight.* The view frame is ended at render time and the composite
    at the present, so a frame can be in flight across the gate exactly as a P1 present is across the next one.
    `view_begin` checks the gate as today (`skip=gate`: the frame is SW, `direct_frame()` false). If the gate closes
    between the view frame and the present, the present skips (5.6.1 a, no citro3d call); the overlay hand-out is
    dropped at the next `direct_policy` (counted `composite_skipped`); the queued view frame retires at the next
    `frame_begin` (citro3d's ONSUSPEND hook drains the queue, 5.6.4 item 1) or in the release's bounded wait. The
    release (`release_and_fini`) deletes the view and overlay textures after `C3D_Fini`, frees the overlay buffer and
    surface, and sets `tex_view` false; `overlay_target()` returns nullptr from then on. `error_screen()` off the main
    thread is 5.6's hand-off, unchanged (no citro3d call off main). Counted at exit: view frames begun/ended, composite
    frames, targets created/deleted, binds restored.
  - *Profiler zones* (`a1_3ds_profile.h`): `c3d_overlay` (hook 3b's `overlay_begin`: rounding, merging, the dirty-tile
    clear; top level) and `c3d_composite` (the composite frame's CPU part, nested in `present`; its wait stays in
    `c3d_wait`, the VBlank in `c3d_vblank`). A direct view frame is `c3d_draw` without `c3d_readback`; a seed is inside
    `c3d_draw`, a sync is `c3d_readback`. So the bench can say: `render` (= `c3d_draw`, no GPU wait) + `blit` (0 on a
    direct frame) + `present` (`c3d_wait` = the unhidden GPU time, `c3d_composite`, `c3d_vblank`) + `bottom`.
  - *Test-only keys* (`core/config.h`): `overlays 0|1|2` (1: FPS, position, crosshair and a constant message every 2 s,
    set from the front end through the engine's own globals `displaying_fps`, `ShowPosition`, `Crosshairs_SetActive`,
    `screen_printf`; 2: position and crosshair only, deterministic for the capture comparison), `direct_check N`,
    `overlay_audit N`, `seed_check 0|1`, `band_from_end 0|1`, `view_v_flip 0|1`, `g3a 0|1` (below), `sw_every N`
    (every N-th play frame is drawn by SW as a forced fallback, counted `forced_sw`, so the sync and the seed after it
    run in Azahar: critic round 1, minor 7).
  - *Configs* (`scripts/3ds/tests/c3d/`): `m6-direct(-swbe).cfg` (direct play, `overlays 1`, `direct_check 30`,
    `overlay_audit 30`, `seed_check 1`), `m6-cal(-60).cfg` (`calibrate 1`, `alt_load 1`), `m6-readback-cal.cfg`
    (`readback_cal 1`, `mode direct`), `m6-capture-direct.cfg` / `m6-capture-readback.cfg` (`overlays 2`, the bench
    spots), `m6-bench-direct.cfg` (= `m5-bench-c3d.cfg` + `mode direct`), `m6-g3a.cfg`, and `m6-ineligible.cfg` for
    scripts 20 (map), 14 (terminal), 13 (bottom screen, both settings: Classic must log `not_native`).
  - *Host tests:* band rounding, clamping, merging and the ≤ 4 rule; the eligibility mask → reason table; the
    `tex_view`/`dest_view` transition table (seed, sync, discard, new dest, and the three `sync_skipped` rows); the
    dirty-tile rule (a shrinking band leaves nothing outside the new bands); the hash (above); the GX accounting of a
    composite frame (4 bands + end + 1 linked ≤ 24; with `direct_check` + 2) and of a direct view frame with its seed
    (+ 1).
  - *Acceptance (Azahar, non-authoritative; both backends where marked).* (1) `direct_check 30` on L0/L6/L20 and the
    L01 film, overlays on: every check ≥ 99.9 % of pixels Δ ≤ 1 (8.3's M6 tolerance) **and 0 pixels with Δ > 32** (a
    dropped or misplaced crosshair is ~0.1 % of the frame: critic round 1, minor 9), `overlay_leaks` 0, `seed_check` 0
    differences; a `sw_every 50` run with `syncs` > 0 whose direct checks after each seed still pass — both backends. (2) Captures: `--perf` end-of-run top screens (`screen-perf-L<n>-top.png`), `mode
    direct` vs `mode readback`, `overlays 2`, L0/L6/L20: ≥ 99.9 % Δ ≤ 1 and 0 pixels Δ > 32 (the engine's overlay
    drawing itself, through patch 47's colour mapping); the frame's mode at the capture is logged (a non-direct last frame
    would pass silently). (3) Ineligible frames: scripts 20, 14 and 13 with `m6-ineligible.cfg`: `readback=`
    with the reasons `map`, `terminal`, `not_native` and no direct frame while they hold; a damage flash or a liquid
    (a film or the L0 spot's first frames) logged as `gamma`. (4) The frame-indexed test, `m6-cal.cfg` @30 and @60:
    every burst capture shows N or N − 1, colour and band agreeing. (5) `readback_cal 1` in `mode direct`: 30/30. (6) GX
    high-water ≤ 24. (7) Scripts 07, 13, 14 pass with `mode direct`. (8) The M1-M5 gates on the final binary: M5's
    series (diff, play, look-up, present diff, fail_every, vram_cap) in `mode readback`, both backends, unchanged;
    `present_diff` 0 on every presented `main_surface`. (9) OFF builds: `objdump -d` identical to `build/m6/start/`,
    `nm` 0; four builds, 0 warnings.
  - *G3-a, how it is measured* (0.5: VRAM 9.1, command-buffer and queue budgets at 800x480, Azahar). `g3a 1` (test only):
    the direct view target is **1024x512** (2 048 KiB VRAM) and the view is drawn with the same `Mtx_Ortho(0, 512, 256,
    0)`, i.e. at 800x480 in the target's pixels; the composite's normalised texcoords are unchanged (nearest, so the
    picture is a 2x decimation, not M7's filter); `vram_cap 2048` (9.1's M7 column); readback, seed and sync frames
    halve through `GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_XY)` (`gx.h`:33-38) into the same 512x256 staging, the seed
    is a clear (test-only deviation). Recorded on L0/L6/L20 and the L01 film: VRAM in use (allocator figures, the
    targets, the pool's high-water, `vramSpaceFree` at the end), the pool's evictions and `promotions_blocked` (the
    2 MiB landscape part against a 2 048 KiB cap: whether M7 must drop the part to RGB8 or FCRAM), `cmdbuf_usage` and
    `gx_high_water` (resolution-independent: the same draws, so G3-a's new information is the **VRAM part**; the
    other two are the direct-mode figures, recorded alongside). G3-a "holds" if every allocation succeeds with the M7
    caps and the queue/command-buffer high-waters stay within 2.4's limits; recorded, not deciding (0.5).
  - *M7's landscape budget (decided here, built in M7).* M7's pool cap is 9.1's 2 048 KiB; Marathon 2's part 0 is
    2 048 KiB as RGBA8 (9.1 M5 note). M7 converts landscape parts to **`GPU_RGB8`** (1 536 KiB) if its host test shows
    the landscape texel's alpha is constant (then RGB8 is lossless: RGB565 is rejected, its 5-bit steps alone break
    8.3's Δ ≤ 2), else keeps RGBA8 and lets the part fall back to FCRAM sampling as M5 does (`promotions_blocked`);
    G3-a's numbers (above) and the console's FCRAM-sampling cost (section 10) inform it. The VRAM total stays 9.1's
    5 815 KiB.
  - *The bench* (the first that can show a speedup). `perf-bench.sh`, `build-3ds-c3d-prof`, HUD + Map, L0/L6/L20,
    `RUNS=1`, at `fps_target=30` and `60`, three configs on the same binary: `m5-bench-sw.cfg` (SW), `m5-bench-c3d.cfg`
    (readback), `m6-bench-direct.cfg` (direct); the stats printing as in M5 (not `quiet`, for comparability with M5's
    numbers; one extra `quiet 1` direct series @60 if time allows). Reported: `frame + overlay`, busy, `render`,
    `blit`, `present` with `c3d_wait`/`c3d_composite`/`c3d_vblank`, the direct-frame share (valid only if ≥ 95 % of
    world frames are direct on the spot), `gpu_ms view`. 9.3's direct column gets these Azahar figures and the
    console's `bottom` (2.3-2.9 ms, 5.7.1) as the CPU work now overlapping the view frame. Azahar overstates `render` and understates copies (`PERF.md`
    "How to read these numbers"): the console's M6 short check decides.
  - *Known limitations (recorded, not fixed in M6).* Persistent tints (under a liquid) and a non-default brightness
    keep `using_default_gamma` false on every frame (`screen.cpp`:2135), so those frames are readback, slower than SW
    (G2-a): the bench reports the direct share per spot so this is not read as a regression (critic round 1, minor 14).
    `DisplayTextCursor`'s mapping (patch 47) runs only with the console's input line, which no config opens: untested
    in Azahar. Script-HUD icons (`Image_Blitter::Draw`, `screen_shared.h`:718, Lua only) blitted from a source with
    partial alpha but no blending would write non-premultiplied texels; Marathon 2 has none.
  - *Not in M6:* supersampling (M7; `g3a` is only a budget probe), the preference (M8), stereo (M9), the clip-at-cut
    landscape fallback (a console decision pending since the M3-M5 checks), Classic in direct mode (5.4).

- **Status (2026-11-07): M6 implemented and accepted** (`PERF.md` "Stage 10 M6", Azahar — non-authoritative; commits
  `d6b252cee`, `896fbb3a9` review follow-ups, `b94124840` the policy counter; spec REVISE then APPROVE round 2, code APPROVE
  with 6 minors folded). One session. `stage10-bb` merged first (user decision). On both Azahar backends: `direct_check`
  100.000 % (host GPU Δ 0, software backend Δ ≤ 1) on L0/L6/L20 and the L01 film, `seed_check` 0, the sync path, overlay
  leaks 0; frame-indexed captures 12/12 @30 and 18/18 @60 (frame N, band and corners right: `view_v_flip 1`, `band_from_end
  1`); `readback_cal` 30/30 on the texture target; map, terminal, darken, gamma and Classic (`not_native`) frames readback;
  captures 99.55-99.77 % within Δ 1, 0 over 32 (recorded exception, user decision: Review log "M6 acceptance, capture
  gate"); the M1-M5 gates unchanged; OFF builds identical, `ppcheck.sh` IDENTICAL. **G3-a holds** (VRAM free 329 KiB at
  M7's caps on L0; the 2 MiB landscape fills the 2 048 KiB pool: RGB8 for M7). **Bench:** @30 busy direct − SW −6.04 /
  −4.20 / −5.01 ms; @60 direct 59.66-59.71 fps on all three (SW 54.37 / 59.62 / 59.53, readback 50.26-52.07). The hash:
  no change in Azahar, kept as a worst-case bound. **Console check run 2026-11-07** (`PERF.md` "Stage 10 M6 — console, short
  check", medium variant, 15 launches): no hardware bug. Orientations right (frame-indexed test 18/18 @60; @30 17 of 24, the 7
  being menu or non-composite presents, a harness window); `direct_check` Δ 0 and `seed_check` 0 on PICA; the forced-SW sync
  16 of 16; HOME x2, sleep and HOME > Close with two frames in flight clean; **@60 direct 60.5 fps on L0/L6/L20 (goal >= 55
  met; SW also 60.5 on the console)**, busy −0.66 / −0.37 / −0.80 ms against SW, readback 53.8-54.7 fps; `c3d_wait`
  0.70-0.78 ms; G3-b `gpu_ms view` 1.3-1.5 ms (recorded, not deciding).

### M7 — 2x2 supersampling (to be built LAST, after M9; user 2026-11-08; G3 measured but not deciding)
- **Open for its spec (2026-11-08):** M9 will have shipped first, so M7 inherits M9's VRAM budget (section 6 needs a second
  linked screen target and a second view target) and must say whether SSAA is **off while 3D is on** (the 1024x512 view
  target is 2 048 KiB, two of them cannot fit). M9's console fill-rate figure (2 eyes = 2x fill) is the first real evidence
  for M7's 4x.
- View at 800x480 (texture 1024x512), halved by `GX_TRANSFER_SCALE_XY`
  (`gx.h`:33-38, readback) or bilinear composite (direct); cache cap lowered
  (9.1).
- Accept: output vs CPU 2x2 box of the same 800x480 render read back
  ≤ 1/channel.

### M8 — Renderer preference (to be built; user 2026-11-07)
- Touch `preferences.cpp/.h` ("Renderer: Software / GPU (experimental)",
  default Software, `<graphics c3d_renderer="0|1">`), `PATCHES.md` 48.
- Accept: switches at the next frame; persisted.

### M9 — stereo 3D (to be built after M8, before M7; open question 9 answered yes 2026-10-19, user 2026-11-07)
- **VRAM (checked 2026-11-08 against 9.1's direct column, 5 815 of 6 144 KiB):** a second linked screen target (375 KiB) and a
  second view target (512 KiB) add 887 KiB against 329 free, so M9's spec must lower the texture cache cap (3 584 KiB) by
  about 560 KiB or more. Assumes each eye has its own full view target, as section 6 says; the spec decides.
- **Non-stereo frames and the ramp-up (user decision 2026-11-08):** one image to both eyes on every non-direct frame; the
  parallax eases from 0 to the slider's value over `stereo_ramp` frames when direct frames resume (section 6). The spec
  defines the state machine in `core/` (host-tested), the config key and its default, the slider-change rule and the
  tests above; the length is tuned on the console.
- Accept: section 6 tests.

## 8. Verification without hardware

### 8.1 In-frame dual render
On a diff frame `render_frame()` runs, per selected class: clear `dest` to
`0x00000000`, `draw(filter(SW))` → A (seed saved); clear the GPU target to
`(0,0,0,0)`, `draw(filter(C3D))` with alpha 1 on written pixels, readback
**keeping alpha** → B. Coverage: A = top byte ≠ 0 (SW table pixels carry
`0xFF`, 0.1), B = alpha ≠ 0. Exceptions: **tinted** draws over a grey clear
(`0xFF808080` / 128,128,128,255), compares colour, coverage = changed pixels;
**static** (SW writes `seed ^ seed<<8`, top byte 0, `low_level_textures.h`:
597-600) uses "pixel ≠ clear". Log `[c3d] DIFF frame=… class=… covered=…
cov_mismatch(edge/non-edge)=… within_tol=…% mean=… max=…`, optional
`c3d-diff-<frame>-<class>-{sw,c3d}.bmp`; then render the real frame. Same
view, world state and frame: no cross-run determinism needed. Edge band =
pixels within 1 px of a coverage boundary in A.

### 8.2 Content and Azahar setup
- Bench spots L0/L6/L20 (`debug-perf`, fixed seed, `a1_3ds_profile.h`:65-67);
  menu demo (`12-sprite-check.txt`); films from
  `tests/replays/Marathon 2/Tooncinator Films/` via `debug-c3d film`; the Lua
  effects plugin.
- Azahar: `resolution_factor=1`, `texture_filter=0`, `texture_sampling=0`,
  `shaders_accurate_mul=true`, `is_new_3ds=true` (`qt-config.ini`:350, 358,
  360, 352, 396). Gates M2-M5 run under the default host-GPU backend
  (`graphics_api=2`, :346) **and** Azahar's software rasteriser backend; both
  recorded; a disagreement beyond tolerance is investigated before acceptance.

### 8.3 Tolerances (fixed before each milestone; changes go in the Review log)

| Class | Coverage (outside edge band) | Colour, common non-edge pixels |
| --- | --- | --- |
| walls flat (M1) | 0 | — |
| walls/floors full bright (M2), infravision | ≤ 0.1% | ≥ 98% Δ ≤ 2, mean ≤ 1.0 |
| walls/floors lit (M3) | ≤ 0.1% | ≥ 98% Δ ≤ 4, mean ≤ 1.5 |
| sprites, weapons (M4) | ≤ 0.5% | as lit |
| tinted | ≤ 0.5% | ≥ 98% Δ ≤ 3 |
| landscape (M5) | ≤ 0.1% | ≥ 98% Δ ≤ 2 |
| static (M5) | density ±2 points | — |
| full frame (M4+) | ≤ 0.5% | ≥ 97% Δ ≤ 4 (static excluded) |
| GPU present (P1), direct vs readback (M6) | — | ≥ 99.9% Δ ≤ 1 (P1 Classic/menus: identical) |

### 8.4 Host tests (`scripts/3ds/tests/c3d-host/build.sh`)
`core/` only: conversion with real-format tables (3.3), tiling vs tex3ds, fan
indices, shade LUT vs SW formula, per-vertex UV/w vs transliterated SW mapper
code (`scottish_textures.cpp`:891-1097, 1102-1170, 657-732), GX-queue entry
accounting (2.4 rules, incl. the in-frame readback transfer: worst case 24 + 1 = 25 < 32), overlay band computation
(5.3: 8-row alignment, merging), diff metric.

### 8.5 Performance
`perf-bench.sh` (`PERF.md`:522-528) with `debug-c3d`; BENCH gains the c3d
zones; deterministic counters (command words, GX entries, draws, uploads,
KiB) are authoritative in Azahar; `C3D_GetDrawingTime/ProcessingTime`
(`renderqueue.h`:38-39) and every wait are meaningless in Azahar. Timings
"Azahar — non-authoritative"; instruction-count rankings are reliable
([PERF.md](PERF.md):436-447). Gate a-halves use exactly these (0.5).

## 9. Budgets

### 9.1 VRAM (6 144 KiB)

| Item | P1 | Readback (M0-M5) | Direct (M6) | Direct + SSAA (M7) |
| --- | ---: | ---: | ---: | ---: |
| screen target 240x400 (linked) | 375 | 375 | 375 | 375 |
| present texture 512x256 (all surfaces, incl. 640x480 halved on the CPU and in-game dialogs) | 512 | 512 | 512 | 512 |
| view target (400x240; Classic 480x240) | — | 375 / 450 | — | — |
| view texture-target 512x256 / 1024x512 | — | — | 512 | 2 048 |
| overlay texture 512x256 | — | — | 512 | 512 |
| shade LUT + noise (M5: 64 + 256) | — | 320 | 320 | 320 |
| texture cache VRAM pool (cap) | — | 3 584 | 3 584 | 2 048 |
| **total** | 887 | ≤ 5 166 / 5 241 | 5 815 | 5 815 |

**M5 (critic, M5 code review major 1):** a Marathon 2 landscape's part 0 is **2 048 KiB** (1 024 × 512 RGBA8; part 1
128 KiB), inside the pool's cap but not always one contiguous `vramAlloc` block. A promotion allocates first and
evicts only for the cap; a failed allocation leaves the part in the linear pool for its generation (sampled from
FCRAM, cost in section 10) and evicts nothing; a landscape is the last LRU victim. M7's 2 048 KiB cap is the whole of
part 0: M7's spec decides between RGB8 (1 536 KiB, `GPU_RGB8`, a new kind) and FCRAM sampling, with the console's
numbers. Cutting at 256 rows is not an option: the cut would sit 13 rows from the horizon (row 269 of 540), on
screen in nearly every outdoor frame.

In direct mode a dialog switches to the 640x480 menu surface, presented
through the same 512x256 texture; the overlay texture is idle then. Totals
hold in every mode with the default caps.

### 9.2 Linear heap (~17 848 KiB free in game, New 3DS)

| Item | KiB |
| --- | ---: |
| command buffer (one buffer, 0.3; 512 default, up to 1 024 by M1) | 512-1 024 |
| present staging 512x256 RGBA8 (P1 calibration, 5.1) | 512 |
| zero-copy `main_surface` 512x256 (plan B, 5.7.8; from the first HUD + Map game) | 512 |
| vertex ring 2 × 2 048 × 32 B (M0: 2 × max 387, rounded up to 1 024; M1 re-measured over the menu demo and a film: unchanged; **M3: 2 048**, the L08 film's span quads overflowed 1 024) | 128 |
| index ring 2 × 4 096 × 2 B (M0: 2 × max 597; M3: 4 096) | 16 |
| readback staging 480x240 (M6 `mode direct`: 512x256 for the texture target, 512) | 450 / 512 |
| M6 overlay buffer 512x256 RGBA8 (the SDL surface's pixels; made by `direct_policy`, out of frame, before the first direct frame) | 512 |
| M6 overlay texture, linear fallback only when VRAM refuses it | (512) |
| linear texture pool (cap; holds promotions pending; M5: a landscape part that found no VRAM block, 2 048) | 4 096 |
| **total** | ≤ 6 738 (M6 `mode direct`: ≤ 7 312, ≤ 7 824 with the overlay's linear fallback) |

No change to `__ctru_linear_heap_size` needed on the New 3DS
(`a1_3ds_platform.cpp`:43-50 "revisit then": revisited). Azahar's split is
logged in P1. Old 3DS out of scope. malloc < 1 MiB.

### 9.3 Per-frame CPU (console, L0 @30, ms; all but "today" are estimates)

| Zone | HUD + Map today | + P1 | readback (M4-M5) + P1 | direct (M6) | Classic today | Classic + P1 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| render | 5.78 | 5.78 | R + 1.0 submit + G + 0.8 readback | R + 1.0 | 6.65 | 6.65 |
| blit | 0.96 | 0.96 | 0.96 | 0 | 1.41 | 1.41 |
| present | 4.33 | ~1.0 copy + ~0.2 | ~1.2 | ~0.2-1.5 | 8.7 | ~3-4 (sequential UHADD8 halving) |
| busy | 14.02 | **~11** | ~11 ± 2 | **~7-9** | 18.24 | **~13** |

`R` = M0 null-mode floor; `G` = unoverlapped GPU wait. At Framerate Target 60
each frame carries ~1.1 ms more `world` ([PERF.md](PERF.md):364-366). Whether
P1 alone meets the target depends on P0's console baseline; G0/G1 decide.

## 10. Needs console verification

1. citro3d + SDL N3DS + the bottom-screen console coexisting: HOME, sleep,
   HOME > Close, error screen, `C3D_Fini` before `gfxExit` (normal exit and
   `vhalt`).
   **Console 2026-10-19 (`PERF.md` "Console G0-b", both sessions):** HOME and back,
   sleep, HOME > Close in a dialog, release before `gfxExit` (normal exit, `vhalt`
   on main and on a second thread): pass. **HOME > Close in a game hangs** when a
   present runs after the suspend (`release()` never returns): open. The port's
   error screen has not been seen after boot on either thread (the "readable"
   main-thread screen was Luma's exception screen).
   Root causes and fixes: 5.6 (A1, A2, fatal hand-off); its console runs are
   items 14 and 15.
2. No tearing with the corrected VBlank guard (5.1 step 5) @30 and @60.
   Ordering margin: `gxCmdQueueInterrupt` updates the completion counter
   before running the callback, so the binary does not prove that
   `onQueueFinish` has issued the previous swap by the time
   `gxCmdQueueWait` (inside `C3D_FrameBegin(0)`) returns (per critic); the
   ≥ 1 ms CPU staging copy in step 2 between `C3D_FrameBegin` and the guard
   makes it safe in practice. The console run confirms it.
   **Console 2026-10-19:** pass @30 and @60 (`alt_load`; every capture frame N).
3. Every gate b-half (G0-b … G3-b) and P0/P1 series @30/@60 in both Bottom
   Screen settings.
   **Console 2026-10-19:** G0-b and the P1 series done (all four); P0 SW figures
   taken with the same-commit OFF build. G1-b ... G3-b: not reached.
4. Real GPU time, readback wait, and M6 frame overlap (`C3D_GetDrawingTime`).
5. VRAM vs linear sampling speed (linear textures before promotion); VRAM
   bank placement.
6. TEV, rasteriser and bilinear precision vs Azahar (one `diff 30` run over
   3dslink; M7 SSAA).
   **M1 (Azahar, 2026-10-27):** walls coverage equals SW's except tie pixels (a centre exactly on an
   edge), which Azahar leaves out on top edges where SW covers them. On the console: PICA's tie rule
   from the DIFF lines' `ties` / `tie_mismatch`, and whether float24 positions (clip × w) ever move a
   non-tie pixel on a shared edge (`NEXT-SPRINT.md` "M1 console check" step 5).
   **Console 2026-11-02 (short check, L0, 20 frames):** 440 ties, 380 tie mismatches (Azahar 140): PICA
   leaves out most exact ties SW covers; 0 non-tie mismatches.
   **M3:** PICA's modulate rounding against Azahar's `(a·b)/255` (the lit-colour prediction assumes it); the
   alpha ×2 clamp (`m3-alpha2.cfg`: self-luminous coverage intact = clamps); texcoord1 precision at LUT bin
   edges and float24 at `u ≈ −2 048`; the unbinding of unit 1 after a lit frame on hardware (`[c3d] shade:
   citro3d context layout confirmed`, `unit1_left_bound=0`, no hang at the next present).
   **M4:** PICA's texcoord interpolation and texel choice on rectangles (the float24 model and `rect_bias`, design 7
   M4 "Rectangles on the GPU"); the `CONSTANT_COLOR` blend's rounding (predicted Δ ≤ 1); A8 sampling into the alpha
   test and `GPU_CLAMP_TO_EDGE`; non-square POT textures (`C3D_TexInit(S, T)`) on hardware; play-mode readback cost
   and HOME/suspend in play mode (`NEXT-SPRINT.md` "M4 console check, short version").
   **M5:** landscape texel choice under PICA's float24 texcoords (host model: rows 98.16 % with the per-polygon row
   offset δ, columns 99.04 %; pre-stated fallback: δ per band of 16 lines, 99.81 % rows in the model); how TEV's
   `MODULATE` rounds `255·(hi + 1)/255` on masked static draws (the alpha test's step); the 2 MiB landscape
   part's conversion (+ 128 KiB part 1) and its `TextureCopy` at level start and when infravision toggles (with item
   10; Azahar's level start: `level start gen 1: KiB 2944`); the GPU cost of sampling the part from FCRAM against VRAM;
   whether a 2 MiB contiguous VRAM block is available next to the render targets (M6's targets too); the
   landscape span path's columns under float24 (`m5-lookup.cfg`: Azahar's software backend 97.72 % strict, every miss one
   column late; host GPU exact; Review log "M5 code review, round 1"); the G2-b bench (readback vs SW
   busy on L0/L6/L20) (`NEXT-SPRINT.md` "M5 console check, short version").
   **Console 2026-11-07 (M3-M5 short checks, `PERF.md` "Stage 10 M3-M5 — console, short checks"):** *verified* —
   M3 modulate rounding (walls / floors `mean_nb` 0.23 / 0.72 against Azahar's 0.21 / 0.71, `max_nb` equal, command
   words equal), the ×2 clamp (clamps), unit 1's unbinding (`unit1_left_bound=0`, no hang); M4 rectangle texels at
   `rect_bias 1` (78 non-edge sprite pixels against Azahar's 79), the constant-colour blend (bit-exact, 59 040 px),
   A8 sampling and non-square POT (no mismatch), HOME / lid / Close in play mode (clean `gfxExit`); M5 landscape
   texels without a cut (strict 100 %, 254 700 px), the 2 MiB part's conversion at level start, the present with
   static (31 checks exact). *Refuted (as the host model predicted):* the span path's columns under float24 (97.84 %
   strict, 0 non-edge, `within_nb` 99.87 %; the pre-stated remedy is the clip-at-cut fallback, the user's call).
   *Measured:* G2-b readback − SW busy **+6.15 / +7.77 / +7.25 ms** on L0 / L6 / L20. *Not settled:* static's look
   (seen for about 1 s, not noticed), the landscape part's `TextureCopy` when infravision toggles, FCRAM-against-VRAM
   sampling cost (one landscape promotion was blocked after 4 170 play frames: `promo 84/0/1(land 1)`, so a 2 MiB
   VRAM block is not always free late in a session), texcoord1 at LUT bin edges.
7. Perspective-correct interpolation and large-UV precision on hardware.
8. Readback-target orientation/flags; GX tiling minimum sizes (SPECULATIVE).
   **Console 2026-10-19:** the P1 present target's orientation and flags are right
   (`present_diff`: 153 checks, 0 differences). Readback (M0+): not reached.
   **Azahar 2026-10-27 (M0, non-authoritative):** the unlinked 400x240 view target reads back
   right with `Mtx_Ortho(0, w, h, 0, …)`, `FLIP_VERT(0)`, untiled RGBA8 out (7, M0 block). On the
   console: the same 30-frame `readback_cal` run (`NEXT-SPRINT.md` "M0 console check"), plus
   that a view frame causes no top-screen swap. **Revise 1:** the readback's "current frame" check is
   meaningful on the console only with the full-image + sentinel check (every pixel clear(k) or its
   corner colour, 0 mismatches) and the in-frame transfer with the `C3D_FrameBegin(0)` retirement;
   Azahar completes transfers synchronously and cannot show the PPF race, so it cannot tell a
   working wait from a missing one. Also on the console: `m0-readback-cal-resize.cfg` (target
   deleted and created every frame, no svcBreak). **Revise 2:** the draws before the in-frame
   transfer are split off with `C3D_FrameSplit(GX_CMDLIST_FLUSH)` (0.3); on the console both
   calibration configs must give `mismatched 0 (sentinel 0)` on all 30 frames.
   **M1:** the calibration also checks alpha (`alpha ok 30` in Azahar); the walls diff frames are the
   first readbacks of real draws: on the console `sentinel=0` and 0 non-edge mismatches prove the ring
   flushes, the flushing split, the retirement and the invalidate (`NEXT-SPRINT.md` "M1 console
   check").
   **Console 2026-11-02 (short check, `PERF.md` "Stage 10 M1 — console, short check"):** calibration 30/30
   ok (orientation, current frame, channels, alpha; 0 mismatched, sentinel 0); walls diff on L0: 0 non-edge,
   `sentinel=0`, 0 discards, rings' physical range ok, command words identical to Azahar. **Settled** for
   the readback, the ring flushes and the flushing split (L0, HUD + Map); the resize config, Classic and
   L6/L20 wait for the full runbook.
9. `C3D_FrameEnd(GX_CMDLIST_FLUSH)` + ranged flushes vs the whole-heap flush.
10. Level-start hitch with the promotion cap (TextureCopy throughput, PPF
    latency); whether pre-warming is needed.
11. citro3d per-draw CPU cost at 804 MHz with L2.
12. Linear heap free at `main loop ended` with citro3d on.
    **Console 2026-10-19 (P1):** 16 767 KiB of 19 904 KiB free (profile build);
    17 792-17 812 KiB after the release.
13. Stereo comfort/ghosting; SSAA look.
    **M6 (2026-11-07):** the texture target's orientations (`view_v_flip 1`, `band_from_end 1`, the seed's `OUT_TILED(1)`
    with no flip: all right in Azahar), PICA's blend rounding against `direct_check` (Azahar Δ 0), the composite's
    `c3d_wait` (Azahar runs the GPU inside `C3D_FrameEnd`), the bottom screen's VBlank wait before the composite at @60
    (`bottom_pending` 105-203 of ~1 670 frames in Azahar), whether VRAM contents survive HOME for `tex_view`, contiguous
    512 KiB / 2 MiB VRAM blocks after pool churn: `NEXT-SPRINT.md` "M6 console check, short version".
    **Console 2026-11-07 (`PERF.md` "Stage 10 M6 — console, short check"):** *verified* — the orientations (`view_v_flip 1`,
    `band_from_end 1`, the seed's `OUT_TILED(1)`, no flip; `seed_check` 0 of 96000), PICA's blend rounding (`direct_check`
    Δ 0 in 54 samples), two C3D frames in flight across HOME / sleep / Close, the bottom screen's VBlank wait before the
    composite (`bottom_pending` 5-6 % of frames at @60, no artefact). *Measured* — `c3d_wait` 0.70-0.78 ms (Azahar 0.01),
    the view frame's GPU time 1.3-1.5 ms. *Refuted* — none. *Not reached* — whether VRAM contents survive HOME for `tex_view`
    (right picture after each return, no pixel check) and contiguous 512 KiB / 2 MiB VRAM blocks after pool churn (step 3's
    `alloc-fail 5` is a lead, not examined).
14. 5.6.1: d1 confirms the binary facts (main thread, rights 1 at ONSUSPEND and
    ONRESTORE, no hook after Close); the post-close frame never retires in 5 s
    (run 1); HOME > Close in a game ×3, HOME and back, sleep in a game with the gate,
    0 skipped presents on HOME/sleep (runs 2-4); every close returns to the launcher
    and Aleph One launches again.
15. 5.6.2 / 5.6.3: the boot-time control; the port's error screen readable on the
    bottom for `vhalt()` on main and for errors on other threads (`debug-fatal`
    modes 1-4, 6, 6b), shown by the main thread, exactly one screen each; the
    launcher starts again after each.
    Optional: mode 7 (the fallback's `svcExitProcess` without `aptExit`).
16. 5.7 (plan B): B-a's `two_frame 0`/`1` series @30 decides B-b (5.7.11) (*run on the console
    2026-10-26: saving 0.435 / 0.350 / 0.355 ms, below the 0.5 ms rule; `present_diff` 0 with
    `two_frame 1` over 151 checks; see 5.7.10 item 1*); then B-b's one-channel
    write masks and TEV operands give `present_diff` 0; the upload wait (`c3d_upload`) and the three
    fills' drawing time; the `zero_copy 0`/`1` series decide whether B-b is kept (5.7.9, 5.7.10).
    *B-b was built anyway on the user's decision (2026-10-26, branch `stage10-bb`, merged into `stage10-c3d` 2026-11-07 before M6 (user decision); passes
    Azahar, `PERF.md` "Stage 10 B-b"). Its console items (`present_diff` with `zero_copy 1`, the
    `zero_copy 0`/`1` series @30 and @60) are `NEXT-SPRINT.md` "B-b console runbook", not run yet.*

## 11. Open questions

1. ~~Confirm or replace the target in 0.5.~~ **Answered (user, 2026-10-16):**
   confirmed — HUD + Map at Framerate Target 60 sustains ≥ 55 fps (busy
   ≤ 18 ms) on L0/L6/L20 on a New 3DS; no regression at 30; Classic at 60 not
   worse than 53.16 fps.
2. ~~Proceed on G0-a, or pause stage 10 at G0 until a console session?~~
   **Answered (user, 2026-10-16):** pause at G0 until a console session
   confirms G0-b; do not proceed on G0-a alone. This session implements P0
   and P1 only.
3. ~~G1: build the 3D renderer at all after P1?~~ **Answered (user,
   2026-10-19):** yes, aiming at M7 and M9 (0.5).
4. Direct mode for Classic, or readback + P1 there?
5. Runtime switch: debug file only, or the M8 preference?
6. SSAA default on or off?
7. Support `software_alpha_blending` (see-through liquids) later?
8. Mipmaps/linear filtering as an option (not SW-faithful)?
9. ~~Stereo wanted?~~ **Answered (user, 2026-10-19):** yes (M9; direct mode,
   so HUD + Map only, section 6).
10. 800 px wide mode (`gfx.h`:101) instead of SSAA?
11. Texture conversion on another thread (core 1 runs audio)?
12. GPU-drawn crosshair to keep direct frames overlay-free?
13. ~~Fatal error off the main thread: park the main thread, or hand the fatal
    path to it?~~ **Answered (user, 2026-10-23, backlog item 14):** hand the whole
    fatal path to the main thread; the error screen takes its own HID reference;
    SDL's double HID release patched (done, `3ds-port` `5cd45ce`). 5.6.2 b, 5.6.3.

## Review log

### Round 1 (critic: REVISE, 8 major, 12 minor)

1. **Self-luminous test wrong — accepted.** `SDL_pixels.c`:865 ORs `Amask`;
   `pixel_format_32` is ARGB8888 (`screen.cpp`:222). 3.2 masks `0x00FFFFFF`
   for RGB and the test; host tests use real-format tables incl. black
   normal, black/dark self-luminous. Readback writes `0xFF` alpha (5.2); the
   diff uses it for coverage (8.1).
2. **Command-buffer overflow is fatal — accepted** (completed in round 2,
   item 2): headroom check before every draw, discard to SW, `cmdbuf_kib 32`
   no-crash test.
3. **Upload staging corruption/stalls — accepted, option A** (completed in
   round 2, item 1): in-frame conversion into linear-pool textures, later
   batched promotion, deferred frees.
4. **citro3d default state — accepted.** 2.4 per-class state;
   `C3D_FrameBegin(0)`.
5. **Direct GX calls land in citro3d's queue — accepted.** All GX ops via
   `gx_op()` in-frame (the M0 readback too since revise 1; retirement confirmed by `C3D_FrameBegin(0)`).
6. **Swap ownership — accepted, model (a)** (completed in round 2, items 3-4):
   one linked target, citro3d swaps, one writer per screen.
7. **Settle locally settleable items — accepted.** Moved to 0.3 with
   citations; still UNVERIFIED: perspective-correct interpolation, TEV clamp,
   linear vs VRAM sampling, Azahar GPU timing, readback-target orientation.
8. **No goal; gain comes last — accepted.** 0.5 target, P0/P1 first, gates
   (split into a/b halves in round 2, item 6); renderer behind G1; 9.3 says
   M0-M5 bring no speedup.
9. **Whole-heap flush — accepted.** `C3D_FrameEnd(GX_CMDLIST_FLUSH)` + ranged
   flushes (2.4); section 10 item 9.
10. **Landscape clamp — accepted.** Edge-line padding, splits at `VertRepeat`
    rows (3.2).
11. **Overlay cost — accepted** (completed in round 2, item 8): conservative
    rectangles; direct present 0.2-1.5 ms.
12. **Static noise follows surfaces — accepted, partly fixed.** w = 1 for
    non-transparent static bitmaps; transparent ones listed as approximated.
13. **Key-colour coverage — accepted.** Clear to 0, coverage by SW top byte
    and C3D alpha; tinted/static special cases (8.1).
14. **Azahar backends — accepted.** M2-M5 gates under both (8.2).
15. **float24 texcoords — accepted.** `floor(min u/v)` subtraction (2.2).
16. **VRAM fragmentation — accepted.** Size-class slabs + log (3.3).
17. **Tinted global table — accepted.** A8 mask kind (3.2).
18. **Unload not hooked — accepted.** Hook 2b (`shapes.cpp`:1179-1193).
19. **Scope — accepted.** M7-M9 optional and gated; film playback in M1.
20. **Citations/rules — accepted.** No "one user clip plane" claim; both
    macros on every engine hook; pre-warm decided on deterministic counts +
    console.

### Round 2 (critic: REVISE, 6 major, 5 minor)

1. **GX queue overflow after ~15 promotions — accepted.** 0.3 records the
   32-entry queue, its svcBreak and the per-op entry costs (per critic).
   2.4: every GX op through `gx_op()` with a per-frame budget of 24 and a
   high-water log; ops grouped right after `C3D_FrameBegin(0)` while
   `gpuCmdBufOffset == 0`. 3.3: conversion needs no GX op; promotions are
   batched at the start of a later frame, ≤ 8 per frame, the rest wait in
   the linear pool. Tests: M2 ≥ 16-new-texture frame with high-water ≤ 24,
   `gxq_budget 4` run.
2. **`C3D_FrameSplit` frees nothing; frames cannot be abandoned —
   accepted.** FrameSplit recovery removed (2.4). "Discard" is defined in
   1.5: stop issuing, end the frame normally, skip readback, redo in SW.
   The reserve now includes `kEndReserveWords` (FrameEnd split + finalize +
   one transfer per linked target), measured in M1.
3. **VBlank guard at the wrong time — accepted.** 5.1 step 5: the guard runs
   immediately before every `C3D_FrameEnd` that transfers to the linked
   target (the destination is chosen there; the previous queue finished at
   `C3D_FrameBegin`). "citro3d does not guard" recorded as settled (0.3).
   Test: frame-indexed colours @30/@60 with `alt_load` light/heavy frames.
4. **SDL can still write the top framebuffer — accepted, port-only fix.**
   Verified `screen.cpp`:2581-2591, `SDL_n3dsframebuffer.c`:80/157,
   `ScenarioChooser.cpp`:563, `a1_3ds_platform.cpp`:968/995, `shell.cpp`:661,
   `csalerts_sdl.cpp`:322-329. (a) `a1_c3d_present` accepts every surface
   (`main_surface` is always 32-bit, AUDIT.md:416; anything else converted),
   so `present_screen()` never falls through to SDL; (b) lazy init at the
   first `present_screen()`, after the chooser; (c) `error_screen()` and a
   `--wrap=gfxExit` wrapper release the top screen and `C3D_Fini()` before
   the console or `gfxExit` touch it. The wrapper also covers `vhalt()`
   (→ `shutdown_application` → `SDL_Quit` → `gfxExit`), so no engine hook is
   needed there. Tests: `bit_depth 16` menu via C3D only; `test_vhalt` and
   HOME > Close log the release before `gfxExit`.
5. **VRAM overflow with an in-game dialog — accepted, CPU halving.** 640x480
   is halved on the CPU with the existing UHADD8 box (`video_3ds.cpp`:66-77,
   227-239) before upload, so the present texture is always 512x256 (9.1:
   ≤ 5 623 KiB in every mode); Classic and menus become byte-identical to
   today's present. Cost estimate in 9.3 (~3-4 ms instead of 8.7).
6. **No gate passable without a console — accepted.** 0.5 splits every gate
   into an a-half (Azahar, deterministic/relative: identical picture,
   present-zone drop, `R_est = R_az/render_az × 5.78 ms`, budgets) and a
   b-half (console, section 10 item 3); G1 stays a user decision, decidable
   from a-half data with the stated caveat; the "pause at G0" alternative is
   noted for the user (0.5, open question 2).
7. **`C3D_FrameSplit` does not start the GPU; nested FrameBegin fails —
   accepted.** 5.3: M6 uses two C3D frames per game frame (view frame ended
   inside `render_frame()` so the GPU starts; composite frame at present,
   whose `C3D_FrameBegin` is the sync point). 2.4: every `C3D_FrameBegin`
   return value is checked; no frame nesting by construction (5.1, 5.2, 5.3).
8. **Dirty rectangles unspecified — accepted, conservative rectangles.** 5.3:
   computed in one hook-3 helper in `screen.cpp` from overlay state visible in
   that TU (`Crosshairs_IsActive`, `displaying_fps`, `ShowPosition`,
   `Console::input_active`, the static `Messages`, `screen_shared.h`:550-777),
   anything else → whole surface. Still the single hook-3 engine touch.
9. **Immediate frees in `collections_changed()` — accepted.** 3.3: marks
   "flush pending"; frees run right after the next `C3D_FrameBegin(0)`.
10. **"Frame retired" observable — recorded as settled** (0.3): the return of
    `C3D_FrameBegin(0)`; all deferred actions (frees, staging reuse, flushes)
    hang off it; view-only frames never present (used flag).
11. **Cross-thread swaps safe — recorded as settled** (0.3); one writer per
    screen kept (round 2 item 4).

Critic's round-2 console items merged into section 10 (items 1-13).

### Round 3 (critic: APPROVE, 0 major, 6 minor)

1. **`gfxExit` wrapper re-entered on the `vhalt` path — accepted.** 1.3: an
   `s_c3d_up` flag makes `__wrap_gfxExit` and `a1_c3d_release_top()`
   idempotent (`if (s_c3d_up) release_and_fini(); __real_gfxExit();`), so the
   second `gfxExit` from `error_screen()` after `SDL_Quit()` never touches
   citro3d's freed queue or command buffer. Test (1.5, P1): `test_vhalt`
   reaches the error screen and logs one release and two `gfxExit` calls.
2. **Readback's queue entry — accepted** (superseded by M0 revise 1: the transfer is in-frame). 2.4: the out-of-frame readback
   transfer counts +1 in the view frame's queue (cleared only by
   `onQueueFinish`'s safe-transfer path or the next `C3D_FrameBegin`, per
   critic); worst case 24 + 2 + 1 = 27 < 32. Added to the 8.4 host test.
3. **Overlay regions must be full-width bands — accepted.** 5.3: regions are
   400-wide bands aligned to 8 rows (tiles), merged, one `gx_op` each (≤ 4),
   destination offset computed in tile rows from the texture's row-0 end
   (orientation UNVERIFIED, checked by M6 calibration); band computation
   added to 8.4.
4. **VBlank-guard ordering margin — accepted.** Section 10 item 2 records
   that the binary does not prove `onQueueFinish` issued the previous swap
   before `gxCmdQueueWait` returns, and that the ≥ 1 ms staging copy between
   `C3D_FrameBegin` and the guard makes it safe in practice; console confirms.
5. **`bit_depth 16` test does not test ownership — accepted.** Replaced in
   5.1 and P1 acceptance by a counter of `present_screen()` false returns
   while citro3d is up (must stay 0) and `debug-c3d odd_surface 1`, which
   hands `a1_c3d_present()` an odd-size surface (config list in 7).
6. **640x480 path must convert the format too — accepted.** 5.1 step 2: each
   staging pixel is `to_fb(box(r0, r1))` (`video_3ds.cpp`:61-77, as the CPU
   present at :198-201), not `half_blit()` (which writes XRGB); P1
   acceptance checks byte-identity.

**User decisions (2026-10-16)** recorded in the status line, 0.5 and 11:
target confirmed; stage 10 pauses at G0 until G0-b is confirmed on a console;
G1 decided by the user after G0-b; this session implements P0 and P1 only.

### P1 follow-ups 2 (architect, 2026-10-19; critic review pending)

Section 5.6 added for the two console findings of 2026-10-19 (A1: HOME > Close in
a game hangs; A2: `error_screen()` on a second thread draws nothing), with the P1
follow-ups 2 entry in 7, section 10 items 14-15 and open question 13. Two
corrections to earlier text, by the evidence in 5.6: the `odd` hang is not
established to be in `release()` (`PERF.md`:1102 says both are); the black Azahar
error-screen captures were not an Azahar limit (`PERF.md`:812-814, :952). The
critic's objections go here, each accepted or rebutted.

### P1 follow-ups 2, round 1 (critic: REVISE, 3 major, 7 minor)

New facts taken in: the user's item 14 decision ((a) own HID reference in the
error screen; (b) SDL2's double HID release patched, done on `3ds-port` `5cd45ce` +
fix-up; (c) hand the whole fatal path to the main thread), and the "readable"
main-thread screen being Luma's exception screen.

1. **The fatal hand-off unspecified — accepted.** New 5.6.3: static fixed buffers,
   an atomic request with the thread id, a bounded 1 ms-step wait for main's
   acknowledgement (5 s), then an untimed `svcWaitSynchronization` on a never-signalled
   event (not a sleep loop: the heap is freed before `svcExitProcess`); main serves at
   the former park points with a re-entrancy guard, releases citro3d directly, shows
   the error with its own HID reference and exits through the console-proven
   dialog-exit path; the SDL audio thread gets the fatal-exit variant (no SDL audio
   call in `on_suspend`/`on_resume`, no `SDL_Quit`, `gfxExit` directly, `_Exit`), its
   id recorded at the first callback (`audio_3ds.cpp`:157; mixer lock verified at
   `SDL_audio.c`:731-738, :1199-1206); fallback draws on the failing thread and ends
   with `svcExitProcess` (aptExit skip UNVERIFIED, console list); `vhalt` intercepted
   by a fenced engine edit, `PATCHES.md` 54 (chosen over a `--wrap` of the mangled
   `shutdown_application`, which would still run `stop_recording()` first), so 7's
   "no engine edit" is withdrawn. Tests: `debug-fatal` modes 1-7, incl. the audio
   callback with a scripted HOME and the held-lock fallback; the launcher criterion.
   `park_main_thread` removed; open question 13 marked answered.
2. **`abort()` off the main thread never reaches `on_abort` — accepted.** 5.6.3's
   table records the per-`_reent` handler and the `_exit` → `__appExit` teardown on
   the failing thread (dump 6); `-Wl,--wrap=abort` routes it into the hand-off; mode 2
   logs `signal()`'s previous value on both threads first. Dump 5 noted as
   SPECULATIVE, same class.
3. **Binary-settled facts — accepted, all nine.** 5.6.1's table now has the
   `aptMainLoop` structure and hook threads (i, ii), the ONSUSPEND → ReleaseRight
   order and the skipped reacquire block (i), the cached rights flag (i), citro3d's
   own hook and why the post-close `frame_begin` returned (vi), ONEXIT only from
   `aptExit` after the log closed, now 5.6.1 c's reason (v); NONBLOCK semantics and
   the warning against positive timeouts in 5.6.1 b (iii); `consoleInit`'s
   first-call-only device in 5.6.2 (iv). d1 and run 1 no longer expect impossible
   lines (vii); the gate text names `!gspHasGpuRight()` authoritative and
   `s_in_home` the Azahar input, atomics dropped (viii); the old "gate closes for
   good" and "sleep hook mid-present" risks removed, run 3 predicts 0 skipped
   presents; 5.6.4 lists what is settled and keeps only GSP's behaviour without
   rights (ix).
4. **CPU present after an abandon — accepted.** The abandon sets `s_surrendered`;
   `frame_begin()` returns `Ok`/`AlreadyInFrame`/`Abandoned`; on `Abandoned` the
   present returns true without the `present.cpp`:488-492 branch.
5. **citro3d's hook left registered after an abandon — accepted, documented** in
   5.6.1 b (reachable only after an abandon outside a Close); GSP's handling of
   pending commands at `gspExit` UNVERIFIED (5.6.4); runs 1-2 pass only if the
   launcher returns and Aleph One launches again.
6. **`consoleInit` swaps the top screen too — accepted.** 5.6.2 a: never
   `consoleInit` while citro3d is up; on main the release comes first; in the
   fallback with citro3d still up, log only.
7. **Fact 2 strengthens A2 — accepted.** The contradiction row and 5.6.3 item 5's
   second half are gone; the boot-time error screen is the positive control, rerun
   after fix a; d6 is a confirmation. `PERF.md` and `NEXT-SPRINT.md` are not edited
   here; they were corrected in `dcd9b55` and `17894b2`.
8. **Run 1's window and d3 — accepted.** `p1-home-close-diag.cfg` sets
   `retire_wait_ms 5000`; d3 logs `gspHasGpuRight()` and `s_in_frame` before the
   post-close `frame_end`.
9. **Order contradiction — accepted.** 5.6.1 a: (1) serve a pending release, (2) the
   gate, (3) `ensure_up()`.
10. **"Queues nothing" — accepted, reworded.** `C3D_FrameEnd(0)` submits no GPU work
    but makes one whole-linear-heap `GSPGPU_FlushDataCache` IPC;
    `fn-main-log.txt`:447-452 shows it completing after a Close.

Branch split and order for the implementer: 5.6.0.

### P1 follow-ups 2, round 2 (critic: APPROVE, 8 minor, folded)

All eight accepted and folded into 5.6.
1. **Two fatal errors at once.** 5.6.3 step 1: a losing thread logs one raw line and
   blocks forever (step 2), never draws or exits; any audio thread entering the
   hand-off sets `s_audio_parked` first, winner or loser; `fatal_exit()` (step 5)
   picks the variant from that flag at exit time; main's own fatal paths
   (`vhalt_hook` on main, `die_with_message` via `on_abort`) end in `fatal_exit()`
   and check it; `s_fatal_active` makes main acknowledge a request without serving it
   while its own error screen is up. Test mode 8 (two failures, both orders).
2. **Fallback recursion.** Step 3 calls `error_screen_here()`, the screen without the
   hand-off; mode 7 checks one fallback screen and no second hand-off line.
3. **Service points inside callbacks.** Step 4 serves only at `present_screen()`,
   `present_bottom()` and `__wrap_hidScanInput` on main; `console_write()` and
   `bottom_present()` dropped; `apt_hook`/`event_filter` set `s_in_hook`/`s_in_filter`
   and the service point does nothing while either is set.
4. **Fallback with citro3d up.** Nothing drawn → no wait, immediate exit (step 3,
   5.6.2 a); the fallback writes its log lines straight to the log fd; the invariant
   "no fatal error from inside `tee_write` or newlib malloc" stated after step 6.
5. **Run 1's post-close frame.** New `debug-c3d close_frame 1` (piece 5): the release
   forces one GPU frame while the gate condition holds; in `p1-home-close-diag.cfg`
   and run 1's text. Chosen over disabling `exit_requested()`'s `aptShouldClose()`
   term (stays in `render_c3d/`, independent of the engine's quit path).
6. **Two screens on `vhalt()` on main.** `__wrap_abort` on main goes straight to
   `fatal_exit()` once an error screen was shown (`s_error_shown`): exactly one
   screen; mode 1 and script 17 expect one A.
7. **Mode 2's probe uninstalls `on_abort` on main.** The probe restores at once with
   `signal(SIGABRT, prev)`.
8. **Main inside the engine's audio lock.** Step 6 records `SDL_LockAudio` via
   `OpenALManager::ResyncPlayers`/`StopAllPlayers`; new mode 6b fires during a
   music-volume change or level transition, either outcome accepted if Azahar ends
   cleanly.

From the critic's answers: 5.6.4 item 5 now lists the DSP (`ndspExit` skipped by the
fallback's `svcExitProcess`) as a console check. The round-1 note asking the
coordinator to correct `PERF.md`/`NEXT-SPRINT.md` is replaced: done in `dcd9b55`
and `17894b2`.

### B design, round 1 (critic: REVISE, 2 major, 6 minor; 2026-10-26)

Folded into 5.7: (1, major) TEV stage 0 is set once at init, so the reorder draws would leak into
the staging path and `close_frame`: one helper sets stage 0 and the write mask before every draw,
and `present_diff` runs across path switches (scripts 13/14, game -> menu) with a negative control;
(2, major) the estimate was optimistic (the extra frame and the wake-up were left out; @60 is not
0.05 ms repeatable) and the lower end is within the noise: B is split into B-a (port-only
pre-measurement, two-frame staging present, zones `c3d_fill`/`c3d_upload`/`c3d_vblank`) and B-b
(the engine edit) on a console go of ≥ 0.5 ms @30; the keep criterion is @30, with @60 and Classic
as no-regression checks; (3) `C3D_GetDrawingTime()` sampled after each `frame_begin()`; (4) the
gate text corrected, and `gate_closed()` re-checked between frames U and D (`aptShouldClose()` can
flip off the main thread); (5) the OFF check adds `main_surface_pixels` to the `nm` pattern and an
`objdump -d` comparison; (6) `blit` and `bottom` in the A/B metrics; (7) the buffer reuse is checked
(one log line, same linear free); (8) the TEV operand semantics marked UNVERIFIED.

### B design, round 2 (critic: APPROVE, 5 minor, folded; 2026-10-26)

(1) `frame_begin()` takes its zone as a parameter so the upload wait lands in `c3d_upload`; (2) the
gate re-check only with `suspend_gate 1`; (3) the new zones inside the `A1_RENDER_C3D` fence; (4)
`c3d_fill` of the go formula from the `two_frame 0` runs; (5) `perf-table.py` columns and the
present-total comparison with G0-b.

### M0 pre-implementation (coordinator, 2026-10-26)

M0 is the first milestone built on P1, plan A and B-a, which changed `back/present.cpp`,
`back/context.cpp` and `core/config.*` after the design was approved. Checked against the code
(`e2394b1`), with the result in section 7's "M0 specification" block:
(1) `debug-c3d calibrate` is P1's present-target test, so M0's readback calibration gets
`readback_cal`; (2) "null mode" and `R` were used (0.5, 9.3) but never defined; (3) M0 has no C3D
rasteriser, so the diff's B side is a second SW draw into a scratch bitmap, and `renderer c3d`
with `mode readback|direct` does nothing yet; (4) the class/fullbright filter and the state-change
count had no definition; (5) `core/gx_queue.h` cannot count the readback's +1 entry (2.4), and
`frame_end` warns above 4 entries, a present-only limit; (6) section 7's zone list has
`c3d_readback` but no zone for `R` or the diff work; (7) `front/` may not include `<3ds.h>`, so
the calibration's citro3d side is a new `back/view.cpp` behind a plain `back/view.h`; (8)
`back/present.cpp` binds its vertex buffer once at init, so another draw would leave the present
with the wrong buffer, and plan B-b edits that file, so M0 restores the binding from `view.cpp`
instead of editing it; (9) the config parser is `core/config.cpp`'s one-token form, not
`profile_3ds.cpp`'s (M1's `film <path>` will need more).

### M0 pre-implementation, round 1 (critic: REVISE, 1 major, 8 minor)

1. **A second traversal calls `rand()` — accepted, record/replay.** `add_random_flutter()`
   (`weapons.cpp`:2922-2924) runs inside `render_viewer_sprite_layer`, so A and B could differ for a
   fluttering weapon and Lua's `math.random` would shift. A diff frame now traverses once into a
   recorder (copies, fixed capacity) while SW draws the direct frame into scratch A; a `replay` check
   (direct vs replay) runs every diff frame; classes replay the recording into A and B; the real frame
   is the replay into `dest` (`render_frame` returns `true`). 1.2's re-run paragraph marked superseded.
   With replay A and B see identical input, so a flutter frame cannot make (c) fail; no extra film
   needed for it.
2. **`front/` needs more than `view.h` — accepted.** `front/` includes `context.h` and `view.h`;
   `view.h`'s exports are listed.
3. **`C3D_SetBufInfo` — accepted.** Copy `*C3D_GetBufInfo()`, restore with `C3D_SetBufInfo(&saved)`
   (kept alive; copy semantics VERIFIED in revise 1); program, `AttrInfo`, `C3D_TexBind(0)`, TEV 1-5 untouched.
4. **Calibration start and fix rules — accepted.** Starting projection and flags given; one change
   per failure kind, one re-run each.
5. **Null-mode bookkeeping inflates `R` — accepted, bounded.** Fixed generation-tagged table and fixed
   sample arrays, allocated once; the state-key comparison stays in `R` (it is M1's batching test).
6. **Filter by composition — accepted.**
7. **Drawing-time sample after a view frame — accepted.** Skipped via a flag in `context.cpp`.
8. **`frame_end` frame kind — accepted.** `FrameKind::Present`/`View`; `note_out_of_frame()`
   host-tested with 24 + 2 + 1.
9. **Cleared `dest` on diff frames (speculative) — accepted, moot.** A and B are scratch bitmaps;
   `dest` is written only by the real-frame replay, over the previous frame as usual.

Console items from the critic, added to section 10 item 8's readback line when M0 is recorded: the
unlinked target's orientation and flags; that a view frame's `C3D_FrameEnd` with no linked target
causes no top-screen swap; that the out-of-frame `C3D_SyncDisplayTransfer` waits for its own
completion.

### M0 pre-implementation, round 2 (critic: APPROVE, 5 minor, folded)

(1) "Transposed" is not fixed by swapping `GX_BUFFER_DIM` (it scrambles a non-square tiled buffer):
log and stop; the fix would be the examples' `(h, w)` + `Mtx_OrthoTilt` arrangement, a design change.
(2) The recording is an event list (`SetView`, `Begin`, primitives, `SetForeground`, `End`) re-emitted
in order by every replay. (3) The copy is taken before forwarding; replays hand over temporary copies.
(4) Recorder memory ≈ 1.1 MiB (8 192 events; corrected in revise 1 to ≈ 1.9 MiB, 240 B per event), diff runs only; on overflow forwarding continues to the
end of the traversal. (5) Null mode is benchmark-only (its second traversal shifts `rand()`).
Console items carried: readback orientation/flags; no top-screen swap from a view frame; the
out-of-frame transfer waits for its own completion; whether `C3D_SetBufInfo` copies its argument.

### M1 pre-implementation (coordinator, 2026-10-27)

M1's five lines were written before M0. Checked against the code (`fc4e1e493`), with the result in
section 7's "M1 specification" block: (1) the present binds its program and AttrInfo once
(`present.cpp`:432, 435-438) and its program is private, so a world shader needs
`--wrap=C3D_BindProgram` to restore it (and `C3D_SetAttrInfo` copies: verified); (2) transparent
walls cannot pass a flat coverage test (SW leaves holes), so C3D checks exclude them on both sides,
counted; (3) `build_y_table` samples edges at column centres, the GPU's rule, so "0 outside the band"
needs no position offset; (4) with the split charged 1, a view frame takes 3 queue entries (clear, split, transfer;
corrected in round 1), so `gxq_budget 4` cannot show a refusal: `gxq_budget 2` is added; (5) `cmdbuf_kib 32` may be enough for
a walls-only frame: 16 is the fallback value, and smaller is refused (the present checks no reserve);
(6) the film enters through `boot_begin`'s argv (by reference) and the upstream command-line path,
no engine edit; (7) "≤ 2 % inside the band" needs the band size: the DIFF line gains `band=`;
(8) the world shader gets its own shbin so the present's `DVLE[0]` is unchanged.

### M1 pre-implementation, round 1 (critic: REVISE, 3 major, 7 minor)

1. **Command-buffer measurements read zeroed globals — accepted.** `C3D_FrameEnd` zeroes
   `gpuCmdBuf/Size/Offset` and `C3D_GetCmdBufUsage()` is written only by a split; every measurement is
   now taken inside the frame (`P0` after `frame_begin`, around draws, before `C3D_FrameEnd`), and
   `C3D_FrameEnd`'s own words from the usage after it where its split ran on a non-empty buffer.
2. **The program restore would re-bind the world program — accepted.** `draw.cpp` binds through
   `__real_C3D_BindProgram`; save before any bind; `prog_restore_mismatch` counted, must be 0.
3. **w tolerance — accepted; a tolerance change (reported to the user).** SW's `world_x` carries a
   relative error up to about `t/16 384` (`scottish_textures.cpp`:933-939), so `max(1e-3·sw, 2)`
   fails on 0.2-23 % of honest samples. Gates: float vs double rel ≤ 1e-4 everywhere; the kickoff's
   rel ≤ 1e-3 vs SW where SW's own quantisation bound is ≤ 0.5e-3·sw; elsewhere within that bound.
4. **Discard path flushes the ring bytes it wrote — accepted.** 5. **Staging sentinel and flush kept
   for diff frames, `sentinel=N` on the line — accepted.** 6. **`C3D_FrameEnd` adds no queue entry on
   an empty buffer (3 entries per view frame); clear only before the first draw — accepted.**
7. **A deterministic `cmdbuf` discard — accepted:** `reserve_extra N`, two configs.
8. **Skipped checks (`down|gate|size`) apart from discards, `--min-checks` — accepted.**
9. **Coverage-model host test (full SW per-column coverage vs acceptance + pixel-centre fan) —
   accepted.** 10. **Indices relative to the current half's `BufInfo_Add` base, physical range checked
   at allocation — accepted.**
Console items: PICA's tie rule and float24 near ties (inside the band); readback orientation/flags
(M0); the present's shader re-upload words after a view frame (deterministic, Azahar suffices).

### M1 pre-implementation, round 2 (critic: REVISE, 1 major, 3 minor; applied as prescribed)

Round 1's findings 1, 2 and 4-10 resolved; the scheme of finding 1 verified (`C3D_FrameBegin`+0x78-0x9c
and `C3Di_SplitFrame`+0x3c-0x6c share the base, `GPUCMD_Split` keeps `size − offset` the space left).
The second round is the last (kickoff: max 2), so its fixes were applied by the coordinator exactly as
the critic prescribed, and are re-checked in the critic's review of the M1 commit:
(1, major) the w gates: samples restricted to SW's domain (`|tx·v.i| < 2³¹`, `|world_x| ≤ 32 767`),
excluded ones counted; unclamped w compared, clamp tested apart; gate (a) gains an absolute 0.05 term;
gate (c) budgets the float error. (2) `C3D_FrameEnd`'s words only when `gpuCmdBufOffset != 0` before
it, `lroundf`. (3) the vertex ring halves get the physical-range check and `BufInfo_Add`'s return is
checked. (4) the coordinator entry's "4 entries" corrected to 3.
**Superseded kickoff bullets** (`KICKOFF_M1.md` "Accept"): `gxq_budget 4` now must pass *without*
a discard (a view frame takes 3 entries) and `gxq_budget 2` is the refusal test; `cmdbuf_kib 32`
must run without a crash, discards recorded, and the deterministic `cmdbuf` discard comes from
`reserve_extra`; the w host test's plain rel ≤ 1e-3 holds only where SW's own quantisation allows
judging it (gates (a)-(c)). These three changes are to be reported to the user.

### M1 implementation, tie pixels (coordinator, 2026-10-27; tolerance definition)

The film acceptance run (200 walls checks) found 1-2 non-edge coverage mismatches, each an exact
tie on an edge shared with neighbouring walls (1-px slivers, e.g. `(346,96)(347,97)(347,125)(346,127)`:
the centre (346.5, 96.5) lies on the top edge; SW covers it, Azahar does not). 8.3's "0 outside the
edge band" assumed ties lie on A's coverage outline. Changed: the band also holds the tie pixels of
every drawn polygon (exact integer test), and ties and tie mismatches are reported apart, so the
tolerance stays 0 for every non-tie pixel and the console run measures PICA's tie rule. No vertex
bias (Azahar's rule is not the hardware's, and 1/16-px snapping would defeat a small bias).

### M1 commit review (critic: APPROVE, 0 major, 6 minor; 2026-10-27)

Review of `54d1e03c4`, with round 2's spec fixes re-checked: sync and cache order verified against the
1.7.1 disassembly (`C3D_FrameBegin(0)` retires the half before ring writes; nothing runs before
`C3D_FrameEnd`'s `gxCmdQueueRun`, so the ranged ring flush is in time on every path; `C3Di_SplitFrame`
never calls `C3Di_UpdateContext`, so restoring state before the split adds no shader upload and the
20-word end reserve holds; `C3D_BindProgram` writes no command words). Minors: (1) acceptance logs
predated the final constants — the whole set was re-run on `ac9a0241a` (`PERF.md` "Stage 10 M1");
(2) results not yet recorded — this entry, the M1 status and `PERF.md`; (3) the no-memory skip comment
— fixed; (4) zone `c3d_draw` per polygon in the back end — one scope per check in `frame.cpp`, front
end included; (5) `world_init` marked an unusable program ready — freed instead; (6)
`prog_restore_mismatch` proves little — `m1-present-diff.cfg` (present self-check with walls view
frames interleaved, scripts 13 and 08: 0 RGB differences) is the recorded evidence. Console items:
section 10 items 6 and 8, the ring physical-range check on the console, the discard paths on hardware.

### M2 pre-implementation (coordinator, 2026-10-31)

The "M2 specification" block (section 7) checked against the code at `d4dd81eb3`, the SW mappers and
citro3d 1.7.1. Corrections to earlier sections, each stated in the block: (1) 3.2's "transparent =
`_TRANSPARENT_BIT`" is per use: the floor mapper never skips texel 0, so the conversion kind carries the
keying (`normal` / `normal_keyed`); (2) 2.2: the perspective weight `w` is evaluated at the same shifted
point as the texcoords, or the `uv_half` shift bends the interpolation; (3) 2.3: the `[16, 32767]` clamp is
SW's shading-depth clamp, the perspective weight clamps to `[16, 2^24]` (floor vertices near the horizon);
(4) 3.3's VRAM size-class slabs are deferred to M5, M2 allocates per texture through citro3d with
accounting caps and logs fragmentation; (5) 1.3's hook 2a moves to the end of `load_collections()`;
(6) view frames use a sentinel with alpha 0x01 (textured RGB can equal the M1 sentinel). M2 is split over
two sessions because the short M1 console check has not run (kickoff): parts 1-5 first, the upload code
after the check.

### M2 pre-implementation, round 1 (critic: REVISE, 2 major, 7 minor; applied)

Major: (1) texture orientation — tex3ds does not flip, its `.t3x` gives PNG row 0 `t = 1.0`, so `t = 0`
samples the last memory row: the tiler writes source row `j` into memory row `W − 1 − j`; the tex3ds test
fixes tiling and byte order only; part 7 starts with an orientation run; (2) the colour gate may be
unreachable because SW's own integer mapping is not exact: part 4's host texel-agreement prediction
decides, and a figure under 98 % stops session 1 with a revised gate recorded and reported. Minor: (3) the
floor `sy'` sign guard; (4) tex3ds test details (A ≠ 0, `-r`, the 4-byte compression header; byte order
confirmed); (5) bitmaps capped at 512²; (6) `software_alpha_blending` → check skipped `pref`; (7) no
eviction of slots with a pending or same-frame promotion; (8) the GX high-water item cannot fail: replaced
by KiB/cmdbuf/linear-cap checks; (9) the hook 2a caller list completed. Verified by the critic: the wall
and floor texcoord formulas, the δ-shift exactness, the floor coverage rule (brute-forced `build_x_table`),
transparency per use, the bind restore (`C3D_TexBind` always dirties; no internal callers), the diff
ignoring alpha, `kViewReserveEntries` 3. New console items: perspective precision with large w ratios,
large repeat counts, texture-cache invalidation on recycled linear addresses, the attribute sample point.

### M2 pre-implementation, round 2 (critic: REVISE, 1 major, 2 minor; applied as prescribed)

Eight of round 1's nine fixes verified (orientation, `-r` + header, A ≠ 0, the orientation run, the
`sy'` sign guard, the 512 cap, `pref`, reachability, the replaced GX item, the caller list). (1, major)
the eviction exclusion was too broad (`promote_pending` is nearly every linear slot, so `promote_max 0`,
`gxq_budget 4` or `vram_cap 512` would discard `tex`): only a slot whose copy was issued this frame is
protected, a waiting slot is evicted and dropped from the queue, `promote_max 0` never marks pending, and a
host test fills the linear cap in both modes; (2) `o.z = 0` is an acceptance skip (`degenerate`), not
unsupported; (3) the texcoord host test fails only below the gate recorded here (98 % until revised).
Max rounds reached; implementation starts.

### M2 implementation, colour gate (coordinator, 2026-10-31; user decision; a tolerance change)

Part 4's host texcoord test (`a6d99f2d3`): exact SW-texel agreement at the pixel centre, walls 98.30 %,
floors 89.62 % at `δ = −½` (26.69 % / 7.87 % at `δ = 0`), 100 % within one texel; by W, floors 94.36 % (128),
90.41 % (256), 84.69 % (512). Against the exact projective map at the pixel corner the GPU agrees 99.93 % /
99.98 %: the misses are SW's integer inexactness (the floor branch's truncate-then-multiply, :1069-1072,
and `source_dx` truncation accumulating along a row), not the formulas. Under the round 1 reachability
rule session 1 stopped and the user chose the **neighbourhood rule** (*Colour metric* bullet): Δ is the
minimum over A's covered 3×3 neighbourhood, gate ≥ 98 % within and mean ≤ 1.0 on it; strict figures
printed; host gate within one texel ≥ 99.5 %. `uv_half` default 1 (`δ = −½`) is confirmed by the host;
the Azahar measurement in session 2 still records both.

### M2 commit review (critic: APPROVE, 0 major, 3 minor; 2026-11-02; folded in `38ca4868e`)

On `1c3b9c6f5` (effort medium). Sync and lifetimes checked against `libcitro3d.a`: `C3D_FrameBegin` waits
for the queue (`gxCmdQueueWait(-1)`) and clears it, `C3D_FrameEnd` runs it in order (copy, clear, draws,
split, readback); the conversion flush covers every byte a promotion later copies; nothing is freed while
a frame in flight or the context's unit-0 pointer can reach it (`restore_state` on every `view_end`
path); a promotion costs 1 GX entry after `C3D_FrameBegin` zeroed the offset. Minors, all fixed: (1) a
large non-convex polygon could overflow the ring mid-polygon: spans are merged into rectangles and the
ring is asked for all of them first (`c3d_span_ring_refused`, `c3d_span_quads_max`); (2) a non-finite
span corner counted as `nonfinite`; (3) an allocation failing with room under the cap evicted until the
pool was empty: at most 2 more victims (`kEvictAfterAllocFail`). Speculative, not acted on: after an
abandon `release_and_fini` returns before `texcache_release_all` (citro3d never comes back after an
abandon). Needs console verification: the RAW `GX_TextureCopy` linear → VRAM, the ranged conversion flush
on real caches (both **verified** on the console 2026-11-03, `PERF.md` "Stage 10 M2 — console, short
check"), the cost of promotions and per-miss flushes (`c3d_tex`; still not measured).

### M2 acceptance, load test colour (coordinator, 2026-11-02; user decision; a recorded exception)

`15-load-test.txt` with `m2-load.cfg` (diff 30): three generation changes, deferred frees, 0 discards,
0 non-edge coverage mismatches, walls 99.51 % within; **floors 97.78 % within (< 98 %)**, all of it after
the teleport to level 1 (L0's checks 99.85 %). A dump of level 1 (`--perf 1`) puts the misses on one
far, steep ceiling with a high-frequency texture: rows 0-59 fail the neighbourhood rule at 7.7 %, rows
140-239 at 0.0-0.2 %; no constant screen shift helps (80.3 % exact unshifted, ~22 % at ±1 px), so it is
not an offset. The host prediction, split by texels per pixel along the row (informative, `38ca4868e`):
floors < 1: 98.14 % exact; 1-2: 93.16 %; ≥ 2: **80.72 % exact, 100 % within one texel** — the Azahar
ceiling's 80.3 % exactly. Where neighbouring pixels sample texels ≥ 2 apart, SW's one-texel misses
appear in no neighbour, so the neighbourhood rule cannot absorb them. The user accepted M2 with this
recorded; a texel-space metric (compare texel indices, not colours) is the fix if a later milestone
needs it.


### M3 pre-implementation (coordinator, 2026-11-03)

Kickoff `KICKOFF_M3.md`. The design checked against the code at `6533f8b01` and the citro3d binary: texcoord1
is already in the vertex, the AttrInfo and the shader (M3 is a front-end change there); `C3D_Init` zeroes the
texture units and sets every TEV stage to a pass-through, a null unit is disabled by the state flush, and
`C3D_SetTexEnv` copies (three new 0.3 rows). Corrections folded into the M3 specification: the `SHRT_MAX`
shading-depth clamp is reproduced by the LUT's clamp, not by clamping the vertex value (M2's note said it
would be kept); texcoord1's `v` is a row centre chosen per polygon, and the LUT's first and last columns hold
the exact clamped values, so the common cases are exact. The colour gate is M2's neighbourhood rule with 8.3's
lit numbers unchanged (no tolerance change; host prediction: lighting adds ≤ 2 per channel, mean ≤ 0.47). The
second film is L08 "Nuke and Pave", the only Marathon 2 level with a light that can go negative (offline
parse of `Map.sceA`).

### M3 pre-implementation, round 1 (critic: REVISE, 1 critical, 1 major, 5 minor; applied)

(1) *Critical:* `C3D_TexBind(1, NULL)` dereferences NULL + 12 (texture.o `C3D_TexBind`+0x10-0x20; Azahar
hides it): unit 1 now keeps the LUT from the first lit frame until `C3D_Fini`, the Copying → Vram step
re-binds before deleting, `tex1_restore_mismatch` counts foreign unit-1 binds; new 0.3 row; section 10 item 6
gains "unit 1 enabled during the present". (2) *Major:* SW's `table_index` is a `short` with no lower clamp,
so a strongly negative (or ≥ 2²³) ambient wraps; L08's light at −32 768.0 gives black (exactly −2³¹) or an
out-of-bounds table, not full bright: the front end computes SW's wrapped index, and polygons where it is
negative or can wrap are excluded on both sides (`c3d_ambient_wrap`); film paragraph corrected (the
coordinator's entry above said "capped at 255: full bright", which is wrong). (3) `alpha_x2` defaults to 0
(M2's written alpha; the ×2 has no M3 use and only adds the UNVERIFIED clamp), run as the `m3-alpha2.cfg`
A/B for M5. (4) Stage 1 and unit 1 are set only after the SAVE block; `lit 0` / `textures 0` frames touch
neither. (5) Host grid replaced by a practical one with bin edges, `d ± ½` and a one-bin `u` nudge. (6) The
infravision plugin also sets the duration in `Triggers.init`. (7) The floor sign guard's fictitious `w`
named as a known inexactness.

### M3 pre-implementation, round 2 (critic: APPROVE, 2 minor, folded)

The wrap exclusion through `c3d_only` is sound on both sides (`filter.cpp`:30-35, :43, :58); leaving unit 1
bound is safe for the present and `view.cpp`'s calibration (neither reads unit 1). Folded: the wrap test uses
the ambient after the `fullbright` rewrite and skips shadeless polygons; `c3d_ambient_wrap` is printed apart
from `excluded`. Console-only (section 10 item 6): the ×2 clamp, PICA's modulate rounding, texcoord1
precision, unit 1 enabled during the present (no visual effect, no cost, no hang).

### M3 acceptance, unit 1 after the frame (coordinator, 2026-11-05; a design change after the code review)

The first acceptance series ran the M3 binary as reviewed (`eff8931b5`): every host-GPU run passed, but every
**software-backend** run stopped after the first lit check: Azahar itself died (no timeout, no exit line;
macOS crash reports: `EXC_BAD_ACCESS` in `Pica::Texture::LookupTexelInTile` on a `SwRenderer` worker). Cause:
with the LUT left bound, unit 1 is enabled during the present, whose shader outputs no texcoord1, so the
unit samples an undefined coordinate. (The same series also lost runs to a full disk, from outside the
session; those were re-run.) Fix: `restore_state` unbinds unit 1 again with the store `C3D_TexBind` makes for
unit 0 (`back/shade.cpp` `shade_unbind_unit1`; the API's own NULL path for unit ≥ 1 reads NULL + 12), the
context layout of citro3d 1.7.1 checked at run time on the first lit frame, fallback = leave it bound
(`unit1_left_bound`). With it, `m3-diff.cfg` L0 under the software backend runs to the end and passes (walls
99.69 % / 0.11, floors 99.87 % / 0.15). The specification's "Binding and the state the present relies on" (1),
the 0.3 rows and section 10 item 6 are revised accordingly; the whole acceptance series is re-run on the new
binary.

### M3 acceptance, unit 1 after the frame — review (critic: APPROVE, 2 minor; 2026-11-05)

On `087ab93cb`, against libcitro3d.a 1.7.1: `__C3D_Context` is the one object `C3D_TexBind` and
`C3Di_UpdateContext` use; offsets (flags +0x20, slot +0x11c) and dirty bit 24 match `C3D_TexBind`'s store; the
flush rebuilds the unit mask from the non-null slots, so unit 1 is disabled at the present's first draw; no
race (main thread only; the unit-0 re-bind's bit 23 and the unbind's bit 24 both survive). Folded: (1) the
Copying → Vram re-bind only in the fallback (two early-discard paths of `view_begin` could otherwise leave
unit 1 bound into the present); (2) `unit1_left_bound` and the layout line join the acceptance, re-run in
full. Note added: the context access needs no LTO.

### M3 acceptance, software-backend films (coordinator, 2026-11-05; investigated at the user's request)

With unit 1 unbound after each lit frame, every acceptance item passed except the two films under Azahar's
**software** backend: Azahar died mid-film (L01 after 96 of 200 checks, L08 after 36), at the same frame in
repeated runs, with no crash report. The user chose to investigate before accepting. Bisection on the L08 film
(`PERF.md` "Stage 10 M3"): `lit 0` runs to the end; texcoord1 values are not it (constant, or clamped to
[−1, 2]: still dies); the LUT's format is not it (RGBA8: still dies); keeping the LUT's linear copy alive
changes nothing. **What decides is the memory region**: textures and LUT both in linear memory → the whole
film passes; one in VRAM and the other in linear memory (either way round) → dies at the first such frame; both
in VRAM → fine until a newly converted texture is sampled from linear memory. **Azahar's software rasteriser
faults when one draw samples textures from FCRAM and VRAM together**; PICA reads physical addresses and the
host-GPU backend is unaffected, so the port's design is unchanged. Under the software backend the films are
run all-linear (`gxq_budget 3`, so nothing is promoted) and **pass** (L01 walls 99.87 %, floors 99.80 %; L08
99.69 % / 99.47 %; 0 non-edge); the configs' notes say so. 8.2's "both backends" rule is met that way for the
films. The console check (two texture units, mixed regions as in the default) is the hardware answer.

### M4 pre-implementation (coordinator, 2026-11-06)

Piece 0 first: M3's negative-ambient Azahar test passed on both backends (`198eb38e3`). The M4 specification
(section 7, M4) was then written against the code at `198eb38e3`. Checked against the kickoff: the RLE path
the kickoff calls "written and host-tested" does not exist (`tex_convert` refuses RLE and non-square bitmaps),
so RLE decoding, non-square POT padding and the clamp are new; the cache's LRU eviction, which the kickoff and
section 7's M5 line place in M5, exists since M2 (only the hardening runs stay in M5). Decisions taken in the
specification: one quad per rectangle with corners on pixel boundaries and SW's fixed-point steps evaluated at
the corners, plus a small texel bias chosen by a float24 host model; texcoord1 of a rectangle exact through the
LUT's column 0; tinted without a TEV change (stage 0's alpha already passes the texel's); play frames recorded
once and replayed (into C3D, or into SW after a discard or a scan reason, never a second traversal except on
recorder overflow, whose only side effect is the weapon flutter's C `rand()`); the copy of a play frame keeps the
pixels C3D did not cover, as SW does; only resource discards count toward "10 in a row". Pending: the colour
gates for sprites, weapons and the full frame (a tolerance change: the user's decision) and the critic's review.

### M4 pre-implementation, gates (coordinator, 2026-11-06; user decision; a tolerance change)

8.3's sprite/weapon and full-frame rows predate M2's neighbourhood rule. Asked with the numbers, the user chose:
**sprites, weapons**: M2's neighbourhood rule at M3's lit numbers (≥ 98 % within Δ ≤ 4, `mean_nb` ≤ 1.5),
non-edge coverage mismatches ≤ 0.5 % of A's covered pixels; **full frame** (and the play check): every pixel
compared (`Coverage::All`, no coverage clause: coverage is gated per class), neighbourhood ≥ 97 % within Δ ≤ 4, no
mean gate, landscapes and static excluded on both sides (M5's classes). **tinted** stays 8.3's row as written
(strict ≥ 98 % Δ ≤ 3, coverage ≤ 0.5 %).

### M4 pre-implementation, round 1 (critic: REVISE, 3 major, 8 minor; applied)

(1) The sprite/weapon gates could pass with no sprite on screen (`c3d-diff.py` gives 100 % on `common == 0`):
`--min-covered N` added; `weapons` dropped from `m4-tinted.cfg` (an invisible player's weapon is `kTinted`).
(2) `Coverage::All` counted pixels both sides leave empty as passing: `common` is now "A or B differs from the
clear", `empty` printed apart; the play check is judged by `c3d-diff.py`, which also fails on zero play checks.
(3) The trim analysis missed case (iii) (no bottom trim when `ty1 >> 16 == last`: SW reads the next column's
header byte) and "every difference in the band" is false for runs of 1-2 rows: both corrected; the host test
transliterates the column loop with (iii)'s read and bounds the non-band share (0.057 % measured). Minors: the wrap
is part of the cache key and reaches the VRAM copy (4); the float24 model is UNVERIFIED and in section 10 (5); the
plugin holds invisibility in [1 500, 2 000] ticks (no flicker, k 15) (6); play needs `textures 1 && lit 1` and the
M1-M3 configs get `play 0` (7); cheap refusals before recording (8); the "ten in a row" run is defined for every
frame kind (9); the copy pass tests the sentinel before alpha, and the tinted-over-hole difference is named (10);
`:858`, no `flash_over` for rectangles, tinted `k ≥ 32` unsupported (11). Also from the host tests (main session):
`rect_bias` is in 1/131072 texel with default 1 (a bias ≥ 1/65536 flips SW's truncated positions just below
integers), and `blend_left_tinted` is dropped (citro3d has no blend getter; the present diff is the proof).

### M4 pre-implementation, round 2 (critic: APPROVE, 3 minor, folded)

All round-1 findings checked fixed in the spec and in the early core/back code. Folded: (1) a float32-storage
model row in the host test (the same agreement as double on the grid: 100.000 % at ε = 1/131072); (2)
`Coverage::All`'s neighbourhood skips empty A-neighbours when only one side drew the pixel; (3) `blend_of`'s
comment: the front end refuses tinted `k ≥ 32`, the clamp is a guard.

### M4 commit review (critic: REVISE, 1 major, 9 minor; applied before the acceptance)

On `f389f85a5`. No renderer defect: record/scan/replay, the dest handling, the blend switch and its restore on
every path, `rect_setup` and the tint factor checked against the sources. (1, major) `c3d-diff.py play` pooled the
play checks: each check is now gated on its own (a synthetic log with one bad check among 95 fails). Minors: (2)
`--expect-offs` replaces the "a C3D frame" rule by "c3d 0, cmdbuf discards > 0"; (3) Classic's 640 × 320 world view
is supported, not refused: a Classic `play_check` run is added; (4) the 512-slot linear lookup is not measured: a
hash on `(bitmap, tables)` is an M6 prerequisite (recorded there); (5) per-class gates are separate invocations
(the series and `PERF.md` give the exact lines); (6) the diff configs get `play 0`, play has its own runs incl. a
film; (7) the "M4 console check, short version" is written with the record; (8) the play check reads `dest` through
`row_addresses[y]`, as the copy writes; (9) `copied` and `empty_frames` in `[c3d] play final`, and
`view_play_ready` refuses a size `view_ensure` failed for; (10) the Invisibility Test's compilers are made blind and
deaf. The first acceptance series was stopped (four runs in) and re-run in full on the fixed binary; its L6/L20
sprite gates had failed on `--min-covered` (no sprite on those bench spots), which the tooling is meant to catch.

### M4 commit review, round 2 (critic: APPROVE, 3 minor)

All round-1 findings fixed (4 and 7 deferred as recorded). Minors: (1) narrowing the sprite gate to L0 and the
films is a gate change: taken to the user (next entry); (2) `m4-play-film-swbe.cfg`'s header named the non-swbe file;
(3) a comment block in `back/view.cpp` headed the wrong function. (2) and (3) are folded after the acceptance series
(comment lines only: no code change, so the accepted binary's code is unchanged).

### M4 acceptance, sprite gate (coordinator, 2026-11-06; user decision; a recorded exception)

The L6 and L20 bench spots show no sprite (0 sprite pixels in all 20 checks of the stopped first series), so
`--min-covered` fails their sprite class by design. The user accepted: **sprites are gated on L0's bench spot and
the two films** (L01 combat, L08); L6 and L20 gate weapons and the full frame.

### M4 acceptance, play gate on killed runs (coordinator, 2026-11-06)

Three play gates of the final series failed with "no [c3d] play final line": the L01 film in play mode (both
backends) and script 07 end on the timeout kill, and `[c3d] play final` is printed only at a clean exit
(`frame_final_stats`). Their per-second `[c3d] play frames=…` lines carry the same cumulative counters; `c3d-diff.py
play` now falls back to the last one when the final line is missing (it says so). Script 14 opens its terminal at
L0's start, which is all landscape: 0 C3D frames by construction, gated with the new `--min-c3d 0`. A tooling fix:
the renderer and the accepted binary are unchanged; re-gated from the same logs, all four pass.

### M5 pre-implementation (coordinator, 2026-11-07)

The M5 specification (section 7, M5) was written against the code at `12550c9be`. Checked against the kickoff:
there is no landscape conversion and `texcache_get` refuses a non-rectangle A8 texture; `fail_every` is not parsed;
the first-10-frames counts exist only as KiB summed over all levels; the allocator applies one wrap to both axes;
and `_solid` (smear) polygons are drawn by no SW mapper in a release build, so M5 skips them instead of sending the
frame to SW. Decisions taken in the specification: landscapes affine with SW's 1/65536 positions plus a 1/131072
bias, padded rows and a per-axis wrap (repeat s, clamp t) in the cache key, RGBA8 (RGB565 fails Δ ≤ 2 by itself),
VertRepeat as SW's spans split at band rows; static as a 128 × 128 noise texture of SW's LFSR states with alpha
`max(2, hi)` and the alpha test `max(1, drop >> 8)` (density within 1.2 points of SW by construction, no sentinel
collision), the A8 mask on unit 0 for transparent bitmaps, a Lit/Static mode switched between draws like M4's blend;
the static check measured against a third replay with the drop forced to 0 (SW's coverage); `fail_every` on the
cache's create() (hard failures; soft ones are absorbed by evict-and-retry) with a cache audit; per-level start
counts; G2-a measured on the final binary with M0's `R_est` and a fresh bench, predicted to fail its second half.
Gates: landscape 8.3 as written (strict) with its own printed tolerance; static density measured pooled per run
and per check over 4 096 coverage pixels (the ±2 points of 8.3 unchanged). Pending: the critic's review.

### M5 pre-implementation, round 1 (critic: REVISE, 2 major, 7 minor; applied)

(1) The landscape row constant is `R − (R >> 1)`, not `R >> 1` (wrong for odd R; the host grid has R = 135). Found
independently by the first host run, which also showed that ε alone leaves float32 rows at 96.6 % (SW's exact-integer
positions): a per-polygon row offset δ, the middle of the feasible interval, gives 99.996 % (float32) and 98.16 %
(float24 model). (2) A per-check static density gate at |C| ≥ 4 096 fails on sampling noise alone (simulated: 5-10 %
of checks): the gate is pooled per run with ΣC ≥ 40 000 (σ 0.25 points), per-check figures printed; the noise
texture holds all 65 535 states (256 × 256, 256 KiB; 9.1 updated). Minors: the play check's static exclusion is a
change to what a gate includes, so it goes to the user (3); `fail_every 50` runs over the L01 film (≈ 29 forced
failures) with a floor of 10, and a noise allocation failure discards `tex` (4); the Mask kind for square wall
bitmaps is stated (5); the noise RGB is host-checked against SW's pixel formula through the readback's ARGB, and
laid out down columns like SW's walk (6); G2-a's prediction no longer contradicts its own scaling (7); the alpha
floor of 2 goes: A = min(255, hi + 1), ref = min(255, (drop >> 8) + 1), host-asserted ≤ 0.39 points (≤ 0.78 for
drop ≥ 0xFF00) (8); see-through liquids, the honest VRAM sum and the infravision burst log added (9).

### M5 pre-implementation, round 2 (critic: APPROVE, 2 minor, folded)

The row offset's pinned-line argument, the noise encoding `A = min(255, hi + 1)` / `ref = min(255, (drop >> 8) + 1)`,
the 256² column-ordered texture and the pooled static gate checked. Folded: the VertRepeat band formula carries
`R − (R >> 1)` and δ (1); section 10 item 6 gains the M5 console items (float24 landscapes with the band fallback,
MODULATE rounding on masked static, the 1 MiB conversion and copy, G2-b) (2). The VRAM table counts the noise at
256 KiB.

### M5 implementation, landscape size (coordinator, 2026-11-07; a design correction)

The first L6 run excluded every landscape: the bitmaps are 540 × 1 024 (4 MiB padded), not 270 × 512 (3.2, the
kickoff, revision 1 of the spec). Not a tolerance or gate change: the mapping, the padding rule and the gates stand;
the texture is cut at row 512 into two cache entries and a polygon crossing the cut goes through the span path
already built for VertRepeat (section 7, M5, "Implementation, session 1"). Memory per landscape is 2 MiB, not 1;
RGB8 is the recorded option. To be checked in the commit review.

### M5, play check and static, user decision (2026-11-07)

Asked with the first L01 play run's numbers (9 of 155 play checks had static; 2 795 of 14.5 M compared pixels were
SW's static coverage), the user chose to **leave SW's static coverage out of the play checks**, as 8.3 already does
for the frame check; static is gated by its own density check. In the same answer the user decided that stage 10
**continues until stereoscopic 3D (M9)** whatever G2-a shows (0.5).

The play check's mask is SW's static primitives drawn alone (over clear 0), so it also leaves out static pixels
that later geometry hides in the real frame: conservative, counted (`static_px`; critic, M5 code review minor 6).

**Amended the same day (user):** M7 (2x2 supersampling) and M8 (the Renderer preference) are to be built too: stage 10
builds every milestone, M5 → M6 → M7 → M8 → M9; the gates are measured and reported, they no longer decide.

### M5 code review, round 1 (critic REVISE, 2026-11-07)

On `13910b169` (+ the docs commits). Ruling: the row-512 cut is sound for the mapping (part membership is monotonic
in y within a band; clamp on T is safe either side of the cut); its memory side was not. **Major 1** (a 2 MiB part
that cannot get a VRAM block drained the VRAM pool every frame and was re-converted mid-play: L01 play `evict 1359
alloc-fail 1659`, bursts at frames 200/571/3836): fixed in `core/texcache.cpp`: `alloc_promotion` checks that
older victims cover the cap, allocates **before** evicting, and on a failed allocation evicts nothing and blocks the
slot's promotion for its generation (`promotions_blocked`, the third `promo` figure); `lru_victim` takes a landscape
only when nothing else qualifies. Host test `test_m5_promotion_pressure` (a FakeAlloc that refuses VRAM blocks over
1 MiB: 30 frames, 0 evictions, one block; the cap-eviction path; the landscape last). 9.1, 9.2 and section 10 updated;
RGB8 vs FCRAM is M7's decision (9.1). **Major 2** (part 1 and the cut-crossing span path never ran on the GPU): a first
Look Up Test run (`run-azahar.sh --look-up-test`: the view pitches up to 28° and sways) on L6 created no part 1 and
`c3d_land_spans=0`: at Marathon 2's pitch limit (~30°) rows past 512 never reach the screen (rows 512-539 need the
top line's `k′ ≤ −243`, far beyond `dtanpitch` at 30°), which is why no film shows them. So part 1 is reachable only
through a pinned or VertRepeat case in other scenarios, and a test key does the GPU coverage: **`land_cut N`** (test
only, 64..512, `core/config.h`; `landscape_set_part_rows`) moves the cut. A first try at 256 showed the direction: the
sky above the horizon is rows 269 → 539, so a cut at 256 (below the horizon) put the whole visible sky in part 1 and
crossed nothing. **`land_cut 300`** sits 31 rows above the horizon, inside the visible sky from level to 28°; part 0
is padded to 512 rows (row 299 repeated), part 1 holds rows 300-539 (240 → 256); a part 1 over 512 rows is refused.
Host: the whole landscape-vs-SW sweep runs at both cuts (cut 300: rows 99.987 % float32, 98.20 % float24) and the
cut-300 conversion of both parts is checked (the cut-256 version caught part 1's row count capped at the cut).
`m5-lookup(-swbe).cfg` (`land_cut 300` + the plugin) on L0/L6, both backends, in the acceptance series
(`c3d_land_spans` > 0, part-0 and part-1 creates, the strict landscape gate, `ring_hw_v`). The rows were measured
first (new `c3d_land_rows`: the visible sky is rows 319-500 on L6, 302-500 on L0 while the view sways; a cut at 256
lies below the horizon and one at 300 under L6's lowest sky row, both tried), so **`land_cut 400`**. Results (Azahar,
non-authoritative): host GPU L6 / L0: `c3d_land_spans` 236 / 300, both parts created (2 048 + 1 024 KiB), the strict
landscape figure **100.000 %**, `ring_hw_v` 1 017 of 2 048, 0 discards. **Software backend L0: strict 97.72 %**
(neighbourhood 100 %), against 99.88 % for the same look-up at cut 512 (60 spans) and 99.58 % for the plain L0 diff:
every differing pixel of the dumped check is **one column late** (SW's pixel = C3D's left neighbour; 12 distinct x),
i.e. the column texcoord under float24 on the span path's one-line quads (290 span polygons), which the host GPU draws
exactly. This is section 10's open item "landscape texel choice under float24" (columns get no help from δ) with a
measured case; **recorded exception**: the software-backend look-up is gated at 97 %, the host-GPU one strict at 98, and
the console decides (if it shows the same, the pre-stated remedy is the critic's: clip a convex polygon at the cut's
line instead of SW's spans, so it keeps the polygon path's precision). In Marathon 2 at the real cut (512) the span path
carries only VertRepeat / non-convex landscape polygons (40-60 per 60 checks), which pass at 99.58-99.88 %.
**Minors:** 3, an unmasked static's key carries no texture (it never merges with a masked draw of the same bitmap);
4, the audit is tested with a promotion in flight through `forget`, `bump_generation` and `on_retired`; 5,
`landscape_params` (filter.cpp) is the one landscape rule of the filter, the play scan and the rasterizer; 6, recorded
in "M5, play check and static, user decision". Also: `c3d-diff.py play --off-reason` (`m5-fail1.cfg` goes off on
`tex`, not `cmdbuf`).

### M5 code review, round 2 (critic APPROVE, 2026-11-07)

On `663d05820`. Both majors fixed; the software-backend look-up exception accepted (test-only `land_cut 400`; the
production landscape gate stays strict at 98 on both backends; section 10 keeps the look-up as a console item). One
non-blocking minor applied: at the cap, the real heap can refuse a promotion only because the victims still hold their
blocks, so `alloc_promotion` now evicts for the cap and retries once before it blocks; the blocked landscape parts are
counted (`promo done/deferred/blocked(land n)`). Host test: a FakeAlloc whose VRAM heap equals the cap.

### M5 acceptance (coordinator, 2026-11-07)

The series (`build/m5/tools/series.sh`, on `9d6fde66f`) passed every gate whose class was on screen. Gates that fail
only on `--min-covered` (no landscape at L20 or in the L06 / L21 films, 1 858 pixels in L16; per-film static under
40 000) were read by hand: every check with sky was exact on the host GPU. Two harness fixes, no renderer change:
`m5-static-film(-swbe).cfg` asked for 800 checks of a film that lasts ~488 (the summary line never came; now
`diff_frames 450`), and the L06 present-diff run is killed at the film's end before its final line (added
`m5-present-diff-l0.cfg`, L0's bench spot, which shows static and exits cleanly: 46 checks, 0 differences). After the fix the host-GPU static gate passes
(+0.37 points, ΣC 81 730); under the software backend the 450 checks hold ΣC 35 820 (+0.49 points), under the
40 000 minimum because the film's last static stretch comes after check 450 (the first run reached 63 426 at −0.35
points): read by hand, no tolerance change. G2-a as in `PERF.md` "Stage 10 M5".

### M3-M5 console short checks (coordinator, 2026-11-07)

Fifteen launches on the New 3DS, one sitting, code = tag `port-stage10-M5`, no subagents, no code change
(`PERF.md` "Stage 10 M3-M5 — console, short checks"). **No hardware bug found in M3, M4 or M5's pipeline.** What
Azahar could not say, and the console did: PICA's modulate and constant-colour blend round as Azahar's
`(a·b)/255` (lit means within 0.02, tinted bit-exact); texcoord interpolation on rectangles matches at
`rect_bias 1`; unit 1's unbinding is safe on hardware; play frames in flight survive HOME, sleep and Close with
the gate closed. **One predicted miss:** the landscape span path (`land_cut 400`) scores 97.84 % strict against
the 98 % gate: no non-edge coverage mismatch, `within_nb` 99.87 %; it is the float24 behaviour the software
backend showed (97.72 %), not a regression. The user's call whether to adopt the clip-at-cut fallback. It is not
only a test path: the default-config L0 play run (`m5s-pres`, `land_cut 512`) drew `land_spans=827` in 827 frames,
so the span path runs in ordinary play wherever a landscape polygon crosses the cut; that run had no landscape
diff, so the default's exactness on L0 was not measured. **G2-b** (readback − SW busy, stats printing on; ~2 ms lower with `quiet 1`, L0 +3.46): +6.15 / +7.77 / +7.25 ms,
2-3× Azahar: M6's direct mode is the way to remove it; the plan (user decision 2026-11-07) stands. **Two
observations that need a follow-up, neither blocking:** (1) every 1 s window in play mode has one frame of 70-87 ms
(SW: 40-50), and the user saw a regular micro-stutter; cause **found and verified the same day** (`PERF.md` "Stutter follow-up"): `stats_tick()`'s once-a-second printing over
3dslink and to the SD log; with the test-only key `quiet 1` the windows fall to 40-53 ms, the user felt it "much
smoother", and L0's busy time drops 18.41 → 16.36 ms (C3D − SW +5.51 → +3.46 ms), so G2-b is overstated by about 2 ms; (2) static's look was never seen (25 of 827 frames at the bench spot), so `MODULATE` on
masked static rests on the counters alone. Not run: M3 step 3 (L08 film), M4's software rerun and Classic.

### M6 pre-implementation (coordinator, 2026-11-07)

The M6 specification (section 7, revision 0) written against `02c6319d9`. Before it, the user decided the B-b question
(option (b)): `stage10-bb` merged into `stage10-c3d` (`02c6319d9`; after the merge 4 builds 0 warnings, host tests
2 654 263 checks 0 failures, `present_diff` 49 checks 0 differences on L0 with play mode and zero-copy). Findings that
change the design: overlays are drawn into `world_pixels` before the blit (hook 3 redirects two surfaces, not
`main_surface`); `DisplayText`/`DisplayTextCursor` map colours with `world_pixels->format`, so patch 47 also touches
`screen_shared.h`; the crosshair band comes from the preferences, not a 64x64 box; direct mode never clears the view
texture (SW's uncovered-pixel semantics) with seed/sync rules; the unlinked view target and the texture target cannot
coexist at the full pool cap, so `mode direct` uses the texture target for every 400x240 view frame; the overlay surface
is RGBA8888 in linear memory (the band upload reads it directly). Tolerance: 8.3's existing M6 row (≥ 99.9 % Δ ≤ 1); no
change requested.

### M6 pre-implementation, round 1 (critic: REVISE, 4 major, 11 minor; applied)

On revision 0. (1, major) the present precedes `bottom_frame` (`screen.cpp`:1852, :1859), so nothing but the text hid the
view frame's GPU time: hook 3c draws the bottom screen first on a direct frame. (2, major) clearing only `current & dirty`
left expired rows outside the bands (audit and `direct_check` would fail by design): every dirty tile is cleared,
`dirty = current`. (3, major) the sync rule called citro3d with the gate closed or citro3d down: sync only when up, open and
`tex_view`, else counted `sync_skipped_{gate,off,discard}`. (4, major) Classic is 480x240, not 640x320, and its unlinked
target next to the texture target and the overlay texture overflows VRAM at the full pool cap: every view ≤ 512x256 uses the
texture target in `mode direct`. Minors: (5) the overlay exists before a frame is decided direct; (6) frames that clear are
not seeded; (7) `sw_every N` exercises the sync; (8) the direct calibration draws corner quads and fills rows 240-255, and a
wrong band row is "any other row"; (9) 0 pixels Δ > 32 added to the check and capture gates, the capture's mode logged;
(10) composite binds through `__real_C3D_TexBind`, two stale-picture cases recorded; (11) the cursor mapping untested and
the icon path recorded as limitations; (12) 9.2 rows added; (13) G3-a's new information is VRAM only, the seed's GX entry
in the host test; (14) persistent tints recorded as a limitation; (15) the hash's record is a deterministic probe counter
(Azahar's `c3d_draw` showed no change; host 292 → 3.5 ns).

### M6 pre-implementation, round 2 (critic: APPROVE, 2 minor + 1 nit, folded)

On revision 1: all round-1 findings fixed. Folded: (1) hook 3c moves `present_bottom`'s VBlank wait before the composite:
counted as `bottom_pending` (upper bound) and reported with the @60 bench; (2) 9.2's wording on when the overlay buffer is
made aligned with the spec; (3) `sync_skipped_off` also counts play off with citro3d up (noted). Console items unchanged,
plus whether the bottom screen's VBlank wait ever happens at @60.

### M6 code review (critic: APPROVE, 0 major, 6 minor; folded before the acceptance)

On `d6b252cee`. Patch 47's fencing and OFF identity, eligibility, bands, hook 3c, every citro3d call behind the gate, the
index, target/texture lifetimes, the composite's binds and restore and the zero-copy interplay checked. Folded: (1) mode
direct's readback frames fill no sentinel (`view_end_play_full` checks none; it cost a 512 KiB fill and flush on every
ineligible frame, Classic included); (2) the dirty-tile rule moved into core (`overlay_tiles_to_clear`) and tested on a
simulated 400x240 surface over 5 000 random band sequences, plus the GX accounting of the composite (4 bands + 2), the
seeded view frame and `direct_check`'s own frames (which fit `gxq_budget 3`); (3) after an audit finds a leak every tile is
cleared next frame (the dirty mask records bands, not writes); (4) `direct_check`'s frames re-check the gate; (5) a
seed_check frame redrawn by SW is counted as SW, and a calibration view frame forgets the texture; (6) `FrameKind::Composite`
with its own queue warning (≤ 6).

### M6 acceptance, capture gate (coordinator, 2026-11-07; user decision; a recorded exception)

The direct-vs-readback captures (acceptance (2), `overlays 2`, end-of-run top screens of the bench spots) measured
**99.755 / 99.765 / 99.549 %** of pixels within Δ 1 on L0 / L6 / L20 against 8.3's 99.9 %, **0 pixels over Δ 32**, max Δ
10 / 9 / 16. Every differing pixel is an anti-aliased edge of the overlay text (rows 6-69, the position readout), the direct
picture 1-2 levels brighter. Cause, SDL 2.32.10's arithmetic, not the GPU: readback mode blends the TTF glyphs onto the
opaque XRGB `world_pixels` with SDL's fast RGB blitter (÷ 256, floor), direct mode onto the transparent RGBA8888 overlay with
the generic `ALPHA_BLEND_RGBA` (`SDL_blit.h`:460-466, ÷ 255, truncation toward zero), which the GPU then composites
premultiplied; the two roundings differ by 1-2 per layer and compound where the 1-pixel shadow and the glyph overlap. The
GPU composite itself is exact: `direct_check` (the composite against the read-back view with the overlay composited by exact
arithmetic) is 100.000 %, Δ 0, on L0/L6/L20 and the L01 film on the host GPU (Δ ≤ 1 under the software backend), 610 and 420
checks in the films. **User decision (2026-11-07): a recorded exception** — captures that carry overlay text are gated at
**≥ 99.5 % within Δ 1 and 0 pixels over Δ 32**; 8.3's 99.9 % stays for everything else, `direct_check` keeps 99.9 % (it
measured 100 %). Under the exception the three captures pass.

### M6, the texture-cache hash, user decision (2026-11-07; an exception to rule 1)

The hash (M6 prerequisite) showed no change in Azahar's `c3d_draw`. The probe counters show the old scan examined ~780 /
300 / 110 slots per frame on L0 / L6 / L20 (~830 in the L01 film) against ~1 per lookup now. Estimated console saving:
0.03-0.15 ms per frame (the scan's ~25 KB per frame exceeds the 16 KB L1, a cost Azahar does not model), plus up to ~1 ms on
level-start frames; not measurable at the console's ±0.05 ms. Against it: `CLAUDE.md` rule 1 (no measured gain), a second
record of live slots to keep in sync (a miss can only cause a duplicate conversion, never a wrong texture), and
`next_promotion` still scans all 512 slots on every call (at least once per view frame, now the larger scan). **User
decision: keep it** (no pixel can change; the gain cannot be negative). If the scans ever matter, `next_promotion` (a
pending list) is the next target, measured with an A/B key.

### M6 console check (coordinator, 2026-11-07; medium variant, no renderer code written)

Run on the New 3DS at `port-stage10-M6` (code equal to the tag): 15 launches, `PERF.md` "Stage 10 M6 — console, short check".
Findings: **no hardware bug.** (1) The frame-indexed test passes on every capture that went through the direct composite
(18 of 18 @60; 17 of 17 @30). The runbook's strict rule "every capture ok" is not met @30 (17 of 24): presents 120-125 are menu
and loading-screen presents (the first game frame stats print at present 223 on the console) and present 1803 is one
non-composite present (probably a gamma readback frame, inferred from counters). The capture window (120, 600, 1200, 1800)
is fixed in `present.cpp`; it is placed for Azahar's faster level load. Not changed (no code in scope); a later
harness edit could start the window from the first game frame. (2) Steps 2 and 3 pass as in Azahar. (3) The bench: direct holds
60.5 fps @60 on all three spots, the goal's >= 55 is met; **the software renderer also holds 60.5 on the console**, so the goal
alone does not show direct mode's value: busy time does (−0.66 / −0.37 / −0.80 ms against SW, one run per cell; the console
@30 comparison was not run). Readback stays under 55 (G2-a/G2-b again). (4) G3-b: the view's GPU time is 1.3-1.5 ms; whether
that leaves room for M7's 4x fill is for the M7 plan (a linear 4x would be ~5-6 ms, an unverified extrapolation: fill is not
the only cost). Open for M7: the two VRAM questions in section 10 item 13 were not reached.

### M7 moved to the end (coordinator, 2026-11-08; user decision)

After the M6 console check the user asked to do M8 and M9 first and **M7 last**: the order is now M6 → **M8 → M9 → M7**.
Checked in the design before agreeing: neither M8 (a preference, `PATCHES.md` 48) nor M9 (needs direct mode, section 6) depends
on M7; the two interact only through VRAM. 9.1's direct column is 5 815 of 6 144 KiB, M9 adds about 887 KiB (second screen
target and second view target), so M9 shrinks the texture cache cap, and M7's 1024x512 view target (2 048 KiB, two eyes would
not fit) means SSAA is probably off while 3D is on: a decision for M7's spec, noted in M7's and M9's headings. Side benefit: M9's
2x fill gives the first console measurement of the GPU's fill headroom before M7's 4x (G3-b today: view 1.3-1.5 ms). M8's open
question, which `debug-c3d` settings the shipping "GPU (experimental)" means, was answered the same day (user, option a): the whole bundle `renderer c3d`, `mode direct`, `play`, `textures`, `lit`, `overlays`; the `debug-c3d` file still overrides it (`KICKOFF_M8.md`).

### M9: non-stereo frames and the ramp-up (coordinator, 2026-11-08; user decision)

Asked whether the frames that fall back from direct mode (readback, software) would disable 3D. Answer recorded: only for that
frame. In the console runs 100 % of bench frames were direct; in a hand-played session 63 of 1 799 frames were pause-darken
readback and 210 more were map frames that never reach `render_frame`; damage and underwater flashes are readback too (5.4).
The design had not said what the 3D screen shows then. **User decision: ease the 3D back in with a ramp** (section 6, M9 block).
Written into M9's spec now, not after, so the spec answers it first: one image to both eyes on non-stereo frames, the parallax
ramps from 0 to the slider on the return, a host-testable state machine, `stereo_ramp N` tuned on the console. Not decided:
the ramp length, and whether a ramp is wanted for a slider change (proposed: yes, same path).
