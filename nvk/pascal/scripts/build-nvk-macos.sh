#!/bin/sh
# SPDX-License-Identifier: MIT
#
# Build NVK with the Pascal (pg106) backend on the Mac, for OWNER-RUNBOOK.md
# step 3. Nothing is installed: the result is <workdir>/out/libvulkan_nouveau.dylib
# (+ nvk_icd.json next to it), which the step 3-4 programs load with --driver.
#
# Needs what build/build263.sh needs: Xcode command line tools, Rust (stable),
# meson + ninja, bindgen + cbindgen, glslang, Python with mako/packaging/yaml,
# and Mesa's CL compiler (mesa_clc, -Dmesa-clc=system).
#
# Usage: build-nvk-macos.sh [workdir]     (default ~/pascal-test/nvk)
set -eu
WORK=${1:-$HOME/pascal-test/nvk}
HERE=$(cd "$(dirname "$0")" && pwd)
[ "$(uname -s)" = Darwin ] || { echo "macOS only (on Linux use linux-check.sh)" >&2; exit 2; }

sh "$HERE/prepare-mesa.sh" "$WORK"
S=$WORK/mesa
B=$WORK/build
if [ ! -f "$B/build.ninja" ]; then
    # The option set of build/build263.sh, the build the Turing+ driver ships.
    meson setup "$B" "$S" -Dbuildtype=debugoptimized -Dvulkan-drivers=nouveau \
        -Dgallium-drivers= -Dplatforms= -Degl=disabled -Dglx=disabled -Dglvnd=disabled \
        -Dllvm=disabled -Dmesa-clc=system -Dshared-glapi=disabled -Dvulkan-layers= -Dtools= \
        -Dbuild-tests=false -Dexpat=disabled -Dzstd=disabled -Dlibunwind=disabled \
        -Dlmsensors=disabled -Dvalgrind=disabled -Dxlib-lease=disabled -Dgallium-rusticl=false \
        -Dspirv-tools=disabled > "$WORK/setup.log" 2>&1 ||
        { echo "meson setup failed; see $WORK/setup.log"; tail -20 "$WORK/setup.log"; exit 3; }
fi
ninja -C "$B" src/nouveau/vulkan/libvulkan_nouveau.dylib > "$WORK/ninja.log" 2>&1 ||
    { echo "build failed; see $WORK/ninja.log"; grep -E 'error' "$WORK/ninja.log" | head -20; exit 4; }

D=$B/src/nouveau/vulkan/libvulkan_nouveau.dylib
# Same check as build263.sh: a strong undefined symbol makes dlopen fail.
U=$(nm -m "$D" | grep '(undefined)' | grep -v weak | grep 'dynamically looked up' || true)
[ -z "$U" ] || { echo "unresolved strong references:"; echo "$U"; exit 5; }
nm -gU "$D" | grep -q nvkmd_pg106 || nm "$D" | grep -q nvkmd_pg106 ||
    { echo "the pg106 backend is not in $D (nvk-pascal.patch not applied?)"; exit 5; }

mkdir -p "$WORK/out"
cp "$D" "$WORK/out/libvulkan_nouveau.dylib"
cp "$HERE/../icd/nvk_icd.json" "$WORK/out/nvk_icd.json"
echo "built $WORK/out/libvulkan_nouveau.dylib ($(shasum -a 256 "$WORK/out/libvulkan_nouveau.dylib" | cut -c1-12))"
