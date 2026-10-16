#!/usr/bin/env bash
# SD card write speed test for the console (sdwrite.cpp). Output:
# build/sdwrite-test/sdwrite.3dsx; run it with 3dslink -s to see the results
# on the host too.
set -euo pipefail
: "${DEVKITPRO:?}" "${DEVKITARM:?}"
here=$(cd "$(dirname "$0")" && pwd)
out="$(cd "$here/../../../.." && pwd)/build/sdwrite-test"
mkdir -p "$out"
export PATH="$DEVKITARM/bin:$PATH"
F=(-march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft)
arm-none-eabi-g++ "${F[@]}" -O2 -Wall -D__3DS__ -I"$DEVKITPRO/libctru/include" -c "$here/sdwrite.cpp" -o "$out/sdwrite.o"
arm-none-eabi-g++ "${F[@]}" -specs=3dsx.specs -L"$DEVKITPRO/libctru/lib" "$out/sdwrite.o" -lctru -o "$out/sdwrite.elf"
"$DEVKITPRO/tools/bin/3dsxtool" "$out/sdwrite.elf" "$out/sdwrite.3dsx" >/dev/null
echo "-> $out/sdwrite.3dsx"
