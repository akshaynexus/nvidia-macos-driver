# Offload switching design — HD 630 owns the panel, GP106 renders (and drives HDMI)

Phase 6 deliverable (`HANDOFF-PASCAL.md` §6, gate: "design doc + the complete owner
runbook"). Companion document: `research/evidence/OWNER-RUNBOOK.md`.

*Status 2026-11-07.* The userspace half of this design is now code. It was
built and tested on Linux only:
- NVK's Pascal backend over the bridge (`nvk/pascal`; review in
  `BRIDGE-NVK-REVIEW.md`);
- the plugin's Pascal branch (`plugin/nvmtl_vk.c`, `38beec8`);
- the runbook step 3–4 programs (`pascal/tests-mac`).

The dated *Implemented* notes below say what each design point became. The
last piece on the Metal side, the PG106Accel accelerator (§3.3,
`kexts/PG106Accel`), is now code as well. It is cross-built and audited on
Linux, but it has not been loaded on a Mac yet.

*Owner decision 2026-11-08: the GP106 accelerates the external HDMI
display.* PG106Accel becomes WindowServer's accelerator for the HDMI screen
(through PG106FB, the GP106's HDMI IOFramebuffer), the Intel HD 630 stays
WindowServer's GPU for the internal panel, and the GP106 keeps serving
offscreen Metal/Vulkan clients. This reverses three headless choices of
2026-11-07, for the HDMI framebuffer only. Each one is marked where it was
stated (§3.3), and the new mode is §3.4. The boot-arg
`pg106accel_display=0` restores the headless role.

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
   -------------+  composite NVRD-frames from IOSurfaces        +<-- to iGPU; HDMI only
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
- **Verified on the owner's machine (2026-11-07, `HELIOS300-HW-PROBE.md`).**
  - **NVIDIA side.** The dGPU's own VBIOS (86.06.4F.00.08) has a DCB with
    exactly one active output, TMDS (HDMI). There is no eDP/LVDS entry, so
    the dGPU has no path to the panel. The NVIDIA driver likewise exposes
    only `HDMI-A-1`. No miniDP is wired on this unit.
  - **Intel side.** The panel is `eDP-1` on the Intel device (00:02.0).
  - **ACPI.** `MXMX`/`MXDS` exist, but they are Intel switchable-graphics
    template code gated on `SGMD`/`SGGP`, and they cannot add a panel output
    the VBIOS doesn't have.
  - **How games reach the panel.** When the NVIDIA GPU "runs the laptop
    screen" (games, PRIME offload: `gnome-shell` holds a 1 MiB context on
    it), it renders, and the Intel GPU scans the frames out. This design is
    that path: Pascal renders into IOSurfaces and Intel composites them onto
    the panel. Full-screen games on the internal display therefore go
    through §2, not through a dGPU display head.
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

*Implemented 2026-11-07 (Linux-tested, not on hardware).* The host-page
backing exists end to end. `VK_EXT_external_memory_host` →
`nvkmd_pg106_import_host_mem` → `pg106_br_bo_import_host` (BO_IMPORT_HOST,
bridge ABI 0.2). The kext wires the pages for the BO's lifetime
(NOUVEAU-UAPI.md §6.1.4), and the backend binds them into the GPU VA space
like any BO. Two zero-copy levels use it:
1. **The plugin (Metal apps).** On Pascal it always takes its headless path.
   It renders into a private image, and after each GPU write the GPU's own
   copy writes the pixels into the IOSurface's imported pages
   (`nvmtl_vk_image_read_host`, `plugin/NVMTLObjects.m`
   `nvmtlSurfaceOutDirtied:`). That is one GPU copy, zero CPU copies, and no
   kext page-off. So the "Fallback" above needs no Pascal kext code at all.
2. **Direct rendering into the surface pages.** `pascal/tests-mac/offload_app`
   (`pg106_offload --vulkan`) does this: the compute shader writes the
   IOSurface rows. With `--copy` it does a CPU copy instead, for comparison.

The refusal path is the plugin's existing CPU copy (`nvmtl_vk_image_read`).
`test_nvkmd_pg106` `test_host_import` and `offload_selftest` cover the
mechanism on Linux.

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
  `third_party/mesa:src/nouveau/headers/nvidia/classes/clc1c0qmd.h`). That is exactly the
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

