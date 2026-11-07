#!/bin/sh
# SPDX-License-Identifier: MIT
#
# Cross-compile NVK (Mesa's Vulkan driver, NAK included) with the Pascal
# (pg106) backend for macOS x86_64 on Linux. Produces the same artifact as
# build-nvk-macos.sh does on the Mac:
#     <workdir>/out/libvulkan_nouveau.dylib   + nvk_icd.json + SHA256SUMS + check.txt
#
# Toolchain (all user-local; see nvk/pascal/CROSS-BUILD.md for how each was made):
#   SDK      macOS SDK with .tbd stubs        $PG106_MACOS_SDK (default: xtool's MacOSX26.5.sdk)
#   CCTOOLS  Apple ld64 + cctools (cctools-port) $PG106_CCTOOLS (dir with x86_64-apple-darwin17-ld)
#   clang    any clang >= 18 on PATH (the system one)
#   rustup   stable with target x86_64-apple-darwin; bindgen + cbindgen in ~/.cargo/bin
#   mesa_clc, vtn_bindgen2  build-machine binaries from build-nvk-linux.sh ($PG106_NATIVE_BIN)
#   glslangValidator, meson >= 1.7, ninja, python3 with mako/yaml/packaging
#
# Usage: cross-build-nvk-macos.sh [workdir]      (default ~/pascal-build/nvk)
#   The Mesa tree is <workdir>/mesa (prepare-mesa.sh makes it if missing).
#   PG106_MACOS_MIN  deployment target (default 13.0)
set -eu
WORK=${1:-$HOME/pascal-build/nvk}
HERE=$(cd "$(dirname "$0")" && pwd)
SDK=${PG106_MACOS_SDK:-$HOME/.config/swiftpm/swift-sdks/darwin.artifactbundle/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX26.5.sdk}
CCTOOLS=${PG106_CCTOOLS:-$HOME/.local/share/helios300-cross/cctools/bin}
NATIVE_BIN=${PG106_NATIVE_BIN:-$WORK/native/bin}
MIN=${PG106_MACOS_MIN:-13.0}
T=x86_64-apple-darwin17
S=$WORK/mesa
B=$WORK/build-macos
X=$WORK/cross
export PATH="$NATIVE_BIN:$HOME/.cargo/bin:$PATH"

die() { echo "error: $*" >&2; exit 2; }
[ -f "$SDK/usr/lib/libSystem.B.tbd" ] || die "no macOS SDK at $SDK (set PG106_MACOS_SDK)"
[ -x "$CCTOOLS/$T-ld" ] || die "no ld64 at $CCTOOLS/$T-ld (set PG106_CCTOOLS)"
for p in clang rustc bindgen cbindgen mesa_clc vtn_bindgen2 glslangValidator meson ninja llvm-nm llvm-objdump; do
    command -v $p >/dev/null || die "$p not on PATH (see nvk/pascal/CROSS-BUILD.md)"
done
rustc --print target-libdir --target x86_64-apple-darwin >/dev/null 2>&1 &&
    [ -d "$(rustc --print target-libdir --target x86_64-apple-darwin)" ] ||
    die "rust std for x86_64-apple-darwin missing: rustup target add x86_64-apple-darwin"
[ -f "$S/VERSION" ] || sh "$HERE/prepare-mesa.sh" "$WORK"

# Compiler/linker wrapper: clang for the target, the SDK, Apple's ld64.
# -mlinker-version tells the clang driver which ld64 flags it may use.
LDV=$("$CCTOOLS/$T-ld" -v 2>&1 | sed -n 's/.*PROJECT:ld64-\([0-9.]*\).*/\1/p' | head -1)
mkdir -p "$X"
for tool in clang clang++; do
    cat > "$X/$tool" <<EOF
#!/bin/sh
exec $tool -target x86_64-apple-macos$MIN -isysroot "$SDK" --ld-path="$CCTOOLS/$T-ld" -mlinker-version=$LDV -Qunused-arguments "\$@"
EOF
    chmod +x "$X/$tool"
done
cat > "$X/macos.cross" <<EOF
[binaries]
c = '$X/clang'
cpp = '$X/clang++'
objc = '$X/clang'
ar = '$CCTOOLS/$T-ar'
ranlib = '$CCTOOLS/$T-ranlib'
strip = '$CCTOOLS/$T-strip'
install_name_tool = '$CCTOOLS/$T-install_name_tool'
rust = ['rustc', '--target', 'x86_64-apple-darwin', '-C', 'linker=$X/clang']
pkg-config = 'false'

