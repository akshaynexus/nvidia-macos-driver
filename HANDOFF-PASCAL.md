# AUTONOMOUS HANDOFF: NVIDIA Pascal (GP106M) macOS driver — work until goal reached

You are an autonomous engineering agent on a Linux workstation. Your mission is the
display/compute enablement of one specific GPU and the switching architecture around it.
Work continuously through the phases below. Do not stop to ask questions; when a step
genuinely requires hardware or a decision only the machine's owner can make, record it in
`research/evidence/BLOCKERS.md` with everything a human needs to act, and continue with
every other step that does not depend on it.

## 1. Mission

Target GPU: **NVIDIA GeForce GTX 1060 Mobile (GP106M**, PCI `10DE:1C20` or `10DE:1C60`)
in an Acer Predator Helios 300 (2017): Intel Core i7-7700HQ, Intel HD Graphics 630,
**no display MUX** (Optimus only; internal panel wired to the iGPU, HDMI/miniDP wired to
the dGPU — verified, see the research doc).

Goal: a macOS driver that brings this GPU up and renders through it, plus an efficient
iGPU↔dGPU switching design, built from open-source components. "Done" is defined per
phase in §6 — each phase has objective acceptance gates. Do not claim a gate is passed
without the evidence it names.

## 2. Read first (in this order)

1. `HELIOS300-PASCAL-RESEARCH.md` in this repo — the full blueprint: card/ID inventory,
   wiring facts, port plan, firmware requirements, verified nvkm inventory (§7), Phase 0
   code map into this repo (§8), open encoding question (§6). It is your spec.
2. `helios300-gp106-simtest.py` — the existing constant/format tests. Keep them green
   (`python3 helios300-gp106-simtest.py`) and extend them as you learn more.
3. `docs/HOW-IT-WORKS.md` and `kexts/NVRM` — how the existing Turing+ driver structures
   its kexts, plugin, translator and NVK bridge. Your Pascal kernel work is a *separate
   GPL-2.0 kext tree*, but its userspace must slot into this plugin/translator pipeline.

## 3. Non-negotiable constraints

- **Never touch the owner's hardware.** The owner's Helios 300 and their Mac are off
  limits to any automation: no remote access, no installs, no config changes. Everything
  you do happens on your Linux box against source code, simulators, and your own test
  hardware (if the box has an NVIDIA Maxwell-or-newer GPU with nouveau, you may use it as
  a *reference oracle* for MMIO/command-stream behavior — read-only experiments only).
- **Final kext compilation and all macOS hardware validation are the owner's steps.** You
  produce source, build scripts, and instructions; you cannot run macOS. Flag anything
  that can only be verified on macOS in `BLOCKERS.md`.
- **Licensing is structural, not cosmetic.** nouveau/NVKM is GPL-2.0-only: your kernel
  port must live in its own GPL-2.0 kext tree. Never merge GPL code into the PolyForm-NC
  nullmoth kexts (`kexts/`, `plugin/`). NVK/Mesa is MIT. Ship `linux-firmware`
  `nvidia/gp106/*` blobs under NVIDIA's redistribution license, like the GSP firmware
  this repo already ships.
- **Work in your own clone/branch.** Do not push to the owner's `main`; do not open PRs
  upstream. Commit to a work branch with small, well-described commits and a dated log
  (`research/evidence/ROADMAP.md`, newest entries last).
- **The repo's AGENTS.md applies to humans who later submit your work** — write code so a
  person can review and own every line; no generated filler.

## 4. Architecture (decided — do not relitigate without new evidence)

- Kernel: port nouveau's `nvkm` GP106 subset to XNU as a GPL-2.0 kext: falcon/ACR secure
  boot, MMU v2, FIFO/GPFIFO, GR, CE. Display (`engine/disp`, CPU-driven on Pascal) is a
  later phase.
- Firmware: ACR/GR blobs from `linux-firmware/nvidia/gp106/` (list in research doc §3.2).
- Userspace: NVK (supports Pascal, sm_60) behind a bridge shaped like the nouveau CS uapi
  (VM bind + exec), replacing the GSP message-ring assumptions in `nvk/nvk-macos.patch`.
  The Metal plugin (`plugin/`) and AIR→SPIR-V translator (`translator/`) are reused as-is.
