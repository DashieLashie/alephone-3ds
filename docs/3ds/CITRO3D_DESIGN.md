# Aleph One 3DS port: stage 10 — citro3d renderer (design)

Status: design approved (critic round 3: APPROVE); P0/P1 in progress; paused
at G0 for console. Branch `stage10-c3d` (from `3ds-port` `6c3e077`). Written for the c3d-implementer agent, which builds **one
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
| Inside a frame, `C3D_SyncDisplayTransfer`/`C3D_SyncTextureCopy` queue (no wait); outside a frame they run the queue and wait for PPF | `renderqueue.h`:77-78; per critic |
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
≤ 11 + 2 for `C3D_FrameEnd`. The **readback** (5.2) is an out-of-frame
`C3D_SyncDisplayTransfer` but adds **+1 entry to the view frame's queue**,
which is cleared only by `onQueueFinish`'s safe-transfer path or the next
`C3D_FrameBegin` (renderqueue.o onQueueFinish+0x18-0x50, per critic); the
wrapper counts it there. Worst case 24 + 2 + 1 = **27 < 32**. The wrapper
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
`C3D_FrameEnd(GX_CMDLIST_FLUSH)`, then out-of-frame `C3D_SyncDisplayTransfer`
(runs the queue and waits, 0.3) into linear staging,
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
`film <path>`, `ssaa 0|1`, `test_vhalt 0|1`, `odd_surface 0|1`. `run-azahar.sh` gets
`--c3d FILE` (like `--script`, `run-azahar.sh`:201-206). Profiler zones (port
only, `a1_3ds_profile.h`:28-39, `profile_3ds.cpp`:55-57): `c3d_wait`,
`c3d_readback`, `c3d_upload`.

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

### M0 — renderer harness, calibration, null mode (after G1)
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

### M1 — flat-shaded walls only
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

### M2 — textured walls and floors (full bright), texture cache v1
- Add: `core/tex_convert.cpp`, `core/tiling.cpp`, `back/texcache.cpp` (3.3);
  floors/ceilings/liquids, transparent walls, `_SCALE_2X/4X`; hooks 2a/2b
  (`PATCHES.md` 46).
- Accept: host tests 3.3 incl. tex3ds; diff `walls,floors` `fullbright 1`:
  ≥ 98% of non-edge pixels Δ ≤ 2, mean ≤ 1.0, under both Azahar backends
  (8.2); UV offset recorded; load test (`15-load-test.txt`) logs deferred
  flushes and diffs after the level change pass; level start with ≥ 16 new
  textures in one frame passes with queue high-water ≤ 24; `gxq_budget 4`
  run passes (promotions deferred).

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
accounting (2.4 rules, incl. the out-of-frame readback's +1 in the view
frame's queue: worst case 24 + 2 + 1 = 27 < 32), overlay band computation
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
| vertex ring 2 × 16 384 × 32 B (re-sized from M0) | 1 024 |
| index ring 2 × 32 768 × 2 B | 128 |
| readback staging 480x240 | 450 |
| linear texture pool (cap; holds promotions pending) | 4 096 |
| **total** | ≤ 7 234 |

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
   present runs after the suspend (`release()` never returns): open. The error
   screen shows from the main thread, not from a second thread.
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
7. Perspective-correct interpolation and large-UV precision on hardware.
8. Readback-target orientation/flags; GX tiling minimum sizes (SPECULATIVE).
   **Console 2026-10-19:** the P1 present target's orientation and flags are right
   (`present_diff`: 153 checks, 0 differences). Readback (M0+): not reached.
9. `C3D_FrameEnd(GX_CMDLIST_FLUSH)` + ranged flushes vs the whole-heap flush.
10. Level-start hitch with the promotion cap (TextureCopy throughput, PPF
    latency); whether pre-warming is needed.
11. citro3d per-draw CPU cost at 804 MHz with L2.
12. Linear heap free at `main loop ended` with citro3d on.
    **Console 2026-10-19 (P1):** 16 767 KiB of 19 904 KiB free (profile build);
    17 792-17 812 KiB after the release.
13. Stereo comfort/ghosting; SSAA look.

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
   `gx_op()` in-frame; only the readback transfers out of frame (it waits).
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
2. **Readback's queue entry — accepted.** 2.4: the out-of-frame readback
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