[properties]
bindgen_clang_arguments = ['--target=x86_64-apple-macos$MIN', '-isysroot', '$SDK']

[host_machine]
system = 'darwin'
subsystem = 'macos'
kernel = 'xnu'
cpu_family = 'x86_64'
cpu = 'x86_64'
endian = 'little'
EOF
export MACOSX_DEPLOYMENT_TARGET=$MIN   # rustc's object min-version

if [ ! -f "$B/build.ninja" ]; then
    # The option set of build/build263.sh (the build the Turing+ driver ships)
    # and build-nvk-macos.sh, plus the cross file.
    meson setup "$B" "$S" --cross-file "$X/macos.cross" \
        -Dbuildtype=debugoptimized -Dvulkan-drivers=nouveau \
        -Dgallium-drivers= -Dplatforms= -Degl=disabled -Dglx=disabled -Dglvnd=disabled \
        -Dllvm=disabled -Dmesa-clc=system -Dshared-glapi=disabled -Dvulkan-layers= -Dtools= \
        -Dbuild-tests=false -Dexpat=disabled -Dzstd=disabled -Dlibunwind=disabled \
        -Dlmsensors=disabled -Dvalgrind=disabled -Dxlib-lease=disabled -Dgallium-rusticl=false \
        -Dspirv-tools=disabled \
        "-Dc_args=-ffile-prefix-map=$S=/src -ffile-prefix-map=$HOME=/build" \
        "-Dcpp_args=-ffile-prefix-map=$S=/src -ffile-prefix-map=$HOME=/build" \
        "-Drust_args=--remap-path-prefix=$S=/src --remap-path-prefix=$HOME=/build" \
        > "$WORK/setup-macos.log" 2>&1 ||
        { echo "meson setup failed; see $WORK/setup-macos.log"; tail -20 "$WORK/setup-macos.log"; exit 3; }
fi
nice ninja -C "$B" src/nouveau/vulkan/libvulkan_nouveau.dylib > "$WORK/ninja-macos.log" 2>&1 ||
    { echo "build failed; see $WORK/ninja-macos.log"; grep -E 'error' "$WORK/ninja-macos.log" | head -20; exit 4; }

D=$B/src/nouveau/vulkan/libvulkan_nouveau.dylib
O=$WORK/out
mkdir -p "$O"
cp "$D" "$O/libvulkan_nouveau.dylib"
# Same as build263.sh: strip the debug map (stabs name every .o by absolute path).
"$CCTOOLS/$T-strip" -S "$O/libvulkan_nouveau.dylib"
# Ad-hoc signature (what `codesign -s -` gives on the Mac), when rcodesign
# (cargo install apple-codesign) is available. Not required to load an
# x86_64 dylib from Terminal; a library-validating host needs its own signing.
if command -v rcodesign >/dev/null; then
    rcodesign sign --binary-identifier libvulkan_nouveau "$O/libvulkan_nouveau.dylib" > "$WORK/sign.log" 2>&1 ||
        { cat "$WORK/sign.log"; exit 5; }
    SIGNED="ad-hoc signed (rcodesign)"
else
    SIGNED="unsigned (no rcodesign)"
fi
cp "$HERE/../icd/nvk_icd.json" "$O/nvk_icd.json"
python3 "$HERE/macho-check.py" "$O/libvulkan_nouveau.dylib" "$SDK" > "$O/check.txt" 2>&1 || {
    cat "$O/check.txt"; echo "Mach-O check failed"; exit 5; }
llvm-nm "$O/libvulkan_nouveau.dylib" | grep -q nvkmd_pg106_enum_pdev ||
    { echo "the pg106 backend is not in the dylib (nvk-pascal.patch not applied?)"; exit 5; }
llvm-nm -gU "$O/libvulkan_nouveau.dylib" | grep -q ' _vk_icdGetInstanceProcAddr$' ||
    { echo "vk_icdGetInstanceProcAddr is not exported"; exit 5; }
python3 - "$O/nvk_icd.json" <<'EOF'
import json, sys
j = json.load(open(sys.argv[1]))
assert j["file_format_version"] == "1.0.1", j
assert j["ICD"]["library_path"] == "libvulkan_nouveau.dylib", j
assert j["ICD"]["api_version"].startswith("1.3."), j
EOF
( cd "$O" && sha256sum libvulkan_nouveau.dylib nvk_icd.json > SHA256SUMS )
cat "$O/check.txt"
echo "built $O/libvulkan_nouveau.dylib ($(cut -c1-12 "$O/SHA256SUMS" | head -1), $(wc -c < "$O/libvulkan_nouveau.dylib") bytes, $SIGNED)"