*Implemented 2026-11-07.* The pg106 backend does not use kernel syncobjs.
Every exec-context flush ends with a host-class control push: `WFI` (scope
all), `MEM_OP_C/D` SYS_MEMBAR, then a 4-byte `SEMAPHORE` release of the
context's next sequence number into a GART word. GPU waits across contexts
are `SEMAPHORE` ACQ_GEQ on that word (`clc06f.h:75-108`;
`nvk/pascal/src/nvkmd/pg106/nvkmd_pg106_ctx.c`). CPU waits poll it with
the EVENT-WAIT backoff, bounded by `NVK_PG106_TIMEOUT_MS` (default 10 s);
past the bound the context is lost. The plugin observes the Vulkan fence,
copies out, then calls `IOSurfaceUnlock`, which bumps the seed. On Pascal
the NVAccel completion mark (`NVRM_SS_PLANE_GPU_MARK`) is not available:
`nvmtl_nvk_surface_dirty` answers "not supported" on any backend other than
NVRM (`nvk-pascal.patch`). So the seed bump is the only readiness signal,
as this section planned. `BRIDGE-NVK-REVIEW.md` §2 explains why there are no
syncobjs.

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

*Implemented 2026-11-07* (backend behaviour, Linux-tested):

| Failure | What happens |
| --- | --- |
| Refused EXEC (`-ENODEV`, killed channel) | context lost → `VK_ERROR_DEVICE_LOST` on its waits and syncs |
| Fence wait past `NVK_PG106_TIMEOUT_MS` | same as a refused EXEC |
| Kext loaded but bring-up unfinished or failed (`READY` 0) | no Vulkan device, so the plugin offers no Pascal Metal device |
| User client refuses the process (`-EACCES`: only when `pg106_render_users=0` restricts RENDER to admins) | no device, logged |
| A second process while the single v0 channel is held (`-EBUSY`) | `vkCreateDevice` fails cleanly |
| Host import refused | the plugin's CPU copy |

A failing kext connection never fails `vkEnumeratePhysicalDevices` for
other GPUs.

## 3. Lifecycle and power

### 3.1 The dGPU never powers off

