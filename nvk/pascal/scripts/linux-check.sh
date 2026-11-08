#!/bin/sh
# SPDX-License-Identifier: MIT
#
# Linux build + test of the Pascal nvkmd backend (nvk/pascal), without a
# GPU and without the full NVK build:
#
#   1. Mesa 17ca6174 + nvk-macos.patch + nvk-pascal.patch (prepare-mesa.sh),
#      and a check that nvk-pascal.patch matches nvk/pascal/src (mkpatch.py).
#   2. meson-configures that tree for Mesa's Vulkan runtime only
#      (-Dvulkan-drivers=swrast) and builds libmesa_util/libvulkan_runtime,
#      to get Mesa's real config defines, generated headers and util code.
#   3. compiles every pg106 backend TU, the patched nvkmd.c and the bridge
#      with Mesa's own compile flags plus -Werror.
#   4. links them with the bridge mock + loopback GPU and runs
#      test_nvkmd_pg106 (SIMULATION: no kext, no GPU).
#
# Why not the full NVK: NVK needs bindgen, cbindgen and mesa_clc (with
# LLVMSPIRVLib/libclc) at configure time. Those are not on the reference
# box; with them, `meson setup -Dvulkan-drivers=nouveau` on the same tree is
# the full build (build/build263.sh on the Mac).
#
# Usage: linux-check.sh [workdir]     (default /tmp/pg106-build-nvk)
#        SANITIZE=1 linux-check.sh ...  builds the test objects with ASan+UBSan
set -eu

WORK=${1:-/tmp/pg106-build-nvk}
HERE=$(cd "$(dirname "$0")" && pwd)
PASCAL=$(cd "$HERE/.." && pwd)
REPO=$(cd "$PASCAL/../.." && pwd)
MESA=$WORK/mesa
BLD=$WORK/bld-check
OUT=$WORK/check

sh "$HERE/prepare-mesa.sh" "$WORK" --macos-only
python3 "$HERE/mkpatch.py" "$WORK/mesa-macos" "$WORK/mkpatch" --check
sh "$HERE/prepare-mesa.sh" "$WORK"

if [ ! -f "$BLD/build.ninja" ]; then
    meson setup "$BLD" "$MESA" -Dbuildtype=debug \
        -Dvulkan-drivers=swrast -Dgallium-drivers=llvmpipe -Dplatforms= \
        -Degl=disabled -Dglx=disabled -Dopengl=false -Dgles1=disabled \
        -Dgles2=disabled -Dglvnd=disabled -Dllvm=enabled -Dvulkan-layers= \
        -Dtools= -Dbuild-tests=false -Dexpat=disabled -Dzstd=disabled \
        -Dlibunwind=disabled -Dlmsensors=disabled -Dvalgrind=disabled \
        -Dspirv-tools=disabled -Dgallium-rusticl=false > "$WORK/setup-check.log" 2>&1 ||
        { tail -30 "$WORK/setup-check.log"; exit 3; }
fi
ninja -C "$BLD" src/util/libmesa_util.a src/c11/impl/libmesa_util_c11.a \
    src/util/blake3/libblake3.a src/util/libmesa_util_clflush.a \
    src/util/libmesa_util_clflushopt.a src/vulkan/runtime/libvulkan_lite_runtime.a \
    src/vulkan/runtime/libvulkan_instance.a src/vulkan/util/libvulkan_util.a > "$WORK/ninja-check.log" 2>&1 ||
    { tail -30 "$WORK/ninja-check.log"; exit 4; }

# Mesa's flags for a Vulkan runtime TU (vk_sync.c), relative -I made absolute.
FLAGS=$(python3 - "$BLD" <<'EOF'
import json, shlex, sys
bld = sys.argv[1]
for c in json.load(open(bld + "/compile_commands.json")):
    if c["file"].endswith("vulkan/runtime/vk_sync.c"):
        args, out, skip = shlex.split(c["command"])[1:], [], False
        for a in args:
            if skip: skip = False; continue
            if a in ("-MQ", "-MF", "-o", "-c"): skip = True; continue
            if a == "-MD" or a.endswith(".c") or a.startswith("-fdiagnostics-color"): continue
            if a.startswith("-I") and not a.startswith("-I/"):
                a = "-I" + bld + "/" + a[2:]
            out.append(a)
        print(" ".join(shlex.quote(o) for o in out))
        break
EOF
)
VK=$MESA/src/nouveau/vulkan
INC="-I$VK -I$VK/nvkmd -I$MESA/src/nouveau/headers -I$MESA/src/nouveau/headers/nvidia/classes \
-I$VK/nvkmd/pg106/bridge/include -I$VK/nvkmd/pg106/core/include -DNVTYPES_USE_STDINT=1"

SAN=
[ "${SANITIZE:-0}" = 1 ] && SAN="-fsanitize=address,undefined -fno-omit-frame-pointer"
rm -rf "$OUT"; mkdir -p "$OUT"
cc_one() { # src obj [extra flags]
    src=$1; obj=$2; shift 2
    eval "cc $FLAGS $SAN $INC -Werror -Wno-error=unused-parameter $* -c \"$src\" -o \"$obj\""
}
for f in ctx dev mem pdev sync va; do
    cc_one "$VK/nvkmd/pg106/nvkmd_pg106_$f.c" "$OUT/nvkmd_pg106_$f.o"
    echo "compiled nvkmd_pg106_$f.c"
done
# nvkmd.c is upstream code; NVK builds it with -Wno-pointer-arith
cc_one "$VK/nvkmd/nvkmd.c" "$OUT/nvkmd.o" -Wno-pointer-arith; echo "compiled nvkmd.c (patched)"
cc_one "$VK/nvkmd/pg106/bridge/src/bridge.c" "$OUT/bridge.o";  echo "compiled bridge.c"
cc_one "$VK/nvkmd/pg106/core/src/uapi.c" "$OUT/uapi.o";        echo "compiled uapi.c"
cc_one "$VK/nvkmd/pg106/bridge/src/transport_iokit.c" "$OUT/transport_iokit.o"; echo "compiled transport_iokit.c (non-Apple stubs)"
# Test-only objects (mock "kext", loopback GPU, stubs, the test itself).
cc_one "$VK/nvkmd/pg106/bridge/src/mock.c" "$OUT/mock.o"
for t in loopback_gpu stubs test_nvkmd_pg106; do
    cc_one "$PASCAL/tests/$t.c" "$OUT/$t.o" "-I$MESA/src/nouveau/vulkan -Wno-missing-prototypes"
done

L=$BLD/src
cc $SAN -o "$OUT/test_nvkmd_pg106" "$OUT"/*.o -Wl,--start-group \
    "$L/vulkan/runtime/libvulkan_lite_runtime.a" "$L/vulkan/runtime/libvulkan_instance.a" \
    "$L/vulkan/util/libvulkan_util.a" "$L/util/libmesa_util.a" \
    "$L/util/libmesa_util_clflush.a" "$L/util/libmesa_util_clflushopt.a" \
    "$L/c11/impl/libmesa_util_c11.a" "$L/util/blake3/libblake3.a" -Wl,--end-group \
    -lm -lpthread -ldl -lz
echo "linked $OUT/test_nvkmd_pg106"
"$OUT/test_nvkmd_pg106"
