# Releases of the 3DS port

First release prepared 2026-11-17 (user decisions the same day): `stage10-c3d` merged into `3ds-port` (`19cf68747`),
**the GPU renderer on by default** (`24e827ede`, `PATCHES.md` 48), a `.3dsx` zip **and** a `.cia`, shipped as
**experimental without a further console check**. The fork's `master` mirrors upstream Aleph One and is not touched;
releases come from `3ds-port`.

## Building one

```sh
scripts/3ds/make-release.sh 0.1.0-beta1      # -> build/release/0.1.0-beta1/
```

It configures a fresh `build-3ds-release` (`A1_RENDER_C3D=ON`, profiler off, devkitPro's `-O2 -DNDEBUG`), stops if
the build prints a warning, and writes:

| File | What |
| --- | --- |
| `alephone-3ds-VERSION-3dsx.zip` | `3ds/alephone/alephone.3dsx`, `README-3DS.txt` (from `docs/3ds/release-readme.txt`, version and commit filled in), `COPYING.txt` |
| `alephone-3ds-VERSION.cia` | the same ELF, the build's SMDH as icon, banner from `ports/3ds/res/banner.png` + `banner.wav`, romfs from the build, settings `ports/3ds/res/alephone.rsf`; title ID `000400000A1E3000` (keep it: a later CIA then installs over the old one) |
| `SHA256SUMS.txt` | checksums |

The CIA version fields come from `MAJOR.MINOR.PATCH`. The banner is regenerated with
`python3 -I scripts/3ds/make-banner.py` (the port's icon scaled up beside the title; 1 s of silence as its sound).

## Tools (for the CIA only)

devkitPro ships neither `makerom` nor `bannertool`. Built from source (2026-11-17), sources in
`~/Documents/3ds-tools-src/`: `Project_CTR` (`makerom`, 3DSGuy, commit `d52c2b0`) and `3ds-bannertool` (carstene1ns'
maintained fork of Steveice10's archived tool, commit `734d33b`). Build and install into `~/Documents/3ds-tools-src/bin`
(the script's default `TOOLS_DIR`):

```sh
cd ~/Documents/3ds-tools-src/Project_CTR/makerom && make deps && make
cd ~/Documents/3ds-tools-src/3ds-bannertool && cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build
mkdir -p ~/Documents/3ds-tools-src/bin && cp ~/Documents/3ds-tools-src/Project_CTR/makerom/bin/makerom \
  ~/Documents/3ds-tools-src/3ds-bannertool/build/bannertool ~/Documents/3ds-tools-src/bin/
```

(Output paths of the two builds are as their READMEs describe; check them if `cp` fails.)

**Local patch (2026-11-17):** bannertool does not build with AppleClang 17: `source/3ds/lz11.cpp`:115
`u8 pad[padLength] = {0};` is an initialised variable-length array (an error, not just a warning). Changed to
`u8 pad[4] = {0};` (`padLength` is 1..3 there; the write still uses `padLength`). Re-apply it after a fresh clone.
Built 2026-11-17 and first used for `0.1.0-beta1`: makerom accepted `alephone.rsf` without a message; the CIA's NCCH
carries title ID `000400000a1e3000` (checked by reading the header).

## What a CIA changes against the 3dsx (UNVERIFIED until a CIA has booted on the console)

- **Memory.** Under the Homebrew Launcher the port gets the New 3DS extended memory the launcher was started with. A CIA
  asks for it in its settings: `SystemModeExt: 124MB` (New 3DS), `SystemMode: 80MB` (Old 3DS, unsupported). The heap
  allocator (`a1_3ds_platform.cpp` `__system_allocateHeaps`, `PATCHES.md` 25) takes whatever the resource limit grants,
  heap capped at the 96 MiB area and the rest linear, so no code change; the boot log's `memory at boot` lines show the
  split. If the linear heap came out too small for citro3d (about 1 MiB) plus the renderer's pools (design 9.2, up to
  ~7.8 MiB), the GPU renderer would fail to start: the log says so.
- **Sound.** Without the launcher, `ndspInit()` loads the DSP firmware from `sdmc:/3ds/dspfirm.cdc` (libctru) instead of
  the launcher's `hb:ndsp` service. Players without that dump (the DSP1 tool makes it) get no sound; the port's
  "DSP failure" path (`audio_3ds.cpp`) handles it and the game runs silent. Said in the README.
- **Services.** `alephone.rsf` lists the services the build references (`strings` on the ELF: `APT:U`, `cfg:u`,
  `dsp::DSP`, `fs:USER`, `gsp::Gpu`, `hid:USER`, `ir:rst`, `ps:ps`, `ptm:sysm`, `soc:U`) plus libctru's fallbacks. A
  missing one shows as a service error at start.
- **Exit paths.** A7's fatal fallback exit and HOME > Close were verified as a 3dsx (`homebrew 1`, `aptchainload 1`: the
  chainload branch). A CIA is not homebrew to libctru: `aptExit()` takes its application-close branch. The d7 boot line
  (`[3ds] env: homebrew 0, ...`) records which; the exit itself is unverified.

## Not checked on the console before this release (user decision: ship as experimental)

From `NEXT-SPRINT.md`: the rest of "M9 fix console check" (edge control, bench @60, menus in 3D mode (block H), the
Renderer preference, re-entries), "M8 console check" (the preference itself), "M6b console check" (fades on the GPU), and
any CIA boot. What was verified on the console: direct mode (M6), stereo with both M9 fixes (2026-11-15), the fatal paths
(A7). The README tells players to switch the Renderer to Software if they see a problem.

## Publishing (outward-facing: only when the user says)

Push `3ds-port` to the fork (`alephone-3ds`), tag the release commit `3ds-v<VERSION>`, push the tag, and create a GitHub
release on the fork with the zip, the CIA and `SHA256SUMS.txt` (`gh release create ... --repo DashieLashie/alephone-3ds`;
never against `origin`). The GPL needs the source: the tag on the public fork covers it.