> **Superseded in part (2026-11-07, `HELIOS300-HW-PROBE.md`).** The machine's
> ACPI exposes power resource `PG00` (`_ON`/`_OFF`), referenced by
> `_PR0`/`_PR3` on PEG0. So a full power-off (D3cold) is an ACPI operation
> and needs no NVIDIA firmware. The kext can request it through the platform
> (IOACPIPlatformDevice / IOPCIDevice power states) when no render sessions
> are open. The cost is a full re-POST (`gm200_devinit_post` from the
> machine's VBIOS, now modelled) plus ACR/GR boot on the next session.
> Reclocking still needs the PMU, so that limit stands. The text below is
> the original analysis.

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

*Implemented 2026-11-07: the plugin side, then the accelerator.*

**Discovery.** `plugin/nvmtl_vk.c` treats a Metal device made on an
accelerator of class **`PG106Accel`** as the Pascal GP106. So does a process
that starts Vulkan before Metal, when it finds `PG106Accel` or the PG106
kext's registered `PG106` service. The plugin only *names* these IOKit
classes; it includes and links nothing from the GPL kext.

**What changes on Pascal:**
- the same NVK and `nvk_icd.json`, with `NVK_KMD=pg106`;
- always headless; **superseded 2026-11-08 for HDMI (§3.4):** the
  GP106's HDMI display is WindowServer's on the GP106, while apps'
  IOSurfaces stay in system memory as before;
- never WindowServer's device; **superseded 2026-11-08 (§3.4):**
  WindowServer gets the GP106 when PG106Accel drives the HDMI display, and
  only then;
- no NVDEC/NVENC (those go through NVRM).

**Placement.** The per-app list is the existing allow-list,
`/Library/GPUBundles/nvmtl-allow.txt`. A process on it gets the GP106 when it
asks; anything else gets no device from this plugin and stays on Intel. That
is the PRIME-offload model; `pg106_offload` is allowed by name. Per-app
profiles (`nvmtl_app_profile.h`) apply unchanged.

**The PG106Accel accelerator.** `Metal.framework` creates an `MTLDevice`
only for an `IOAccelerator` that carries `MetalPluginName` /
`MetalPluginClassName` (NVAccel: `kexts/NVRM/accel/Info.plist:37-38`). The
plugin's `-initWithAcceleratorPort:` hands that port to Apple's
`MTLIOAccelDevice`, which opens the accelerator's family user clients. A
plain IOService nub is not enough.

*Implemented 2026-11-07* (`kexts/PG106Accel`, PolyForm; cross-built and
audited on Linux, not yet loaded). Each requirement from the design, and
what it became:
1. *"An `IOGraphicsAccelerator2` subclass with `IOProviderClass = PG106`, no
   linkage to PG106."* **Corrected: the provider is the GP106's
   `IOPCIDevice`, not PG106.** `IOGraphicsAccelerator2::start` casts its
   provider to `IOPCIDevice` and fails otherwise ("Provider is NOT an
   IOPCIDevice!"; `safeMetaCast` in `IOGraphicsAccelerator2::start` of the
   macOS 26 binary
   `research/prior-art/meteor-lake-xe/research/IOAcceleratorFamily2-recovery-reference.macho`).
   The family's `configureDevice` and its display machine's `start` take
   that `IOPCIDevice` too (`kexts/NVRM/accel/re`, `iofam`).
   So PG106Accel matches the seven GP106 mobile ids that PG106 accepts, in
   `IOMatchCategory IOAccelerator`, beside PG106. It finds PG106 through
   I/O registry notifications on the `PG106` class, keeping the one whose
   provider is the same PCI device. It includes and links nothing from
   `pascal/`, and the cross-build fails if a `pascal/` file enters the
   build. PG106 still publishes no Metal properties.
2. *"The NVAccel personality's `MetalPluginName` / `MetalPluginClassName`."*
   Done, and gated. `MetalPluginName` is withheld until PG106 has published
   *and* set its registry property `PG106Ready = true` (kdev, `87630db`),
   and it is withdrawn if PG106 terminates. So while bring-up has not
   finished (today it stops at the ACR HS step), no Metal process sees the
   GP106. None of them loads the plugin for it, or creates family objects on
   an accelerator whose family paths are unproven (§6 Q10).
   - The kernel has no property-change notification, so PG106Accel polls
     `PG106Ready` every 2 s while it is false.
   - Readiness is latched. `PG106Ready` goes false again during D3cold, and
     PG106 boots the GPU when a render session opens, so withdrawing the
     name then would hide the GPU from the process that would wake it.
   - A PG106 without the property is taken as ready. If bring-up fails after
     readiness was seen, the plugin's NVK reads
     `PG106_BR_GETPARAM_READY = 0` and returns no device.
   - `PG106AccelState` in the registry gives the current state.
3. **Superseded 2026-11-08 (§3.4): the display machine now accepts exactly
   one framebuffer, the GP106's PG106FB, and the accelerator has a display
   pipe and flips.** What follows is the headless role, which
   `pg106accel_display=0` restores.
   *"No display machine, no framebuffer and no flips."* The family needs a
   display machine object (the factory is pure virtual), so there is one.
   It refuses every framebuffer in `found_framebuffer`, which is the virtual
   (vtable +0x8d8) through which `IOAccelDisplayMachine::start` adds the
   framebuffers it finds; the cross-build checks that offset. There is no
   display pipe and no flip code. With that and the plugin's own refusal of
   WindowServer, PG106Accel has no path to becoming WindowServer's
   accelerator.
4. *"No GPU VA for IOAccel memory maps."* The family still assigns every
   mapping a GPU address. PG106Accel hands out placeholders from a private
   range and never writes them into the GP106's MMU. It refuses the family's
   video memory, because only PG106 can allocate GP106 VRAM, so family
   resources are system memory only. Family event stamps are written by the
   CPU at once, since no family command stream reaches the GPU.
5. *"It loads from the AuxKC, not by OpenCore injection."* Yes: it links
   IOAcceleratorFamily2 (399 imports, all present in the macOS 26 binary).
   The load order is PG106 first, then PG106Accel (runbook step 4c).

**Render access** follows PG106's policy (§6 Q8). It is open to every local
user by default. With `pg106_render_users=0`, or `PG106RenderUsers = false`
in PG106's personality, PG106Accel refuses its family user clients to
anyone who is not an administrator, so those users do not see a Metal
device that could not render. `-pg106acceloff` keeps it from attaching.

No plugin change was needed: the plugin already keys on the class name
`PG106Accel`.

*Expected but unverified:*
- Metal's default device (`MTLCreateSystemDefaultDevice`) stays the HD 630,
  because the GP106 drives no display. Runbook step 4c checks this. With
  the HDMI display attached (§3.4) the expectation is still the HD 630,
  because the main display is the internal panel. Runbook 4e checks it.
- `MTLIOAccelDevice`'s initialisation accepts an accelerator that has no
  VRAM and only placeholder GPU addresses (§6 Q10).

### 3.4 Display-attached mode: the GP106 drives HDMI (2026-11-08)

*Owner decision 2026-11-08* (relayed by the coordinating session): the
GP106 accelerates the HDMI monitor. This is code, cross-built and audited
on Linux only. Nothing of it has run on a Mac: no frame has reached an
HDMI screen through it, and the PG106 side of its interface is not written
yet (see *Status*).

**Shape.**

```
 internal panel (eDP, Intel)          HDMI monitor (GP106 SOR)
          ^                                    ^
 Intel accelerator (Apple)          PG106FB (GPL IOFramebuffer, disp2)
          ^                                    ^  base-channel flip at vblank
 WindowServer composites             PG106Accel display pipe (PolyForm)
 the panel on the HD 630                       ^  copy engine: IOSurface -> VRAM ring
                                     WindowServer composites HDMI on the
                                     GP106 Metal device (plugin -> NVK -> PG106)
```

- **Pairing.** PG106Accel watches `PG106FB` services. It takes one only when
  all of these hold (`kexts/PG106Accel/pga_logic.h` `pga_fb_decide`,
  host-tested):
  - its provider chain reaches PG106Accel's own `IOPCIDevice`;
  - it publishes a compatible `PG106FBKifABI`;
  - PG106 has been ready once (`PG106Ready`);
  - no other PG106FB already has the pipe;
  - a frame path exists.

  It then hands the framebuffer to the family
  (`IOAccelDisplayMachine::found_framebuffer`), which creates the display
  pipe and sets up the accelerator/framebuffer pairing. The family's own
  sweep reaches the same filter (vtable +0x8d8, cross-build check 5).
  Every other framebuffer is refused, the Intel panel's included: it is
  not a PG106FB, and it is on another PCI device.
- **WindowServer.** The plugin gives WindowServer a device on the GP106
  only while PG106Accel publishes `PG106AccelDisplay = true`, and refuses
  it otherwise (`plugin/nvmtl_ws_gpu.h`). That makes the decision per
  accelerator, which on this machine means per display.
  - To make sure WindowServer's first look already sees the display,
    `MetalPluginName` is held after `PG106Ready` until PG106FB has been
    offered the pipe, for 10 s at most. PG106FB publishes about 100 ms after
    `PG106Ready`, whether or not a monitor is connected (disp2).
  - A pipe that appears later (PG106FB late, or a frame path that only
    appears later, such as BAR1) is not seen by a WindowServer that already
    looked. A logout fixes that; runbook 4e covers it.
- **A frame.**
  1. WindowServer composites into an IOSurface on the GP106 Metal device.
     The plugin's Pascal path renders with NVK and copies the result into
     the IOSurface's system memory before the command buffer completes
     (`NVMTLEventSync.m`, `nvmtlSurfaceOut`).
  2. WindowServer waits for that completion before it swaps
     (`commitAndWaitUntilSubmitted` → `waitUntilCompleted`). The reason is
     that NVK's pg106 backend answers the kernel surface mark with
     `0x80000002`, so `nvmtl_ws_kernel_waits()` is false.
  3. The swap arrives as a display-pipe transaction. PG106Accel's worker,
     on its own work loop, takes it from there:
     - maps the IOSurface into PG106's accelerator VM (cached for 8
       surfaces);
     - copies it with PG106's copy engine (`c1b5`) into the free buffer of
       a three-buffer VRAM ring (on screen / waiting for vblank / being
       filled; `pga_ring`);
     - waits for the copy's fence;
     - asks PG106FB to flip the base channel to that buffer.
  4. PG106FB's `FLIP_DONE` (base notifier left `NOT_BEGUN`, at vblank)
     completes the transaction (`signalTransactionInterrupt`). One frame is
     in flight at a time. A present that arrives while one is in flight
     waits up to 1.25 frame periods, then replaces the frame queued behind
     it. This is NVAccel's async swap shape.
