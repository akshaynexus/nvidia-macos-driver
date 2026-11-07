#!/bin/sh
# SPDX-License-Identifier: MIT
#
# Linux "NAK on nouveau" variant, for testing the macOS dylib's shader
# compiler on the real GP106 under Linux nouveau, and for NAK's own tests:
#
#   tree   upstream Mesa 17ca6174 + only the compiler hunks of
#          nvk/nvk-macos.patch (src/nouveau/compiler/**, src/compiler/**,
#          except src/nouveau/compiler/meson.build, a macOS build-only
#          change). NAK is therefore the NAK the macOS dylib carries; NVK
#          and its nouveau (DRM) backend are upstream, so it runs on
#          nouveau.ko. No other source is changed.
#   build  -Dvulkan-drivers=nouveau -Dbuild-tests=true (clang, LLVM, mesa_clc).
#   test   meson test --suite nouveau, and NAK's test binary with
#          nvdisasm_tests (encoder vs NVIDIA's disassembler for SM 5.0-12.0,
#          SM 6.1 included) when $NVDISASM points at a CUDA 12.x nvdisasm.
#          hw_tests are skipped: they need a nouveau GPU (see bundle).
#   bundle <workdir>/bundle/: libvulkan_nouveau.so + ICD json, the nak test
#          binary, the step 3 programs (Linux build), run-nouveau.sh. Copy it
#          to the Helios for nvk/pascal/CROSS-BUILD.md "Real GP106 under
#          nouveau" (owner-approved runs only: it needs the desktop stopped
#          if the dGPU is not already on nouveau).
#
# Usage: build-nak-nouveau.sh [workdir]     (default ~/pascal-build/nak-nouveau)
#   needs the same tools as build-nvk-linux.sh; reuses its tarball/crates
#   when $PG106_NVK_WORK (default ~/pascal-build/nvk) has them.
set -eu
WORK=${1:-$HOME/pascal-build/nak-nouveau}
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../../.." && pwd)
NVKW=${PG106_NVK_WORK:-$HOME/pascal-build/nvk}
FULL=17ca6174dcc6cb22059ac343bc29f8af7800f42e
SHA=602ae1e398fd07c8ac5e0389051836f7cccff77de82c43ec97acdfdfeab3fc0d
S=$WORK/mesa
B=$WORK/build
export PATH="$HOME/.cargo/bin:$PATH"
export PKG_CONFIG_PATH="${PKG_CONFIG_PATH:-$HOME/.local/opt/spirv-llvm-translator/lib/pkgconfig}"
export CC=${CC:-clang} CXX=${CXX:-clang++}
mkdir -p "$WORK"

TAR=$WORK/mesa-17ca6174.tar.gz
[ -f "$TAR" ] || { [ -f "$NVKW/mesa-17ca6174.tar.gz" ] && cp "$NVKW/mesa-17ca6174.tar.gz" "$TAR"; } ||
    curl -fsSL -o "$TAR" "https://gitlab.freedesktop.org/mesa/mesa/-/archive/$FULL/mesa-$FULL.tar.gz"
[ "$(sha256sum "$TAR" | cut -d' ' -f1)" = "$SHA" ] || { echo "checksum mismatch for $TAR" >&2; exit 3; }

if [ ! -f "$S/VERSION" ]; then
    rm -rf "$S" "$WORK/x"; mkdir -p "$WORK/x"
    tar xzf "$TAR" -C "$WORK/x" && mv "$WORK/x/mesa-$FULL" "$S" && rmdir "$WORK/x"
    python3 - "$REPO/nvk/nvk-macos.patch" "$WORK/nak-only.patch" <<'EOF'
import re, sys
p = open(sys.argv[1]).read()
keep = []
for f in re.split(r'(?=^diff )', p, flags=re.M):
    m = re.search(r'^\+\+\+ b/(\S+)', f, re.M)
    if not m:
        continue
    n = m.group(1)
    if (n.startswith('src/nouveau/compiler/') or n.startswith('src/compiler/')) \
            and n != 'src/nouveau/compiler/meson.build':
        keep.append(f)
open(sys.argv[2], 'w').write(''.join(keep))
print(f'nak-only.patch: {len(keep)} files from nvk-macos.patch')
EOF
    ( cd "$S" && patch -p1 -s --no-backup-if-mismatch < "$WORK/nak-only.patch" )
    mkdir -p "$S/subprojects/packagecache"
    [ -d "$NVKW/crates" ] && cp "$NVKW/crates/"* "$S/subprojects/packagecache/" 2>/dev/null || true
fi
if [ ! -f "$B/build.ninja" ]; then
    meson setup "$B" "$S" -Dbuildtype=debugoptimized -Dvulkan-drivers=nouveau \
        -Dgallium-drivers= -Dplatforms= -Degl=disabled -Dglx=disabled -Dopengl=false \
        -Dgles1=disabled -Dgles2=disabled -Dglvnd=disabled -Dllvm=enabled \
        -Dmesa-clc=enabled -Dvulkan-layers= -Dtools= -Dbuild-tests=true \
        -Dexpat=disabled -Dzstd=disabled -Dlibunwind=disabled -Dlmsensors=disabled \
        -Dvalgrind=disabled -Dgallium-rusticl=false > "$WORK/setup.log" 2>&1 ||
        { tail -30 "$WORK/setup.log"; exit 3; }
fi
nice ninja -C "$B" > "$WORK/ninja.log" 2>&1 || { grep -E -A5 '^FAILED' "$WORK/ninja.log" | head -40; exit 4; }

meson test -C "$B" --suite nouveau --print-errorlogs > "$WORK/meson-test.log" 2>&1 ||
    { tail -30 "$WORK/meson-test.log"; exit 5; }
grep -E '^(Ok|Fail|Skipped):' "$WORK/meson-test.log"
if [ -n "${NVDISASM:-}" ]; then
    PATH="$(dirname "$NVDISASM"):$PATH" "$B/src/nouveau/compiler/nak" --skip hw_tests:: \
        > "$WORK/nak-unit.log" 2>&1 || { tail -40 "$WORK/nak-unit.log"; exit 5; }
    grep '^test result' "$WORK/nak-unit.log"
else
    echo "NVDISASM unset: nvdisasm_tests not run (meson test skips them too)"
fi

# ---- bundle for the Helios ------------------------------------------
BU=$WORK/bundle
rm -rf "$BU"; mkdir -p "$BU"
cp "$B/src/nouveau/vulkan/libvulkan_nouveau.so" "$B/src/nouveau/compiler/nak" "$BU/"
strip -S "$BU/libvulkan_nouveau.so" "$BU/nak"
cat > "$BU/nouveau_icd.json" <<'EOF'
{ "ICD": { "api_version": "1.4.363", "library_path": "./libvulkan_nouveau.so" },
  "file_format_version": "1.0.1" }
EOF
VULKAN_INCLUDE=$S/include sh "$REPO/pascal/tests-mac/build-macos.sh" --linux > /dev/null
cp "$REPO/pascal/tests-mac/build/pg106_vk_smoke" "$REPO/pascal/tests-mac/build/offload_selftest" "$BU/"
cp "$HERE/run-nouveau.sh" "$BU/"
( cd "$BU" && sha256sum * > SHA256SUMS )
echo "bundle: $BU ($(du -sh "$BU" | cut -f1)); glibc needed: $(objdump -T "$BU/libvulkan_nouveau.so" "$BU/nak" | grep -o 'GLIBC_[0-9.]*' | sort -V | tail -1)"