- Switching: **offload architecture** — the Intel HD 630 (Apple's native kexts) owns all
  displays and WindowServer; the NVIDIA side runs display-less and renders into IOSurfaces
  the Intel side composites (x86 shared memory is cache-coherent: effectively zero-copy).
  There is no MUX on this laptop; dGPU-driven external displays come only with the disp port.
- Accepted hard limits (document, never chase): no Pascal PMU firmware → **no reclocking**
  (GPU runs at VBIOS boot clocks) and **no runtime D3** (software idle only; battery cost
  is real).

## 5. Key technical facts (verified; details and sources in the research doc)

- GP106 nvkm wiring: `acr=gp102_acr_new, bar=gm107_bar_new, fb=gp102_fb_new,
  pmu=gp102_pmu_new, ce=gp102_ce_new, fifo=gp100_fifo_new, gr=gp104_gr_new,
  sec2=gp102_sec2_new` (`nvkm/engine/device/base.c`). Note upstream renames: old
  `subdev/secboot` is subsumed by `subdev/acr`; falcon core is `engine/falcon`.
- Classes: `PASCAL_COMPUTE_B` 0xC1C0 (GP106), channel `PASCAL_CHANNEL_GPFIFO_A` 0xC06F.
  Headers: `open-gpu-kernel-modules src/common/sdk/nvidia/inc/class/clc1c0.h`,
  `clc1c0qmd.h`, `clc06f.h`; MMU format: `open-gpu-doc pascal/gp100-mmu-format.pdf`.
- MMIO BAR0: PMC 0x000000, PFIFO 0x002000, PTIMER 0x020000, PFB 0x100000, PPMU 0x10A000,
  PGRAPH 0x400000, PDISP 0x610000.
- **Open encoding question you must resolve first** (flagged by
  `helios300-gp106-simtest.py`): tinygrad encodes the pushbuffer type field as
  `typ << 28`; the documented layout says bits [31:29]/[31:30]. Reconcile against
  `clc06f.h` and `dev_pbdma.ref.txt` before writing any real submission code.
- ACR chain (the hardest part): Falcon boot ROM → HS ACR ucode (PMU/SEC2) → signature
  validation → FECS/GPCCS LS load into GR. Port `nvkm/subdev/acr/gm200.c`, `acr_r352.c`,
  `gp102.c`, `lsfw.c`. Newer Pascal laptops needed the GP108 ACR bits — keep both blob
  variants available.

*Corrections to §5, 2026-11-07 (the facts above are left as written; sources in
`research/evidence/MMIO-MAP-CORRECTIONS.md`, `PBDMA-DOORBELL.md`, `PORT-AUDIT.md`):*
- *PTIMER is at **0x009000** on GF100+ (TIME_0 0x9400, SDK `gm107/dev_timer.h`), not
  0x020000 (an NV50-era envytools address).*
- *Pascal submission kicks GP_PUT at **USERD+0x8c** (`nvif/chan506f.c`); the BAR0+0x90
  doorbell in the research doc (from tinygrad) is Volta+ usermode and not used on GP106.*
- *`acr_r352.c` no longer exists upstream (old `secboot`); the master files are
  `subdev/acr/{base,gm200,gp102,lsfw,hsfw}.c` plus `engine/sec2/gp102.c`. On GP10x the HS
  ACR runs on SEC2, which then boots the SEC2 RTOS; FECS/GPCCS are started by the RTOS
  through `BOOTSTRAP_FALCON`, not loaded directly by ACR.*
- *GP106 POSTs through the VBIOS PMU DEVINIT application (`gm200_devinit_post`), not by
  interpreting the BIT `I` scripts on the host (`POSTED-DETECTION.md` §2b).*
- *Pascal userspace (NAK, gallium) builds QMD **v02_01**, not the v01_07 named in §6 Phase 0;
  the core follows userspace.*

## 6. Phases and acceptance gates

**Phase 0 — Host-side foundation (Linux, pure userspace).**
Extract the nvkm GP106-relevant subset as a portable core library (C or Rust, no Linux
kernel dependencies). Implement and unit-test: PMC probe decode, MMU v2 page-table builder
(PDE3..PTE_64K/4K per `gp100-mmu-format.pdf`), QMD v01_07 builder (extend the existing
simtest), pushbuffer/GPFIFO encoders (after resolving §5's encoding question), ACR blob
parser (WPR/LSB headers).
*Gate:* full test suite green, including golden vectors cross-checked against Mesa/NVK
sources and nouveau headers; coverage report committed.

**Phase 1 — VBIOS/DEVINIT in simulation.**
Write a VBIOS BIT parser and a DEVINIT script interpreter; run it against the Helios 300's
VBIOS (request the dump in `BLOCKERS.md` if absent; until then use any GP106 VBIOS you can
obtain legitimately and mark the substitution).
*Gate:* interpreter reproduces a known-good register-write trace for that VBIOS, diffs
clean against a second run; trace committed as evidence.

**Phase 2 — ACR secure boot in simulation.**
Model the falcon loading sequence (IMEM/DMEM writes, mailbox handshake, WPR setup) as a
simulator; feed it the real `gp106` blobs.
*Gate:* simulator drives the documented sequence end-to-end and validates all blob
signatures/headers structurally; every step's expected register values annotated with their
nouveau source line. Label clearly: this is simulation, not hardware proof.

**Phase 3 — XNU kext skeleton (source; compiled later by the owner).**
GPL-2.0 kext tree with: PCI personality matching `10DE` display/3D class and gating on
GP10x IDs; BAR mapping and the Phase 0 core wired in; a UserClient matching the
`NVRMUserClient` shape for userspace transport. Build scripts using MacKernelSDK; you
cannot compile these — provide a script the owner runs on the Mac, plus a stub layer that
lets the *core logic* keep building and testing on Linux (the nullmoth kexts' os-xnu layer
shows the mapping patterns).
*Gate:* core logic test suite still green on Linux; kext source passes a header/symbol
audit (no unresolved references to macOS-only symbols from the portable core); owner-run
build script included.

**Phase 4 — NVK bridge.**
Write the nouveau-CS-uapi-shaped backend for the userspace bridge (VM bind + exec
semantics). Validate NVK behavior on Linux: if the box has any supported NVIDIA GPU with
nouveau, run NVK against the real kernel uapi and record the semantics; otherwise validate
against Mesa source and label it.
*Gate:* documented uapi subset with test vectors; bridge code reviewed against
`nvk/nvk-macos.patch`'s interface points.

**Phase 5 — Display (disp port) and framebuffer plan.**
Port `engine/disp` (gf119 base + gp102) logic into the simulator for HDMI/miniDP heads;
produce the NVRMFB-style framebuffer design (source may remain macOS-side).
*Gate:* simulated mode-set sequence for a 1080p panel with timings derived from a real
EDID; design doc for the macOS framebuffer with open questions listed.

**Phase 6 — Switching (offload) design + integration plan.**
Concrete design for the HD 630-owns-everything offload path: IOSurface handoff, lifecycle,
power (software-idle for the dGPU), and what the owner must change on macOS to test it.
*Gate:* design doc + the complete "owner runbook" (what to build, install, and measure on
the Mac — written for a careful human, with rollback for every step).

**Definition of reached goal:** Phases 0–6 gates passed with committed evidence,
`BLOCKERS.md` lists only items that intrinsically require the owner's Mac/Helios 300, and
the owner runbook is complete. macOS compile + hardware bring-up is explicitly the owner's
phase — you do not have access and must not pretend otherwise.

## 7. Evidence rules (enforce on yourself)

- Simulator/unit-test success is always labeled as such. Hardware claims require hardware
  logs. Never convert a mock pass into a device claim.
- Every external fact carries its source (file path or URL). Sources live in
  `HELIOS300-PASCAL-RESEARCH.md` already — cite, don't re-derive.
- One variable per commit; failing experiments are recorded, not deleted.
- Run `python3 helios300-gp106-simtest.py` and your expanded suite in CI-style before every
  push to your work branch.

## 8. Prior art to mine (do not reinvent)

- `github.com/TheTom/pascal-egpu` — GP106 macOS-focused research/plan (RESEARCH.md, PLAN.md).
- `tinygrad` TinyGPU — macOS PCIe transport pattern (DriverKit RPC: MAP_BAR, CFG_READ...).
- `VedCloud/Meteor-Lake-Xe-LPG-Metal-Graphics` — proof the "port a Linux KMD to xnu + run
  Mesa + NullMoth plugin" architecture works; study its trap list (stolen memory,
  task-wake race, IOKit pinning) and its evidence discipline.
- `nullmoth/nvidia-macos-driver` upstream + X512's Haiku port lineage.
- envytools (MMIO/register database), Karol Herbst's low-level NVIDIA guide.

Begin with §2 reading, then Phase 0. Keep the existing tests green at all times.
