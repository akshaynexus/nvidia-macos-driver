# Research: Helios 300 NVIDIA (Pascal) + iGPU switching on macOS

Research notes — reference material assembled from open-source drivers, upstream docs and
hardware reports. Nothing here is committed project policy; every external claim carries its
source. Verify anything you rely on before building on it.

## 0. Binding safety constraints (user directive)

This working machine is a **real, running macOS system**. On this machine, never:

- run `package/install.sh` / `uninstall.sh`, the 1401 app, or any installer for this driver;
- install, load, or inject any kext (no `/Library/Extensions` copies, no AuxKC rebuild, no
  OpenCore config changes, no NVRAM/boot-arg writes);
- reboot, shut down, or restart for driver-testing purposes;
- compile *for the purpose of installing* — compilation is allowed only into throwaway
  directories for unit tests that exercise code paths in isolation.

Permitted and expected: reading code, writing reference documents and test harnesses,
unit tests that mock or isolate the hardware (like `tools/test_card_support.py`, verified to
touch only `tempfile` dirs), and static comparison against open-source sources (nouveau,
Mesa, linux-firmware). Any real-hardware validation of a Pascal port happens on separate,
dedicated hardware — never here.

## 1. What cards the Predator Helios 300 shipped with

| Generation | Models | dGPU | Chip | PCI device IDs (10DE) | macOS status today |
| --- | --- | --- | --- | --- | --- |
| 2017–2018 | PH315-51, PH317-51/52, G3-572 | GTX 1050 Ti / 1060 / 1070 (laptop) | GP107M / GP106M / GP104M | 1C8C (1050 Ti M), 1C20/1C23/1C60 (1060 M), 1C8D/1C92 (1050 M), 1BA1/1BA2/1BE1 (1070 M), 1C21/1C61 (1050 Ti on GP106M) | **None** (Pascal, no GSP) |
| 2019 | PH315-52 | GTX 1660 Ti | TU116 | 2182 | Driver supports Turing — listed, unvalidated |
| 2020 | PH315-53 | RTX 2060/2070 | TU106 | 1F08/1F11 | Same |
| 2021 | PH315-54, PH317-55 | RTX 3060/3070 | GA106/GA104 | 2560/2484 etc. | Same |
| 2022 | PH315-55, PH317-56 | RTX 3070 Ti (MUX + Advanced Optimus) | GA104 | 24A0 etc. | Same; the only generation with a real MUX |

