# Prior-art audit — pascal-egpu and the Meteor Lake Xe-LPG port

Date: 2026-11-05. Method: both repos cloned to `/tmp/prior-art/`
(`pascal-egpu`, `meteor-lake`; shallow clones, not committed — per AGENTS.md and
the branch's file scope). Citations of the form `pascal-egpu/FINDINGS.md:L` are
line numbers in those clones at fetch time; our repo is cited as repo-relative
paths. This document is an audit, not a plan: it states what the prior art
established, what our port already handles, and what it does not. Facts without
a source are stated as unknowns, per `HANDOFF-PASCAL.md` §7.

---

## 1. TheTom/pascal-egpu — what it actually established

**What it is.** A userspace Python driver for a desktop GTX 1060 (GP106,
`10de:1c03`) in a Razer Core X over Thunderbolt on an Apple Silicon Mac Studio,
using tinygrad's TinyGPU DriverKit extension for BAR access. The repo is much
more than its RESEARCH.md/PLAN.md: `hw/` holds ~30 real-hardware probe scripts
(devinit interpreter, ACR descriptors, falcon loaders, priv-ring probes) and
`FINDINGS.md` is the honest record of a **failed** bring-up that still yielded
the most valuable hardware facts about GP106 outside nouveau.

**Verified hardware facts (their measurements, not ours):**

- `PMC_BOOT_0 = 0x136000a1` on real GP106 silicon — exactly the constant our
  Phase 0 tests encode (`pascal/tests/test_pmc.c`, GP106 vector; provenance in
  `pascal/README.md` "Key facts"). Their read: `pascal-egpu/README.md:19`,
  `pascal-egpu/FINDINGS.md:15`.
- The "What doesn't work" symptom→return-value→cause table
  (`pascal-egpu/FINDINGS.md:29-44`) is a decoding key we do not have:
  `0xbad00200` = PMC scratch clock-gated, `0xbad0acXX` = FBP priv-ring stations
  unregistered, `0xdead5ec1/2` = Falcon IMEM/DMEM priv-rejected, `0xffffffff`
  = SEC2 powered off / GPCCS station dead. **Our kext logs should classify
  failures with this table** (see §D, P0-2).
- The circular dependency that killed their project
  (`pascal-egpu/FINDINGS.md:89-107`): PMU DEVINIT firmware loads from VRAM
  (DMA), VRAM needs FBP priv-ring stations registered, FBP stations get
  registered by PMU DEVINIT — the only known cut-through is the VBIOS Option
  ROM running at system POST (`FINDINGS.md:78-87`). **This does not block us:
  the Helios 300's system firmware runs the dGPU VBIOS at POST** (Optimus
  laptop; research doc §2), so devinit has already executed by the time our
  kext attaches. Their failure mode is structurally an eGPU/Apple-Silicon one
  (`FINDINGS.md:137`: "Apple Silicon EFI does not run GPU Option ROMs").
- Falcon security latch: `UCODE_LEVEL=3` (SCTL `0x3000/0x3002`) is enforced by
  hardware and **cannot be cleared from the host** — SCTL writes silently
  dropped, PMU reset-race writes rejected, PLMs mostly unwritable, DBGCTL
  locked (`pascal-egpu/FINDINGS.md:169-180`). Unsigned falcon execution is a
  false positive: CPUCTL→HALTED after STARTCPU with TRACEPC stuck at 0x101 is
  a **security exception, not a halt** (`FINDINGS.md:164-167`). Confirms our
  design choice that only the signed ACR path can run FECS/GPCCS
  (`HELIOS300-PASCAL-RESEARCH.md` §3.2); there is no shortcut and it is not
  worth looking for one.
- PMC_ENABLE on GP106: max writable `0x5c6cf1e1`; **bit 4 (PMU) is silently
  dropped** — the PMU engine cannot be host-enabled (`FINDINGS.md:153`).
  Consistent with our accepted hard limit (no PMU firmware, no reclocking,
  `HELIOS300-PASCAL-RESEARCH.md` §3.3).
- VBIOS facts from their real dump (`FINDINGS.md:23-25,112-113`): BIT header at
  0x1e2, init-script table ptr at 0x497e, script 0 = 6 INIT_SUB_DIRECT calls;
  PMU DEVINIT tables/boot-scripts at BIT `I` +0x14/0x16 and +0x18/0x1a. Our
  `pascal/core/src/vbios_bit.c` should gain those BIT-`I` offsets when wiring
  devinit table discovery (§D, P2).
- Falcon instance-block binding correction: PLAN.md:245 claims `0x480`
  (`FALCON_IMEMC`) binds the instance block; the later hardware-derived
  `NOUVEAU-REFERENCE.md:8-11` corrects it: **`0x054` = instance address +
  location, `0x048` bit 0 = context-binding enable, `0x10c` = DMACTL**. Our
  `pascal/core/src/acr_boot.c:459` already uses `0x054` — we independently
  picked the right register; record the cross-check.
