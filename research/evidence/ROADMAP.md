# ROADMAP — Pascal (GP106M) Helios 300 bring-up

Work branch: `pascal-gp106` (per HANDOFF-PASCAL.md §3: no pushes to owner's main,
no upstream PRs). Newest entries last. One variable per commit.

## 2026-11-05

- `fdf120f` **Phase 0 gate item: pushbuffer encoding question resolved.**
  Primary source: NVIDIA `clc06f.h` (MIT) saved at `research/reference/`.
  SEC_OP field is [31:29] (3 bits). tinygrad `typ<<28` with typ=2 == 1<<29 ==
  canonical INC_METHOD (why it silently worked); typ=1 corrupts COUNT/TERT_OP.
  GPFIFO `LEVEL_MAIN`=0 corrected in `helios300-gp106-simtest.py` (was asserting
  1 = SUBROUTINE). Evidence: `research/evidence/ENCODING-RESOLUTION.md`.
  Primary sources committed: `research/reference/{clc06f.h,clc1c0.h,
  mesa_clc1c0qmd.h,hwref_gp100_dev_mmu.h}` (fetched from NVIDIA open-gpu-kernel-modules
  and mesa gitlab, 2026-11-05).
- **Phase 0 core library** (`pascal/`, GPL-2.0-only where nouveau-derived,
  MIT where NVIDIA-header-derived only):
  - `pmc.c`/`mmio.c`: PMC_BOOT_0 probe decode (GP10x family 0x130, GP106 chipset
    0x136; synthetic vector matches real GTX 1060 boot0 `0x136000a1`), BAR0 map
    (9 blocks). Tests: `tests/test_pmc.c` (6 groups).
  - `mmu.c`: MMU v2 hierarchy builder (big PDE → L2 → L1 → dual leaf PDE →
    4K/64K PTE pages), apertures, kinds; golden vectors independently computed
    from `hwref_gp100_dev_mmu.h` + nouveau `vmmgp100.c` literals. 103 checks.
  - `pushbuf.c`/`qmd.c`: canonical SEC_OP method writer, GPFIFO pack/unpack,
    QMD v01_07 builder + validator; golden vectors reproduce the python simtest
    words exactly. 72 + 65 checks.
  - `acr.c`: ACR HS ucode / WPR / LSB / r352-signature parsers, validated
    against REAL firmware blobs. Truncation + 20k-mutation fuzz clean under
    ASan/UBSan.
  - Firmware: `research/firmware/gp106/` — gr/fecs_sig.bin, gr/gpccs_sig.bin
    (real GP106), acr/*.bin (**substitution**: linux-firmware ships no
    `nvidia/gp106/acr/`; used `nvidia/gp102/acr/` since GP106 wires
    `acr = gp102_acr_new` — documented in `research/firmware/gp106/README.md`).
  - Suite: `cd pascal && make test` — 5 binaries, 156 PASS lines, plus the
    repo python simtest (5 checks). All green.
  - Coverage: `research/evidence/PHASE0-COVERAGE.txt` (merged gcov via
    gcov-tool across all binaries; regenerated after each phase lands).

## 2026-11-05 (later) — Phases 1-6 parallel work

- **Phase 1a** `vbios_bit.c`: BIT-section parser. Key correction against
  master nouveau: BIT is located by scanning for `ff b8 'B' 'I' 'T'`
  (`nvkm/subdev/bios/base.c:189`), NOT via a PCIR+0x30 pointer (that chain
  doesn't exist upstream); devinit scripts live under token `'I'`
  (`init.c:363`), `'D'` kept as lookup-only. 2,499 checks incl. every
  truncation prefix. Synthetic VBIOS only — real dump requested (BLOCKERS B1).
- **Phase 1b** `devinit.c`: 48-opcode DEVINIT interpreter (of master's 69;
  21 skipped loud-error on I2C/AUX/PLL-dependent ops, never silent), 67-record
  golden trace, run-twice diff clean, bug-compatible quirks flagged in-code
  (0x5a ZM_REG_INDIRECT upstream bug kept). Portability fix by integrator:
  messages made static literals + bounded copy, removing `<stdio.h>`/
  `<stdarg.h>` from the portable core (caught by the kext symbol audit).
  Phase 1 gate applied to synthetic script — real VBIOS pending (B1).
- **Phase 2** `acr_boot.c`: 3,888-step ACR secure-boot plan (WPR shadow →
  HS sig patch → falcon reset → desc setup → BL/HS load from REAL gp102
  blobs → boot/mailbox/WPR-window), every step annotated with nouveau
  file:line. SIMULATION, NOT HARDWARE PROOF. Trace evidence:
  `research/evidence/PHASE2-ACR-TRACE.txt` (3,904 lines).
- **Phase 3** `pascal/kext/`: GPL-2.0 XNU skeleton (wide NVIDIA personality
  + software GP10x gate, BAR map, PMC probe via core, NVRMUserClient-shaped
  UserClient, klibc shim for core sources, hand-written xcodeproj +
  MacKernelSDK build script for the owner). Linux stub suite + symbol audit
  green. Owner runs `pascal/kext/scripts/build-macos.sh` on the Mac.
- **Phase 4** `uapi.c` + `research/evidence/NOUVEAU-UAPI.md`: the NVK-needed
  nouveau CS uapi subset (GETPARAM/VM_INIT/VM_BIND/EXEC; structs layout-pinned
  to `nouveau_drm.h` by `_Static_assert`), 9 interface points mapped in
  `nvk/nvk-macos.patch` with file:line, replacement surface per point.
- **Phase 5a** `edid.c`: parser validated against a REAL EDID
  (Acer ED320QR S, linuxhw/EDID dump, refetched byte-identical; committed at
  `research/reference/edid/`). Extracted DTD: 148.5 MHz, 2200x1125, 60.000 Hz
  — matches the CEA-1080p numbers the disp model uses. Not the Helios panel
  (that's iGPU-wired/unavailable) — documented.
- **Phase 5b** `disp_mode.c` + `research/evidence/DISP-PLAN.md`: 23-step
  1080p60 HDMI mode-set plan (GF119+ EVO methods == PDISP registers identity
  exploited), ordering invariants + run-twice diff asserted. SIMULATION.
- **Bonus** `dispatch.c`: complete compute-launch submission model — runlist
  entry + RAMFC + SET_OBJECT(0xC1C0) → SET_INLINE_QMD → LOAD_INLINE_QMD_DATA
  (64 QMD words) → SEND_PCAS_A → SEND_SIGNALING_PCAS_B → ENDSEG + GPFIFO entry
  + doorbell, 95 checks, nouveau ramfc/runlist fields cited.
- **Phase 6** `research/evidence/OFFLOAD-DESIGN.md` + `OWNER-RUNBOOK.md`:
  offload switching design (80 citations) and the owner runbook (every step
  with rollback, no persistent state).
- Suite state: `pascal && make test` = 12 binaries + python simtest, all
  green; `pascal/kext && make test` + symbol audit green.

## 2026-11-05 (evening) — validation round: differential testing + port audit

Adversarial audit + 6 differential/research passes (agents), then integration
fixes. Evidence: `research/evidence/PORT-AUDIT.md`, `PRIOR-ART-AUDIT.md`,
`UAPI-ORACLE.md`, `BRINGUP-TRACE.txt`.

- **Port audit** (line-by-line vs torvalds master + Mesa codegen): 8/11
  modules clean; 3 real bugs found and FIXED:
  - edid.c: DTD byte-10/11 sync MSBs are four 2-bit fields (E-EDID §3.10.3) —
    H MSBs were dropped entirely.
  - acr.c: `flcn_acr_desc_v1` `__aligned(8)` padding — ucode_blob_base @608,
    not 604.
  - disp_mode.c: RASTER_SIZE carries actives (head907d_mode), not totals;
    HDMI block keyed by head, not SOR (gf119.c:259).
- **Real VBIOS evidence**: 5 genuine GP10x ROMs (TechPowerUp via Internet
  Archive, carved from NVGI flash wrappers) in `research/firmware/vbios/` —
  BIT/token/script-table parse in bounds on all 5; real GP106 script 0 runs
  the interpreter to INIT_DONE (220 steps, 913 records, run-twice clean).
  `tests/test_vbios_real.c`, `tests/test_devinit_real.c`.
- **Mesa codegen differential** (`tests/mesa_diff` + `tests/mesa_ref/`,
  Mesa @ `759935be`): all method words memcmp-identical to Mesa's generated
  emit macros; QMD all 64 words identical via Mesa's own pack functions;
  mutation-verified test sensitivity. No Mesa GPFIFO/ENDSG emitter exists
  (kernel-built) — covered by clc06f.h DRF vectors instead.
- **EDID corpus differential** (`tests/edid_corpus`, 36 real dumps,
  `research/reference/edid/corpus/`): 812/812 fields match `edid-decode`
  after the byte-10/11 fix; two regression vectors lock the fixes.
- **UAPI oracle** (`tests/uapi_oracle`, `UAPI-ORACLE.md`): box's NVIDIA GPU
  runs the proprietary driver (nouveau NOT bound — recorded finding); ABI
  layout pinned against the real system kernel `nouveau_drm.h` (21
  size/offset/flag asserts, compile-time). tinygrad differential
  (`tests/tinygrad_compat`): method/GPFIFO words agree with tinygrad master;
  LEVEL=SUBROUTINE for EXEC entries confirmed by kernel `nvif/chan506f.c`;
  tinygrad has no Pascal QMD (v01_07) — v01_07↔v03_00 mapping documented.
- **End-to-end bring-up integration** (`tests/bringup`,
  `BRINGUP-TRACE.txt`): 7-stage composed pipeline over one shared mock bus;
  9821 writes, byte-identical across two from-scratch runs; cross-module
  non-interference (ACR WPR vs MMU regions vs PDISP) asserted. Finding F1:
  EVO window (0x640000+) not in the BAR0 map — future phase.
- **Prior-art audit** (`PRIOR-ART-AUDIT.md`, 97 citations): pascal-egpu's
  failure decoder (0xbad* priv-ring signatures) and VedCloud's trap list
  audited against our design; P0 gaps: IOSurface pinning rule unstated,
  no 0xbad* failure decoder for first hardware session, no POSTED-state
  detection before devinit.
- Suite: **19 test binaries + python simtest, all green**; coverage total
  **94.91%** (was 65.89% before this round).

## 2026-11-05 (night) — acceleration pipeline round: top/PBDMA/CE/GR/diag/bridge

Upstream sources moved to **git submodules** (third_party/{open-gpu-kernel-modules,
mesa@759935be,tinygrad}, research/prior-art/{pascal-egpu,meteor-lake-xe});
policy: reference, don't copy; linux-firmware deliberately vendored minimally
(third_party/README.md). gen_mesa_ref.sh now prefers the submodule.

- **nvkm_top wiring** (`top.c`): PTOP table reader (0x022700 window, per
  v5.15 gk104.c; SDK has no GM10x-format NV_PTOP header — stands on nouveau
  alone). `pg106_acrboot_falcon_base()` now derives the SEC2 base from the
  table; 0x3c0000 demoted to documented fallback. Audit outstanding-item 1
  closed in model form.
- **PBDMA decoder** (`pbdma.c`): consumption-side model — GPFIFO walk with
  GET/PUT wrap, all SEC_OP forms incl. legacy TERT, hardware error model
  (dev_pbdma.ref INTR bits). Our `pg106_sub_launch` output decodes back to
  the exact intended method stream. **Doorbell settled**
  (PBDMA-DOORBELL.md, PORT-AUDIT item 2): Pascal rings via USERD+0x8c GPPut
  (kernel nvif/chan506f.c); tinygrad's BAR0+0x90 is the Volta+ work-submit
  token, absent on GP106.
- **GPFIFO LENGTH units fixed**: field is a DWORD count (kernel chan506f.c
  packs size>>2); dispatch.c + tests corrected (was passing bytes).
- **CE copy engine** (`ce.c`): PASCAL_DMA_COPY_B (0xC1B5) 1D/2D + semaphore
  release; per-method payload equality vs tinygrad's Ampere CE (class-id
  delta documented), constants from SDK clc1b5.h.
- **devinit completes the opcode set**: INIT_PLL/PLL2/IO_RESTRICT_PLL(_2)/
  PLL_INDIRECT/RAM_RESTRICT_PLL (full gf100 pll_set + gt215 calc port,
  hand-derived golden divisors), I2C family over optional vtable hooks,
  BIT-era no-op stubs. ALL scripts of ALL 5 real ROMs now execute: the GP106
  ROM's 5/5 scripts run to INIT_DONE; 4 GP104/107-era ROMs stop at byte 0xac
  (unknown to current master nouveau — documented, matches nvbios_exec).
- **Diagnostics** (`diag.c` + POSTED-DETECTION.md): 0xbad*/0xdead5ec1
  failure decoder (pascal-egpu FINDINGS, line-cited; SDK PRI ringstation
  codes), falcon security-fault vs halt discrimination, liveness
  classification. POSTED rule: devinit runs only when the 0x2240c latch is
  readable and clear; owner liveboot procedure documented (P0-2/P0-3 closed).
