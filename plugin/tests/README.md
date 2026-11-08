# plugin/tests: the plugin's shader and Vulkan paths, run on Linux

SIMULATION. Nothing here has run on a Mac. They are what can be proven before the first macOS run of the Metal
stack (macos_helios300 `tools/macos/install-gpu.sh`, playbook in `tools/macos/README.md`).

| file | what it runs |
| --- | --- |
| `air_xlate_check.c` / `.py` | AIR bitcode → LLVM C API (the plugin's exact reader sequence, `nvmtl_air_reader.h`, against host libLLVM 23.1.4, the version `NVMTLDriver.bundle/Contents/MacOS/air-runtime/libLLVM.dylib` ships) → printed IR → `nvmtl_translate` (translator/wrapper built for the host) → SPIR-V → `spirv-val --target-env vulkan1.2`; default and `NVMTL_CANONICAL_POINTERS` modes |
| `nvmtl_vk_dryrun.c` | the plugin's Vulkan side (`nvmtl_vk.c`, which needs macOS to build) repeated against an NVK ICD: instance, the plugin's 17 wanted extensions (`../nvmtl_vk_device_exts.h`, shared with the plugin) and feature chain at `vkCreateDevice`, the set-0 layout (321 bindings, mutable bands), the bindless heap (set 2, 5 bindings, with the plugin's fallbacks), the compute pipeline layout, `nvmtl_spirv_ok`'s capability gate, one compute pipeline per SPIR-V file (NAK compiles for SM 6.1), one dispatch and one `vkCmdCopyBuffer` blit with fences |

## Running

```sh
# 1. AIR -> SPIR-V. Default: the plugin's own Apple-compiled nvconv.metallib (17 kernels, AIR v27).
python3 -I plugin/tests/air_xlate_check.py --out ~/pascal-build/gpuaccel/axc
#    any metallib: --metallib F.metallib ...   (modules without an entry point are skipped: the plugin links them)

# 2. The sim ICDs: nvk/pascal/scripts/build-nvk-linux.sh ~/pascal-build/nvk builds both
#    libvulkan_pg106_sim.so    NVK + NAK over the bridge's recording mock (host semaphores executed)
#    libvulkan_pg106_kdsim.so  NVK + NAK over the kext's dispatcher (pascal/tools/pg106_kdsim.c: bridge -> loopback
#                              -> pg106_kd_handle, kdisp's page tables, the PBDMA model; PG106_KDSIM_TRACE=1 prints
#                              every kick, PG106_KDSIM_CHANS=1 is the single-channel kext)
cc -O1 -Wall -Wextra -Wno-missing-field-initializers -I ~/pascal-build/nvk/mesa/include \
   -o nvmtl_vk_dryrun plugin/tests/nvmtl_vk_dryrun.c -ldl
./nvmtl_vk_dryrun --driver ~/pascal-build/nvk/sim/libvulkan_pg106_kdsim.so \
   $(for f in ~/pascal-build/gpuaccel/axc/default/*.spv; do echo --spv $f; done) \
   --dispatch <nvconv_copy's .spv> --blit
```

## Results, 2026-11-08 (driver pascal-gp106 after fe44634)

Translator, LLVM 23.1.4 (`air_xlate_check.py`; spirv-val vulkan1.2 on every output):

| corpus | AIR | default | canonical pointers |
| --- | --- | --- | --- |
| nvconv.metallib (the plugin's) | v27, macOS 15.7 SDK | 17/17 | 17/17 (was 13/17) |
| owner's vphone-linux test metallibs | v28 (macOS 26 SDK) 132, v29 (macOS 27 SDK) 79 | 196/211 | 196/211 |
| iOS 26 QuartzCore, RenderBox, IconRendering (local copies, Apple binaries, not in git) | v28 | 244/249 entry points (was 240), 36 function-only | 234/249 |

Fixed for LLVM 23.1.4: `half +qnan` / `-inf` / `+nan(0x1)` symbolic literals (lex.rs, emitter types.rs);
`getelementptr [N x i8], p, i64 %i` single-index strides (byte_stride_gep.rs, a retry in the wrapper);
`llvm.copysign.*` from instcombine (copysign_text.rs); the plugin retypes an unrolled module's byte GEPs again after
pointer canonicalization (NVMTLLibraryLoad.m, cache suffix p5.u3).

Not translated, refused visibly: GPU-encoded indirect command buffers (`air.*_command`), Metal visible function
references, imageblocks, `air.get_thread_index_in_quadgroup`. AIR v29 (macOS 27 SDK) adds a sixth operand to
`air.atomic.*` (refused "expects 5 operands"); the Helios runs 26.7.2, whose runtime compiler writes v28. Canonical
mode (Cyberpunk's profile only) still refuses 15 iOS modules: instcombine's `<3 x half>` / `<3 x i16>` bitcasts in some
contexts, a struct member access, visible functions, one imageblock.

Vulkan side, NVK 17ca6174 + nvk-pascal.patch, simulated GP106 (`nvmtl_vk_dryrun`, both sim ICDs):
- `vkCreateDevice` with the plugin's extensions and features: OK. Pascal NVK lacks VK_EXT_image_drm_format_modifier
  and VK_KHR_cooperative_matrix (the plugin handles both). `shaderFloat16` was 0 below SM 7.0, so the plugin would
  have refused every module declaring Float16 (164 of the 244 iOS kernels/shaders, 33 of 196 test ones); NAK already
  computes 16-bit float ALU at 32 bits there, and nvk-pascal.patch now offers it from SM 5.0.
- bindless heap: all 5 bindings created.
- 137 compute modules: 119 pipelines compiled by NAK for SM 6.1, 3 refused by the plugin's cooperative-matrix gate
  (the nvconvm* tensor-core candidates, as intended on Pascal), 15 refused by NVK for an imageblock binding (200), a
  band the plugin's layout does not build (it logs it).
- dispatch + fence, blit + fence: OK over the mock and over kdisp (272 frames, 0 refused by kdisp, every fence
  released, 52/52 kernel allocations freed at teardown).

Not covered: graphics pipelines (vertex/fragment), real GPU execution of anything but host semaphores, IOSurface,
the IOKit transport, WindowServer.
