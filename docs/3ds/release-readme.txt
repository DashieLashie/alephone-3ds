Aleph One for Nintendo 3DS — version @VERSION@ (commit @COMMIT@)
====================================================================

Aleph One is the open-source engine of Bungie's Marathon trilogy. This is an unofficial port to the
Nintendo 3DS family, built for the New Nintendo 3DS / New 2DS XL. Source code (GPL v3, see COPYING.txt):
https://github.com/DashieLashie/alephone-3ds (branch 3ds-port).

This release is EXPERIMENTAL. It ships the new GPU renderer, with stereoscopic 3D, switched on.


What you need
-------------
- A New Nintendo 3DS, New 3DS XL or New 2DS XL with custom firmware (Luma3DS) and the Homebrew Launcher.
  Old 3DS / 2DS models are not supported (untested and probably too slow).
- Game data. The Marathon games are free from the Aleph One project:
  https://alephone.lhowon.org/  (download Marathon 2 "Durandal" for the default setup).


Installing
----------
1. Copy the "3ds" folder from this zip to the root of the SD card. It holds 3ds/alephone/alephone.3dsx.
2. Unpack the game data so that its files (Map, Shapes, Sounds, Images, ...) are in
       sd:/3ds/alephone/Marathon2/
   For another scenario (Marathon, Marathon Infinity, ...) put it in its own folder under sd:/3ds/alephone/
   and write the folder's name in sd:/3ds/alephone/scenario.txt (the game creates that file, with
   instructions, on its first start).
3. Start "Aleph One" from the Homebrew Launcher.

CIA version (alephone-3ds-@VERSION@.cia): install it with FBI or another CIA manager instead of step 1;
the game then starts from the HOME Menu. The game data goes in the same place (step 2). A CIA needs the
DSP firmware dump for sound: run "DSP1" once if sd:/3ds/dspfirm.cdc does not exist yet.


Controls (defaults; change them in Preferences > Controls)
---------------------------------------------------------
  Circle Pad        move, sidestep            C-Stick (New 3DS)   turn, look up/down
  ZR / ZL           trigger / second trigger   R                   action (doors, switches, terminals)
  L                 walk (always-run is on)    Y                   auxiliary trigger
  D-Pad left/right  previous / next weapon     D-Pad up/down       zoom the map
  SELECT            map                        X                   recenter view
  START             end the game in progress   A / B (in menus)    select / back


Graphics options (Preferences > Graphics)
-----------------------------------------
- Renderer: "GPU (experimental)" (the default for new players: if you ran an earlier build, your saved
  preferences may still say "Software"; switch it here) draws the 3D view with the console's GPU and enables
  stereoscopic 3D: raise the 3D slider. "Software" is the original renderer: no 3D, the most tested path.
  If you see a rendering problem, switch to Software and please report it.
- Bottom Screen: "HUD + Map" (default; the view fills the top screen, the HUD and map are on the bottom
  screen) or "Classic" (the original layout on the top screen; always drawn by the Software renderer).
- Framerate Target: the GPU renderer and stereo 3D were tuned at 60.


Known limits
------------
- Stereo 3D works only with the GPU renderer and the HUD + Map layout; menus, the map, terminals and some
  effects are shown flat, and depth eases back in afterwards.
- No network games, no OpenGL renderer, no film export, no Opus/FLAC music.
- Files the game writes (preferences, saves, logs) are in sd:/3ds/alephone/.
