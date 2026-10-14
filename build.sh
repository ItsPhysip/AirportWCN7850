#!/bin/bash
# build.sh [out dir]  ->  <out>/QwzMac.kext
# Plain clang against acidanthera's MacKernelSDK (no Xcode). MacKernelSDK is cloned into deps/ on first run.
# Every .c file is compiled as C++, as itlwm does.
set -euo pipefail
cd "$(dirname "$0")"
ROOT=$PWD
OUT=${1:-$ROOT/build}
MKSDK=$ROOT/deps/MacKernelSDK
VERSION=$(cat VERSION)
[ -d "$MKSDK/Headers" ] || git clone -q --depth 1 https://github.com/acidanthera/MacKernelSDK "$MKSDK"
[ -f firmware/FwBinary.cpp ] || { echo "STOP: run scripts/fetch-firmware.sh first"; exit 1; }

OBJ=$OUT/obj; KEXT=$OUT/QwzMac.kext
mkdir -p "$OBJ" "$KEXT/Contents/MacOS"

COMMON=(-arch x86_64 -mmacosx-version-min=13.0 -fapple-kext -mkernel -nostdinc
  -isystem "$MKSDK/Headers" -isystem itl80211/openbsd -isystem itl80211 -isystem include -iquote itl80211/linux -I qwz -I controller -I firmware
  -include controller/PrivateSPI.pch
  -DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT -D__PRIVATE_SPI__ -DIEEE80211_STA_ONLY
  -DITLWM_VERSION=\"$VERSION\" -DGIT_COMMIT=\"$(git rev-parse --short HEAD 2>/dev/null || echo none)\"
  -DQWZMAC_VERSION=\"$VERSION\"
  -x c++ -std=gnu++14 -fno-rtti -fno-exceptions -fno-builtin -fno-common -fno-cxx-exceptions
  -O2 -g -Wno-deprecated-register -Wno-everything)
# Debug builds use -O0 like an Xcode Debug kext build.
[ "${DEBUG:-0}" = 1 ] && COMMON+=(-DDEBUG=1 -DQWZ_DEBUG -O0)

SRCS=$(find itl80211 include/HAL controller qwz firmware -name '*.c' -o -name '*.cpp' | sort)

# Compile in parallel; each object keeps its path so same-named files don't clash.
fail=0; pids=()
for s in $SRCS; do
  o=$OBJ/${s//\//_}.o
  if [ ! -f "$o" ] || [ "$s" -nt "$o" ] || [ -n "$(find qwz include controller itl80211 firmware -name '*.h*' -newer "$o" 2>/dev/null | head -1)" ]; then
    ( xcrun clang++ "${COMMON[@]}" -c "$s" -o "$o" 2> "$o.log" || { echo "FAIL $s"; exit 1; } ) &
    pids+=($!)
    if [ ${#pids[@]} -ge 10 ]; then wait "${pids[0]}" || fail=1; pids=("${pids[@]:1}"); fi
  fi
done
for p in ${pids[@]+"${pids[@]}"}; do wait "$p" || fail=1; done
if [ $fail = 1 ]; then
  for l in "$OBJ"/*.log; do [ -s "$l" ] && grep -q 'error:' "$l" && { echo "== $l"; grep -m 20 'error:' "$l"; }; done
  exit 1
fi

# kmod_info (what Xcode generates for kext targets) must keep C symbol names.
xcrun clang -arch x86_64 -mmacosx-version-min=13.0 -mkernel -nostdinc -isystem "$MKSDK/Headers" \
  -DKERNEL -DQWZMAC_VERSION=\"$VERSION\" -x c -c kmod/kmod_info.c -o "$OBJ/kmod_info.o"

xcrun clang++ -arch x86_64 -mmacosx-version-min=13.0 -fapple-kext -nostdlib -Xlinker -kext -Xlinker -no_data_const \
  -L"$MKSDK/Library/x86_64" -lkmodc++ -lkmod -lcc_kext "$OBJ"/*.o -o "$KEXT/Contents/MacOS/QwzMac"
# Without these the kernel and OpenCore reject the kext ("Invalid Parameter").
for sym in _kmod_info __start __stop __realmain; do
  nm "$KEXT/Contents/MacOS/QwzMac" | grep -q " $sym\$" || { echo "STOP: $sym missing from the kext"; exit 1; }
done
sed "s/@VERSION@/$VERSION/g" Info.plist > "$KEXT/Contents/Info.plist"
echo "QwzMac $VERSION $(shasum -a 256 "$KEXT/Contents/MacOS/QwzMac" | cut -c1-8) $(stat -f %z "$KEXT/Contents/MacOS/QwzMac") B -> $KEXT"
