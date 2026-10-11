# Aleph One 3DS port: stage 5 — controls

Recorded 2026-10-11 on branch `3ds-port`, on top of the stage 5
prerequisites (`1dfa858`). Goal: full single-player control on a New 3DS.
Engine changes are fenced with `A1_PLATFORM_3DS` and listed in
[`PATCHES.md`](PATCHES.md) (entries 16-22); the rest is port code in
`ports/3ds/source/input_3ds.cpp`.

| ![Controls dialog](img/stage5-controls-top.png) | ![Bottom screen legend](img/stage5-legend-bottom.png) |
| --- | --- |
| Preferences > Controls with the 3DS defaults and button names | Bottom screen during play |
| ![Turned with the C-Stick](img/stage5-game-turned.png) | ![START menu](img/stage5-game-start-menu.png) |
| Level 1 after walking (Circle Pad) and turning (C-Stick) | START in a game |

*All captured in Azahar by the game itself while a test script played (see "Testing").*

## Summary

| # | Brief item | Result |
| --- | --- | --- |
| 1 | How the engine maps SDL controller input; which indices SDL's 3DS backend reports; a raw-event debug overlay | **Done.** Table below, confirmed in Azahar. `debug-input` flag file: every input event in `log.txt` and on the bottom screen |
| 2 | 3DS default bindings when no prefs exist; remapping UI usable with D-Pad + A/B | **Done**, with two deviations from the brief (below). Remapping verified (bind, clear, cancel, accept, reset) with D-Pad/A/B/START only |
| 3 | Touch: relative mouse look (toggle in prefs), absolute pointer in menus | **Done.** Toggle = Controls > "Touch Screen Aiming" (the mouse-aiming switch, relabelled) |
| 4 | Text entry through libctru swkbd | **Done.** Tapping or pressing A on a text field opens the system keyboard with the field's text |
| 5 | HOME menu / sleep: pause game and audio, resume cleanly | **Done.** Plus HOME > Close: exits in 0.2 s from a game (was 4.6 s) |
| | Hardware (DoD: finish Marathon 2 level 1 with the built-in controls) | **Not done**: no console in this session. Checklist at the end |

## How the engine reads controllers

`Input/joystick_sdl.cpp` opens every joystick SDL recognises as a
**game controller** (`SDL_GameController`). Controller buttons and stick
directions become pseudo key codes after SDL's keyboard scancodes
(`joystick.h`): button *b* is code 415 + *b*; axis *a* is 415 + 21 + *a*
when past +16384 and 415 + 21 + 6 + *a* when past −16384. Key bindings
(`preferences.cpp`) are sets of such codes per action, so a pad button is
bound exactly like a key. On top of that, when "Controller Feel" is "Treat as
Analog Stick", the axes bound to Turn Left/Right and Look Up/Down are read
as analog aim (`process_joystick_axes()`), once per game tick.

In menus, `shell.cpp` handles the D-Pad and A on the main menu;
`sdl_dialogs.cpp` maps D-Pad = previous/next widget, A = click the focused
widget, B = cancel. START is hard-wired as the pad's Escape (in a game: the
"cancel the game in progress?" dialog; while binding a key: clear it).

## SDL's view of the controls

SDL 2.32.10's N3DS joystick (`src/joystick/n3ds/SDL_sysjoystick.c`): one
joystick, "Nintendo 3DS", 23 buttons, 4 axes. Button *i* is libctru's
`KEY_*` bit *i*; the axes are the Circle Pad and the C-Stick, scaled from
the HID range ±160 to ±32767 with Y flipped (up is negative). SDL has a
built-in game controller mapping for it. Logged at boot:

```
[3ds] input: joystick 0 "Nintendo 3DS", GUID 000010324e696e74656e646f20334400, game controller: yes
[3ds] input:   23 buttons, 4 axes, 0 hats
[3ds] input:   mapping 000010324e696e74656e646f20334400,Nintendo 3DS,crc:3210,a:b0,b:b1,back:b2,dpdown:b7,dpleft:b5,dpright:b4,dpup:b6,leftshoulder:b9,lefttrigger:b14,leftx:a0,lefty:a1,rightshoulder:b8,righttrigger:b15,rightx:a2,righty:a3,start:b3,x:b10,y:b11,platform:Nintendo 3DS
[3ds] input: 1 touch device(s)
```

