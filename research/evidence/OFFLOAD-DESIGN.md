# Offload switching design — HD 630 owns displays, GP106 renders headless

Phase 6 deliverable (`HANDOFF-PASCAL.md` §6, gate: "design doc + the complete owner
runbook"). Companion document: `research/evidence/OWNER-RUNBOOK.md`. Design only —
no code in this branch implements the switching layer yet.

Every external claim carries a source. Claims that are design intent rather than
established fact are labeled **[design]**. Simulation-proven items are labeled as
such; nothing here is hardware-proven (`research/evidence/BLOCKERS.md` B2: all
macOS verification is owner-only).

## 1. System shape

```
                 +-----------------------------------------------+
                 |  macOS / WindowServer                         |
                 |  (runs on the Intel HD 630)                   |
                 +-----------------------------------------------+
      displays   |  AppleIntelKBLGraphics* + Apple IOAccelerator |   internal panel wired
   -------------+  composite NVRD-frames from IOSurfaces        +<-- to iGPU; HDMI/miniDP
   panel, TB/C  |  (Apple's kexts, untouched)                   |    wired to dGPU (no MUX)
                 +-----------------------^-----------------------+
                                         | IOSurface (system memory, host pages)
                                         | readiness via surface seed / mark,
                                         | CPU-visible copy-out or direct GART map
                 +-----------------------+-----------------------+
                 |  App process (Metal client)                   |
                 |  NVMTLDriver.bundle  .................... plugin/            |
                 |    | AIR -> SPIR-V ............. translator/ (metal2vulkan) |
                 |    | SPIR-V -> Pascal pushbuffer .. NVK  ... nvk/nvk-macos.patch |
                 |    | Vulkan uapi (VM bind + exec) . [uapi bridge, see        |
                 |    |                                research/evidence/NOUVEAU-UAPI.md] |
                 +-----------------------+-----------------------+
                                         | UserClient (NVRMUserClient shape)
                 +-----------------------v-----------------------+
                 |  Pascal GPL-2.0 kext (display-less)           |
                 |  PMC probe / BAR map / MMU v2 / GPFIFO / GR   |
                 |  (pascal/ core, Phase 0 complete: ROADMAP.md) |
                 +-----------------------+-----------------------+
                                         | BAR0/BAR1 MMIO, GDDR5
                                 GTX 1060 Mobile (GP106M, 10DE:1C20/1C60)
```

Fixed wiring facts:

- The Helios 300 (2017–2018) is Optimus-only, **no MUX**: the internal panel is
  physically wired to the Intel iGPU; HDMI/miniDP are wired to the dGPU
  (`HELIOS300-PASCAL-RESEARCH.md` §2).
- Therefore "switching" can only mean **runtime offload** (render on NVIDIA,
  present via Intel); a display MUX does not exist on this machine (research §2,
  "Consequences for macOS").
- The Intel side needs no changes: HD 630 (KBL) is natively driven by Apple's
  `AppleIntelKBLGraphics*` kexts (research §4, architecture 1; research §5).

**Zero-copy claim.** "On x86 the shared memory is cache-coherent, so the handoff
is effectively zero-copy" — `HELIOS300-PASCAL-RESEARCH.md` §4, architecture 1.
This is a property of the x86 PCIe/coherence model for host-memory
backed surfaces; it is a claim from the research doc, not yet a measurement on
this design. Two honest caveats, both cited:

- The shipped Turing+ driver is *not* zero-copy on the headless path: "on such a
  GPU, IOSurface textures are private per process and copied out after every GPU
  write" (`plugin/nvmtl_vk.c:2464–2476`); the copy-out is a copy-engine
  page-off into host pages (`kexts/NVRM/nvrm_surfshare.cpp:51–68`,
  `nvrm_ce_copy_to_pages`).
- The Pascal uapi path can be genuinely zero-copy **[design]**: NVK maps host
  pages into GPU virtual address space via VM bind, so the render target *is*
  the IOSurface's backing store; no VRAM staging copy exists. This depends on
  the uapi surface documented in `research/evidence/NOUVEAU-UAPI.md` (sibling
  document; see §4 below). Until that bridge exists and is measured, treat
  "zero-copy" as the goal, with the proven Turing+ copy-out mechanism
  (`nvrm_surfshare_pageoff`) as the fallback that already works in this repo.

## 2. Surface lifecycle

### 2.1 Allocation

- The IOSurface itself is always allocated **on the macOS side, in system
  memory** — by the app (via `newTextureWithDescriptor:iosurface:` /
  `newBufferWithIOSurface:`) or by WindowServer for its own window surfaces.
  The driver never allocates the IOSurface; it is referenced by ID
  (`IOSurfaceGetID`), as the existing plugin does
  (`plugin/NVMTLDevice.m:1157–1177`; `plugin/NVMTLDevice_contract.m:72`).
- System memory is the only choice here: the dGPU surface must be readable by
  WindowServer compositing through the *Intel* accelerator, and the Pascal kext
  has no IOAccelerator of its own — an NVAccel-published surface would not be
  visible to Apple's compositor path (the Turing+ driver's NVAccel exists
  precisely for this role, and Pascal deliberately does not ship one:
  `docs/HOW-IT-WORKS.md` §1–2; `HANDOFF-PASCAL.md` §4 "the NVIDIA side runs
  display-less").
- **[design]** Preferred backing: the IOSurface's host pages are bound into the
  GPU's MMU v2 address space (page tables built by `pascal/core/src/mmu.c`,
  format per `pascal/README.md` and `HELIOS300-PASCAL-RESEARCH.md` §3.1 —
  Pascal is MMU v2, `gp100-mmu-format.pdf`). NVK then renders directly into
  them. This is the uapi VM-bind path (`research/evidence/NOUVEAU-UAPI.md`).