- ACR blob set cross-validation: their `firmware/blobs/gp106/acr/` —
  `bl.bin`, `ucode_load.bin`, `ucode_unload.bin`, `unload_bl.bin`
  (directory listing; "all 23 NVIDIA-signed firmware blobs, extracted from
  Arch linux-firmware-nvidia package", `FINDINGS.md:226`) — are **byte-for-byte
  identical (md5s compared 2026-11-05) to the GP102 ACR blobs we vendored** as
  the substitution documented in `research/firmware/gp106/README.md` and
  `research/evidence/BLOCKERS.md` B3. Two independent projects attempting GP106
  ACR converge on the same four binaries. Our B3 "no `nvidia/gp106/acr/` in
  linux-firmware" statement stands (their provenance note says "Arch package",
  but the files equal the gp102 set, so it is the same ucode either way).
- PMU situation on Pascal, confirmed twice: nouveau runs PMU with
  `gm200_pmu_nofw` — basic PMU engine control needs no proprietary firmware,
  **reclocking does** (`pascal-egpu/RESEARCH.md:149-157`). Matches our §3.3
  hard limits verbatim.
- MMU format source: they used `open-gpu-doc pascal/gp100-mmu-format.pdf` +
  tinygrad's NVDev v2 encoder (`RESEARCH.md:395-428`) — the same two sources
  our `pascal/core/src/mmu.c` cites. No new format facts.
- macOS-side PCIe mapping lessons (eGPU-specific, low direct relevance to an
  internal laptop dGPU but recorded for completeness): IOKit `IOPCIDevice`
  enumeration + TinyGPU DEXT MAP_BAR/CFG_READ RPC works for Pascal; the
  Ampere+ restriction is in tinygrad userspace only
  (`RESEARCH.md:329-391`, `README.md:11-14`). Options A/B/C for BAR access
  (DEXT via socket / custom DEXT / IOKit kext with SIP off) at
  `PLAN.md:47-80`. **Nothing in their work touches ACPI/_DSM, Optimus power
  states, or laptop-side PCIe/BAR quirks** — searched; absent. That part of
  our environment remains uncovered by this prior art (unknown, not
  contradicted).
- Their dead ends worth not repeating: FLR unsupported by GTX 1060
  (`FINDINGS.md:38`), D3/D0 cycling has no effect on locked registers
  (`FINDINGS.md:133-134`), signature injection via SIG_DATA registers is a
  no-op because ACR does the verification (`FINDINGS.md:127-128`), priv-ring
  raw RING_CMD writes made things progressively worse until lockup
  (`FINDINGS.md:42-43,120-122`).