Confirmed event by event with `scripts/3ds/tests/input/01-raw-indices.txt`
(`--input-debug`):

| 3DS control | Joystick button / axis | Game controller | Engine code (bindings) |
| --- | --- | --- | --- |
| A, B, X, Y | 0, 1, 10, 11 | a, b, x, y | 415, 416, 417, 418 |
| SELECT, START | 2, 3 | back, start | 419, 421 |
| D-Pad right, left, up, down | 4, 5, 6, 7 | dpright, dpleft, dpup, dpdown | 429, 428, 426, 427 |
| R, L | 8, 9 | rightshoulder, leftshoulder | 425, 424 |
| ZL, ZR (New 3DS) | 14, 15 | lefttrigger, righttrigger (axis 0 / 32767) | axis 4 / 5 positive: 440, 441 |
| Circle Pad | axes 0 (x), 1 (y) | leftx, lefty | 436/442 (x), 437/443 (y) |
| C-Stick (New 3DS) | axes 2 (x), 3 (y) | rightx, righty | 438/444, 439/445 |
| Touch screen | button 20 (`KEY_TOUCH`) + an SDL touch device | none | none (see Touch screen) |
| HOME, power | not reported (the system handles them) | | |

ZL, ZR and the C-Stick come from the `ir:rst` service; libctru 2.7's
`hidInit()` starts it on a New 3DS (`hidShouldUseIrrst()`), so SDL needs
nothing extra. SDL has no L3/R3/Guide on the 3DS.

**SDL quirk fixed in the port:** SDL takes the first value it is given for
an axis as that axis's resting position and sends no event for it, and the
N3DS driver reports an axis only when its HID value changes. A stick that
rests at exactly 0 (every stick in Azahar) therefore lost its first push:
the deflection only arrived when the stick was let go, so the first walk
and the first turn of a session did nothing. `input_init()` makes the first
stick read report 1 HID unit, which SDL records as the resting position.
Whether real sticks rest at exactly 0 is untested; the fix is harmless
either way.

## Default controls

Used when there is no preferences file, and by "RESET ALL KEYS TO DEFAULTS"
(`preferences.cpp`, `PATCHES.md` 20):

| Control | In a game | In menus |
| --- | --- | --- |
| Circle Pad | move forward/back, sidestep | |
| C-Stick | turn, look up/down (analog, with acceleration) | |
| ZR / ZL | trigger / 2nd trigger | |
| R | action (doors, switches, terminals) | |
| L | Run/Swim, i.e. **walk**: "always run" is on by default | |
| D-Pad left / right | previous / next weapon | previous / next item |
| D-Pad up / down | zoom the map in / out | previous / next item |
| SELECT | map | |
| X | recenter view | |
| Y | auxiliary trigger | |
| A, B | (unbound) | select, back |
| START | "cancel the game in progress?" | clears a binding while binding |
| Touch screen | drag to look (if Touch Screen Aiming is on) | point and tap |

**Deviations from the brief** (decided here; easy to change in one table):

- **One button per action.** The brief has both L and SELECT for the map,
  and the D-Pad and L/R both for weapons. The Controls dialog shows one
  controller binding per action and keeps only that one when accepted
  (`preferences.cpp`, the widget loop and the save loop), so a second button
  would be dropped silently the first time someone presses ACCEPT. SELECT
  keeps the map and the D-Pad the weapons (unambiguous in the brief), R takes
  action, and L takes Run/Swim.
- **Always run** (`_inputmod_interchange_run_walk`) is on by default: with
  both thumbs on the sticks, holding a run button is impractical. L walks.

The keyboard and mouse defaults are upstream's (harmless without them).
**Existing preferences keep their bindings**: a prefs file written by an
earlier build (stage 4) still has upstream's controller defaults (L3, Guide,
...) until "RESET ALL KEYS TO DEFAULTS" is pressed or the file is deleted.

**C-Stick response** (`joystick_sdl.cpp`, `PATCHES.md` 19). Upstream turns
at a speed proportional to the deflection. On the New 3DS nub that is
twitchy near the centre and slow at the edge. The 3DS build rescales the
deflection past the dead zone so that 90% counts as full, squares it (half
deflection turns at a quarter of the speed, for fine aim), and when the stick
is held at full yaw deflection ramps the turn speed up to 2x over half a
second (15 ticks). The Controller Advanced sensitivity, dead zone and
invert settings apply as before.