- Fallback (proven in-repo on Turing+): render in GDDR5, then CE-copy into the
  surface's host pages after each write (`kexts/NVRM/nvrm_surfshare.cpp:53–68`;
  `plugin/nvmtl_vk.c:2489–2508` for the mark/try-lock flow). The Pascal kext
  can reuse the same design (its own GPL implementation of a page-off — never
  the PolyForm-NC file itself, per `HANDOFF-PASCAL.md` §3 licensing rule).

### 2.2 Handoff / cross-process references

- Cross-process sharing of the surface is done by macOS IOSurface IDs / mach
  ports, not by the GPU driver; the GPU side only needs the surface's host
  pages and size. The repo's RM-object sharing precedent for Turing+ is the
  `NV04_SHARE` + `RS_ACCESS_TYPE_CLIENT` + `RS_ACCESS_DUP_OBJECT` path in
  `kexts/NVRM/nvrm_surfshare.cpp:32–39` (ABI in `kexts/NVRM/nvrm_surfshare_abi.h:41–47`);
  **[design]** on Pascal no RM object share is needed for the zero-copy path
  because the shared object is plain host memory already mapped by both the app
  and the kext.
- WindowServer learns a frame is ready through the IOSurface's own
  update/seed semantics plus the driver's completion mark; the repo's existing
  mark ABI distinguishes a dirty plane from a "WindowServer completion mark"
  (`NVRM_SS_PLANE_GPU_MARK`, `kexts/NVRM/nvrm_surfshare_abi.h:11–12`) and the
  plugin sends that mark on NVK surface escapes
  (`plugin/nvmtl_vk.c:2509–2514`). **[design]** Pascal reproduces this
  contract at the UserClient level.

### 2.3 Fence / semaphore per frame

What the Turing+ driver does today (this repo, cited):

- Completion waits are a **software-visible wait policy**, not Apple IOFence
  objects: `nvkmd_nvrm_wait_block()` defaults to event waits for WindowServer
  and polling/backoff for other processes; `NVRM_EVENT_WAIT=1` enables event
  waits after 20 µs with a bounded 2 ms wait and completion-state rechecks
  (`docs/EVENT-WAIT.md` §"What the setting does").
- The NVK-side sync object is a mapped GART memory word (semaphore surface)
  that the GPU increments and software polls — `nvRmSemSurfCreate` allocates
  mapped GART memory and returns a device-visible handle
  (`nvk/nvk-macos.patch`, `src/nouveau/vulkan/nvkmd/nvrm/nvRmSemSurf.c`, patch
  line ~7550), and the timeline/binary sync implementation lives beside it
  (`.../nvkmd/nvrm/nvkmd_nvrm_sync.c`, patch line ~10877).

Why Pascal takes the software path **[design, reasons cited]**:

