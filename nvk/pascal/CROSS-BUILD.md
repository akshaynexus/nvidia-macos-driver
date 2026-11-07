# Building the Pascal NVK: macOS cross build and Linux builds

NVK is Mesa's Vulkan driver; NAK is its Rust shader compiler. This page
covers the full build of NVK, NAK included, with the Pascal backend
(`nvk/pascal`). It is built twice from one Mesa tree (`prepare-mesa.sh`:
17ca6174 + `nvk-macos.patch` + `nvk-pascal.patch` + `nvk-build-fixes.patch`):

- **macOS x86_64**, cross-compiled on Linux. This makes the
  `libvulkan_nouveau.dylib` + `nvk_icd.json` that runbook step 3 loads.
- **Linux native.** This builds the build-machine tools the cross build
  needs. It also builds an ICD that runs the step 3–4 programs through real
  NVK + NAK against a simulated GP106.

A third build is upstream Mesa plus only the NAK changes of
`nvk-macos.patch`. NAK's own tests run on it, and it is how the macOS NAK
can be run on the real GP106 under Linux nouveau.

## What was built and run (CachyOS x86-64, 2026-11-08)

| what | result |
| --- | --- |
| macOS dylib (`cross-build-nvk-macos.sh`) | `libvulkan_nouveau.dylib`, Mach-O x86_64 `MH_DYLIB`, 20,058,128 bytes, LC_BUILD_VERSION macos minos 13.0 / sdk 26.5, install name `@rpath/libvulkan_nouveau.dylib`, ad-hoc signed. SHA-256 `c6fdb3769fe5cd0b…89a7`. A second build in a fresh workdir gave the same bytes. |
| its link check (`macho-check.py`) | links only `libSystem.B`, `libc++.1`, `libz.1`, `IOKit`. 3293 undefined symbols: 353 are bound to one of those four, and each of them is exported by the SDK's `.tbd` stub for `x86_64-macos` (re-exports followed). The other 2940 are weak and looked up at load time: Mesa's entry-point tables (`nvk_*`, `vk_*`, `wsi_*`), which are NULL if absent, the same as `build263.sh`'s builds. **0 strong unresolved, 0 missing from the SDK.** `vk_icdGetInstanceProcAddr`, `vk_icdGetPhysicalDeviceProcAddr` and `vk_icdNegotiateLoaderICDInterfaceVersion` are exported. The pg106 backend and its real IOKit transport are present (`IOServiceOpen`, `IOConnectCallMethod`, … from IOKit), as is NAK (`nak_compile_shader`). |
| ICD manifest | `nvk_icd.json`: file_format 1.0.1, api 1.3.363, `library_path` `libvulkan_nouveau.dylib` (next to it, as `--driver` and the plugin expect) |
| step 3–4 programs (`pascal/tests-mac/build-macos.sh --cross`, now with Apple ld64) | `pg106_vk_smoke`, `offload_selftest`, `pg106_offload`: Mach-O x86_64 executables, all imports exported by the SDK stubs (16, 19, 78). |
| Linux native build (`build-nvk-linux.sh`) | NVK, NAK, `mesa_clc`, `vtn_bindgen2`; sim ICD `libvulkan_pg106_sim.so` |
| sim ICD + `pg106_vk_smoke` | finds `10de:1c20 NVIDIA GeForce GTX 1060 Mobile (NVK GP106)`, Vulkan 1.3.363, heaps 6144 MiB VRAM + 256 MiB + host. All four checks run to completion: memory, compute pipeline creation, 200 dispatches with 0 fence timeouts, and image clear. All **FAIL on data, as expected**: the loopback GPU executes only host semaphore/WFI/MEMBAR methods. |
| sim ICD + `offload_selftest` | zero-copy (host import) and copy modes each submit 60 frames and get all fences back. Data FAIL, as expected. |
| NAK SM 6.1 vs NVIDIA's disassembler (`nak-sm61-check.py`) | **5/5 shaders** of those runs (`smoke.comp`, `offload.comp`, and NVK's fill/copy/image-copy meta shaders, 16–60 instructions) decode with `nvdisasm -b SM61` (CUDA 12.9.88). Opcodes, predicates and destination registers match NAK's final IR exactly. |
| NAK's own tests (`build-nak-nouveau.sh`) | `meson test --suite nouveau`: nil, nak, mme_builder, nvk_mme OK (nvk_rs skipped by Mesa). NAK test binary with `nvdisasm_tests` against nvdisasm 12.9.88 for SM 50, 52, 53, 60, **61**, 62, 70, 75, 80, 86, 89, 90, 100, 120: **24 passed, 0 failed** (45 `hw_tests` need a GPU on nouveau). |
| `linux-check.sh` (backend unit test) | 221 checks, 0 failures |