**Button names** in the Controls dialog are the console's (`sdl_widgets.cpp`,
`PATCHES.md` 21): "ZR", "C-Stick Up", "Circle Down", "SELECT", "D-Pad Left",
instead of "RT", "RS Up", "LS Down", "Back", "Left".

## Touch screen

SDL's N3DS driver reports the bottom screen as an SDL touch device, but it
synthesises mouse events only for a window on the touched display, and the
game's window is on the top screen. So touches never reached the engine. The
port installs an SDL event filter (`input_3ds.cpp`) that turns finger events
into mouse events, in one of two modes:

- **Pointer** (menus, dialogs, a paused game): the bottom screen covers the
  part of the top screen the menus are drawn in. Menus have a 640x480 logical
  size that SDL letterboxes to 320x240 in the middle of the 400x240 top
  screen: the same size as the bottom screen, so the mapping is 1:1 with a
  40-pixel offset (bottom (2,2) → window (42,2); computed from the renderer
  with `SDL_RenderLogicalToWindow`, not hard-coded). Finger down = motion +
  left button down, drag = motion with the button held (sliders work),
  finger up = button up. A tap on a paused game resumes it (the engine
  already did that for mouse clicks).
- **Look** (gameplay input active and mouse aiming on): finger motion
  becomes relative mouse motion; taps do nothing. At the default mouse
  sensitivity (0.25) a drag across the whole bottom screen turns about
  220 degrees; the mouse sensitivity, acceleration and invert settings
  apply. The engine switches the mode where it turns mouse aiming on and off
  (`enter_mouse()` / `exit_mouse()`, `PATCHES.md` 18).

The toggle is the existing "Mouse Aiming" switch, labelled **"Touch Screen
Aiming"** on the 3DS (Controls > AIM). There is no mouse, so it can mean
nothing else, and it keeps the mouse sensitivity and feel settings.

A finger that is already down when the mode changes (the tap that resumed
the game), or when the game comes back from the HOME menu or the keyboard,
is ignored until it is lifted.

SDL's touch events are dropped after translation (the engine does not use
them). Pushed mouse events do not change SDL's own mouse state
(`SDL_GetMouseState()`), so a touch can never act as the mouse-button
"Trigger" binding.

## Text entry

`w_text_entry` (player name, save names, file names, number fields) on the
3DS opens the **system keyboard** when it is clicked: A on the focused
field, or a tap on it (`sdl_widgets.cpp`, `PATCHES.md` 21). The keyboard
shows the field's label as hint ("Name"), starts with the field's text, and
on OK replaces the text; the result goes through the field's filters (Mac
Roman, printable, digits only for number fields, length). Cancel leaves the
field alone. Password fields use the keyboard's password mode; number
fields the number pad.

SDL's own keyboard hook is bypassed: `SDL_StartTextInput()` on the N3DS
driver opens a blocking keyboard immediately, and the engine calls it
whenever a text field *gains focus* (walking past it with the D-Pad would
have opened a keyboard), with no initial text, and appends the result to the
field. The port wraps `SDL_StartTextInput` (`-Wl,--wrap`) to only enable
text events.

Buttons held when the keyboard closes (A, START) are masked until released,
and a touch held on return is ignored, so the key that confirmed the
keyboard does not also click the dialog.

## HOME menu, sleep, applets

An APT hook (`input_3ds.cpp`) runs inside `aptMainLoop()`, i.e. inside
`SDL_PumpEvents()` on the main thread:

- **Suspend** (HOME menu, a library applet such as the keyboard) and
  **sleep** (lid closed): pause SDL audio (the OpenAL mixer stops being
  called), and send the window a focus-lost event. The engine already
  handles that: a game in progress is paused (and its OpenAL sources), the
  main menu's demo timer stops.
- **Restore / wake-up**: resume SDL audio, send focus-gained and exposed
  (dialogs redraw), redraw the bottom screen. A paused game stays paused
  until a button or a tap, as when a desktop window loses focus. Nested
  HOME-then-sleep counts as one suspension.

```
[3ds] +91962.1 ms: suspended (HOME menu or applet); SDL audio paused
[3ds] +94977.4 ms: resumed (HOME menu or applet) after 3.0 s; SDL audio resumed
```

The audio probe confirms the mixer stopped: "longest gap 3040 ms" for a
3.0 s HOME visit.

