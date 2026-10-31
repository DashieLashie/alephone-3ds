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
| M7, M8, M9 | Supersampling, preference, stereo — **all optional** | — |

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
   `C3D_FrameEnd`, 0.3), while the CPU draws overlays and the bottom screen.
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
- **B-b (on the go):** the engine edit (5.7.2, `PATCHES.md` 55), the reorder draws (5.7.3), the
  zero-copy path selection (5.7.5), `zero_copy` (5.7.7); B-a's `two_frame` becomes the zero-copy
  present's structure (zero-copy presents are always two-frame).

## 6. Stereoscopic 3D (M9, optional, last; direct mode only)

Two view targets, two linked screen targets (`GFX_LEFT`/`GFX_RIGHT`),
`gfxSet3D(true)` (`gfx.h`:79; `particles/source/main.c`:175-183), slider
`osGet3DSliderState()` (`os.h`:313). One `render_tree()` (centre-eye
visibility); each eye redraws the same ring with `stereo` shifting
`clip.x += k·(w − w0)`; landscapes at max shift; overlays at zero parallax.
Right-eye framebuffers already exist (0.2). GX budget: +1 linked target.

**Risk:** side-eye gaps at portal edges (**approximated**); 2x GPU fill.
**How we test without hardware:** Azahar 3D render mode (`render_3d`,
`qt-config.ini`:290); slider 0 ⇒ eyes pixel-identical.

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
- **Next (user decision 2026-10-27):** kickoff `KICKOFF_M2.md`. The upload code is written only after
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
    is never tuned against Azahar. The same holds for the 0.1 % non-edge coverage rule on grates. `c3d-diff.py log LOG --colour
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
    `δ = −½`, walls and floors separately (it predicts `uv_half`); the test fails only below the gate
    recorded in the Review log (98 % until revised under the reachability rule; round 2); the horizontal coverage model and
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

### M4 — sprites, weapons, invisibility; play mode on (readback)
- Add: rectangles (RLE, flip, clip), mask textures, tinted blend; play mode on
  (frames with landscape/static discard per frame).
- Accept: diff `sprites,weapons,tinted` (tinted over a grey clear, 8.1):
  coverage outside band ≤ 0.5%, colour as M3; full-frame ≥ 97% Δ ≤ 4;
  `12-sprite-check.txt`, `06-gameplay.txt`, 07, 13, 14 pass in play mode;
  bench C3D vs SW recorded.

### M5 — landscapes, static, eviction, fallback hardening
- Accept: diff `landscape` ≥ 98% Δ ≤ 2 (L6); `static` written-pixel density
  within ±2 points of SW; `vram_cap 512` and `fail_every 50` runs pass,
  evictions/discards counted; first-10-frames upload counts logged; 5 films ×
  3 min `diff 30` within tolerance. Gate **G2-a**.

### M6 — direct mode (HUD + Map)
- Touch: `screen.cpp` hook 3 (`PATCHES.md` 47).
- Accept: per-frame mode logged; direct vs readback captures with overlays on
  ≤ 1/channel; ineligible frames (5.4) readback; frame-indexed test in direct
  mode shows N or N−1 @30/@60 with `alt_load`; GX high-water ≤ 24; scripts
  07, 13, 14 pass. Gate **G3-a**.

### M7 — 2x2 supersampling (optional, after G3)
- View at 800x480 (texture 1024x512), halved by `GX_TRANSFER_SCALE_XY`
  (`gx.h`:33-38, readback) or bilinear composite (direct); cache cap lowered
  (9.1).
- Accept: output vs CPU 2x2 box of the same 800x480 render read back
  ≤ 1/channel.

### M8 — Renderer preference (optional)
- Touch `preferences.cpp/.h` ("Renderer: Software / GPU (experimental)",
  default Software, `<graphics c3d_renderer="0|1">`), `PATCHES.md` 48.
- Accept: switches at the next frame; persisted.

### M9 — stereo 3D (optional; only if open question 9 says yes)
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
| shade LUT + noise | — | 128 | 128 | 128 |
| texture cache VRAM pool (cap) | — | 3 584 | 3 584 | 2 048 |
| **total** | 887 | ≤ 4 974 / 5 049 | 5 623 | 5 623 |

In direct mode a dialog switches to the 640x480 menu surface, presented
through the same 512x256 texture; the overlay texture is idle then. Totals
hold in every mode with the default caps.

### 9.2 Linear heap (~17 848 KiB free in game, New 3DS)

| Item | KiB |
| --- | ---: |
| command buffer (one buffer, 0.3; 512 default, up to 1 024 by M1) | 512-1 024 |
| present staging 512x256 RGBA8 (P1 calibration, 5.1) | 512 |
| zero-copy `main_surface` 512x256 (plan B, 5.7.8; from the first HUD + Map game) | 512 |
| vertex ring 2 × 1 024 × 32 B (M0: 2 × max 387, rounded up to 1 024; M1 re-measured over the menu demo and a film: unchanged) | 64 |
| index ring 2 × 2 048 × 2 B (M0: 2 × max 597) | 8 |
| readback staging 480x240 | 450 |
| linear texture pool (cap; holds promotions pending) | 4 096 |
| **total** | ≤ 6 666 |

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
9. `C3D_FrameEnd(GX_CMDLIST_FLUSH)` + ranged flushes vs the whole-heap flush.
10. Level-start hitch with the promotion cap (TextureCopy throughput, PPF
    latency); whether pre-warming is needed.
11. citro3d per-draw CPU cost at 804 MHz with L2.
12. Linear heap free at `main loop ended` with citro3d on.
    **Console 2026-10-19 (P1):** 16 767 KiB of 19 904 KiB free (profile build);
    17 792-17 812 KiB after the release.
13. Stereo comfort/ghosting; SSAA look.
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