- **Fallbacks** (`pga_choose_path`). Each one is logged and shown in
  `PG106AccelFramePath`:
  - without PG106's copy channel, the CPU copies into the same ring through
    BAR1;
  - without VRAM through the kif, the CPU copies into PG106FB's own scanout.
    PG106 always has VRAM (PG106FB cannot exist without it), but
    `PG106KifABI` or `CAP_VRAM` may not be published yet. PG106FB gives its
    BAR1 address (INFO `bar1_phys`, only while BAR1 really maps the
    scanout, never in its 10 Hz shadow-copy mode, which would overwrite
    the copy). PG106Accel maps it write-combined, and writes there only
    after a HOME flip has put PG106FB's own surface back on screen (agreed
    with disp2);
  - with neither, PG106Accel does not pair (`PGA_FB_NO_FRAME_PATH`), and
    HDMI stays an unaccelerated PG106FB display.
- **VRAM, GPU VAs, stamps** (Q10). These come from PG106 as far as its
  INFO cap bits allow:
  - family video memory is PG106 VRAM, mapped through BAR1;
  - `tmpTotalVRAM`, `totalTextureMemory` and `VRAM,totalMB` report the
    GP106's VRAM;
  - family GPU VAs come from PG106's accelerator VM. VRAM maps, and system
    memory maps whose pages are all wired, are written into its page
    tables;
  - family event stamps are 4-byte semaphore releases on the copy channel.
    The CPU writes them instead when there is no channel, when
    `pg106accel_gpustamps=0` is set, or when writeStamp runs where it may
    not block. `eventTimeout` completes a release that never landed.

  Two parts stay stubs: the family's contexts and legacy surfaces. Their
  pure slots are not reconstructed well enough to submit through, and
  Metal on this GPU does not render through them. The ordering this gives
  is therefore "after this channel's copies", not "after NVK's work". The
  plugin provides the second by completing command buffers only after
  NVK's fence.