**HOME > Close** (`aptMainLoop()` returns false; SDL then sends `SDL_QUIT`
on every poll). Upstream would open the "cancel the game in progress?"
dialog (which nobody can see), then on the next `SDL_QUIT` play the quit
screens, two cinematic fades and reload the scenario for the menu.
Measured 4.6 s from the request to `exit` (Azahar), while the system
waits. On the 3DS a system close quits without the dialog and skips the
quit screens, the fades and the reload (`PATCHES.md` 16 and 22): **0.2 s**.
From the main menu it was already immediate.

## Bottom screen

During play the bottom screen shows a legend of the default controls and
the touch modes (screenshot at the top). With the `debug-input` flag file it
shows the raw input events instead, as they arrive, the "debug overlay" of
the brief:

```
JOYBUTTONDOWN 15 (ZR)
CTRLAXIS righttrigger 32767
FINGERDOWN 2,2 (bottom px)
MOUSEMOTION 42,2 rel 42,2
MOUSEBUTTONDOWN 1 at 42,2
```

The same lines go to `log.txt` with timestamps
(`[3ds] input +47220.3 ms: FINGERDOWN 2,2 (bottom px)`).

## Testing

**Tool: input scripts.** Azahar here cannot be given key presses (no screen
or accessibility access for this shell). A test script at
`sdmc:/3ds/alephone/debug-input-script` is played from the moment the main
menu appears: `press`/`release`/`tap` buttons, `circle`/`cstick` positions,
`touch`/`drag`/`untouch`, `home MS`/`sleep MS` (the real APT hook runs and
the main thread blocks, as on a console), `swkbd TEXT`/`swkbd-cancel` (the
next keyboard's answer), `screens TAG`, `log`, `quit` (as HOME > Close).
The input is injected below SDL: the libctru functions SDL's joystick and
touch code call (`hidScanInput`, `hidKeysDown`, `hidKeysUp`,
`hidCircleRead`, `irrstCstickRead`, `hidTouchRead`) are wrapped with
`-Wl,--wrap`, so the whole path (SDL's N3DS driver, game controller
mapping, event filter, engine) is exercised. Without a script the wrappers
pass through (plus the post-keyboard debounce and stick priming). Checked
in the ELF: `N3DS_PumpEvents`, `N3DS_JoystickUpdate` and `N3DS_PollTouch`
call the wrappers, and all 3 engine calls of `SDL_StartTextInput` reach the
bypass.

`run-azahar.sh` gained `--script FILE`, `--input-debug` and
`--reset-prefs` (moves the prefs file aside to `*.bak`).

**Results** (Azahar 2126.2, New 3DS mode; scripts in
`scripts/3ds/tests/input/`):

| Script | Checks | Result |
| --- | --- | --- |
| `01-raw-indices.txt` (`--input-debug --reset-prefs`) | every button except A, both sticks, touch | indices as tabled above; touch (2,2) → window (42,2) |
| `02-prefs-explore.txt` | open Preferences by tap, D-Pad, A opens a section, B twice back to the menu | pass |
| `03-player-name.txt` | tap the Name field: keyboard hint "Name", initial "Bob User", answer "Durandal 3DS"; a cancelled second keyboard changes nothing; ACCEPT; reopen | pass; `name="Durandal 3DS"` in the prefs file |
| `04-controls-explore.txt` | Controls dialog with 3DS defaults and names | pass (screenshot above) |
| `05-remap-dpad.txt` | A starts binding ("waiting for button"), Y binds Y; START clears; B cancels the dialog (nothing saved); bind again + ACCEPT persists; RESET ALL KEYS restores | pass, every step captured |
| `06-gameplay.txt` | new game (D-Pad + A); Circle Pad walks, C-Stick turns, ZR fires, touch drag looks, SELECT map, HOME 3 s pauses (B resumes), START dialog (B = no), HOME > Close | pass; exit 0.2 s after the close request |
| `07-suspend-menus.txt` | HOME and sleep at the main menu; HOME with a dialog open and a finger held across it | pass: dialog redrawn, held touch ignored |

Non-3DS check (`ppcheck.sh`, both volumes, 8 TUs): `shell.cpp`,
`interface.cpp`, `mouse_sdl.cpp`, `joystick_sdl.cpp`, `sdl_widgets.cpp`,
`sdl_dialogs.cpp` (includes `sdl_widgets.h`) byte-identical; `main.cpp` and
`preferences.cpp` differ only in `__LINE__` values inside logging macros
(4 and 20 lines, inspected). All 0 diagnostics. 3DS build: 0 warnings.

**Problems found by the tests and fixed:**

1. The first push of each stick was lost (SDL's resting-value logic, above).
2. Touch look swallowed the first drag of a game: entering gameplay blocked
   touches "until the finger is lifted" even with no finger down.
3. HOME > Close from a game took 4.6 s; the reload of the scenario
   (`load_environment_from_preferences()`, 3.8 s) and the fades were the cost.

**Observations, not fixed:**

- The main menu has no highlighted item until the D-Pad picks one, so A
  alone does nothing there (upstream behaviour; D-Pad down then A starts a
  game).
- Dialogs open with nothing focused; the first D-Pad press focuses the first
  item (in Controls: the tab bar, where A switches tabs). The mouse column's
  look entries are not focusable, so D-Pad rows go keyboard → controller.
- One run of `05` (out of 4) stopped logging 6 s into the script with the
  emulator still running, and was killed by the timeout; three reruns of
  the same script and every later run (about 10) completed. Azahar's own log
  is buffered and was lost with the kill, so the cause is unknown (emulator
  or game). Watch for it on hardware.
- Level 1 takes about 35 s to load in Azahar (from A on the chapter screen to
  gameplay input).

## Hardware checklist (DoD: finish level 1 on a New 3DS)

1. Phase 0 (ftpd): `sdmc:/3ds/dspfirm.cdc` present (else run DSP1), and
   `Marathon2/` in `sdmc:/3ds/alephone/`. **Delete or rename
   `sdmc:/3ds/alephone/Marathon 2 Preferences`** if an earlier build wrote
   one, or use RESET ALL KEYS TO DEFAULTS, to get the 3DS bindings.
2. Optional: an empty `sdmc:/3ds/alephone/debug-input` to log every event
   (and see them on the bottom screen); remove it for the real play-through
   (the legend is more useful).
3. Run (`cmake --build build-3ds --target run-3dslink`, or the `.3dsx`
   from the Homebrew Launcher). In the log: the `[3ds] input:` controller
   lines as above.
4. Check against the table: every button, both sticks (C-Stick and ZL/ZR
   need a New 3DS), the touch pointer in Preferences, the keyboard on
   Preferences > Player > Name (real swkbd: hint, initial text, OK/Cancel),
   HOME during a game (paused, sound stops, resumes on a button), lid
   close/open, HOME > Close (time to exit in the log).
5. Play Marathon 2 level 1 to the exit with only the built-in controls.
   Note anything that is awkward (C-Stick speed: Controller Advanced
   sensitivity; touch look speed: Mouse Advanced sensitivity) and the
   values chosen.
6. Fetch `log.txt` (and `log.prev.txt`) through ftpd.

Still open from the prerequisites and due in the same session: the
hardware numbers of `STAGE5-PREREQS.md` items 1-5 (entropy source, audio
stack/timing, linear heap decision), and the player name from CFGU (item 7).

## Known limitations

- The game and menu screens still use a 640x480 logical size letterboxed
  to 320x240 (`STAGE4.md`); the game view is small.
- One controller binding per action (the Controls dialog's design).
- The Lua console / chat line has no keyboard (it is not bound by default).
- `debug-input-script` is a test tool; it is ignored unless the file exists.

## Files

| Path | Purpose |
| --- | --- |
| `ports/3ds/source/input_3ds.cpp` (new) | touch translation, APT hook, swkbd, bottom-screen legend / debug overlay, raw event log, test script player, HID and `SDL_StartTextInput` wrappers, stick priming |
| `ports/3ds/include/a1_3ds_platform.h` | `input_init`, `set_touch_look`, `exit_requested`, `text_input` |
| `ports/3ds/source/a1_3ds_platform.cpp` | `elapsed_ms`, `save_screens` for the input module; the script starts at the main menu |
| `ports/3ds/CMakeLists.txt` | `input_3ds.cpp`, `--wrap` for 7 functions |
| `Source_Files/…` (8 files) | `PATCHES.md` 16-22 |
| `scripts/3ds/run-azahar.sh` | `--script`, `--input-debug`, `--reset-prefs` |
| `scripts/3ds/tests/input/*.txt` | the 7 test scripts |
| `docs/3ds/STAGE5.md`, `docs/3ds/PATCHES.md`, `docs/3ds/img/stage5-*.png` | this document, patch log, screenshots |