- **GR bring-up model** (`gr_boot.c`): 136-step post-ACR GR init (reset,
  fuse discovery, GPC MMU, sw_nonctx pack, ZCULL/LTC/FBP, ZBC, FECS boot +
  mailbox size queries); 32 steps flagged [HW-VALUE] (fuse/firmware-derived).
  Falconless verdict: does not exist on GP10x in mainline — signed path only
  (we have the real sigs).
- **Bridge backend** (`pascal/bridge/`, MIT): nvkmd-shaped userspace library
  (dev/vm/bo/va/ctx/exec ops) over a transport vtable; ops validated by
  core uapi validators then serialized to wire frames (byte-goldens);
  P0-1 closed: binding table enforces unbind-before-release + NOUVEAU-UAPI.md
  §6 pinning/lifetime section added.
- **Web Driver comparison** (WEBDRIVER-COMPARISON.md, 45 citations): NVDA*
  Web stack anatomy + Metal timeline (378->387, capped 10.13.6). Verdict:
  Web-Driver route is NOT viable on the Helios 300 (OS cap + Optimus
  unsupported + headless rmStart failures) — our headless offload path is
  novel vs everything the Web Drivers achieved on this hardware.
- **BAR0 map corrected to GP106 reality** (MMIO-MAP-CORRECTIONS.md): PTIMER
  moved 0x020000 -> 0x009000 (SDK gm107/dev_timer.h), NV50-era PCOPY fixed
  window removed (CEs come from PTOP), PTOP 0x022000 added, EVO window
  0x640000 added (finding F1). Every range cited to swref/nouveau; simtest
  section 2 + mmio.h updated, all dependent assertions re-verified.
