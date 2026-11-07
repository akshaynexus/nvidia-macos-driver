#!/bin/sh
# SPDX-License-Identifier: MIT
#
# Native Linux build of the same Mesa tree the macOS dylib is built from
# (Mesa 17ca6174 + nvk-macos.patch + nvk-pascal.patch, prepare-mesa.sh):
#
#   1. meson + ninja with -Dvulkan-drivers=nouveau, -Dmesa-clc=enabled and
#      LLVM: builds NVK, NAK, mesa_clc and vtn_bindgen2.
#   2. <workdir>/native/bin/{mesa_clc,vtn_bindgen2}: the build-machine tools
#      cross-build-nvk-macos.sh needs (-Dmesa-clc=system).
#   3. <workdir>/sim/libvulkan_pg106_sim.so: NVK + pg106 backend linked with
#      the bridge mock and nvk/pascal/tests/loopback_gpu.c (sim_icd.c sets
#      the transport hook). An ICD that finds a simulated GP106 (SM 6.1), so
#      the step 3-4 programs drive NVK and NAK for real on Linux.
#      SIMULATION ONLY: no 3D/compute/copy method executes.
#
# The tree's own libvulkan_nouveau.so does not link on Linux (the NVRM
# backend calls nvrm_xnu_wait_event, built only for darwin); the sim ICD
# provides a stub. The NVRM backend's macOS-only getprogname() is mapped to
# glibc's program_invocation_short_name, and NVTYPES_USE_STDINT=1 is set
# globally (Linux uint64_t is unsigned long; the two nvtypes.h clash
# otherwise). Those are build flags here, not source changes.
#
# Needs: clang, meson >= 1.7, ninja, rustup stable, bindgen >= 0.71.1
# (not 0.72.0/0.73.0/0.73.1), cbindgen, glslangValidator, LLVM + clang
# development files, SPIRV-Tools, and LLVMSPIRVLib matching the LLVM version
# (PKG_CONFIG_PATH; nvk/pascal/CROSS-BUILD.md builds it user-local).
#
# Usage: build-nvk-linux.sh [workdir]     (default ~/pascal-build/nvk)
set -eu
WORK=${1:-$HOME/pascal-build/nvk}
HERE=$(cd "$(dirname "$0")" && pwd)
PASCAL=$(cd "$HERE/.." && pwd)
S=$WORK/mesa
B=$WORK/build-linux
export PATH="$HOME/.cargo/bin:$PATH"
export PKG_CONFIG_PATH="${PKG_CONFIG_PATH:-$HOME/.local/opt/spirv-llvm-translator/lib/pkgconfig}"
export CC=${CC:-clang} CXX=${CXX:-clang++}

[ -f "$S/VERSION" ] || sh "$HERE/prepare-mesa.sh" "$WORK"
if [ ! -f "$B/build.ninja" ]; then
    meson setup "$B" "$S" -Dbuildtype=debugoptimized -Dvulkan-drivers=nouveau \
        -Dgallium-drivers= -Dplatforms= -Degl=disabled -Dglx=disabled -Dopengl=false \
        -Dgles1=disabled -Dgles2=disabled -Dglvnd=disabled -Dllvm=enabled \
        -Dmesa-clc=enabled -Dvulkan-layers= -Dtools= -Dbuild-tests=false \
        -Dexpat=disabled -Dzstd=disabled -Dlibunwind=disabled -Dlmsensors=disabled \
        -Dvalgrind=disabled -Dgallium-rusticl=false \
        "-Dc_args=-DNVTYPES_USE_STDINT=1 -include errno.h -Dgetprogname()=program_invocation_short_name" \
        -Dcpp_args=-DNVTYPES_USE_STDINT=1 > "$WORK/setup-linux.log" 2>&1 ||
        { tail -30 "$WORK/setup-linux.log"; exit 3; }
fi
# Everything but the final libvulkan_nouveau.so (see above).
LIBS="src/nouveau/vulkan/libnvk.a src/nouveau/vulkan/libnvk_rs.a src/nouveau/compiler/libnak.a
      src/nouveau/compiler/libnak_rs.a src/compiler/clc/mesa_clc src/compiler/spirv/vtn_bindgen2"
nice ninja -C "$B" $LIBS > "$WORK/ninja-linux.log" 2>&1 ||
    { grep -E -A5 '^FAILED|error' "$WORK/ninja-linux.log" | head -40; exit 4; }

mkdir -p "$WORK/native/bin"
ln -sf "$B/src/compiler/clc/mesa_clc" "$B/src/compiler/spirv/vtn_bindgen2" "$WORK/native/bin/"
echo "build-machine tools: $WORK/native/bin/mesa_clc, vtn_bindgen2"

# ---- sim ICD ----------------------------------------------------------
SIM=$WORK/sim
mkdir -p "$SIM"
FLAGS=$(python3 - "$B" <<'EOF'
import json, shlex, sys
bld = sys.argv[1]
for c in json.load(open(bld + "/compile_commands.json")):
    if c["file"].endswith("nvkmd/pg106/nvkmd_pg106_pdev.c"):
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
PG=$S/src/nouveau/vulkan/nvkmd/pg106
for src in "$PASCAL/tests/sim_icd.c" "$PASCAL/tests/loopback_gpu.c" "$PG/bridge/src/mock.c"; do
    eval "$CC $FLAGS -Wno-missing-prototypes -c \"$src\" -o \"$SIM/$(basename "$src" .c).o\""
done
# The link line meson uses for libvulkan_nouveau.so, plus the three objects.
LINK=$(cd "$B" && ninja -t commands src/nouveau/vulkan/libvulkan_nouveau.so | tail -1)
LINK=$(printf '%s\n' "$LINK" | sed -e "s| -o src/nouveau/vulkan/libvulkan_nouveau.so | -o $SIM/libvulkan_pg106_sim.so |" \
    -e "s|-soname,libvulkan_nouveau.so|-soname,libvulkan_pg106_sim.so $SIM/sim_icd.o $SIM/loopback_gpu.o $SIM/mock.o|")
( cd "$B" && eval "$LINK" )
cat > "$SIM/pg106_sim_icd.json" <<EOF
{ "ICD": { "api_version": "1.3.363", "library_path": "$SIM/libvulkan_pg106_sim.so" },
  "file_format_version": "1.0.1" }
EOF
echo "sim ICD: $SIM/libvulkan_pg106_sim.so (VK_DRIVER_FILES=$SIM/pg106_sim_icd.json, or --driver)"
