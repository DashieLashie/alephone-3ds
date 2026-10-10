# Aleph One 3DS port: stage 3 prerequisites

Recorded 2026-10-10 on branch `3ds-port`, on top of stage 2 (`9266d74`).
Each section records the decision, the commands run and the verification.
Stage 3 itself (CMake port, dependency builds) has not started.

## 1. Library install location

**Decision:** (a) take ownership of `$DEVKITPRO/portlibs/3ds` once, so stage 3
installs into it without `sudo`. Rejected: (b) `sudo` for every install step,
(c) a repo-local prefix (it would split libraries across two prefixes and
change the brief).

**Command** (run by hand in Terminal.app, since `sudo` needs a TTY):

```sh
sudo chown -R "$(whoami)" /opt/devkitpro/portlibs/3ds
```

**Verification:**

```
$ ls -ld /opt/devkitpro/portlibs/3ds
drwxr-xr-x@ 7 dashielashie  wheel  224 Oct 10 01:45 /opt/devkitpro/portlibs/3ds
$ [ -w /opt/devkitpro/portlibs/3ds ] && echo WRITABLE
WRITABLE
$ find /opt/devkitpro/portlibs/3ds ! -user "$(whoami)" | wc -l
0          # of 254 entries
```

Only `portlibs/3ds` changed: `/opt/devkitpro/portlibs` and
`/opt/devkitpro/devkitARM` are still `root:wheel`.