- Suite: **24 test binaries + python simtest, all green**; coverage 88.71%
  over 3,861 core lines (bridge + 5 new modules included).

## 2026-11-07 — independent review and fix round

An independent review against upstream nouveau (torvalds/linux d48723711bae),
the NVIDIA headers and Mesa (759935be) found the earlier entries' claims
overstated. Per-module findings: `PORT-AUDIT.md`, 2026-11-07 section. Fixes,
in order (all after `327f01a`):

- Submission: `3697344` 12-bit method address; `fd37925` CE semaphore on the
  last chunk only; `bb07dd5` QMD v02_01 (was v01_07), NVK's dispatch program
  (SEND_PCAS_A + SEND_SIGNALING_PCAS_B), GP_PUT at USERD+0x8c (was the Volta
  BAR0+0x90), VMM join in the instance block; `7dddfb1` MMU refcounts,
  4K/64K -EEXIST, big-PTE precedence, PA decode, rollback; `e32f445` BAR0 map
  adds PBDMA, PGPC, PRAMIN, PCCSR.
- Boot: `af5c7a5` SEC2 golden 0x087000 (was 0x3c0000); `c7f1006` gr_boot RMW,
  ZBC, FECS size polls, invented unload (PMC_ENABLE = 0) removed; `f8fc91e`
  SEC2 RTOS + BOOTSTRAP_FALCON for FECS/GPCCS with the real linux-firmware
  sec2 v1 and full gr images, scrub poll direction, BL args no longer
  overwritten, MASK steps, register-modelling mocks + mutation tests;
  `7d63edf` header comments; `454a5ad` diag falcon verdict from readable
  state, WPR via 0x100cd4.