- **Cursor and vblank** stay PG106FB's: the hardware cursor on the cursor
  channel, and `kIOFBVBLInterruptType`.

**The interface** (`pascal/bridge/include/pg106_kif.h`, SPDX MIT, ABI 1.0,
agreed 2026-11-08 by PG106Accel/accel2, PG106/kdev3 and PG106FB/disp2).
It is IOKit only: `callPlatformFunction` on the other side's registered
service, versioned request structs (`{abi, size}` prefix, major/minor
rule), and the registry numbers `PG106KifABI` and `PG106FBKifABI`.
- PG106 answers:
  - INFO (with cap bits);
  - VRAM_ALLOC / FREE (CONTIG, SCANOUT, CPU, GPU_VA);
  - VA_ALLOC / FREE and MAP / UNMAP in the accelerator VM;
  - CHAN_OPEN / CLOSE of a copy channel, COPY, RELEASE and FENCE.

  It calls back `EVENT` (PROGRESS / LOST) on PG106Accel.
- PG106FB answers INFO (mode, scanout, generation) and FLIP (with HOME and
  ASYNC flags). It calls back `FLIP_DONE` exactly once per accepted flip,
  `kIOReturnAborted` when the head or GPU goes off.
- No code crosses the GPL/PolyForm line. PG106Accel includes only this MIT
  header from `pascal/`, and cross-build check 4 enforces that.

**Status, 2026-11-08.**
- PG106Accel's side is cross-built and audited (825 imports, all exported by
  macOS 26). The host tests pass (83 checks).