**Caveat:** `dkp-pacman` runs as root, so files from later package installs
or upgrades (`3ds-*`) will be root-owned again. After a `dkp-pacman -Syu`,
re-run the `chown` if a stage 3 install fails with "Permission denied". Files
installed by our own scripts are not in pacman's database, so a future
dkp-libs package that ships the same files would conflict ("exists in
filesystem").

## 2. devkitPro environment variables

**Decision:** create `~/.zshenv` (it did not exist) with the two exports.
zsh reads `.zshenv` for every shell, including the non-interactive ones used
by tools and build subprocesses; `~/.zprofile` and `~/.zshrc` are read only by
login or interactive shells. No startup file mentioned `DEVKIT` before.

**Command** (run by Claude, no `sudo` needed):

```sh
printf '%s\n' \
  '# devkitPro (3DS toolchain) — read by every zsh, including non-interactive ones' \
  'export DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM' > ~/.zshenv
```

**Verification** (fresh shell with an empty environment):

```
$ env -i HOME="$HOME" PATH=/usr/bin:/bin zsh -c \
    'echo $DEVKITPRO; echo $DEVKITARM; ls $DEVKITPRO/cmake/3DS.cmake; $DEVKITARM/bin/arm-none-eabi-gcc --version | head -1'
/opt/devkitpro
/opt/devkitpro/devkitARM
/opt/devkitpro/cmake/3DS.cmake
arm-none-eabi-gcc (devkitARM) 16.1.0
```

Before the change the same shell printed `DEVKITPRO=[] DEVKITARM=[]`.

## 3. SMDH metadata

**Decision:**

| SMDH field | Value |
| --- | --- |
| Title | `Aleph One` |
| Author (publisher) | `Aleph One developers / 3DS port by DashieLashie` |
| Short description | not chosen yet; stage 3 may use a neutral default |

The author string is 47 UTF-16 code units; the SMDH publisher field holds 64.
Stage 3 passes it to `ctr_generate_smdh(... AUTHOR ...)`.

**Verification:**

```
$ python3 -c 's="Aleph One developers / 3DS port by DashieLashie"; print(len(s.encode("utf-16-le"))//2)'
47
```

## 4. 48x48 icon

**Finding:** `data/icons/48x48/` holds only freedesktop MIME-type icons
(`application-x-alephone-*.png`, `text-x-alephone-mml.png`): a document page
with the logo as a badge, not an application icon. The application icon (the
green Aleph One logo) is `flatpak/alephone.png` (512x512 RGBA, installed by
`flatpak/org.bungie.source.AlephOne.yml:21`), the same artwork as
`PBProjects/AlephOne.icns` and `Source_Files/Misc/alephone32.xpm`.
`PBProjects/AppStore/Marathon*/` icons are Bungie/Marathon artwork and are
not used.

**Decision:** downscale `flatpak/alephone.png` to 48x48 as
`ports/3ds/res/icon.png`.

**Command** (macOS `sips`, re-run to regenerate):

```sh
mkdir -p ports/3ds/res
sips -z 48 48 flatpak/alephone.png --out ports/3ds/res/icon.png
```

**Verification:**

```
$ file ports/3ds/res/icon.png
ports/3ds/res/icon.png: PNG image data, 48 x 48, 8-bit/color RGBA, non-interlaced
$ md5 -q ports/3ds/res/icon.png
fd50e6bcdee03ac38166cf83c2abc5db
$ $DEVKITPRO/tools/bin/smdhtool --create "Aleph One" "smdh test" \
    "Aleph One developers / 3DS port by DashieLashie" ports/3ds/res/icon.png test.smdh
$ echo $?
0
```

The resulting `test.smdh` (scratch file, not kept) is 14016 bytes (`0x36C0`,
the standard SMDH size) with magic `SMDH`, short title `Aleph One` and
publisher `Aleph One developers / 3DS port by DashieLashie`, decoded from the
UTF-16 fields. The icon was inspected visually at 4x: ring, disc and stem are
intact.

## 5. Real-hardware probe

**Decision:** deferred. A 3DS with the Homebrew Launcher is available, but
the run was postponed. `std::filesystem` and SDL2 output therefore remain
verified only in Azahar 2126.2 (see `ENVIRONMENT.md`). The stage 0 build
`build/3ds-probe/aleph-probe.3dsx` is still present.

**To do before the engine first runs on hardware** (commands from
`ENVIRONMENT.md`):

```sh
cmake -S ports/3ds/probe -B build/3ds-probe \
      -DCMAKE_TOOLCHAIN_FILE="$DEVKITPRO/cmake/3DS.cmake" -DCMAKE_BUILD_TYPE=Release
cmake --build build/3ds-probe
# Homebrew Launcher: press Y (NetLoader), then
$DEVKITPRO/tools/bin/3dslink build/3ds-probe/aleph-probe.3dsx   # or: -a <3DS IP>
```

Check `RESULT: all passed` in `sdmc:/3ds/probe.log` (the `sdmc:/3ds` entry
count will match the real folder, not 1) and that the top screen is red for
3 s.

## 6. Branch backup

**Decision:** declined for now. `3ds-port` exists only in this local clone;
the only remote is `origin` (upstream `Aleph-One-Marathon/alephone`), which
is never pushed to.

**Notes for later** (checked, nothing changed):

- The clone is shallow (`git rev-parse --is-shallow-repository` → `true`,
  boundary `fc4c274` = tag `release-20260930`). A GitHub fork of upstream
  accepts a push as is; a new empty repository rejects it ("shallow update
  not allowed") until `git fetch --unshallow origin`.
- `gh` is installed but not logged in; git has the osxkeychain and Git
  Credential Manager helpers and there is an SSH key `~/.ssh/id_ed25519`.
- Offline alternative: `git bundle create <file> fc4c274..3ds-port`.

## 7. Stats thread in `--disable-network` builds

**Problem:** with `A1_NO_NETWORK`, `StatsManager::instance()` still created
`StatsManager_uploadThread` (`Misc/Statistics.cpp:51`), which polls every
200 ms and can never upload. The singleton is constructed by `leaving_map()`
(`GameWorld/marathon2.cpp:627`) and by both `Finish()` calls on quit
(`Misc/interface.cpp:2401,3076`).

**Decision:** fence inside `StatsManager` (option B), not at the three call
sites (option A). Any `instance()` call constructs the thread, so one fence
in the class covers present and future callers. It also keeps game-flow
files (`marathon2.cpp`, `interface.cpp`) free of new `#ifdef`s that could
conflict with upstream merges, and matches stage 2's rule that call sites
outside the network code stay unchanged.

**Change** (`Source_Files/Misc/Statistics.cpp`, +4 lines):

```diff
 	entry_mutex_ = SDL_CreateMutex();
 
+#ifndef A1_NO_NETWORK
 	// do uploads in a separate thread
 	thread_ = SDL_CreateThread(Run, "StatsManager_uploadThread", this);
+#endif
 }
 
 void StatsManager::Process()
 {
+#ifndef A1_NO_NETWORK
 	Entry entry;
 	if (CollectLuaStats(entry.options, entry.parameters))
 	{
 		ScopedMutex mutex(entry_mutex_);
 		busy_ = true;
 		entries_.push(entry);
 	}
+#endif
 }
```

Both fences are needed: `allow_stats` can still be `true` (read unfenced
from a shared prefs file, `preferences.cpp:5078`). If only the thread were
fenced, `Process()` would set `busy_` with no thread to clear it, and
`Finish()` would wait forever in the "Uploading stats" dialog. With both,
`busy_` stays `false`, so `Finish()` takes its `else` branch, and
`SDL_WaitThread(NULL, NULL)` is a no-op in SDL2.

**Verification:**

1. Default builds see the same code. `Statistics.cpp` from `HEAD` and from
   the working tree, preprocessed (`g++ -E -P`) with each configured build
   volume's own flags and `config.h`:

   ```
   [aleph-host-build]          A1_NO_NETWORK: 0  lines 197742 / 197742  PREPROCESSED: IDENTICAL
   [aleph-host-build-3ds-net]  A1_NO_NETWORK: 0  lines 191575 / 191575  PREPROCESSED: IDENTICAL
   [aleph-host-build-nonet]    A1_NO_NETWORK: 1  lines 122689 / 122681  PREPROCESSED: DIFFERENT
   ```

   The no-network difference is exactly the 8 fenced lines (the
   `SDL_CreateThread` call and the `Process()` body). Objects are not
   compared byte for byte, because the debug line table shifts with the
   added lines.

2. Stage 2 no-network build:

   ```
   $ docker run --rm -v "$PWD":/src:ro -v aleph-host-build-nonet:/build aleph-host-baseline \
       bash /src/scripts/3ds/host-baseline/build-3ds-features.sh --disable-network
   ...
      Disabled: network play, metaserver and HTTP support
   ...
   -rwxr-xr-x 1 root root 75779624 Oct 10 20:36 Source_Files/alephone
   EXIT=0          # 0 compiler warnings in the log
   ```

3. Object code (`objdump -drC` on `Misc/Statistics.o` from that build):

   ```
   StatsManager::StatsManager() calls (relocations):
     R_AARCH64_CALL26  SDL_CreateMutex
     R_AARCH64_CALL26  std::__cxx11::_List_base<StatsManager::Entry, ...>::_M_clear()   # unwind cleanup
     R_AARCH64_CALL26  _Unwind_Resume                                                  # unwind cleanup
   StatsManager::Process():
     424:  ret
   SDL_CreateThread references in Statistics.o: 0
   "StatsManager_uploadThread" occurrences in alephone: 0
   ```

Recorded in `STAGE2.md`: behaviour change 6 rewritten and a
`Misc/Statistics.cpp` row added to the fence table.

## Summary

| # | Item | Status |
| --- | --- | --- |
| 1 | Library install location | **Done**: `portlibs/3ds` owned by `dashielashie`, writable |
| 2 | devkitPro environment variables | **Done**: `~/.zshenv`, verified in a clean non-interactive shell |
| 3 | SMDH author string | **Done**: `Aleph One developers / 3DS port by DashieLashie` |
| 4 | 48x48 icon | **Done**: `ports/3ds/res/icon.png` from `flatpak/alephone.png`, accepted by `smdhtool` |
| 5 | Real-hardware probe | **Deferred**: run before the engine first runs on hardware |
| 6 | Branch backup | **Declined**: `3ds-port` exists only in this local (shallow) clone |
| 7 | Stats thread fence | **Done**: `Statistics.cpp`; default build unchanged, no-network build OK |

Nothing blocks stage 3. Items 5 and 6 are open risks, not blockers.