- VBIOS/POST: `e2d59fa` GP106 POSTs via the PMU DEVINIT app
  (`gm200_devinit_post`, new `devinit_pmu`); host interpreter re-scoped to
  DCB/IED/DP scripts with the missing opcodes; NPDE, image remap, u32
  offsets. Real ROMs: only the GP107M dump completes the upload; the GP106/
  GP107 desktop dumps are truncated at 0xf000 and `gtx1060-3gb` is an
  old-style image. `6b998d1` real-ROM tests PASS-SKIP without ROMs.
- Display: `6b66ba8` core917d/base907c DMA pushbuffers + supervisor 1/2/3 +
  DP training model (new `disp_core`); RASTER_SIZE back to totals (the
  2026-11-05 "fix" to actives was the bug); HDMI via gk104 regs; TMDS >
  340 MHz div2. `e4da614` EDID extension/CTA fixes. `8de0971` DISP-PLAN and
  POSTED-DETECTION rewritten.
- Kext/bridge/uapi: `68ff50c` kext lifecycle fixes (it would not compile),
  root-only user client, klibc without `#define free`, SDK syntax check;
  `778be52` uapi accepts real NVK binds, kernel errno order, MIT;
  `64e6bcc` bridge VA table per drm_gpuvm, slot reuse, exact reply lengths;
  `f8ae9aa`/`4024190` VBIOS dumps untracked, `fetch_vbios.sh`; `e4860f6`
  runbook load paths (OpenCore / kmutil + AuxKC), rollback, recovery;
  `a3c7b9b`/`9202284` make test with absolute BUILD_DIR, kext + bridge suites.
