# Web Drivers vs. our open path — Pascal (GP106M) on macOS

Research-only comparison of NVIDIA's proprietary macOS **Web Drivers** (the historical
way "full Metal acceleration" existed for Maxwell/Pascal) against this repo's open
pipeline (GPL-2.0 nvkm kext + core + bridge + NVK + NullMoth plugin/translator).
Target machine context: Acer Predator Helios 300 2017, Optimus-only, no MUX
(`HELIOS300-PASCAL-RESEARCH.md` §2).

**Constraints honored:** no proprietary binaries, decompiled code, or driver packages
were downloaded or vendored into this repository. Everything below is documented from
public sources (NVIDIA download pages, community package/kext *metadata*, reverse-
engineering *write-ups*); where a claim rests on inference rather than a cited fact it
is marked **[inferred]**.

---

## 1. What the Web Drivers actually are

### 1.1 Package and kext layout

The Web Drivers are macOS installer packages (`WebDriver-<version>.pkg`, ~61 MB for
387.10.10.10.40.105 — NVIDIA's own download page:
https://www.nvidia.com/download/driverResults.aspx/136062/en-us) that install a
parallel, private copy of Apple's NVDA driver stack under `/Library/Extensions`
(kexts) and `/System/Library/Extensions` (userspace bundles), plus a preference pane.
The complete file inventory is documented in Dortania's OpenCore Legacy Patcher
source, which re-installs these exact paths
(https://github.com/dortania/OpenCore-Legacy-Patcher/blob/main/opencore_legacy_patcher/sys_patch/patchsets/hardware/graphics/nvidia_webdriver.py):

| Path | Role |
| --- | --- |
| `NVDAStartupWeb.kext` | PCI probe/personality kext; decides whether the Web stack loads (see §1.2) |
| `NVDAResmanWeb.kext` | the resource manager (NVRM): channel/allocation/IRQ/VM management |
| `NVDAGP100HalWeb.kext` | Pascal hardware layer ("GP100" = GP10x family in Apple's naming) |
| `NVDAGM100HalWeb.kext` / `NVDAGK100HalWeb.kext` | Maxwell / Kepler hardware layers |
| `GeForceWeb.kext`, `NVDANV50HalTeslaWeb.kext`, `NVDAResmanTeslaWeb.kext` | legacy Tesla variants |
| `GeForceMTLDriverWeb.bundle` | **Metal** renderer plugin |
| `GeForceGLDriverWeb.bundle` | OpenGL renderer plugin |
| `GeForceAIRPluginWeb.bundle` | Core Image / AIR plugin |
| `GeForceVADriverWeb.bundle` | video decode (VDA/GVA) plugin |
| `NVIDIA Driver Manager.prefPane`, `com.nvidia.nvagent.plist`, `com.nvidia.nvroothelper.plist` | menubar switcher between Apple's stock and Web drivers |

A `kextstat` dump from a machine actually running 387.10.10.10.30.106 confirms the
loaded bundle IDs at runtime: `com.nvidia.web.GeForceWeb`, `com.nvidia.web.NVDAGP100HalWeb`,
`com.nvidia.web.NVDAResmanWeb`, `com.nvidia.NVDAStartupWeb`, all version "10.3.3"
(https://macos86.it/topic/279-nvidia-web-driver/). WhateverGreen's source targets the
same bundle IDs, including the unloaded-path list for `com.nvidia.NVDAStartupWeb`
(https://github.com/acidanthera/WhateverGreen/blob/master/WhateverGreen/kern_ngfx.cpp).

Naming note: "NVDAHardwareRestore" appears in some informal driver-file discussions,
but it does **not** appear in the public package inventories we verified (OCLP
`nvidia_webdriver.py`, macos86 kextstat dump, macOS 10.13/10.14 `/S/L/E` diffs at
https://gist.github.com/knightsc/6c188ad9c065d7c134de88564974952c). We treat its
existence/role as **unverified** (see §5).

### 1.2 How loading is gated (NVRAM + build-string pinning)

Three public, user-visible gates decide whether the Web stack loads; all three are
documented in community tooling that manipulates them:

1. **NVRAM flag.** The driver requires the NVRAM variable `nvda_drv` to be the data
   `0x31` ("1"); as a *boot argument* it stopped working in macOS Sierra and became
   `nvda_drv_vrl=1` (Sierra/High Sierra) or the injected NVRAM data
   (https://dortania.github.io/GPU-Buyers-Guide/misc/bootflag.html — "STOP USING IT…
   moved to an NVRAM variable"; validation logic in OCLP `detect.py`:
   https://github.com/dortania/OpenCore-Legacy-Patcher/blob/b372a3f7/opencore_legacy_patcher/sys_patch/patchsets/detect.py).
   Bootloader configs set it via Clover `SystemParameters/NvidiaWeb` or OpenCore
   `NVRAM->Add->7C436110-AB2A-4BBB-A880-FE41995C9F82->nvda_drv`
   (https://www.tonymacx86.com/threads/nvidia-releases-alternate-graphics-drivers-for-macos-high-sierra-10-13-4-387-10-10-10-30.249039/).
   The `NVDAStartupWeb` probe itself reads this to choose Web vs. Apple's stock
   ("Official") stack — WhateverGreen routes `NVDAStartupWeb::probe` to patch exactly
   this decision (`kern_ngfx.cpp`, URL above).
2. **OS build-string pinning.** `NVDAStartupWeb.kext/Contents/Info.plist` carries
   `IOKitPersonalities:NVDAStartup:NVDARequiredOS`, set to the exact macOS build
   (e.g. `17G65`); on mismatch the kext refuses to load. Scripts like automate-eGPU
   read and rewrite this key with PlistBuddy
   (https://github.com/goalque/automate-eGPU/blob/master/automate-eGPU.sh,
   `NVDARequiredOSCheck()`), and a whole class of community fixes consists of editing
   that one string (https://turunen.ee/reactivating-nvidia-drivers-after-a-macos-update/;
   https://www.insanelymac.com/forum/topic/306535-nvidia-web-driver-updates-for-el-capitan-update-07112018/page/28/ —
   replacing the Info.plist invalidates the package signature but works with unsigned
   kext loading allowed).
3. **PCI identity.** Like all NVDA kexts, the personalities match on
   `IOPCIMatch`/`IOPCIPrimaryMatch` against vendor `0x10DE` + device ID (the classic
   pattern is documented in the Olarila NVDA kext guide
   https://olarila.com/topic/126-basic-guide-about-nvidia-kexts-how-to-enable-video-drivers/),
   plus `IOPCITunnelCompatible` for Thunderbolt eGPU matching (automate-eGPU.sh
   `SetIOPCITunnelCompatible()`, URL above).

`NVDAType` in the same plist distinguishes "Official" (Apple-shipped) from the Web
stack (automate-eGPU.sh `DeduceStartup()`, URL above).

### 1.3 Version capping and the Pascal timeline

- **Pascal support started 2017-04-11** with the 378.05.05.05 series on Sierra
  10.12.4 (16E195); the driver-version/build table at
  https://www.hackintosh-montreal.com/t5801p50-tuto-webdriver records
  `10.12.4 (16E195) 378.05.05.05f01 du 2017.4.11` as the first package that
  "GTX 1030, 1050, 1060, 1070, 1080" appear in. NVIDIA's Titan Xp announcement is the
  corporate-side marker (https://blogs.nvidia.com/blog/2017/04/06/titan-xp/).
- **Metal on the Web stack arrived with High Sierra-era drivers (378.10.x, late 2017)
  and was functional in the 387 series (10.13.2+).** Evidence, community-measured:
  - High Sierra guide: after installing the 10.13 web driver the system has
    "support Metal 2 et OpenGL" (https://www.hackintosh-montreal.com/t5801p54-tuto-webdriver,
    méthode 1 step 3);
  - a 10.13.4 (387.10.10.10.30) user measured Unity on the Metal backend at ~1500 fps
    vs ~250 fps on OpenGLCore with the new driver — Metal was not just present but
    the faster path (https://www.tonymacx86.com/threads/nvidia-releases-alternate-graphics-drivers-for-macos-high-sierra-10-13-4-387-10-10-10-30.249039/, comment);
  - WhateverGreen ships a default-*enabled* Metal path with `ngfxgl=1` documented as
    "disable Metal support on NVIDIA"
    (https://github.com/acidanthera/WhateverGreen — README boot-arg table);
  - the Sierra-era 378 packages predate `GeForceMTLDriverWeb.bundle` in the known
    inventories; community discussion at the time framed Maxwell/Pascal's Sierra
    driver as the pre-Metal generation ("the architecture fundamentally changed from
    Maxwell and then nVidia had to implement a Metal binding",
    https://www.tonymacx86.com/threads/10-12-nvidia-pascal-web-driver-disaster-reloaded-under-10-13-high-sierra.228068/page-4).
    **[inferred from bundle inventories + quote: Sierra 378 = OpenGL/OpenCL only; Metal
    for Pascal = High Sierra 378.10/387, i.e. 10.13.0–10.13.6.]**
  - Caveat community members hit: Metal apps + Library Validation produced
    transparent/empty windows because the Web binaries are not Apple platform
    binaries; fixed first by NVWebDriverLibValFix
    (https://github.com/mologie/NVWebDriverLibValFix) and later absorbed into
    WhateverGreen, which spoofs NVIDIA's Team ID `6KR3T733EC` in
    `_csfg_get_platform_binary` (DeepWiki write-up:
    https://deepwiki.com/acidanthera/WhateverGreen/5.1-web-driver-support).
- **The cap: 387.10.10.10.40 series, macOS 10.13.6 only.** One package build per
  macOS build, ending at `.140` for `17G14042`
  (https://hsvn.gitbook.io/guides/nvidia-web-driver-for-macos — full list; the
  MacRumors FAQ lists all 387 URLs with their macOS builds and CUDA pairings:
  https://forums.macrumors.com/threads/faq-nvidia-web-drivers.2170497/).
  A short-lived 387.10.10.15.15.108 added Volta (Titan V) and was pulled
  (MacRumors FAQ, same URL; DarwinKVM compatibility chart:
  https://docs.darwinkvm.com/host/03-CompatibilityCharts/02-GPU/02-NVIDIA/).
- **Mojave (10.14) = zero Web Drivers.** Pascal is unsupported from 10.14 onward
  (https://dortania.github.io/GPU-Buyers-Guide/modern-gpus/nvidia-gpu.html —
  "Highest Supported OS: High Sierra (10.13.6)"). NVIDIA's public statement: "Apple
  fully controls drivers for Mac OS. Unfortunately, NVIDIA currently cannot release a
  driver unless it is approved by Apple. Our hardware works on OS 10.13 which supports
  up to (and including) Pascal."
  (NVIDIA statement as quoted at https://set.ogorodguru.ru/drivers-nvidia-macos-mojave/).
- **Hardware quirk relevant to GP106:** the GP104-based/GDDR5X GTX 1060 variants are
  *not* supported by the Web Drivers; the standard GP106 GTX 1060 is
  (https://dortania.github.io/GPU-Buyers-Guide/modern-gpus/nvidia-gpu.html;
  https://docs.darwinkvm.com/host/03-CompatibilityCharts/02-GPU/02-NVIDIA/).
  The Helios 300's GTX 1060 Mobile (10DE:1C20/1C60) is standard GP106
  (`HELIOS300-PASCAL-RESEARCH.md` §1).

### 1.4 Runtime-observed dependency set

A panic/kext log from a Web-Driver machine shows what the stack links against:
`com.apple.iokit.IOAcceleratorFamily2`, `com.apple.iokit.IOSurface`,
`com.apple.iokit.IOPCIFamily`, `com.apple.driver.AppleMobileFileIntegrity`, plus
Apple's `AppleGraphicsDevicePolicy`/`AppleGPUWrangler`/`AppleGraphicsControl` chain
(https://macos86.it/topic/279-nvidia-web-driver/ — crash-log dependency listing).
I.e., NVIDIA plugged a proprietary stack into Apple's standard IOAccelerator/IOSurface
graphics plumbing, exactly the plumbing our offload design also targets
(`HELIOS300-PASCAL-RESEARCH.md` §4, `research/evidence/OFFLOAD-DESIGN.md`).

---

## 2. Mechanism comparison: Web Drivers vs. our pipeline

Our side references: `docs/HOW-IT-WORKS.md` (NullMoth layers), `HANDOFF-PASCAL.md` §4
(decided architecture), `HELIOS300-PASCAL-RESEARCH.md` §3–4, `research/evidence/OFFLOAD-DESIGN.md`.

| Dimension | NVIDIA Web Drivers (387, 10.13.6) | Our open path (this repo) |
| --- | --- | --- |
| **Kernel structure** | Apple-shaped IOKit stack: `NVDAStartupWeb` (probe) → `NVDAResmanWeb` (resource manager) → `NVDAGP100HalWeb` (Pascal HAL) + `GeForceWeb`; proprietary, single-vendor (§1.1 cites) | GPL-2.0-only kext tree porting nouveau `nvkm` GP106 subset (ACR, MMU v2, FIFO/GPFIFO, GR, CE) + a UserClient matching the `NVRMUserClient` shape; deliberately separate from the PolyForm-NC nullmoth kexts (`HANDOFF-PASCAL.md` §3, §4) |
| **Kernel–user transport** | Proprietary IOUserClient surface inside the NVRM family; internals not public. Public hints: WhateverGreen patches symbol names `nvGpFifoChannel::Prepare`/`::Complete` and `nvVirtualAddressSpace::PreSubmit` on 10.13 — i.e. GPFIFO channel submission + per-VAS submit bookkeeping (DeepWiki: https://deepwiki.com/acidanthera/WhateverGreen/5.1-web-driver-support; code refs `kern_ngfx.cpp` 151–188) | Nouveau **CS uapi shaped bridge** (VM bind + exec semantics) implemented in our bridge so NVK runs unmodified-ish; the exact semantics are public in Mesa `src/nvk/` and Linux uapi (`HELIOS300-PASCAL-RESEARCH.md` §3.1; `research/evidence/NOUVEAU-UAPI.md`) |
| **Metal integration point** | `GeForceMTLDriverWeb.bundle` implements Apple's Metal driver plugin *on top of the proprietary kexts*, registering through **IOAcceleratorFamily2** (the stack links `IOAcceleratorFamily2` + `IOSurface`, §1.4). Metal shaders compiled natively by NVIDIA's compiler inside the bundle (bundle ships `GeForceMTLDriverWeb` — OCLP inventory, §1.1) | `NVMTLDriver.bundle` (Metal plugin over **Vulkan/NVK**, `plugin/`) + **AIR→SPIR-V translator** (`translator/`); NVK compiles to NVIDIA machine code. Metal integration point is Apple's Metal.framework device-plugin contract, surfaces via our NVAccel/IOSurface path (`docs/HOW-IT-WORKS.md` §1, §3) |
| **Display / compositor role** | Full display stack: drives connectors/framebuffer (NVIDIA HAL + IONDRVSupport), WindowServer composites on the NVIDIA accelerator on Macs/eGPUs | **Headless by design on this laptop**: Intel HD 630 owns all displays + WindowServer; our dGPU renders into IOSurfaces the Intel side composites (offload architecture — the only architecture physically possible on a no-MUX Helios 300, `HELIOS300-PASCAL-RESEARCH.md` §2, §4). dGPU display later via nouveau `disp` port on HDMI/miniDP only |
| **Optimus handling** | **None. Officially unsupported on Optimus laptops.** Community consensus on Optimus hardware: enable dGPU only with Optimus disabled in BIOS (desktop-style PEG path) or *disable the dGPU via ACPI* (`_OFF`/`_DSM`+`_PS3`) and run Intel-only — Dortania's own laptop guide's only macOS options are "disable the dGPU" methods (https://dortania.github.io/Getting-Started-With-ACPI/Laptops/laptop-disable.html); the osxlatitude Clevo P650RE verdict: "Optimus technology is not supported by OS X/macOS so you cannot run dual GPUs on a Hackintosh" (https://osxlatitude.com/forums/topic/16999-solved-clevo-p650re-nvidia-gtx-970m/). Patched/creative attempts (eGPU-style headless renderers, framebuffer-copy ideas in Mojave) never produced a working accelerated Optimus path (https://www.tonymacx86.com/threads/possible-nvidia-optimus-functionality-in-macos-mojave.263664/) and headless init with web drivers reportedly failed (`rmStart failed`: https://www.reddit.com/r/hackintosh/comments/8ge546/) | **Optimus is our design target, not a bug.** Offload-only architecture, computed headless GR + IOSurface handoff to the Intel compositor; x86 shared memory is cache-coherent so the handoff is effectively zero-copy (`HANDOFF-PASCAL.md` §4; `HELIOS300-PASCAL-RESEARCH.md` §4) |
| **Power management** | Full desktop-style PM on supported configs: NVSMU + Apple AGPM/IOPlatformPlugin integration; community notes "better native GPU power management" with 387 vs stock (https://www.tonymacx86.com/threads/nvidia-releases-alternate-graphics-drivers-for-macos-high-sierra-10-13-6-387-10-10-10-40.255913/). Laptop Optimus PM (RTD3) was never exercised on macOS — moot since the stack is disabled there | **Accepted hard limit:** no Pascal PMU firmware → no reclocking (VBIOS boot clocks only) and no RTD3; "switching off" is software-idle only, real battery cost (`HELIOS300-PASCAL-RESEARCH.md` §3.3, §4; `HANDOFF-PASCAL.md` §4). This is the one axis where the Web Drivers were strictly better *on desktops*; on our Optimus laptop they were never functional at all |
| **Licensing** | Proprietary NVIDIA EULA; installation gated on accepting it and on Gatekeeper/"Allow" (NVIDIA download page, §1 note: https://www.nvidia.com/download/driverResults.aspx/136062/en-us). Not Apple platform-signed → kernel-level patches needed to run (NVWebDriverLibValFix / WhateverGreen libval fix, §1.3). Distribution today relies on archived third-party mirrors | nouveau/nvkm GPL-2.0-only (own kext tree), Mesa NVK MIT, `linux-firmware` gp106 blobs under NVIDIA's redistribution license — same shipping model as the repo's GSP firmware (`HANDOFF-PASCAL.md` §3). No signature-spoofing needed; we ship our own kexts |
| **Longevity** | Frozen: last driver Jul 2018-era, last supported macOS 10.13.6 (§1.3). Dead end on any modern macOS | Works against current macOS (upstream nullmoth tested on macOS 15.7.x, `docs/HOW-IT-WORKS.md`); our Pascal port inherits the same install path |

---

## 3. What we can learn without copying

All items below are *patterns and hardware facts*, documented publicly; no binary or
decompiled artifact is referenced as source material.

### 3.1 IOKit personality / Info.plist patterns (public metadata)

- `IOPCIMatch` / `IOPCIPrimaryMatch` with `0x10DE` vendor masking + per-device IDs,
  and per-kext device-ID extension as the classic enable path — documented in the
  Olarila NVDA kext guide (https://olarila.com/topic/126-basic-guide-about-nvidia-kexts-how-to-enable-video-drivers/).
  **Reuse for us:** our Phase 3 PCI personality (gating on `10DE` display/3D class,
  GP10x device IDs — `HANDOFF-PASCAL.md` Phase 3) should follow the same
  personality shape (`IOProviderClass: IOPCIDevice`, provider property matching) so
  the kext participates in normal IOKit matching rather than claiming devices ad hoc.
- Build-pin personality key (`NVDARequiredOS`) and type key (`NVDAType`):
  useful as a *cautionary* pattern — the Web Drivers' per-build pinning is exactly the
  fragility that made every macOS update break them (§1.2 cites). Our kexts should
  gate on *capability* (IOGraphicsFamily/IOAcceleratorFamily2 presence), not exact OS
  build. **[design takeaway, not a copy]**
- `IOPCITunnelCompatible` for Thunderbolt-matched personalities (automate-eGPU.sh,
  §1.2) — irrelevant for the internal Helios 300 dGPU but relevant if the owner ever
  tests an eGPU enclosure.
- WhateverGreen's `ngfxcompat=1` ("Ignore compatibility check in NVDAStartupWeb")
  documents that `NVDAStartupWeb::probe` performs a board-id/compat check beyond PCI
  match (https://github.com/acidanthera/WhateverGreen README boot-arg table;
  https://deepwiki.com/acidanthera/WhateverGreen/5-nvidia-graphics-support). Lesson:
  expect probe-time rejections on non-Apple hardware and design ours to be
  OpenCore-friendly from the start.

### 3.2 IORegistry property contract hints (highest-value steal)

vit9696's public reverse-engineering write-up of `nvAcceleratorParent::SetAccelProperties`
in web driver 378.05.05.05f01 (posted in full in the acidanthera bugtracker, text-only
RE commentary: https://github.com/acidanthera/bugtracker/issues/227) documents that an
NVIDIA accelerator must publish **`IOVARendererID` / `IOVARendererSubID`** into the
IORegistry, with AppleGVA (and WindowServer paths) keying off specific values
(`<08 00 04 01>` / `<03 00 00 00>` for VP4-class, `<04 00 04 01>` for VP3). Maxwell/
Pascal Web Drivers "do not properly add these values", which is why WhateverGreen
injects them (`applyAcceleratorProperties`, https://deepwiki.com/acidanthera/WhateverGreen/5-nvidia-graphics-support).

**Direct reuse:** our `NVAccel.kext` (IOAccelerator child of the GPL kext's
personality) must publish a correct `IOVARendererID`/`IOVARendererSubID` pair, or the
Intel iGPU's *offline* decode path and compositor checks will misbehave — the exact
failure mode in that bugtracker thread. This is a documented macOS-side contract we
get for free from their history, no NVIDIA code needed.

### 3.3 NVRM API surface hints from open documentation

- The public NVRM *interface* our repo already uses (`open-gpu-kernel-modules`
  RM/`rm_init_adapter`, class headers `clc1c0.h`/`clc06f.h`) is the same RM the Web
  Drivers embed in proprietary form; class definitions for Pascal are open
  (`HELIOS300-PASCAL-RESEARCH.md` §3.1). Their 10.13 stutter-fix symbol names
  (`nvGpFifoChannel::Prepare/Complete`, `nvVirtualAddressSpace::PreSubmit`) confirm
  the macOS RM kept the **GPFIFO channel + virtual-address-space** model — the same
  model as nouveau's FIFO/GR, reinforcing that our nvkm port is modeling the right
  hardware interface (DeepWiki WhateverGreen §5.1, cited in §2).
- envytools' MMIO/class database remains the open register reference
  (https://envytools.readthedocs.io/en/latest/hw/mmio.html, cited in
  `HELIOS300-PASCAL-RESEARCH.md` §3.1).

### 3.4 Hardware facts their behavior implies (usable by us)

1. **Pascal GR + ACR works on macOS-class bring-up with NVIDIA's own signed blobs.**
   The Web Drivers booted Pascal GR without any Apple-specific hardware path on
   hacked Macs/eGPUs — consistent with our assumption that the *same* signed ACR/FECS/
   GPCCS blobs we take from `linux-firmware/nvidia/gp106/` are the whole firmware
   requirement (`HELIOS300-PASCAL-RESEARCH.md` §3.2). **[inference from §1.1/§1.3]**
2. **Headless initialization is the weak point of their stack, not ours to inherit.**
   Community reports that the Web drivers fail to initialize cards without display
   heads (`rmStart failed`, https://www.reddit.com/r/hackintosh/comments/8ge546/ —
   "the Web drivers fail to properly load in the first place (`rmStart failed`)").
   Our Phase 3/6 design must make `rm_init_adapter()` succeed with **zero heads** —
   treat this as an explicit acceptance test, because the one proprietary datapoint
   we have suggests display-head dependency in the macOS RM start path.
3. **An offline (no-display) GPU can participate in macOS compositing.** Apple's own
   AppleGVA supports an "offline" Intel GPU (framebuffer with no connectors) and
   10.14+ eGPU-on-internal-display works by copying the eGPU framebuffer to the
   internal GPU (vit9696's AppleGVA explainer,
   https://github.com/acidanthera/bugtracker/issues/227; the Mojave eGPU
   framebuffer-copy observation,
   https://www.tonymacx86.com/threads/possible-nvidia-optimus-functionality-in-macos-mojave.263664/).
   This is the platform behavior our IOSurface offload design leans on — macOS does
   not require the renderer GPU to own a display.
4. **NVSMU** (the stock macOS `NVSMU.kext`, present in 10.13–10.14 kext lists:
   https://gist.github.com/knightsc/6c188ad9c065d7c134de88564974952c) shows Apple
   modeled an NVIDIA SMU power component for Kepler; on Pascal we have no PMU
   firmware at all, so expect no equivalent and don't chase one (matches our
   no-reclocking limit, §3.3 of the research doc).

---

## 4. Feasibility verdict for the Helios 300

**The Web-Driver route is not viable on this hardware — three independent reasons,
any one of which is fatal:**

1. **OS dead end.** Web Drivers stop at macOS 10.13.6 (17G14042); nothing exists for
   10.14+, by NVIDIA's own statement (§1.3). Our open path targets current macOS via
   the same install path as the upstream Turing+ driver (`docs/HOW-IT-WORKS.md`).
2. **Optimus = unsupported by construction.** The Web stack only ever worked in
   desktop/PEG or eGPU topologies where the NVIDIA GPU can own displays. On an
   Optimus-only, no-MUX laptop (our machine, `HELIOS300-PASCAL-RESEARCH.md` §2) the
   entire body of community practice is to *power the dGPU off* (`SSDT-dGPU-Off`,
   `_OFF` method) — Dortania's guide documents Optimus laptops' only stable macOS
   configuration as dGPU-disabled (https://dortania.github.io/Getting-Started-With-ACPI/Laptops/laptop-disable.html);
   dual-GPU "is not supported by OS X/macOS" per the Clevo thread verdict (§2 table).
3. **Even headless (compute-only) use of the Web Drivers on Optimus hardware failed.**
   The only community attempt at "dGPU as offline renderer" on Optimus laptops
   reported the drivers failing to init headless cards (`rmStart failed`,
   https://www.reddit.com/r/hackintosh/comments/8ge546/). There is no documented case
   of accelerated Optimus on macOS with Web Drivers — the Mojave-era idea
   (https://www.tonymacx86.com/threads/possible-nvidia-optimus-functionality-in-macos-mojave.263664/)
   never got past theory.

**What our open path must achieve to equal or beat "Metal acceleration":**

- **Compute first (Phases 0–4):** GP106 ACR/FIFO/GR up in the GPL kext, NVK bridge on
  the CS uapi, `NVMTLDriver.bundle` enumerating a `MTLDevice` so Metal compute
  (MPS included) runs on GP106 (`HANDOFF-PASCAL.md` Phases 0–4; `docs/HOW-IT-WORKS.md`
  §3). This alone exceeds anything the Web Drivers ever did on this laptop (nothing).
- **Graphics second (Phases 5–6):** headless render into IOSurfaces, Intel composites
  (offload; `research/evidence/OFFLOAD-DESIGN.md`), publishing `IOVARendererID` per
  §3.2 so the AppleGVA/offline-renderer machinery accepts us; dGPU-direct HDMI/
  miniDP only after the `disp` port (`HELIOS300-PASCAL-RESEARCH.md` §2, §4).
- **Where they stay ahead, permanently:** clocks (VBIOS boot clocks vs. their managed
  clocks) — a firmware wall, not an engineering choice
  (`HELIOS300-PASCAL-RESEARCH.md` §3.3).

---

## 5. Open questions

| # | Question | Evidence that would resolve it |
| --- | --- | --- |
| 1 | Does `NVDAHardwareRestore.kext` exist in any Web Driver package, and what does it do? | A public `pkgutil --files com.nvidia.web-driver` listing or Apple `/S/L/E` dump from 10.12–10.13 showing it; the OCLP inventory and known kext dumps (§1.1) do **not** list it |
| 2 | Did Sierra-era 378 Web Drivers include any Metal support for Pascal? | A 10.12.x system dump showing presence/absence of `GeForceMTLDriverWeb.bundle`; today only High Sierra-era inventories are public (§1.3) |
| 3 | Why exactly does `rmStart` fail on headless Optimus dGPUs with the Web stack (missing head ⇒ missing IRQ/GART setup? Apple policy check?), and does our RM start path avoid it? | A public `nvErrorLog`/`-ngfxdbg` log from an Optimus attempt; on our side, our own Phase 3 headless-init acceptance test passing is the actionable equivalent (§3.4 item 2) |
| 4 | Does the Helios 300's ACPI expose `_OFF`/`_DSM` power methods for `PEGP` that our software-idle design can call when switching off? | Owner dumps the DSDT from the machine (Windows Device Manager → "BIOS device name", per the Dortania disable guide, §2 table); would refine `research/evidence/OFFLOAD-DESIGN.md` |
| 5 | Did any Optimus laptop ever get even *compute* (CUDA) working with Web Drivers in a real macOS session? | A reproducible forum report with `kextstat` + `nvidia-smi`/CUDA sample output on Optimus hardware; absence of any such report is itself the evidence backing §4 |
| 6 | What exact IORegistry property set does WindowServer need from a *headless* accelerator (beyond `IOVARendererID`) to composite its IOSurfaces? | Our own macOS-side bring-up logs (owner-run) comparing IORegistry of the Intel accelerator vs. ours; WhateverGreen's property-injection code is the public reference implementation to mirror (§3.2) |

---

*Written 2026-11-05, branch `pascal-gp106`. Sources verified via public web
documents; no binaries fetched. Companion docs: `HELIOS300-PASCAL-RESEARCH.md`,
`research/evidence/OFFLOAD-DESIGN.md`, `docs/HOW-IT-WORKS.md`.*