Device IDs cross-checked against NVIDIA's own 390.12 supported-chips list
(https://download.nvidia.com/XFree86/Linux-x86/390.12/README/supportedchips.html) and the
envytools PCI ID database (https://envytools.readthedocs.io/en/latest/hw/pciid.html).

**The "old Helios 300" the Pascal effort targets is the 2017–2018 generation: GTX 1050 Ti,
1060, 1070 — all Pascal.**

## 2. Display wiring facts (what "switching" can mean here)

- 2017–2021 Helios 300: **Optimus only, no MUX.** The internal panel is physically wired to
  the Intel iGPU; frames rendered on the NVIDIA chip must pass through it.
  (https://www.ultrabookreview.com/47441-acer-predator-helios-300-review-ph315-54/ — "no MUX";
  https://laptopmedia.com/series/acer-predator-helios-300-ph317-55/ — "No MUX switch")
- On these models **HDMI and miniDP are wired directly to the dGPU**; Thunderbolt/USB-C video
  goes through the iGPU. An external display on HDMI/miniDP is driven by the NVIDIA chip with
  no iGPU involvement (PH315-54 review, same source).
- Only the 2022 generation (PH317-56) gained a MUX / Advanced Optimus
  (https://www.ultrabookreview.com/54807-acer-predator-helios-300-2022-review/). That machine's
  RTX 3070 Ti is Turing-era compatible with this driver already — MUX = BIOS-level dGPU-direct
  mode, no new macOS work needed beyond what the driver already does with `nvfbheads`.

Consequences for macOS:
- "Boot and switch efficiently between iGPU and NVIDIA" on the Pascal machines can only be a
  **runtime offload architecture** (render on NVIDIA, present via Intel), never a display MUX.
- dGPU-direct display output is possible only on the HDMI/miniDP ports, and only once a Pascal
  display engine (nouveau `disp`) is ported.

## 3. Pascal bring-up: what the open-source world gives us

### 3.1 The two working references

| Reference | License | What it proves |
| --- | --- | --- |
| Nouveau NVKM (Linux, `drivers/gpu/drm/nouveau/nvkm/`) | GPL-2.0 | Complete CPU-side GP106 bring-up: ACR secure boot, MMU, FIFO, GR, display. No GSP involved. |
| NVK (Mesa, `src/nvk/`) | MIT | Userspace Vulkan driver that supports Pascal (sm_60) — and is **already the userspace in this repo**. |

Key primary sources (from https://github.com/TheTom/pascal-egpu RESEARCH.md, verified):
- GP106 device wiring in nouveau: `nvkm/engine/device/base.c` —
  `acr=gp102_acr_new, bar=gm107_bar_new, fb=gp102_fb_new, pmu=gp102_pmu_new,
  ce=gp102_ce_new, fifo=gp100_fifo_new, gr=gp104_gr_new, sec2=gp102_sec2_new`
- GR init: `nvkm/engine/gr/gp100.c`, `gp102.c`; FIFO: `nvkm/engine/fifo/gp100.c`;
  ACR: `nvkm/subdev/acr/gp102.c`, `gm200.c`, `acr_r352.c`; PMU: `nvkm/subdev/pmu/gp102.c`
- Engine classes: `PASCAL_A` 0xC097 / `PASCAL_COMPUTE_A` 0xC0C0 (GP100),
  `PASCAL_B` 0xC197 / `PASCAL_COMPUTE_B` 0xC1C0 (GP102/104/106).
  **GP106 uses PASCAL_COMPUTE_B (0xC1C0).** Channel class `PASCAL_CHANNEL_GPFIFO_A` 0xC06F.
- Class headers in NVIDIA's open tree:
  `src/common/sdk/nvidia/inc/class/clc1c0.h`, `clc1c0qmd.h`, `clc06f.h`
  (https://github.com/NVIDIA/open-gpu-kernel-modules)
- MMU: Pascal is MMU v2; format doc `pascal/gp100-mmu-format.pdf` in
  https://github.com/NVIDIA/open-gpu-doc
- MMIO map reference: envytools https://envytools.readthedocs.io/en/latest/hw/mmio.html

### 3.2 The signed-firmware requirement (non-negotiable)

Pascal's GR engine cannot start without NVIDIA-signed ACR blobs. Required files from
`linux-firmware/nvidia/gp106/` (freely redistributable — same model this repo already uses to
ship GSP firmware):

```
acr/bl.bin, acr/ucode_load.bin, acr/ucode_unload.bin, acr/unload_bl.bin
gr/fecs_bl.bin, fecs_data.bin, fecs_inst.bin, fecs_sig.bin
gr/gpccs_bl.bin, gpccs_data.bin, gpccs_inst.bin, gpccs_sig.bin
gr/sw_bundle_init.bin, gr/sw_ctx.bin
sec2/sig.bin
```

Boot chain: Falcon boot ROM → HS ACR ucode on PMU/SEC2 → validates LS signatures → loads
FECS/GPCCS onto GR falcons → GR ready. Nouveau ships the GP108 ACR bits for newer Pascal
laptops because the older blobs failed on some (Phoronix,
https://www.phoronix.com/news/Nouveau-Blobs-Pascal-Laptops) — keep both variants when
packaging.

### 3.3 Hard limits (accepted, documented)

- **No reclocking**: NVIDIA never released Pascal PMU firmware; nouveau runs PMU with
  `gm200_pmu_nofw`. GPU runs at VBIOS boot clocks. Performance is far below the Windows
  driver; that is the permanent ceiling, not a bug to fix.
- **No runtime power management / RTD3** without PMU firmware → the dGPU cannot deeply idle;
  battery impact is real. This shapes the switching design below.
- NVK requires the nouveau CS uapi (Linux 6.6+ on the reference platform); the macOS bridge in
  `nvk/nvk-macos.patch` must gain a second backend shaped like that uapi (VM bind + exec)
  instead of the GSP message-ring ABIs.

### 3.4 Phased plan (effort estimate from pascal-egpu, corroborated here)

| Phase | Target | Rough effort |
| --- | --- | --- |
| 0 | Userspace/DriverKit PCIe access (fork tinygrad's TinyGPU DriverKit — MAP_BAR/CFG/MMIO RPC — or reuse this repo's kext PCI claim path) | days |
| 1 | PMC_BOOT_0 probe, bus mastering, VBIOS DEVINIT, FB init, MMU v2 | 2–4 weeks |
| 2 | **ACR secure boot port** (hardest; ~4k lines in nouveau) | 2–4 weeks |
| 3 | GPFIFO channels, QMD build, `SEND_PCAS_A` compute dispatch | 1–2 weeks |
| 4 | NVK-on-nouveau-uapi bridge → Vulkan/Metal rendering via existing translator+plugin | weeks |
| 5 | Display: port `nvkm/engine/disp/gf119+` (CPU-driven on Pascal) → NVRMFB-style framebuffer for HDMI/miniDP | weeks |

## 4. iGPU ↔ NVIDIA switching design (macOS)

Three architectures, in order of realism:

1. **Offload rendering (the efficient runtime path, Optimus-style).**
   Intel iGPU (HD 630, KBL/CFL — natively driven by Apple's `AppleIntelKBLGraphics*` kexts)
   owns every display and WindowServer. The NVIDIA kexts run display-less (headless: no
   NVRMFB heads), the Metal plugin renders into IOSurfaces, and WindowServer composites those
   surfaces through the Intel accelerator. On x86 the shared memory is cache-coherent, so the
   handoff is effectively zero-copy. This is the only "efficient switching" that can exist on
   a no-MUX Helios 300, and it matches how Linux PRIME offload works.
   Prerequisite: Phases 0–4 above, minus display.
2. **dGPU-direct external display.** Port nouveau `disp` and drive only the HDMI/miniDP heads
   (NVRMFB with `nvfbheads` limited), iGPU keeps the internal panel. Two active WindowServer
   GPUs — closest to how the machine behaves in Windows with an external monitor.
3. **MUX machines only (2022+ Helios 300):** BIOS dGPU-direct mode; already Turing+ and inside
   the current driver's scope. No new work.

Power note: because Pascal has no RTD3, "switching off" the dGPU is software-disabled only
(persistent memory mapped, GPU held in reset or idle). True power gating is out of reach
without PMU firmware.

## 5. Intel iGPU hackintosh drivers to use as references / additions

The Helios 300's iGPU (HD 630 on 7th/8th-gen CPUs) is natively supported by macOS, so the
references below matter for *architecture and switching*, not basic Intel support.

| Project | License | Why it matters here |
| --- | --- | --- |
| **WhateverGreen** (acidanthera) + **Lilu** | BSD-3-Clause | The canonical Intel iGPU hackintosh layer: framebuffer patching (`kern_fb.hpp` decodes AppleIntelFramebuffer controller flags), backlight, connector/LSPCON handling, HDMI dividers, DVMT fixes. The source of reference for any work that touches how the iGPU presents displays. https://github.com/acidanthera/WhateverGreen — see `Manual/FAQ.IntelHD.en.md` |
| **Meteor Lake Xe-LPG Metal Graphics** (VedCloud) | mixed; GPL-2.0 i915 | **The most relevant architecture reference found**: retargets *this exact repo tree* to an Intel iGPU — i915 compiled onto xnu via a FreeBSD-LinuxKPI shim (`NMXGPU.kext`, 438 sources), ANV for Vulkan, NullMoth's plugin/translator retained. Proves the "port Linux KMD to macOS" pattern end-to-end and documents every trap (stolen memory, task-wake race, IOKit pinning). https://github.com/VedCloud/Meteor-Lake-Xe-LPG-Metal-Graphics |
| **AirportItlwm / itlwm** | GPL-2.0 | The established precedent for a GPL Linux driver (iwlwifi) ported to a loading macOS kext — the licensing/structural template a nouveau-based kext would follow. |
| **VoodooI2C / VoodooGPIO** (+ linuxirq shims) | GPL-2.0 / ISC | Working examples of porting Linux kernel drivers (interrupts, GPIO, I2C) onto IOKit — the same glue a nouveau port needs for MMIO/interrupt paths. |
| **headkaze's Intel Framebuffer patching guide / FB-Patcher** | — | Practical connector/flags patching methodology for AppleIntelFramebuffer — needed if the iGPU side must be coaxed into accepting NVIDIA-produced surfaces. |
| **WhateverGreen `-igfxfbdump` / `-igfxdump`** | — | Tooling to dump the live framebuffer table and kext on real hardware — the first diagnostic step for offload-path work on the Helios 300. |
| **MacKernelSDK / AMFIPass** | Apple / upstream | Build + load infrastructure this repo already depends on. |

 Licensing caution carried over from the Meteor Lake write-up: GPL-2.0-only (nouveau, i915),
 GPL-3.0 (translator components) and PolyForm NC (this repo's kexts/plugin) are mutually
 incompatible. A Pascal kernel port must live in its **own GPL-2.0 kext tree**; it can consume
 this repo's plugin/translator at arm's length but cannot merge source.

## 6. Source register/class material (for the port itself)

- GP106 MMIO ranges: PMC 0x000000, PBUS 0x001000, PFIFO 0x002000, PTIMER 0x020000,
  PCOPY 0x060000, PFB 0x100000, PPMU 0x10A000, PGRAPH 0x400000, PDISP 0x610000
  (envytools + pascal-egpu).
- QMD v01_07 for Pascal compute (`clc1c0qmd.h`): 256-byte dispatch descriptor —
  QMD_VERSION=7, QMD_MAJOR_VERSION=1, PROGRAM_OFFSET, CTA_RASTER_*, CTA_THREAD_DIMENSION*,
  SHARED_MEMORY_SIZE, REGISTER_COUNT, CONSTANT_BUFFER_* slots, RELEASE0 semaphore fields.
- Submission: GPFIFO entry = GET[31:2]|FETCH[0] + LENGTH[30:10]|GET_HI[7:0]|SYNC[31];
  pushbuffer method = type[31:29]|count[28:16]|subch[15:13]|method[12:2];
  doorbell at BAR0+0x90 (tinygrad `ops_nv.py` reference implementation).
- **Open encoding question (caught by `helios300-gp106-simtest.py`)**: tinygrad encodes the
  pushbuffer type field as `typ << 28` while the docs above describe it at bits [31:29] /
  [31:30]; both round-trip internally but disagree with each other. Before any real
  submission path is written, reconcile the exact field width against `clc06f.h` and the
  PBDMA reference (`dev_pbdma.ref.txt`), not against tinygrad's shorthand.

## 7. Verified nvkm port inventory (GP106-relevant, fetched from torvalds/linux master)

Sizes are file sizes from the GitHub contents API (fetched live). Note two upstream renames
that older references (including pascal-egpu) get wrong: `subdev/secboot` no longer exists on
master (subsumed by `subdev/acr`), and the falcon core lives at `engine/falcon`, not
`subdev/falcon`.

### Core subdevs

| Path | GP106-relevant files |
| --- | --- |
| `subdev/acr` | `gp102.c` (8K, GP106's ACR), `gm200.c` (10K), `base.c` (10K), `lsfw.c` (12K, LS-firmware loading), `priv.h` (4K) |
| `subdev/mmu` | `gp100.c`, `vmmgp100.c` (17K, Pascal v2 page tables), `vmm.c` (51K core), `uvmm.c` (14K uapi), `mem.c` |
| `subdev/bar` | `gm107.c` (2K — exactly what GP106's device table wires) |
| `subdev/fb` | `gp102.c`, `gp100.c`, `ramgp100.c`, `ramgp102.c`, `gddr5.c` (GP106 is GDDR5) |
| `subdev/pmu` | `gp102.c`, `gm200.c` — note nouveau runs PMU **without** proprietary firmware (`gm200_pmu_nofw`) |
| `engine/falcon` | core falcon engine (rename from old `subdev/falcon`) |

### Engines

| Path | GP106-relevant files |
| --- | --- |
| `engine/fifo` | `gp100.c` (4K, what the device table wires), `gk104.c` (21K, the PBDMA/GPFIFO base), `gf100.c` (26K), `runl.c`/`runq.c`/`chan.c`/`chid.c` (runlist machinery) |
| `engine/gr` | `gp102.c` (5K), `gp104.c` (4K, what the device table wires), `gp100.c`, `ctxgp102.c`, `ctxgp104.c`, `gf100.c` (70K base), `ctxgf100.c` (46K base), `regs.h` |
| `engine/ce` | `gp102.c`, `gm107.c` (copy engines, 4 instances) |
| `engine/disp` | `gp102.c` (6K), `gp100.c`, `gf119.c` (33K CPU-driven display base), `dp.c` (17K), `outp.c`, `head.c`, `ior.c` |

The `gr` and `fifo` engines are the biggest port surface (hundreds of KB when the gf100/gk104
generational bases are included); the ACR chain remains the hardest correctness problem.

## 8. Phase 0 code map into this repo

Where the patterns to copy actually live:

- **Kext PCI personality**: `kexts/NVRM/Info.plist` matches *any* NVIDIA display/3D device
  (`0x000010de&0x0000ffff` + class `0x03xxxxxx`) on purpose — the card gate is software
  (`rm_is_supported_pci_device()` in NVRM::probe), not the match dictionary. A Pascal GPL
  kext would use the same wide-match personality and gate on the GP10x IDs itself.
  Same pattern in `kexts/NVRM/fb/Info.plist` and `kexts/NVRM/accel/Info.plist`.
- **UserClient for userspace transport**: `NVRMUserClient` (declared in the same plist) is
  the existing userspace↔kext channel; the offload design reuses this shape for a
  DriverKit-or-kext PCI transport.
- **XNU OS layer**: `kexts/NVRM`'s `os-xnu*.cpp` shows how NVIDIA RM concepts (BAR mapping,
  timers, threads) map onto IOKit — the same adaptations a nouveau port needs.
- **NVK bridge**: `nvk/nvk-macos.patch` on Mesa `17ca6174` is the layer that would gain a
  second backend shaped like the nouveau CS uapi (VM bind + exec) for Pascal.

## 9. Verification checklist for any claimed milestone

Per this repo's own standard (docs/CARD-SUPPORT.md) and the Meteor Lake project's evidence
rules: a milestone counts only with real-hardware evidence — GPU state from IOKit, kernel
logs, stable WindowServer timing, render-readback pixel checks — never a compile or a mocked
test. Collect logs before any reboot; `tools/test_card_support.py` remains the repo's
regression gate for the Turing+ installer work.