**Sources pascal-egpu cites that we can check** (all already in our research
doc's citation set or resolvable): nouveau `nvkm/engine/device/base.c`,
`subdev/secboot/gm200.c` + `acr_r352.c` (old layout; ours is the `subdev/acr`
rename per `HANDOFF-PASCAL.md` §5), `engine/fifo/gk104.c`, `engine/gr/gf100.c`;
tinygrad `runtime/ops_nv.py`, `runtime/support/system.py`,
`runtime/support/nv/nvdev.py`; open-gpu-kernel-modules `clc0c0.h/clc1c0.h/
clc0c0qmd.h/clc1c0qmd.h/clc06f.h`; open-gpu-doc `pascal/gp100-mmu-format.pdf`,
`dev_pbdma.ref.txt`, `dev_ram.ref.txt`, devinit.xml; envytools mmio/bars/pbus;
nova falcon/fwsec kernel docs; Karol Herbst's low-level guide
(`RESEARCH.md:873-903`, `PLAN.md:873-904`, `FINDINGS.md:231-240`). No citation
they use is missing from our reference set — one addition worth keeping:
`FINDINGS.md:236` NVIDIA BIOS-Information-Table spec URL (our BIT parser cites
envytools; the official spec is a better authority).

**One correction they got wrong in PLAN.md and never fixed:** compute class
`0xB1C0` (Maxwell COMPUTE_B) and channel class `0xB06F` (`PLAN.md:401,521`)
are **not** GP106's classes; their own RESEARCH.md later states `PASCAL_COMPUTE_B
= 0xC1C0` (`RESEARCH.md:87-91`) and `PASCAL_CHANNEL_GPFIFO_A = 0xC06F`
(`RESEARCH.md:169`). Our headers (`HANDOFF-PASCAL.md` §5: 0xC1C0/0xC06F from
`clc1c0.h`/`clc06f.h`) match the corrected values. Cite RESEARCH.md, not
PLAN.md, if either is ever quoted.

## 2. VedCloud/Meteor-Lake-Xe-LPG-Metal-Graphics — the working proof

**What it is.** A retargeting of the NullMoth NVIDIA-for-macOS skeleton: i915
compiled onto xnu via a LinuxKPI/`xkpi` compatibility layer as `NMXGPU.kext`
(438 kernel sources), Mesa ANV as the Vulkan backend, the AIR→SPIR-V translator
and Metal plugin reused. Result on real hardware (Dell Latitude 5350, Meteor
Lake `8086:7D45`): Metal + Vulkan with exact-pixel validation (22 cases / 26,000
frames / 80,020 pools, `meteor-lake/README.md:102-105`) and a GPU-composited
WindowServer desktop (`README.md:118-119`,
`research/windowserver-result-224146.md`). **This is the existence proof for
our architecture: a Linux KMD ported to xnu + Mesa + a NullMoth plugin, with
GPL-2.0 kernel code living in its own tree** (`README.md:724-730` documents
exactly the GPL/PolyForm-NC incompatibility our `pascal/kext/README.md`
enforces).

**Their trap list** (each with the documented mechanism):

1. **Stolen memory / live-scanout overlap** (kernel 1.0.13→1.0.15): i915's
   stolen-memory region overlapped the firmware panel buffer; a broad
   stolen-region skip **broke GPU startup**, and the working fix was a precise
   reservation — read the firmware console address/pitch/dimensions via an
   IOKit bridge and start the stolen allocator *after* that image
   (`meteor-lake/README.md:765`, ROADMAP Phase 9→11
   `meteor-lake/ROADMAP.md:431-502`).
2. **GuC lost-wake race / `TASK_WAKING`** (kernel 0.3.0→1.0.8): the FreeBSD
   LinuxKPI woken callback signalled the channel but did not persist
   `TASK_WAKING` under the sleep-channel lock, so a reply arriving between the
   outer flag check and sleep registration signalled an empty channel and the
   waiter slept to expiry (`meteor-lake/README.md:425-429`, ROADMAP Phase 4
   `meteor-lake/ROADMAP.md:259-283`). Fix verified by a 100,000-scenario
   deterministic fixture using the real old/patched callbacks and i915's
   `must_wait_woken`.
3. **IOKit mapping pinning** (kernel 1.0.10→1.0.11): the port's
   `lkpi_unmap_mapping_range()` revocation shim is deliberately empty, so a
   static user mapping is only safe if the backing is pinned for the mapping
   lifetime; `drm_gem_ttm_mmap()` had the pin, the i915 `vm_ops_cpu` path did
   not — an idle mapped object's pages could be reclaimed under a live user
   mapping (`meteor-lake/ROADMAP.md:381-393`; decision row
   `meteor-lake/README.md:763`). Real port bug; kept as a correctness fix even
   after the capture proved it was not the hang's writer.
4. **`kIOMemoryPhysicallyContiguous` does not mean host-contiguous** (1.0.12):
   per Apple's `initWithPhysicalMask` logic, host contiguity needs
   `kIOMemoryHostPhysicallyContiguous | kIOMemoryMapperNone` (or no system
   mapper); the old allocator exported the first physical segment's base while
   ignoring its length, so an `IODeviceMemory` range could extend into memory
   the allocation does not own (`meteor-lake/ROADMAP.md:394-429`,
   `meteor-lake/README.md:764`).
5. **Context use-after-free in the completion path** (1.0.13→1.0.14): a boot
   panic in `guc_retire_inflight_request_prio` was a context freed by timeline
   retirement still reached by breadcrumb completion; fixed by holding context
   refs through retirement and acquiring ownership before publishing
   `SIGNALED` (`meteor-lake/ROADMAP.md:431-483`).
6. **Command-pool reuse was a variable in the hang**: fresh pools, destroyed
   immediately after fence completion; uncertain completions **quarantined**,
   never reused (`meteor-lake/README.md:757-758`, accounting invariant
   `README.md:102-105`).
7. **Injection/boot traps** (Phase 0/0b): kext would not inject (`_kmod_info`),
   then macOS froze at `EXITBS:START` until the OpenCore boot sequence was
   fixed (`meteor-lake/ROADMAP.md:145-201`); one-shot NVRAM opt-in for GPU
   activation so "a failed attach cannot silently re-arm on every boot"
   (`meteor-lake/README.md:421-422`, `README.md:756`).
8. **Diagnostic signatures** (`meteor-lake/README.md:526-532`): `GPU HANG
   12:1:00dfffff` (do not raise the fence timeout — capture state first),
   `VkResult -1000072003` (IOSurface import refused), `GPUWranglerWaitForPublish`
   (kext never published), `mutex ... used before mtx_init` (a `struct mtx`
   that never went through `mtx_init`).
9. **Evidence discipline** (`meteor-lake/ROADMAP.md:130-142`, principles
   `meteor-lake/README.md:735-747`): measurements for every "worked";
   negative results stay negative; counts exact and scoped; local/mock passes
   never reported as hardware passes; one variable at a time with before-state
   and rollback; **collect diagnostics before rebooting/reinstalling** (the
   failed boot's unified log is the evidence); never bypass a failed
   validation; submit IDs never equated to request seqnos without correlation.

**Kext structure / how they shimmed Linux APIs onto xnu:** `NMXGPU` is i915 +
DRM/TTM + LinuxKPI + IOKit glue, "restricted to 8086:7D45 … with AMD and Xe
drivers deliberately excluded and no unresolved driver-internal C symbols"
(`meteor-lake/README.md:418-421`). The shim is **not** a header trick: it is a
build system (`NullMoth/gpu-port/xkpi/`: per-file compiler `cc-i915.sh`,
`all-i915.sh`, `lkobj/`, `xnuobj/`, `override/`, `bsdcompat/`) plus an
`xnu/` glue layer of hand-written xnu-side objects (`nmx_pci.cpp`, `nmx_mm.cpp`,
`nmx_vm.cpp`, `nmx_dma.cpp`, `nmx_fb.cpp`, `nmx_kext.cpp`,
`NullMoth/gpu-port/xkpi/xnu/`), i.e. Linux subsystem APIs re-implemented on
IONotificationCenter/workloops/IOCommandGate by the glue files. MacKernelSDK
provides kernel headers (`meteor-lake/README.md:406`, `MacKernelSDK/` in-tree);
link step is a separate Python script (`desktop-sources/link-kernel-1.0.15.py`,
438 objects, frozen undefined-symbol set — `README.md:610-616`).

**Our shim, compared.** `pascal/kext/src/pg106_klibc_shim.h` maps only
`calloc/realloc/free` onto `kern_os_*` with force-include macros, plus empty
`src/klibc/` headers — appropriate for our scope (a ~10-file portable C core
with no Linux kernel dependencies), versus their 294+ C/H files of LinuxKPI
surface for a 438-source i915. **The invariant worth stealing is their frozen
undefined-symbol set**: they link the same import set every time and treat any
new undefined symbol as a regression (README.md:615-616, ROADMAP Phase 10
"keeps the frozen 1.0.8 undefined-symbol set"). Our equivalent,
`pascal/kext/scripts/symbol-audit.sh`, audits for macOS-only symbols leaking
into the portable core but does not yet snapshot the *kext* link surface
(§D, P2).

**Build system for MacKernelSDK:** ours documents the same osy/MacKernelSDK
clone-adjacent usage (`pascal/kext/README.md:46-50`,
`PG106.xcodeproj` `KERNEL_FRAMEWORK_HEADERS`), matching theirs. Their lesson
that our docs should adopt: **the kernel build was the least reproducible step
in their tree** (absolute paths in `all.sh`, `README.md:638-641`) — our
owner-run `scripts/build-macos.sh` must not bake in absolute paths (it
currently uses `$(SRCROOT)`-relative paths, so we are clean).

## 3. Other GP106/Pascal-on-macOS attempts (searched 2026-11-05)

- **NVIDIA's own Web Drivers (2017)**: beta 378.05.05.05f01 and successors
  supported Pascal on macOS up to High Sierra only, proprietary, build-locked
  (Ars Technica, https://arstechnica.com/gadgets/2017/04/nvidia-releases-beta-gpu-drivers-for-old-mac-pros-and-hackintoshes;
  Low End Mac compatibility table,
  https://lowendmac.com/2024/thunderbolt-3-external-graphics-cards-egpu).
  Dead end past 10.13; no source. Confirms the field is empty since then.
- **OpenCore Legacy Patcher**: ships a *non-Metal* patch set for Pascal
  (no acceleration — panels/input only) and documents Pascal as unsupported
  for Metal on any modern macOS
  (https://github.com/albert-mueller/OpenCore-Legacy-Patcher-T2, patch-set list).
  Not a driver effort; useful only as community-consensus evidence that nobody
  else has done what we are attempting.
- **dartraiden/NVIDIA-patcher + P106M** (Windows only): INF-patching for GP106
  derivatives on Windows; laptop GP106M variants (1C36/1C75) show black screens
  and driver-43 failures there
  (https://github.com/dartraiden/NVIDIA-patcher/issues/207). Notable datapoint:
  GP106M board support is fragile even on Windows; some laptop IDs have broken
  or power-capped VBIOS-level configs. Argues for our B1 (real Helios VBIOS)
  blocker being resolved with the owner's own dump.
- **nullmoth ecosystem forks** (WhySooooFurious, macrwfuse, RussPalms, …): all
  Turing+ only, all derivative of the upstream we already vendor; none attempts
  Pascal. No gitee mirrors or follow-ups of pascal-egpu were found with
  substance (searches "GP106 macOS kext", "Pascal Optimus hackintosh open
  source", "pascal-egpu follow-ups", gitee queries — nothing beyond the repos
  above).

**Conclusion:** the two audited repos are the entire relevant prior art. No
third implementation of Pascal-on-macOS kernel driver work exists in public.

---

## A. Bring-up order comparison

| Step | Ours (`pascal/core` module order; ROADMAP Phase 0-6) | pascal-egpu PLAN.md (`PLAN.md:860-869`) | VedCloud actual (ROADMAP timeline) |
|---|---|---|---|
| PCIe/BAR attach | Phase 3 kext: personality → probe gate → BAR0 map | Phase 1 (PCIe enum, bus mastering, BAR0) | Phase 1: PCI probe, kext attach (8086:7D45) |
| Chip identity | `pmc.c` PMC_BOOT_0 decode | `PMC_BOOT_0` read (README milestone) | PCI ID match in i915 (`README.md:418-421`) |
| Devinit/VBIOS | `vbios_bit.c` → `devinit.c` (interpreter before ACR) | implicit in Phase 2 "FWSEC VBIOS extraction + WPR setup"; discovered PMU-DEVINIT tables in VBIOS (`FINDINGS.md:113`) | n/a (Intel: GuC/HuC from `linux-firmware`, no VBIOS devinit) |
| Secure boot | `acr.c` blob parser → `acr_boot.c` (ACR after devinit) | ACR boot + FECS/GPCCS load (Phase 2-3) | GuC firmware load early in attach (`README.md:423-424`) |
| MMU | `mmu.c` after ACR (matches nouveau: ACR needs instance-block VM) | MMU in Phase 3, **after** ACR — same relative order | GGTT early, before firmware (i915 does MMU/GGTT setup as part of attach) |
| Submission | `pushbuf.c`/`dispatch.c`/`qmd.c` after MMU | FIFO Phase 4 → compute Phase 5 | first submissions after GuC enable (Phase 3) |
| Display | `edid.c`/`disp_mode.c` last (Phase 5) | never (compute-only; PDISPLAY "not needed", `PLAN.md:153`) | framebuffer/compositor last (Phases 6-11) |

**Agreements.** All three agree on the invariants: identify the chip before
anything else; establish address translation before submission; load signed
firmware before the engines it gates; do display last. Our order
(PMC→devinit→ACR→MMU→dispatch→disp) is nouveau's order, and pascal-egpu's
planned order is the same shape.

**Divergences.** (1) pascal-egpu's PLAN puts "FWSEC from VBIOS → WPR" inside
falcon bring-up *before* ACR blob loading (`PLAN.md:180-197`); ours treats
WPR/LSB setup as part of the ACR chain itself (`acr_boot.c`, Phase 2 trace
`research/evidence/PHASE2-ACR-TRACE.txt`). Nouveau's actual code supports ours:
WPR2 establishment is `acr`'s job on GP10x. (2) VedCloud (i915) sets up
translation *before* firmware — an Intel ordering artifact (GGTT is
firmware-load transport), not a competing model for nvkm. **Which divergence is
better-evidenced:** ours and pascal-egpu's RESEARCH.md both derive from nouveau
source (`RESEARCH.md:31-41` boot sequence: devinit→FB→MMU→ACR→FIFO→GR — note
this one variant puts MMU *before* ACR, whereas our order follows the
`HANDOFF-PASCAL.md` §5/§6 phasing and `acr_boot.c`'s instance-block dependency);
the honest statement is that ACR↔MMU ordering is the one step with no
hardware-verified arbiter, because pascal-egpu never got ACR running
(`FINDINGS.md:5`). Their hardware evidence proves only steps 1-2 of the chain
and the hard security facts, not the ordering. Our Phase 2 simulator is the
most complete ACR artifact in either project (`PHASE2-ACR-TRACE.txt`,
3,888 steps) but is explicitly simulation, not proof (`OFFLOAD-DESIGN.md:319-323`).
[2026-11-07: that plan zero-filled the LS images and had no SEC2 RTOS, so
FECS/GPCCS could never start. The current plan (235 steps + the SEC2
BOOTSTRAP_FALCON executor, `f8fc91e`) replaces it; `PHASE2-ACR-TRACE.txt`
was regenerated.]

## B. Trap checklist — VedCloud's traps vs our Pascal port

Verdicts: **addressed** (design/code exists), **partial**, **ignored** (no
mention where it should exist), **N/A** (reason given). Our-port sources:
`pascal/kext/`, `research/evidence/OFFLOAD-DESIGN.md`,
`research/evidence/NOUVEAU-UAPI.md`, `research/evidence/OWNER-RUNBOOK.md`.

| # | Trap (VedCloud evidence) | Our status | Basis |
|---|---|---|---|
| 1 | Stolen-memory / live-scanout overlap (`README.md:765`, ROADMAP Ph 9→11) | **N/A (dGPU), partial (disp phase)** | Our dGPU renders into host IOSurfaces; there is no stolen-memory allocator in nvkm. The analogous hazard — writing VRAM the firmware's console/scanout still reads — applies only when Phase 5 drives HDMI/miniDP heads; `research/evidence/DISP-PLAN.md` sequences mode-set after link training but does not discuss firmware-owned scanout state. Revisit at Phase 5. |
| 2 | GuC lost-wake race / `TASK_WAKING` (`README.md:425-429`) | **Addressed by design, must be pinned at implementation** | Our completion model is bounded poll, not sleep/wake: "software-visible wait policy… event waits after 20 µs with a bounded 2 ms wait and completion-state rechecks" (`OFFLOAD-DESIGN.md:124-128`), and the uapi wait selector is a single scalar call (`NOUVEAU-UAPI.md:184`). No waiter-sleep in the design ⇒ the race shape cannot occur *as designed*. But the moment anyone adds a blocking wait (mach semaphore or thread-sleep path — both listed as alternatives at `NOUVEAU-UAPI.md:184`), the VedCloud rule applies: persist the wake flag under the sleep-channel lock, or keep positive-remaining-time semantics. Not yet written down as a rule anywhere (see §D, P1-1). |
| 3 | IOKit mapping pinning / no-op revocation shim (`ROADMAP.md:381-393`) | **Ignored — real gap in the uapi doc** | Our zero-copy path VM-binds the IOSurface's host pages into MMU v2 (`OFFLOAD-DESIGN.md:91-95`). Nothing in `NOUVEAU-UAPI.md` (searched: no "pin"/"reclaim"/"wire") states that GPU VM bindings must pin the backing pages for the binding lifetime, nor what happens on xnu when an IOSurface's pages move under an active GPU mapping (IOSurface does support page migration). The correct posture per VedCloud: either pin for binding lifetime, or unbind before teardown and treat unbind as mandatory before release (`OFFLOAD-DESIGN.md:167-173` has the teardown order but not the pinning rule). §D, **P0-1**. |
| 4 | `kIOMemoryPhysicallyContiguous` vs host-contiguous + segment-length trust (`ROADMAP.md:394-429`) | **Ignored — not yet applicable, must be encoded** | The kext skeleton allocates nothing DMA-visible yet (BAR0 identity only, `pascal/kext/README.md:62-73`). When pushbuffer/GPFIFO/ACR-DMA allocations land (Phase 4), the allocator must request `kIOMemoryHostPhysicallyContiguous \| kIOMemoryMapperNone` and verify the first segment covers the whole request before exporting any address. Record the rule now so it is not rediscovered by panic. §D, P1-2. |
| 5 | Completion-path context UAF (`ROADMAP.md:431-483`) | **N/A now, note for Phase 4** | No completion/retirement path exists in the kext. When GPFIFO completion handling lands, every completion-owned reference must hold its channel/context object alive through retirement, and ownership must be acquired before publishing completion to userspace. P2. |
| 6 | Command-pool reuse / quarantine (`README.md:757-758`) | **Addressed in spirit** | `OFFLOAD-DESIGN.md:184` maps fence-timeout to device-loss for that submission and "mark surface stale rather than present garbage"; stale = never reused for present. The NVK side is unmodified Mesa, which owns its own pool hygiene. |
| 7 | Kext injection / `EXITBS:START` / one-shot activation (`ROADMAP.md:145-201`, `README.md:756`) | **Partial** | The runbook deliberately touches nothing at boot: "no AuxKC rebuild, no NVRAM or OpenCore change" (`OWNER-RUNBOOK.md:8,298`), and our personality uses a wide match + software gate. What we have *not* adopted is their activation hygiene for the testing period: a failed attach that re-arms on every boot turned into a bricked-looking machine for them (`ROADMAP.md:478-480` — the stale `nullmoth-gpu-attaching` marker parked every subsequent boot). Our manual `kextload`-style testing can create the same loop (probe runs on every boot once the kext is installed). Add a "disable before reboot" step to the runbook. §D, P2. |
| 8 | Diagnostic signatures (`README.md:526-532`) | **Ignored — cheap to add** | Our kext logs IOKit states but has no failure-signature crib sheet, and `pascal-egpu/FINDINGS.md:29-44` gives us GP106-specific values (`0xbad00200`, `0xbad0acXX`, `0xdead5ec1`) we should decode in the BAR0-identity log path itself (one table, ~10 lines). §D, P0-2 (it shapes the first hardware session). |
| 9 | Evidence discipline (`README.md:735-747`, `ROADMAP.md:130-142`) | **Addressed; adopt two refinements** | `HANDOFF-PASCAL.md` §7 already mandates simulation labeling, sourced facts, one-variable commits, failing experiments recorded — the same rules. Missing from ours: (a) **collect before reboot/reinstall** (their §Diagnostics rule, `README.md:487-489`) — our runbook's rollback steps are good but do not state "gather unified log + panic file *before* reverting"; (b) route verification — they prove *which transport* ran, not assume it (`README.md:373-375`); our runbook should log which path (kext probe vs UserClient vs NVK) each evidence item actually exercised. §D, P2. |

## C. Pascal-specific facts pascal-egpu flags that we have NOT yet handled

1. **Failure-value decoding on first contact** (the `0xbad*`/`0xdead*` table,
   `FINDINGS.md:29-44`): not present in `pascal/core` or the kext log path.
   P0-2.
2. **PMC_ENABLE bit semantics on GP106** (`FINDINGS.md:153`): bit 4 (PMU)
   silently drops; host/PFIFO/PGRAPH bits that stick are enumerated. Our
   `pmc.c` decodes BOOT_0 only; no ENABLE read-modify-write helper exists, and
   the kext will need one (enabling PFIFO/PGRAPH bits is the first register
   *write* any bring-up does). Bits verified on real silicon are available —
   use theirs as the starting vector, verify per-register. P1-3.
3. **SCTL/UCODE_LEVEL=3 latch and the "HALTED is a security exception"
   false positive** (`FINDINGS.md:164-180`): our `acr_boot.c` polls for
   falcon halt after start (gm200-style); nothing in the core or the planned
   owner-runbook diagnostics warns that HALTED + TRACEPC==const means
   *security fault*, not success, and that mailbox-echo programs are the
   discriminator (their MAILBOX writes never landed). A Phase-2-sim note +
   runbook diagnostic step. P1-4.
4. **ACR blob variants for laptops (GP108 bits)**: research §3.2 says keep
   both variants (Phoronix,
   https://www.phoronix.com/news/Nouveau-Blobs-Pascal-Laptops); we vendored
   only the GP102 set, and **no GP108 ACR blobs exist anywhere in the repo**
   (searched `pascal/`, `research/firmware/`, research doc — only prose
   mentions). `pascal/core/src/acr.c` has no variant selection either.
   Unknown, and now hardware-flagged: whether GP106M (laptop) silicon
   requires the GP108-generation ACR ucode is exactly what killed the blobs
   on "newer Pascal laptops" (research §3.2). Action: fetch
   `nvidia/gp108/acr/*` into `research/firmware/gp108/` and add a documented
   fallback order (gp106→gp102→gp108) to the blob loader. P1-5.
5. **Priv-ring state cannot be repaired from the host** (`FINDINGS.md:120-122`):
   their RING_CMD experiments locked the GPU up permanently (power-cycle
   recovery only, `FINDINGS.md:200-210`). Our runbook has rollback for
   *software* (unload kext) but nothing about the GPU entering an
   unrecoverable-without-reboot state, and our core has no priv-ring code at
   all — good (absence is correct for a kext on a POSTed laptop), but the
   runbook should state explicitly: **never poke priv-ring master/command
   registers during bring-up debugging**. P1-6.
6. **Devinit-vs-FSEC/strap handling on laptops**: pascal-egpu documents
   devinit discovery (BIT `I` offsets, `FINDINGS.md:25,113`) but says nothing
   about FSEC straps on laptop silicon, and our repo likewise has no FSEC
   strap handling anywhere (searched: no FSEC matches in `pascal/` or the
   research doc). On nouveau the relevant laptop-side concern is that
   devinit has already run under system firmware, so `nvkm_devinit`-style
   re-execution must be *idempotent/skipped* where straps indicate POSTed
   state — ours currently has no "already POSTed" detection at all: the
   devinit interpreter exists (`core/src/devinit.c`) but nothing decides
   whether the kext runs it. **Unknown**, flagged as the key first-hardware-
   session question: read PSTRAPS (0x101000, `PLAN.md:149`) + FB state
   (`WPR2_HI`, `FINDINGS.md:54`) at probe and log them, before ever running
   devinit. P0-3.
7. **Their PLAN.md class-ID errors** (§1 above) — already correct in ours;
   recorded so nobody "fixes" our constants from PLAN.md. No action.

## D. Gap list (prioritized; smallest next action each)

**P0 — blocks the first hardware bring-up session (owner's Mac + Helios 300):**

- **P0-1. Binding-lifetime pinning rule missing from the uapi design.** The
  zero-copy VM-bind path has no stated rule that GPU MMU v2 bindings pin (or
  otherwise protect) the IOSurface backing pages against xnu page migration
  while bound — the exact trap that cost VedCloud kernels 1.0.10→1.0.11.
  Action: add one subsection to `research/evidence/NOUVEAU-UAPI.md` (VM_BIND
  semantics: pin-or-protect requirement + unbind-before-release ordering),
  citing `meteor-lake/ROADMAP.md:381-393`.
- **P0-2. No hardware-failure decoder in the kext probe path.** First
  hardware contact is where `0xbad00200`/`0xbad0acXX`/`0xdead5ec1` will
  appear, and the kext currently logs a raw word. Action: add a ~20-line
  failure-value classifier (table from `pascal-egpu/FINDINGS.md:29-44` +
  PTIMER-alive check) to `pascal/kext/src/PG106.cpp`'s probe log and to
  `OWNER-RUNBOOK.md` step 1's "what the log means" section.
- **P0-3. No POSTed-state detection before devinit.** Nothing decides whether
  the firmware already ran devinit (the Helios's system firmware has), and
  blindly running devinit from the kext is the one step with a documented
  lockup precedent (priv-ring). Action: extend the probe to read and log
  PSTRAPS/FB-WPR2 state (offsets per `PLAN.md:149`, `FINDINGS.md:54`) and add
  an explicit "devinit runs only if X" gate in `OWNER-RUNBOOK.md`, with X
  determined in simulation first.

**P1 — blocks a correct macOS compile / first submission, or risks repeats
of documented hardware damage:**

- **P1-1. Blocking-wait rule.** If/when a mach-semaphore or sleep-based wait
  replaces the bounded poll (`NOUVEAU-UAPI.md:184` alternatives), the
  TASK_WAKING/lost-wake rule must be stated (fixture-tested like VedCloud's
  100k-scenario test). Action: one paragraph in `NOUVEAU-UAPI.md` now.
- **P1-2. DMA-allocation rule for Phase 4.** `kIOMemoryHostPhysicallyContiguous
  | kIOMemoryMapperNone` + verify segment length ≥ request before exporting
  any address (VedCloud 1.0.12). Action: record in `NOUVEAU-UAPI.md` §GEM or a
  new `pascal/kext/README.md` "allocation rules" note.
- **P1-3. PMC_ENABLE helper with verified bits.** Add a read-modify-write
  helper to `pascal/core/src/pmc.c` + test, seeded with pascal-egpu's
  verified bit map (`FINDINGS.md:153`), marked "bit map from one desktop GP106,
  verify on GP106M". Smallest concrete enabler for any engine work.
- **P1-4. Falcon "HALTED ≠ success" discriminator.** Add mailbox-echo as the
  execution-success check in `acr_boot.c`'s plan + Phase-2 simulator, per
  `FINDINGS.md:164-167`; add the same to the runbook diagnostics.
- **P1-5. Vendor GP108 ACR blobs + variant fallback.** Research §3.2 says
  keep both variants; nothing is committed and no selection logic exists.
  Action: `research/firmware/gp108/acr/` + a documented fallback order in
  `research/firmware/README` and `pascal/core/src/acr.c`'s loader.
- **P1-6. "Never touch priv-ring master/cmd registers" rule in the runbook.**
  One sentence + the recovery procedure (power-cycle; their lockup is
  unrecoverable in-band, `FINDINGS.md:200-210`).

**P2 — quality / robustness, do when the corresponding phase lands:**

- **P2-1.** Completion-path ownership rule (hold channel/context refs through
  retirement; acquire ownership before publishing completion) — note in the
  Phase 4 bridge spec, citing VedCloud 1.0.14.
- **P2-2.** Testing-period activation hygiene in `OWNER-RUNBOOK.md`: disable
  the kext before any reboot that follows a failed attach (their stale-marker
  boot-parking incident), and "collect logs before revert" ordering.
- **P2-3.** Snapshot the kext link's undefined-symbol set as a frozen
  baseline (their "frozen 1.0.8 undefined-symbol set" practice) so owner-side
  link regressions are detectable by diff, extending `symbol-audit.sh`.
- **P2-4.** BIT-`I` PMU-DEVINIT table offsets (+0x14/0x16, +0x18/0x1a) in
  `vbios_bit.c`'s parser notes, from `FINDINGS.md:25`; plus add the official
  NVIDIA BIOS-Information-Table spec URL as the parser's authority citation.
- **P2-5.** Route-verification logging in the runbook: each evidence item
  states which transport actually ran (probe/UserClient/NVK), VedCloud-style.

## Uncertainties (stated as unknowns, per handoff §7)

- Whether GP106M laptop silicon accepts the GP102 ACR ucode or needs the
  GP108-generation variants is **unknown** and only resolvable on the GPU
  (OFFLOAD-DESIGN.md open question 4 already tracks this; P1-5 removes the
  packaging obstacle).
- Whether the Helios 300's firmware leaves the dGPU fully devinit-ed and what
  WPR2/priv state it leaves behind is **unknown** (no Optimus-laptop evidence
  exists in either prior-art repo; pascal-egpu's data is eGPU/Apple-Silicon
  only). P0-3's probe logging is the resolution path.
- pascal-egpu's `gp106/acr` blob provenance ("Arch linux-firmware-nvidia
  package") conflicts with our B3 statement that linux-firmware ships no
  `nvidia/gp106/acr/`; the binaries are md5-identical to `gp102/acr`, so the
  substance is unaffected, but the packaging history is unresolved.
- macOS-side Optimus-specific PCIe/ACPI behavior (_DSM, Optimus power gating of
  the dGPU bus) is covered by **neither** prior-art repo; our research doc's
  §4 architecture assumes the dGPU stays up, which is consistent with the
  no-RTD3 hard limit, but no external evidence exists either way.
