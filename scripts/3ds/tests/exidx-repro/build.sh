#!/usr/bin/env bash
# Minimal reproducer for devkitARM's 3dsx.ld placing __exidx_start outside
# .ARM.exidx (docs/3ds/PATCHES.md 12, docs/3ds/devkitpro-3dsx-ld-report.md).
#
#   scripts/3ds/tests/exidx-repro/build.sh            # stock 3dsx.ld -> repro.3dsx
#   FIXED=1 scripts/3ds/tests/exidx-repro/build.sh    # 3dsx.ld.patch applied -> repro-fixed.3dsx
#
# Output goes to build/exidx-repro/. Run the .3dsx (3dslink or an emulator):
# it writes sdmc:/3ds/exidx-repro.log. Stock: "RESULT: FAILED (std::terminate
# called…)"; fixed: "RESULT: passed (caught 42)".
set -euo pipefail
: "${DEVKITPRO:?}" "${DEVKITARM:?}"
here=$(cd "$(dirname "$0")" && pwd)
out="$(cd "$here/../../../.." && pwd)/build/exidx-repro"
mkdir -p "$out"
export PATH="$DEVKITARM/bin:$PATH"
F=(-march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft)
name=repro
B=()
if [ "${FIXED:-0}" = 1 ]; then
	# The spec file links with "-T 3dsx.ld%s"; %s searches -B directories first.
	mkdir -p "$out/fix"
	cp "$DEVKITARM/arm-none-eabi/lib/3dsx.ld" "$out/fix/3dsx.ld"
	patch -s "$out/fix/3dsx.ld" "$here/3dsx.ld.patch"
	B=(-B"$out/fix/")
	name=repro-fixed
fi
arm-none-eabi-g++ "${F[@]}" -O2 -Wall -I"$DEVKITPRO/libctru/include" -c "$here/repro.cpp" -o "$out/repro.o"
arm-none-eabi-as -march=armv6k "$here/pad.s" -o "$out/pad.o"
# pad.o goes after the libraries so its 2-byte .ARM.extab input is the last one.
arm-none-eabi-g++ "${F[@]}" -specs=3dsx.specs "${B[@]}" -L"$DEVKITPRO/libctru/lib" \
	"$out/repro.o" -lstdc++ -lctru -lgcc "$out/pad.o" -o "$out/$name.elf"
"$DEVKITPRO/tools/bin/3dsxtool" "$out/$name.elf" "$out/$name.3dsx" >/dev/null
arm-none-eabi-readelf -SW "$out/$name.elf" | grep -E ' \.ARM\.ex(tab|idx) '
arm-none-eabi-nm "$out/$name.elf" | grep -E '__exidx_(start|end)$'
echo "-> $out/$name.3dsx"