- Evidence regenerated (this entry): `612a022` coverage, `09b318f`
  PHASE2-ACR-TRACE + BRINGUP-TRACE, `3fe16ef` pascal/README, `fe71ac1`
  PORT-AUDIT, `bf36854` BLOCKERS.

Corrections to earlier entries (left as written above):
- 2026-11-05 (night) "Doorbell settled": settled on paper only;
  `dispatch.c` kept writing BAR0+0x90 until `bb07dd5`.
- "coverage ... bridge + 5 new modules included": `coverage.sh` has only
  ever instrumented `pascal/core/src`; bridge and kext are not in the report.
- "every step with rollback, no persistent state" (runbook): false on
  macOS 11+; `kmutil` staging/approval rebuilds the AuxKC (`e4860f6`).
- "real GP106 script 0 runs to INIT_DONE": true of the host interpreter, but
  GP106 does not POST that way; it is not Phase 1 evidence.
- Firmware "substitution" of gp102 ACR blobs: not a substitution, WHENCE
  links them (`research/firmware/gp106/README.md`).

Suite: `make -C pascal test BUILD_DIR=/tmp/pg106-build-docs` — 25 core test
binaries, kext stub tests + symbol audit (the syntax check SKIPs without the
SDK; with `PG106_SDK_FETCH=1` all 9 TUs pass against MacKernelSDK
`3f750085`), `test_bridge`, python simtest: all green. Coverage (core/src
only): 90.45% of 5,573 lines, 25 binaries.