- Turing+ event waits ride on the RM/GSP event machinery of NVIDIA's open
  kernel modules (`docs/HOW-IT-WORKS.md` §1: NVRM "talks to the GPU's GSP
  firmware"). Pascal bring-up has **no GSP at all** — the whole point of the
  nvkm port (`HELIOS300-PASCAL-RESEARCH.md` §3.1 table: "No GSP involved";
  `HANDOFF-PASCAL.md` §4) — so those event objects do not exist.
- The uapi's native primitive is the pushbuffer semaphore: the GPU writes a
  value to a mapped word (semaphore RELEASE methods; for compute the QMD's
  `RELEASE0` semaphore fields fire at dispatch end —
  `HELIOS300-PASCAL-RESEARCH.md` §6, header
  `research/reference/mesa_clc1c0qmd.h`). That is exactly the
  "mapped GART word + CPU poll" shape the repo already runs on Turing+
  (`nvRmSemSurf.c` above), so the Pascal bridge reuses it.
- Apple IOSurface fences (attaching a kernel fence object to a surface) are
  what Apple's own two-GPU Macs use between drivers; **this repo does not use
  them** — its plugin serializes with IOSurface user-data busy tags plus
  CPU-visible copy-out before marking the surface ready
  (`plugin/NVMTLDevice.m:1065–1098`, `nvmtl_busy_*`; the copy-out contract at
  `plugin/nvmtl_vk.c:2464–2476`). Keeping the same scheme on Pascal avoids a
  private ABI with Apple's Intel NVAccel that we cannot build or test.

Per-frame sequence **[design]**: NVK submits exec → GPU (or CE) completes and
releases the semaphore word → bridge/plugin observes completion (bounded poll,
the EVENT-WAIT pattern) → copy-out/flush to surface pages if the VRAM fallback
is in play → surface seed bumped / completion mark set → WindowServer composites
on Intel. No Apple IOFence is attached; correctness relies on ordering:
completion is observed before the mark is sent.

### 2.4 Resize and teardown

**[design]** The IOSurface is immutable in size; a client resize creates a new
IOSurface (new `IOSurfaceGetID`), the old one drains (bounded wait on its final
fence value, the §2.3 mechanism), GPU VA bindings for its pages are unbound
(MMU v2 teardown in `pascal/core/src/mmu.c`), then the surface is released by
macOS refcounting. The Turing+ plugin already keys per-surface state by
`(surfaceID, plane)` and drops caches on new IDs
(`plugin/NVMTLDevice.m:1165–1177`), which is the behavior to preserve.

### 2.5 Error paths

**[design]** Each failure maps to a visible, recoverable state:

| Failure | Detection | Behavior |
| --- | --- | --- |
| GPU channel/engine fault (GR, FIFO) | kext interrupt / pushbuffer watchdog | kill the channel, fail in-flight Vulkan fences; app sees device loss (`VK_ERROR_DEVICE_LOST`); kext stays loaded |
| Kext-side probe/boot failure (ACR etc.) | probe return code at UserClient open | client open fails; plugin falls back to Intel device; system unaffected |
| Surface page map fails (MMU) | bridge exec/VM-bind error | NVK falls back to private VRAM + copy-out (the Turing+ plugin already logs such fallbacks, `plugin/nvmtl_vk.c:2543–2547`) |
| Fence wait exceeds bound | poll timeout in bridge/plugin | treat as device loss for that submission; mark surface stale rather than present garbage |
| Kext unload while clients open | IOKit termination | client handles invalidated; processes must re-open; no persistent kernel state |

Nothing in the error table requires display or WindowServer state to change —
Intel compositing continues even if every Pascal submission fails.

## 3. Lifecycle and power

### 3.1 The dGPU never powers off

- Accepted hard limit: "no Pascal PMU firmware → no reclocking … and no runtime
  D3 (software idle only; battery cost is real)" (`HANDOFF-PASCAL.md` §4;
  detail in `HELIOS300-PASCAL-RESEARCH.md` §3.3: NVIDIA never released Pascal
  PMU firmware, nouveau runs PMU with `gm200_pmu_nofw`, so no runtime power
  management / RTD3 exists).
- Research's own power note: "'switching off' the dGPU is software-disabled
  only (persistent memory mapped, GPU held in reset or idle). True power gating
  is out of reach without PMU firmware" (`HELIOS300-PASCAL-RESEARCH.md` §4).

### 3.2 Software idle design **[design]**

- The PCI device stays in D0 with BAR0/BAR1 mapped for the kext's lifetime.
  Idle = no active GPFIFO channels, GR/CE context-free, pushbuffer empty.
