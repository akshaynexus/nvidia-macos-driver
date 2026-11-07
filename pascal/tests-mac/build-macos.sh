#!/bin/sh
# SPDX-License-Identifier: MIT
#
# Build the OWNER-RUNBOOK.md step 3-4 test programs on the Mac:
#   build/pg106_vk_smoke     step 3: Vulkan smoke (fill, compute, clear, repeat)
#   build/pg106_offload      step 4: GP106 renders into IOSurfaces, the Intel GPU composites
#
# Needs the Xcode command line tools and Vulkan headers. The headers come
# from (first found): $VULKAN_INCLUDE, the Mesa tree nvk/pascal/scripts/
# prepare-mesa.sh made ($MESA_TREE/include, default ~/nvmtl-build/pascal/mesa),
# /opt/homebrew/include, /usr/local/include. No Vulkan loader is needed: both
# programs dlopen NVK directly, the way the Metal plugin does.
#
# Linux (to test the tests, any Vulkan GPU): sh build-macos.sh --linux
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=$HERE/build
mkdir -p "$OUT"

pick_vk() {
    for d in "${VULKAN_INCLUDE:-}" "${MESA_TREE:-$HOME/nvmtl-build/pascal/mesa}/include" \
             /opt/homebrew/include /usr/local/include /usr/include; do
        [ -n "$d" ] && [ -f "$d/vulkan/vulkan.h" ] && { echo "$d"; return; }
    done
    echo "no vulkan/vulkan.h found: set VULKAN_INCLUDE (see the comment at the top)" >&2
    exit 2
}
VKI=$(pick_vk)
echo "vulkan headers: $VKI"
CFLAGS="-std=c11 -O1 -g -Wall -Wextra -I$VKI"

if [ "${1:-}" = "--linux" ]; then
    cc $CFLAGS -D_POSIX_C_SOURCE=200809L -o "$OUT/pg106_vk_smoke" "$HERE/vk_smoke/pg106_vk_smoke.c" -ldl
    cc $CFLAGS -D_POSIX_C_SOURCE=200809L -o "$OUT/offload_selftest" \
        "$HERE/offload_app/offload_selftest.c" "$HERE/offload_app/offload_vk.c" -ldl
    echo "built $OUT/pg106_vk_smoke $OUT/offload_selftest (run them with --any)"
    exit 0
fi

[ "$(uname -s)" = Darwin ] || { echo "this is the macOS build; use --linux elsewhere" >&2; exit 2; }
SDK=$(xcrun --show-sdk-path)
CC="xcrun clang -isysroot $SDK -mmacosx-version-min=11.0"

$CC $CFLAGS -o "$OUT/pg106_vk_smoke" "$HERE/vk_smoke/pg106_vk_smoke.c"
$CC $CFLAGS -o "$OUT/offload_selftest" "$HERE/offload_app/offload_selftest.c" "$HERE/offload_app/offload_vk.c"
$CC -fobjc-arc -O1 -g -Wall -I"$VKI" -o "$OUT/pg106_offload" \
    "$HERE/offload_app/pg106_offload.m" "$HERE/offload_app/offload_vk.c" \
    -framework Cocoa -framework QuartzCore -framework IOSurface -framework Metal
# Ad-hoc signature: enough to run from Terminal; no entitlements are needed.
for b in pg106_vk_smoke offload_selftest pg106_offload; do codesign -s - -f "$OUT/$b" >/dev/null; done
echo "built $OUT/pg106_vk_smoke $OUT/offload_selftest $OUT/pg106_offload"