### Gate status as of 2026-11-07 (HANDOFF-PASCAL.md §6)

| Phase | Status | Evidence / what is missing |
| --- | --- | --- |
| 0 Host foundation | **Met (simulation)** | suite green; Mesa differential (`test_mesa_diff`: gallium v02_01 QMD fill and NVK dispatch replayed with Mesa's macros); coverage committed. QMD is v02_01, not the v01_07 the spec names: v02_01 is what Pascal userspace builds |
| 1 VBIOS/DEVINIT | **Partial** | BIT/image parser and the gm200 PMU POST model exist and match nouveau's sequence; no known-good register trace exists to compare against; the substitute desktop ROMs are truncated (no PMU DEVINIT images), only GP107M uploads; Helios ROM missing (B1) |
| 2 ACR secure boot | **Met as simulation** | ACR HS boot, SEC2 RTOS, cmdq/msgq and BOOTSTRAP_FALCON(FECS, GPCCS) from real linux-firmware images, every plan step cited (`PHASE2-ACR-TRACE.txt`); mock-modelled, not hardware (B4, B10) |
| 3 Kext skeleton | **Source complete, not built** | gate items met on Linux (core suite, symbol audit, owner build script, syntax check vs MacKernelSDK); never compiled, linked or loaded on macOS; Xcode project never opened (B6, B7) |
| 4 NVK bridge | **Partial** | uapi subset documented with vectors; bridge + validators checked against kernel 7.4-rc1 and Mesa 759935be source (no nouveau-bound GPU on this box); not wired into `nvk/nvk-macos.patch` (no pg106 code there) and no kext transport (the user client has two read-only selectors); drm_version open (B9) |
| 5 Display | **Met as simulation (unverified on hardware)** | Core/base pushbuffers, supervisor, HDMI and DP training are modelled against a mock engine. The 1080p mode is derived from the real Acer ED320QR EDID through `edid.c` (`test_disp_mode` `edid-mode` check). The macOS framebuffer design is restored as DISP-PLAN.md §8, with open questions. |
| 6 Offload design | **Design + runbook** | `OFFLOAD-DESIGN.md`, `OWNER-RUNBOOK.md`; runbook steps 0-2 written for the owner and depend on B6/B7 (step 2 runs `pascal/kext/build/pg106_probe`), steps 3-4 marked not available (no NVK ICD on the bridge, no Metal test app); nothing measured |

Goal not reached: Phases 1, 4 and 5 are partial, and the kext has never been
built.

## 2026-11-07 (later): follow-ups the docs pass reported

- Phase 5's framebuffer design is restored as DISP-PLAN.md §8 and updated
  to the DMA-channel model; Phase 5 is now met as simulation.
- `test_disp_mode` now derives the 1080p60 mode from the real Acer ED320QR S
  EDID (preferred DTD → mode → `pg106_head_mode_calc`) and requires it to
  equal the planned mode, so the gate's "timings from a real EDID" is tested
  rather than stated.
- Finding F1 printed by `test_bringup` is marked resolved. The 0x640000
  window is relabelled in `mmio.h` and the simtest as the display channel
  USER/PUT + ARMED mirror, not a method window.
- `test_bringup` stage 4 now prints the real page size (was "0K").
- HELIOS300-PASCAL-RESEARCH.md §6 has a dated correction note (PTIMER,
  QMD v02_01, USERD GP_PUT); the spec text itself is unchanged.

## 2026-11-07 (night) — the real target: probe + its own VBIOS

- `ef273c0` first read-only probe of the owner's Helios 300 over SSH,
  authorised by the owner (HELIOS300-HW-PROBE.md):
  - GP106M 10de:1c20, subsystem 1025:118b;
  - no dGPU ROM chip; the VBIOS comes through ACPI `_ROM`;
  - HDMI is the only dGPU output;
  - ACPI `PG00` `_OFF` via `_PR3`, so D3cold is available.
- The machine's own VBIOS (86.06.4F.00.08) was extracted from Acer's
  BIOS V1.22 package with `pascal/scripts/extract_helios_vbios.py`; the ROM
  itself is gitignored.
  - It is wired into `test_vbios_real`, `test_devinit_pmu` and
    `test_devinit_real`.
  - It is the first ROM from the target machine to complete the
    `gm200_devinit_post` upload (9,643 steps).
  - Its DCB has one active output: TMDS (HDMI) on OR mask 0x2.
- Phase 1 status: the POST model now runs on the real target VBIOS
  (simulation). Still missing is a known-good hardware trace to diff
  against. BLOCKERS B1 is resolved.

## 2026-11-08 — kdev3: ACR memory, VRAM/MMU, FIFO, interrupts, kext providers

All simulation; nothing below has run on the GPU.
- Core:
  - `vram` (allocator + PRAMIN + FB init);
  - `vmm` (VRAM page tables, gp100 TLB invalidate, join, BAR1/BAR2);
  - `fifo` (gp100 runlists, PCCSR, USERD kick, non-stall);
  - `intr` (gp100 PMC routing);
  - `gpu` (two-pass bring-up plus the CE self-test).
- devboot builds the ACR memory itself (WPR, ACR inst + VMM, HS image
  mapped). The B15 `-ENOSYS` stop is gone.
- `tests/mockgpu.h` adds:
  - an independent MMU v2 walker over poisoned VRAM, with a TLB;
  - a runlist/PCCSR state machine;
  - a PBDMA that executes semaphores, NON_STALL_INTERRUPT and CE copies.
- `test_acr_boot`'s toy ACR fetches the HS image by DMA through BINDINST.
  WPR alloc 0x80000 matches the owner's trace.
- The kext:
  - runs `pg106_gpu_boot`, with MSI interrupts;
  - provides `disp_host.h` (PG106_disp.cpp) and `pg106_kif.h`
    (PG106_kif.cpp);
  - gets render BO bus addresses from IODMACommand;
  - keeps client VMs in VRAM;
  - sleeps in SYNCOBJ_WAIT.

  Cross-build: 30 TUs, KPI 369/369.
- The Linux harness `pg106_hwtest` is built and smoke-tested on fake sysfs.
  Hardware run pending (BLOCKERS B18). The render channel waits on the GR
  golden context (B17).

## 2026-11-08 — NVK/NAK on the real GP106 under Linux nouveau (hardware)

The first GPU execution of this repo's shader compiler. NAK, as the macOS
dylib carries it, was built into upstream NVK and run on the Helios' GP106
under nouveau (`nvk/pascal/CROSS-BUILD.md`, runs 1–3).
- Run 1: every program asserted at device open. The cause was the upstream
  winsys VDEC probe: nouveau 1.4.3 gives out an NVDEC channel with no class
  on pre-GSP chips. Fixed by `nvk/pascal/nouveau-vdec-probe.patch` and
  reproduced on drm-shim. The dylib does not build the winsys.
- Run 2: smoke 4/4, offload passed, NAK hw_tests 43/45. The two failures
  were SM 6.x test bugs, fixed by `nvk/pascal/nak-hw-tests-pascal.patch`
  (backport of upstream 023d491a, plus a SM75 gate).
- **Run 3: PASS.** NAK hw_tests 45/45, `pg106_vk_smoke` 4/4,
  `offload_selftest` all passed (copy mode). Bundle SHA256SUMS sha256
  `acb404a9d3ce9d0082b42700dfa22fbbe846971794cd79e941650cbd139bd888`. No
  kernel errors.

Not covered: the pg106 backend and kext path, the zero-copy host-import
mode (Mac-only, runbook 3b), and anything on macOS.
