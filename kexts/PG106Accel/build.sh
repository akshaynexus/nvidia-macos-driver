#!/bin/bash
# NullMoth NVIDIA driver for macOS
# Copyright (c) 2026 NullMoth Systems.
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#
# build.sh <out dir>  ->  <out>/PG106Accel.kext, for macOS 26 (Tahoe), with the active Xcode SDK.
# The reconstructed IOAcceleratorFamily2 headers are NVAccel's (kexts/NVRM/accel/re, iofam). pga_tahoe_fwd.h is
# generated on Linux by cross-build.sh (PGA_REGEN=1) and committed; this script only uses it.
set -euo pipefail
cd "$(dirname "$0")"
OUT=$1
SDK=$(xcrun --sdk macosx --show-sdk-path)
K="$SDK/System/Library/Frameworks/Kernel.framework/Headers"
[ -d "$K" ] || { echo "STOP: no Kernel.framework headers in $SDK"; exit 1; }
mkdir -p "$OUT/PG106Accel.kext/Contents/MacOS"
FLAGS=(-arch x86_64 -mmacosx-version-min=26.0 -fapple-kext -mkernel -nostdinc -isysroot "$SDK" -I"$K"
       -I. -I../NVRM/accel/re -I../NVRM/accel/iofam
       -DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT
       -std=c++17 -fno-rtti -fno-exceptions -fno-builtin -fno-common -O2 -Wall -Wno-unused-parameter -g0
       # Kernel.framework's own headers: IOPCIDevice is marked deprecated (PCIDriverKit) and omits 'override'
       -Wno-deprecated-declarations -Wno-inconsistent-missing-override)
xcrun clang++ "${FLAGS[@]}" -c pg106-accel.cpp -o "$OUT/pg106-accel.o"
xcrun clang++ -arch x86_64 -mmacosx-version-min=26.0 -fapple-kext -nostdlib -Xlinker -kext -lkmodc++ -lkmod -lcc_kext \
  "$OUT/pg106-accel.o" -o "$OUT/PG106Accel.kext/Contents/MacOS/PG106Accel"
cp Info.plist "$OUT/PG106Accel.kext/Contents/Info.plist"
echo "PG106Accel $(shasum -a 256 "$OUT/PG106Accel.kext/Contents/MacOS/PG106Accel" | cut -c1-8) $(stat -f %z "$OUT/PG106Accel.kext/Contents/MacOS/PG106Accel") B"