- PG106FB is disp2's (`pascal/fb`), and its interface terms are agreed.
- PG106's provider (`PG106_kif.cpp`) is kdev3's, and is scheduled after the
  ACR VMM, FIFO channels and non-stall interrupts. Until it lands, PG106
  publishes no `PG106KifABI`. PG106Accel then has no VRAM, copy engine or
  GPU VAs, and its only frame path is the CPU copy into PG106FB's scanout,
  which needs PG106FB to scan out through BAR1 (not its shadow-copy
  fallback).
- On the first owner runs (bring-up stops before READY), PG106Accel stays at
  `waiting for PG106Ready`, as in §3.3.

**Rollback** (runbook 4e):
- `pg106accel_display=0`: headless PG106Accel; HDMI stays an unaccelerated
  PG106FB display.
- `pg106fb_accel=0`: PG106FB does not offer itself.
- `pg106fb=0`: no HDMI framebuffer.
- `-pg106acceloff`: no accelerator.
- To isolate a fault: `pg106accel_ce=0`, `pg106accel_gpustamps=0`,
  `pg106accel_vram=0`.

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

*Implemented 2026-11-07.* Steps 3–4 are concrete.
- **Step 3 (NVK).** `nvk/pascal/nvk-pascal.patch` (on Mesa `17ca6174` +
  `nvk-macos.patch`) adds `src/nouveau/vulkan/nvkmd/pg106/` beside
  `nvkmd/nvrm/`. Selection is `nvkmd_enum_pdev`: NVRM, then pg106. One
  `libvulkan_nouveau.dylib` serves both Turing+ and Pascal.
- **Step 4 (bridge).** That backend links `pascal/bridge` (ABI 0.2, MIT) and
  its IOKit transport (render user client, type 1).
- **Step 6 (out).** On Pascal the headless switch is on unconditionally
  (`nvmtl_gpu_headless`), whatever `debug.nvaccel_headless` says.

Every nvkmd op and every `nvk-macos.patch` interface point is reviewed in
`BRIDGE-NVK-REVIEW.md`.

Licensing chain for this pipeline, since it constrains integration: Pascal kext
GPL-2.0-only; NVK MIT; translator LGPL-3.0; `kexts/`/`plugin/` PolyForm-NC —
mutually incompatible, so the Pascal kext consumes the plugin/translator at
arm's length and never merges source (`HELIOS300-PASCAL-RESEARCH.md` §5
licensing caution; `HANDOFF-PASCAL.md` §3).

## 5. Testability

The owner-facing sequence, measurements and rollbacks live in
`research/evidence/OWNER-RUNBOOK.md`. Order: kext probe → Userspace Client
probe → NVK sandbox Vulkan smoke → render-into-IOSurface with WindowServer
unchanged — each step with explicit rollback. [Corrected 2026-11-07: the
original text said "no persistent system state (no AuxKC rebuild, no
NVRAM/OpenCore changes)". That is not true on macOS 11+: `kmutil load`
stages the kext and approval rebuilds the AuxKC, and the recommended path
is an OpenCore `Kernel->Add` entry. Each has its own rollback in
`OWNER-RUNBOOK.md` Step 1.]

*2026-11-07:* runbook steps 3–4 are written, with programs. Step 3:
`build-nvk-macos.sh` builds NVK into `~/pascal-test`, with nothing installed,
then `pg106_vk_smoke`. Step 4: `offload_selftest` and the `pg106_offload`
window, plus a Metal variant (4c) once PG106Accel is installed. Step 4d measures power in
the states A/B/B′/C and records the dGPU power state (the D3cold note, §3.1).
The RENDER user client is open to every local user (owner decision,
`8374730`), so the programs run without `sudo`.

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
   uapi; QMD `RELEASE0` fields are documented (`third_party/mesa:src/nouveau/headers/nvidia/classes/clc1c0qmd.h`,
   cited in research §6) but not hardware-validated here. Evidence needed: NVK
   smoke test correctness under load (runbook step 3), cross-queue test akin to
   `tools/bench_event_wait.py`'s ("cross-queue test", `docs/EVENT-WAIT.md`).