Not run anywhere: NVK on the GP106 (see "Real GP106 under nouveau"), and
anything on macOS.

## Build breaks a from-scratch build hit (now fixed)

1. `nv_push.h`: `nvk-macos.patch` added an `fprintf(stderr, …)` warning,
   but mesa_clc also compiles that header as OpenCL C
   (`nvk/cl/nvk_copy_indirect.cl`), and OpenCL C has no stdio. `nvkcl.spv`
   failed on every platform. `nvk-build-fixes.patch` leaves the warning out
   of the OpenCL build.
2. `nvkmd_nvrm_pdev.c` includes `class/cla0c0.h`, `cla197.h` and `cla1c0.h`
   (Kepler class ids), which open-gpu-kernel-modules 615.78.08 no longer
   ships. `prepare-mesa.sh` now fetches NVIDIA's 570.86.16 copies (MIT,
   SHA-256 pinned) into `nvkmd/nvrm/class/`, next to `cla26f.h`.

Linux-only build issues of the NVRM (Turing+, macOS) backend. These are
handled with build flags in `build-nvk-linux.sh`, with no source change:
`NvU64` is defined twice (Linux `uint64_t` is `unsigned long`), so
`-DNVTYPES_USE_STDINT=1` is set for all files. `getprogname()` (BSD) is
mapped to `program_invocation_short_name`. `nvrm_xnu_wait_event` (darwin
only) is stubbed in the sim ICD. GCC also rejects a stray `static static`
in `nvkmd_nvrm_pdev.c`, which clang only warns about, so the Linux build
uses clang, the Mac's compiler.

## Toolchain (all user-local, no system packages)

On top of the distro's clang/LLVM 23 (with headers), lld, meson ≥ 1.7,
ninja, SPIRV-Tools, glslang, Python (mako, yaml, packaging) and libdrm:

```sh
# rustup stable (1.99.1) with the macOS target; bindgen 0.73.2, cbindgen 0.29.4
rustup target add x86_64-apple-darwin
cargo install --locked bindgen-cli cbindgen
# SPIRV-LLVM-Translator matching LLVM 23.1 (mesa_clc needs LLVMSPIRVLib 23.1.x)
git clone -b llvm_release_230 https://github.com/KhronosGroup/SPIRV-LLVM-Translator   # db5855d7
git clone https://github.com/KhronosGroup/SPIRV-Headers && git -C SPIRV-Headers checkout cb42dec3  # spirv-headers-tag.conf
cmake -S SPIRV-LLVM-Translator -B slt -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX=$HOME/.local/opt/spirv-llvm-translator -DLLVM_DIR=/usr/lib/cmake/llvm \
  -DLLVM_EXTERNAL_SPIRV_HEADERS_SOURCE_DIR=$PWD/SPIRV-Headers -DBUILD_SHARED_LIBS=ON \
  -DLLVM_SPIRV_INCLUDE_TESTS=OFF -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
ninja -C slt install          # build-nvk-linux.sh finds it through PKG_CONFIG_PATH
```

The scripts default to the toolchain already on the reference box:
- the macOS SDK: xtool's `MacOSX26.5.sdk`, set with `PG106_MACOS_SDK`;
- Apple ld64-956.6 and cctools from cctools-port, in
  `~/.local/share/helios300-cross/cctools/bin`, set with `PG106_CCTOOLS`;
- `rcodesign` (apple-codesign 0.29), used for the ad-hoc signature if
  present.

libclc is not needed: NVK's OpenCL kernels use only Mesa's libcl headers.

## Commands

```sh
sh nvk/pascal/scripts/build-nvk-linux.sh       # ~/pascal-build/nvk: tree, native build, mesa_clc, sim ICD
sh nvk/pascal/scripts/cross-build-nvk-macos.sh # -> ~/pascal-build/nvk/out/{libvulkan_nouveau.dylib,nvk_icd.json,SHA256SUMS,check.txt}
MESA_TREE=~/pascal-build/nvk/mesa sh pascal/tests-mac/build-macos.sh --cross   # step 3-4 programs
NVDISASM=<cuda-12>/bin/nvdisasm sh nvk/pascal/scripts/build-nak-nouveau.sh   # NAK tests + Helios bundle
```

From an empty workdir, the first two commands take 3.5 min on 20 threads.
The build directories take about 1.5 GB.

How the cross build is set up (`<workdir>/cross/`, written by the script):
- `clang`/`clang++` wrappers run `clang -target x86_64-apple-macos13.0
  -isysroot <SDK> --ld-path=<ld64> -mlinker-version=956.6`. Meson therefore
  detects Apple's linker and links against the SDK's `.tbd` stubs.