- Render sessions are counted: every UserClient open increments, close
  decrements. With zero sessions the kext stops its worker threads (timers off)
  and leaves only the BAR mapping and interrupt line armed — the state
  research §4 calls "software-disabled".
- The kext does not attempt reset, clock changes, or link power management —
  all three would need PMU/ACR paths beyond VBIOS boot state (research §3.3).
- Battery implication, qualitative only: the machine permanently carries a
  second powered discrete GPU (PCIe link up, GDDR5 in self-refresh, GPU at
  VBIOS boot clocks). Research §3.3 calls the battery cost "real" and gives no
  number; no measurement exists for this design, and none is claimed here. The
  runbook measures idle power best-effort with `powermetrics`
  (`research/evidence/OWNER-RUNBOOK.md`, step 4).

### 3.3 What "switching" means

There is no display switching and no system-wide toggle. Switching is
**app-level placement**: which Metal device an application creates and renders
on.

- Default placement is Intel (WindowServer's device). An app lands on the
  Pascal device only when it selects that `MTLDevice` (the plugin publishes it
  alongside Apple's — the layer model in `docs/HOW-IT-WORKS.md` §1) or when the
  plugin's per-app profile mechanism routes it there (existing precedent:
  `plugin/nvmtl_app_profile.h` + per-app profile includes such as
  `plugin/nvmtl_app_profile_cp2077.inc`).
- This is the same scope as Linux PRIME offload: per-process renderer choice,
  compositor fixed (research §4, architecture 1, "matches how Linux PRIME
  offload works").
- Because the dGPU cannot power off (§3.1), placement decisions trade *speed*
  against *battery*, never against "GPU fully off". Battery-critical workloads
  should never be placed on the Pascal device — enforced as a hard rule in the
  runbook, not by code.

## 4. Bridge integration (how an app flows)

The userspace pipeline is reused as-is; only the kernel-facing backend changes
(`HANDOFF-PASCAL.md` §4: "The Metal plugin (`plugin/`) and AIR→SPIR-V
translator (`translator/`) are reused as-is"):

1. **App → plugin.** A Metal app gets `NVMTLDriver.bundle` as its MTLDevice
   (`docs/HOW-IT-WORKS.md` §3: "the Metal driver Apple's Metal.framework loads
   for the card"; it implements device/queues/encoders over Vulkan in
   `plugin/nvmtl_vk.c`).
2. **Shaders.** Apple AIR → SPIR-V via `libnvmtl_translate.dylib`, based on
   metal2vulkan (LGPL-3.0) (`docs/HOW-IT-WORKS.md` §3;
   `translator/translator/README.md`).
3. **NVK.** SPIR-V → Pascal machine code and command buffers. NVK supports
   Pascal (sm_60) (`HELIOS300-PASCAL-RESEARCH.md` §3.1). The macOS NVK patch
   builds against Mesa `17ca6174` with a macOS nvkmd backend
   (`docs/HOW-IT-WORKS.md` §3; backend files under
   `src/nouveau/vulkan/nvkmd/nvrm/` in `nvk/nvk-macos.patch`, e.g. the file
   list at patch lines 7532–11888).
4. **uapi bridge (the new part).** On Turing+, NVK talks to NVRM's GSP
   message-ring ABIs. For Pascal the patch "must gain a second backend shaped
   like [the nouveau CS] uapi (VM bind + exec) instead of the GSP message-ring
   ABIs" (`HELIOS300-PASCAL-RESEARCH.md` §3.3; also §8). The exact syscall /
   UserClient surface is specified in `research/evidence/NOUVEAU-UAPI.md`
   (sibling document; if it is not present yet, that document is the single
   source of truth for this layer once it lands). Phase 4's gate requires this
   uapi subset to be documented with test vectors and reviewed against
   `nvk/nvk-macos.patch`'s interface points (`HANDOFF-PASCAL.md` §6).
5. **Kext.** The bridge reaches the Pascal GPL-2.0 kext's UserClient (the
   `NVRMUserClient` shape per `HANDOFF-PASCAL.md` §3 Phase 3; wide-match PCI
   personality pattern per `HELIOS300-PASCAL-RESEARCH.md` §8). The kext drives
   the Phase 0 core: PMC probe (real GTX 1060 boot0 `0x136000a1`, GP10x family
   0x130 / chipset 0x136 — `pascal/README.md`), BAR map (`pascal/core/include/pascal/mmio.h`),
   MMU v2 (`pascal/core/src/mmu.c`), pushbuffer/GPFIFO (`pascal/core/src/pushbuf.c`,
   encoding resolved per `research/evidence/ENCODING-RESOLUTION.md`), QMD
   (`pascal/core/src/qmd.c`), ACR secure boot with the committed GP106/GP102
   blobs (`pascal/core/src/acr.c`; `research/firmware/gp106/README.md`;
   `research/evidence/ROADMAP.md` Phase 0 entry).
6. **Out.** Frames land in IOSurfaces in system memory (§2) and WindowServer
   composites them through Apple's Intel stack — the exact role division of
   architecture 1 (research §4), with the plugin's existing headless detection
   (`debug.nvaccel_headless` → "IOSurfaces are copied out to system memory",
   `plugin/nvmtl_vk.c:2468–2476`) as the switch that selects the offload
   surface mode inside the plugin.

Licensing chain for this pipeline, since it constrains integration: Pascal kext
GPL-2.0-only; NVK MIT; translator LGPL-3.0; `kexts/`/`plugin/` PolyForm-NC —
mutually incompatible, so the Pascal kext consumes the plugin/translator at
arm's length and never merges source (`HELIOS300-PASCAL-RESEARCH.md` §5
licensing caution; `HANDOFF-PASCAL.md` §3).

## 5. Testability

The owner-facing sequence, measurements and rollbacks live in
`research/evidence/OWNER-RUNBOOK.md`. Order: kext probe → Userspace Client
probe → NVK sandbox Vulkan smoke → render-into-IOSurface with WindowServer
unchanged — each step with explicit rollback and no persistent system state
(no AuxKC rebuild, no NVRAM/OpenCore changes).

## 6. Open questions

Each with the evidence that would resolve it:

1. **Is the zero-copy GART path actually attainable on this uapi?** The claim
   "cache-coherent, effectively zero-copy" is from research §4; the Turing+
   driver in-repo is copy-out based (`plugin/nvmtl_vk.c:2464–2476`). Evidence
   needed: `research/evidence/NOUVEAU-UAPI.md`'s VM-bind semantics showing
   host-page backing for render targets, then an owner-run measurement
   (runbook step 4: page-off counters absent vs present).
2. **Does WindowServer accept driver-produced IOSurface frames with no IOFence,
   seed/mark only?** Apple's own two-GPU Macs may attach fences; this repo
   relies on ordered completion + mark (`nvrm_surfshare_abi.h:11–12`). Evidence
   needed: runbook step 4 pixel-stability check plus an IORegistry/diagnostic
   dump of how the Intel NVAccel picks up externally produced surfaces (or
   Apple two-GPU behavior documented in WhateverGreen discussions).
3. **What does the dGPU actually draw while software-idle at VBIOS boot
   clocks?** Research §3.3 states the battery cost is "real" with no figure.
   Evidence needed: owner-run `powermetrics`/watt-meter reading on the Helios
   300 with the kext loaded and idle (runbook step 4).
4. **ACR chain viability on GP106M laptop silicon.** Phase 2 proved the sequence
   in simulation only (`HANDOFF-PASCAL.md` §6 Phase 2 gate labels it "not
   hardware proof"); newer Pascal laptops needed GP108 ACR bits (research §3.2,
   Phoronix link). Evidence needed: kext probe log from the real GP106M
   (runbook step 1).
5. **Semaphore-release visibility ordering across CE/GR to a host-mapped word
   on Pascal at boot clocks.** The design assumes the `nvRmSemSurf`-style
   mapped-word scheme (patch, `nvRmSemSurf.c`) works identically through the
   uapi; QMD `RELEASE0` fields are documented (`research/reference/mesa_clc1c0qmd.h`,
   cited in research §6) but not hardware-validated here. Evidence needed: NVK
   smoke test correctness under load (runbook step 3), cross-queue test akin to
   `tools/bench_event_wait.py`'s ("cross-queue test", `docs/EVENT-WAIT.md`).
6. **Multi-instance interference with the Intel stack.** NVRM/NVAccel on Turing+
   are single global owners and multi-GPU matching needed care
   (`docs/MULTI-GPU.md`); the Pascal kext is a separate bundle with its own
   personality, but coexistence with Apple's Intel accelerator in one system is
   untested. Evidence needed: runbook steps 1–4 ioreg state showing both
   accelerators alive and WindowServer PID stable.
