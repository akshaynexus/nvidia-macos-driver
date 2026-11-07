# pascal/tests-mac — the owner's step 3–4 test programs

Programs for `research/evidence/OWNER-RUNBOOK.md` steps 3 and 4. All MIT. None
link the PG106 kext, the Metal plugin or GPL code: they reach NVK by
`dlopen("/Library/GPUBundles/nvmtl/libvulkan_nouveau.dylib")` (as
`plugin/nvmtl_vk.c` does) and Metal through `Metal.framework`.

| program | runbook | what it checks |
| --- | --- | --- |
| `vk_smoke/pg106_vk_smoke` | step 3 | finds the GP106 in NVK, then runs four checks: (1) fill device-local memory and copy it back, (2) a compute dispatch (`smoke.comp`, `out[i] = 3i + 7`), (3) clear a 256×256 image and copy it back, (4) 200 compute dispatches with a new value each time, with timing. Every result is read back on the CPU. |
| `offload_app/offload_selftest` | step 3b | the step-4 Vulkan path without a window: renders 60 frames into page-aligned memory, first in zero-copy mode (host import), then in copy mode, verifying a pixel grid per frame |
| `offload_app/pg106_offload` | step 4 | a window on the internal panel shows frames the GP106 rendered into two IOSurfaces. The Intel GPU composites them. `--vulkan` (default): NVK writes the pixels into the surfaces' pages. `--metal`: the same through Metal (needs PG106Accel, see below). `--list-metal`: list the MTLDevices. |

Build on the Mac: `sh build-macos.sh` (Xcode command line tools; Vulkan
headers from `$VULKAN_INCLUDE` or the Mesa tree `nvk/pascal/scripts/prepare-mesa.sh`
made). Output in `build/`, ad-hoc signed.

## Why `--vulkan` is the default for step 4

`Metal.framework` creates an `MTLDevice` only for an accelerator service
(`IOGraphicsAccelerator2`) that carries `MetalPluginName`. For NVIDIA that is
`NVAccel` (`kexts/NVRM/accel/Info.plist`), and the plugin's
`-initWithAcceleratorPort:` hands that port to Apple's `MTLIOAccelDevice`.
The PG106 kext deliberately publishes no accelerator. A small non-GPL
`PG106Accel` accelerator is the planned piece (OFFLOAD-DESIGN.md §3.3);
until it exists, `--metal` reports that no GP106 MTLDevice is present.
`--vulkan` already exercises the whole offload data path: NVK → bridge → kext
→ GP106 writes the IOSurface's host pages → seed bump → WindowServer
composites on the HD 630.

## What was run where (2026-11-07)

Only on Linux, to test the test programs themselves. **No result here
concerns Pascal, NVK, the PG106 kext or macOS.**

- `sh build-macos.sh --linux` builds `pg106_vk_smoke` and `offload_selftest`
  with `-Wall -Wextra` and no warnings.
- `pg106_vk_smoke --any` passes 4/4 on an RTX 3070 Ti Laptop (NVIDIA
  615.78.08 proprietary) and on Intel Iris Xe (Mesa ANV 26.2.5), through the
  loader. It also passes 4/4 with `--driver /usr/lib/libvulkan_intel.so`,
  i.e. ICD-direct with no loader, the mode the Mac uses with NVK.
  Without `--any` it prints `FAIL no GP106 Vulkan device` and exits 1, as
  intended.
- `offload_selftest --any` passes on both drivers in zero-copy and copy
  modes (60 frames over two targets each, 0 bad), also under ASan/UBSan.
- `pg106_offload.m` and the macOS branch of `build-macos.sh` were **not
  compiled**: this box has no macOS SDK.

`smoke_comp_spv.h` and `offload_comp_spv.h` are generated from the `.comp`
files by `gen_spv.sh` (glslang 16.6).