- `macos.cross` sets the cctools ar/ranlib/strip and `rustc --target
  x86_64-apple-darwin -C linker=<wrapper>`. It also sets
  `bindgen_clang_arguments` for the target and disables pkg-config, so no
  Linux library can leak in.
- The meson options are `build263.sh`'s (`-Dmesa-clc=system` with the
  native `mesa_clc`/`vtn_bindgen2`, `-Dllvm=disabled`), with the same
  path-prefix maps, which is why the bytes do not depend on the workdir.

## Running the step 3–4 programs on Linux (simulation)

```sh
S=~/pascal-build/nvk/sim
sh pascal/tests-mac/build-macos.sh --linux      # VULKAN_INCLUDE=~/pascal-build/nvk/mesa/include
PG106_SIM_PCI_DEVICE=0x1C20 NAK_DEBUG=print pascal/tests-mac/build/pg106_vk_smoke \
    --driver $S/libvulkan_pg106_sim.so 2> smoke-nak.log
python3 nvk/pascal/scripts/nak-sm61-check.py <cuda-12>/bin/nvdisasm smoke-nak.log
```

`sim_icd.c` sets `NVK_KMD=pg106`, which keeps NVK off the NVRM backend
(on a box with NVIDIA's proprietary driver it would otherwise try the real
RM). It also points the backend's transport hook at the bridge mock.
CUDA 13 dropped SM 5x/6x from nvdisasm. Get CUDA 12's from NVIDIA's
redist (`cuda_nvdisasm-linux-x86_64-12.9.88-archive.tar.xz`, SHA-256
`49296dd5…76ff`) and use it locally only; it is not redistributed here.

## Real GP106 under nouveau (needs the owner's approval)

This is the hardware check of the Pascal shader code the dylib carries. The
same NAK (`build-nak-nouveau.sh`'s tree) runs on the Helios' GP106 with
upstream NVK and Linux nouveau, which has signed GR firmware for GP106.
`run-nouveau.sh` (in the bundle) runs three things:
- NAK's 45 `hw_tests`, which encode instructions for SM 6.1, run them on
  the GPU and compare with a CPU reference;
- `pg106_vk_smoke` with `NAK_DEBUG=print`, whose SM 6.1 code
  `nak-sm61-check.py` can compare with the sim run;
- `offload_selftest`.

The script itself loads, unloads and rebinds nothing. It stops unless the
GP106 is already bound to nouveau. Getting the GPU onto nouveau may need
the desktop stopped, which is the owner's call.

Procedure (the coordinator asks the owner; nothing here has been run):
0. Read-only first: `lspci -nnk -d 10de:` (which driver holds the GP106),
   `uname -r` (nouveau's VM_BIND uAPI needs Linux ≥ 6.6),
   `ls /lib/firmware/nvidia/gp106/` (acr, gr, sec2, nvdec),
   `ldd --version` (the bundle needs glibc ≥ 2.39, `libdrm`,
   `libSPIRV-Tools.so`, `libdisplay-info.so.3`, `libelf`, `libz`, `libudev`).
1. Copy `~/pascal-build/nak-nouveau/bundle/` to the Helios (23 MB,
   `SHA256SUMS` inside).
2. If the GP106 is already on nouveau (`Kernel driver in use: nouveau`),
   go to 4. Nothing needs stopping.
3. Otherwise (proprietary `nvidia` bound), from a text console or ssh:
   `sudo systemctl stop display-manager`; `sudo modprobe -r nvidia_drm
   nvidia_modeset nvidia_uvm nvidia`; `sudo modprobe nouveau`. Then check
   `dmesg | grep nouveau` for the GR/ACR firmware loading without errors.
4. As the desktop user (group `render`): `sh run-nouveau.sh`. Each step has
   a timeout (600 s for the hw tests, 120 s each for the others). The logs
   go to `results-<date>/`.
5. Back: `sudo modprobe -r nouveau; sudo modprobe nvidia_drm; sudo systemctl
   start display-manager` if step 3 was done. Rollback is a reboot. A
   nouveau GR fault can hang the GPU or the machine, and a reboot recovers
   it. Nothing persistent is changed.

## What only the Mac can verify

- That `dlopen` of the cross-built dylib succeeds on macOS 26 x86_64. The
  `.tbd` check covers symbol resolution, not dyld's runtime checks
  (code-signing policy of the host process, chained fixups).
- The IOKit transport against the PG106 kext: never compiled with real
  IOKit headers anywhere but this cross build, never run.
- Everything in runbook steps 3–4 that touches the GPU.