6. **Multi-instance interference with the Intel stack.** NVRM/NVAccel on Turing+
   are single global owners and multi-GPU matching needed care
   (`docs/MULTI-GPU.md`); the Pascal kext is a separate bundle with its own
   personality, but coexistence with Apple's Intel accelerator in one system is
   untested. Evidence needed: runbook steps 1–4 ioreg state showing both
   accelerators alive and WindowServer PID stable.

*Status of these questions, 2026-11-07:*
- Q1: the mechanism exists (host import, §2.1 note). The measurement is
  runbook step 4a, the `zero-copy` vs `copy` line, plus 4b timings.
- Q2: unchanged. Runbook 4b is the pixel-stability check; the seed bump is
  counted per frame by `pg106_offload`.
- Q3: runbook 4d. With the D3cold note, it is now three or four states
  (A/B/B′/C).
- Q5: the release is WFI + SYS_MEMBAR + 4-byte SEMAPHORE release. On Linux
  only the encoding and its replay through a loopback are tested. Runbook
  step 3 `repeat` (200 dispatches with readback) is the first hardware
  evidence.

New questions:

7. **Which MTLDevice is the default with PG106Accel loaded?** The
   expectation is the HD 630 (§3.3), with the HDMI display attached as well
   (§3.4; runbook 4e records it). Evidence needed: runbook 4c
   `--list-metal`, and `MTLCreateSystemDefaultDevice` from any
   non-allow-listed app.
8. **Who may open the RENDER user client — decided 2026-11-07.** The owner
   chose every local user (`8374730`); `pg106_render_users=0` restricts it to
   admins (BRIDGE-NVK-REVIEW.md §7.1). That is what the offload model needs:
   ordinary apps reach the GP106 without root.
9. **One render client at a time** (the v0 kext has one channel). A second
   process's `vkCreateDevice` fails. Real CHANNEL_ALLOC is needed before two
   offloaded apps can run at once.
10. *2026-11-08:* the device now has VRAM, VAs and stamps as far as PG106's
    INFO caps allow (§3.4). Until PG106's kif provider lands, the question
    stands exactly as below, and runbook 4c answers it. After that, 4e's
    `PG106AccelStats` show which family paths ran on real VRAM.
    **Does `MTLIOAccelDevice` accept PG106Accel's cut-down family objects?**
    PG106Accel has no family VRAM (`allocPhysical` fails) and only
    placeholder GPU addresses. Its event stamps complete at once, and its
    contexts and surfaces are stubs (§3.3). NVAccel's equivalents are backed
    by RM, so nothing in this repo has yet shown that Apple's
    `MTLIOAccelDevice` init, and the plugin's three hardware resource pools,
    work with this. Evidence needed:
    - runbook 4c's plugin log (`MTLIOAccelDevice's initWithAcceleratorPort:
      -> …`, `hw resource pools: installed 3`);
    - the `PG106Accel:` kernel log lines (`video memory refused`, `not
      supported on the GP106`), which show which family paths Metal
      actually touches.

New questions (2026-11-08, display-attached mode §3.4):

11. **Does WindowServer composite the HDMI display on the GP106's Metal
    device**, given a PG106Accel display pipe and a plugin that gives it
    the device? Evidence: runbook 4e. Look for the plugin's `WindowServer:
    Pascal GP106 drives the HDMI display` line, a desktop on HDMI, and
    `PG106AccelStats.flips` rising at the refresh rate.
12. **Is the per-frame cost acceptable?** The cost is NVK's copy-out to
    system memory, then the copy engine back into VRAM, with WindowServer
    waiting for completion each frame. Evidence: 4e's `dropped_*` and
    `flip_busy` counters, and how smooth the desktop looks at 60 Hz. A
    zero-copy path would need NVK to render into VRAM that PG106Accel can
    scan out (shared VRAM BOs between the render client and the kif), and
    that does not exist.
13. **Does `found_framebuffer` from PG106Accel's own pairing create a
    working pipe on macOS 26?** NVAccel's offsets (pipe at display machine
    +0x88, framebuffer at pipe +0x98) are checked, and the pairing logs
    `pipe … does not drive it` if they are wrong. Evidence: 4e log.
