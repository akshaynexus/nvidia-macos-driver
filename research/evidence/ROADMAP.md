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
