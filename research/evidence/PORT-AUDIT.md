# PORT-AUDIT — line-by-line comparison of pascal/core against upstream

## 2026-11-07 independent re-audit (supersedes the 2026-11-05 verdicts below)

An independent review on 2026-11-07, against upstream nouveau (torvalds/linux
d48723711bae), the NVIDIA class/SDK headers in `third_party/open-gpu-kernel-modules`
and Mesa (`third_party/mesa` @ 759935be), found the 2026-11-05 audit
**overstated**. "8 of 11 modules CLEAN" was wrong: most of those modules had
real defects, several differential tests compared our code against the wrong
reference (QMD v01_07 instead of the v02_01 Pascal userspace uses), and the
audit's own RASTER_SIZE "fix" introduced a bug. The findings were fixed in the
commits below (all on `pascal-gp106`, after `327f01a`). Everything remains
simulation/unit-test evidence; no module has run on hardware.

| Module | 2026-11-07 findings | Fix |
| --- | --- | --- |
| pushbuf.c, pbdma.c | method address allowed 13 bits (bit 12 aliased into the address); now 12 bits per `clc06f.h` ADDRESS 11:0, mthd >= 0x4000 and non-dword methods rejected, decoder masks 0xfff | `3697344` |
| qmd.c | built Kepler-era **v01_07**; Pascal userspace (NAK `Qmd2_1`, gallium `gp100_compute_setup_launch_desc`) uses **v02_01**. RELEASE0 lacked SEMAPHORE_RELEASE_ENABLE0, so the release was inert. Now v02_01 with the fields NAK/gallium set; `test_mesa_diff` replays gallium's fill with Mesa's v02_01 macros (64 words identical) | `bb07dd5` |
| dispatch.c | doorbell was **BAR0+0x90** (Volta usermode, `userc361.c`); Pascal kicks GP_PUT at **USERD+0x8c** (`nvif/chan506f.c`). Inline QMD + ENDSEG replaced by NVK's program (compute state, SEND_PCAS_A + SEND_SIGNALING_PCAS_B); instance block now joins the VMM (`gf100_vmm_join_`); launch range checks and PUT wrap | `bb07dd5` |
| ce.c | semaphore released after every 2 GiB chunk; now once, after the last | `fd37925` |
| mmu.c | `used` counters did not mirror `nvkm_vmm_pt.refs[]` (unmapping one range could unlink a live neighbour and wrap); no -EEXIST for 4K/64K overlap; walk ignored big-PTE precedence; PA decode not per aperture; failed maps not rolled back | `7dddfb1` |
| mmio.h | GR stopped at 0x41ffff; PBDMA, per-GPC GR, PRAMIN, PCCSR windows missing | `e32f445` |
| top.c | no source change; `test_top`'s SEC2 golden was 0x3c0000, now **0x087000** (measured on GP106, pascal-egpu `FINDINGS.md:160`) | `af5c7a5` |
| acr.c | `ucode_blob_size` read at 604 (padding) instead of 600; HS spans and debug signature not bounded by the image / `sig_prod_size` | `f8fc91e` |
| acr_boot.c | **no SEC2 RTOS and no BOOTSTRAP_FALCON**, so FECS/GPCCS could never start; LS images were zero-filled; scrub poll inverted (waited for 0x10c & 6 to SET); image PIO overwrote the BL's DMEM args; RMW modelled as plain writes; SEC2 fallback base 0x3c0000 (now 0x087000); mocks replayed the plan instead of modelling registers. Now real linux-firmware SEC2 v1 + full FECS/GPCCS images, gp102 SEC2 queues, stateful mock with one mutation test per fix | `f8fc91e`, `7d63edf` |
| gr_boot.c | `nvkm_mask` sites written as plain writes; per-TPC 0x419cc0 writes claimed but not emitted; ZBC incomplete; FECS size queries read once instead of polled; **invented unload steps wrote PMC_ENABLE = 0** (`gf100_gr_fini` writes nothing) | `c7f1006` |
| diag.c | security-fault verdict keyed on CPUCTL STARTCPU, which is write-only; unsourced 0x100ce0/0x100ce4 "WPR2" pair replaced by the 0x100cd4 selector read | `454a5ad` |
| vbios_bit.c | no NPDE size override, no image0/imaged remap, 16-bit table/script offsets, no BIT `p`, no DCB/conn/GPIO parsers; header claimed GP106 needs no special POST handling | `e2d59fa` |
| devinit.c | presented as the GP106 POST; it is not (see devinit_pmu). Re-scoped to DCB/IED/DP scripts; opcodes 0x3a/0x4f/0x50/0x98/0x99/0xa9 added; u32 offsets + remap; count heuristics removed | `e2d59fa` |
| devinit_pmu.c (new) | GP106 POSTs through the VBIOS **PMU DEVINIT** application (`gm200_devinit_post`), not host scripts; modelled here. Real-ROM tests PASS-SKIP when ROMs are absent | `e2d59fa`, `6b998d1` |
| edid.c | a bad extension checksum rejected the whole blob; only block 1 parsed; CTA DTDs ignored; d=0 handled inverted; analog sync flags decoded as polarities | `e4da614` |
| disp_mode.c, disp_core.c (new) | core channel modelled as BAR0 method writes at 0x640000+, which execute nothing on GP102 (it is a DMA channel; 0x640000 is USER/PUT + ARMED). Remodelled as core917d/base907c pushbuffers + supervisor 1/2/3 + DP training. HDMI now uses the gk104 registers; TMDS > 340 MHz sets div2. **RASTER_SIZE reversal:** see below | `6b66ba8` |
| uapi.c | rejected real NVK binds (PTE kind in `op.flags[7:0]`; sparse MAP with handle 0); errno choices now follow the kernel; relabelled MIT | `778be52` |
| bridge/ | binding array overflow, exact-start-only UNMAP, BO slots never reused, `unmap_cookie` never called, short replies accepted | `64e6bcc` |
| kext/ | did not compile (`workLoop` override, IOCommandGate::Action arity, no `super`); gate never added to the work loop; `#define free` broke `OSObject::free`; no `registerService`; boot0 == 0 accepted | `68ff50c` |

**RASTER_SIZE reversal.** The 2026-11-05 audit marked `disp_mode.c` [BUG] for
writing totals and changed RASTER_SIZE to the active size. That was the bug:
nouveau's `head907d_mode` writes `m->h.active`/`m->v.active`, but those fields
hold the **totals** (`dispnv50/head.c:298,303` fill them from `htotal`/`vtotal`).
`6b66ba8` restored totals (1080p60 RASTER_SIZE = 2200x1125), now asserted by
`test_disp_mode` and `test_bringup`.

**Not re-audited:** `pmc.c` had no finding in the 2026-11-07 review and was not
changed; it has not been re-read line by line since 2026-11-05. `top.c`
(PTOP reader, ported from v5.15) was not changed either; only its test golden
was. Treat both as reviewed once, not twice.

## 2026-11-05 audit (historical — verdicts superseded above)

Auditor: adversarial-review pass, 2026-11-05. Upstream authority: torvalds/linux
master nouveau (fetched live), NVIDIA/Mesa headers (then cached under
research/reference/; those copies were later removed, canonical locations are
the `third_party/open-gpu-kernel-modules` and `third_party/mesa` submodules,
see research/reference/README.md), Mesa class_parser codegen at commit
`759935bea4bd12c04456d954294890d3cbd630d3`. Comparison was semantic (field
positions, word values, opcode lengths, register sequences, struct layouts).

**Bottom line: 8 of 11 modules CLEAN. 3 real [BUG]s found and fixed, plus 2
test golden vectors corrected. Differential evidence afterwards: Mesa-generated
encoders memcmp-identical to ours; QMD all 64 words identical; 812/812 EDID
fields match edid-decode across a 36-file real-dump corpus; the real-VBIOS
suite parses 5 genuine GP10x ROMs and the DEVINIT interpreter runs a real
GP106 script 0 to INIT_DONE deterministically.**

## Verdicts

| Module | Verdict | Notes |
| --- | --- | --- |
| pmc.c | CLEAN | boot0 decode, NV4/NV5 legacy quirk, GP10x family, GP106=0x136 all match base.c:3213-3257 |
| mmu.c | CLEAN | 64K big-page PTE packing (`pa>>4`, no page-shift trick — gp100 has none), dual-PDE bit0 unset (linkage = aperture), word0=64K/word1=4K, kind[63:56]/VOL/PRIV/RO bits, aperture encodings, desc geometry, unmap teardown — all match vmmgp100.c/vmm.c/vmmgf100.c + hwref |
| pushbuf.c | CLEAN | SEC_OP 1/3/4/7, ADDRESS [11:0]=mthd>>2, COUNT/SUBCH/GPFIFO fields — clc06f.h exact; later memcmp-equal vs Mesa codegen |
| qmd.c | CLEAN | every MW(msb:lsb) matches NVC1C0_QMDV01_07_*; CONSTANT_BUFFER slot stride 64 confirmed; all 64 words later identical to Mesa `NVVAL_MW_SET` output |
| acr.c | 1 [BUG] fixed | `pg106_acr_desc_v1_parse` read `ucode_blob_base`/vpr 4 bytes early — `flcn_acr_desc_v1.u64` field is `__aligned(8)`; base is at 608, vpr 616, size 632 (matches gp102.c:199 cast). Fix: honor the padding |
| acr_boot.c | CLEAN | all falcon register offsets/poll orders/mailbox values match gm200/gp102/v1.c; [SIM-LIMITATION] SEC2 falcon base 0x3c0000 is a stand-in — real value comes from the device nvkm_top table (MMIO 0x022700+i*4, type 0x0d) — must be read on hardware before use |
| devinit.c | CLEAN | all 48 implemented opcode lengths/semantics match init.c handler-by-handler incl. the 0x7a PMC `\|=1` quirk and 0x65 reset sequencing; [RISK] INIT_PLL (0x79) unimplemented — real VBIOSes may emit it (later evidence: script 0 of our real GP106 ROMs does not) |
| vbios_bit.c | CLEAN | BIT scan signature, token walk, 'I' script pointer, PCIR fields, ROM sigs all match bios/{base,bit,pcir,image}.c |
| dispatch.c | CLEAN | runlist entry + all 15 RAMFC words match gk104_chan_ramfc_write one-for-one, nothing skipped; [RISK] BAR0+0x90 doorbell sourced from tinygrad/research doc, cross-check vs PBDMA path before hardware |
| edid.c | 1 [BUG] fixed | DTD byte 10/11 sync extraction: byte 11 is four 2-bit MSB fields ([7:6] H-off, [5:4] H-wid, [3:2] V-off, [1:0] V-wid) per E-EDID §3.10.3; module read it as two 4-bit V fields, dropping H MSBs entirely (harmless on the reference panel whose byte 11 is 0x00). Fixed + corpus regression test |
| disp_mode.c | 1 [BUG] + 1 [RISK] fixed | RASTER_SIZE must carry actives (1920x1080), not totals — head907d_mode writes m->h.active/v.active; two test vectors updated accordingly (totals remain pinned via BLANK_END 191/40). HDMI register block re-keyed from SOR to head (`hoff = head*0x800`, gf119.c:259) |

## Post-fix differential evidence (separate agents, same day)

- Mesa codegen (nv_push_clc06f/clc1c0 generated by class_parser.py at the
  pinned commit): every our-vs-Mesa word sequence memcmp-identical; QMD:
  all 64 words identical, 19 fields compared via Mesa's own generated ranges.
  `pascal/tests/test_mesa_diff.c` (43 checks; mutation-verified: injected
  field-shift bugs are caught).
- EDID corpus differential vs `edid-decode`: 36 real dumps (14 vendors,
  research/reference/edid/corpus/), 812/812 compared fields matched after the
  byte-10/11 fix; regression vectors for the fixed paths.
- Real VBIOS: 5 genuine GP10x ROMs (TechPowerUp dumps via Internet Archive
  captures, carved from NVGI flash wrappers; provenance + sha256 in
  research/firmware/vbios/README.md): BIT parse + token walk in bounds on all;
  real GP106 script 0 through the interpreter -> INIT_DONE, 220 steps, 913
  trace records, run-twice byte-clean; opcode histogram recorded (no
  I2C/AUX/PLL in script 0).
- End-to-end bring-up composition (test_bringup.c): 7-stage pipeline over one
  shared mock bus — probe -> devinit -> ACR boot (3888 steps) -> MMU -> modeset
  -> compute launch -> full-scenario determinism (9821 writes, byte-identical
  twice).

## Outstanding items before hardware (as of 2026-11-05; status 2026-11-07 in brackets)

1. SEC2 falcon base: read the top table on device (0x022700+i*4, type 0x0d);
   do not trust the 0x3c0000 stand-in. [Fallback is now 0x087000, measured on
   a GP106; the table read on the Helios is still owner-side, BLOCKERS B4.]
2. GPFIFO doorbell (BAR0+0x90 / PUT value) not yet cross-checked against the
   PBDMA doorbell path. [Resolved in code by `bb07dd5`: USERD+0x8c GP_PUT.]
3. Helios 300's own VBIOS dump (BLOCKERS B1) — the committed ROMs are
   desktop/other-mobile GP10x substitutes. [Must be the full flash: the
   substitute GP106/GP107 desktop images stop at 0xf000 and lack the PMU
   DEVINIT images.]
4. GP108 ACR blob variant not vendored (research §3.2 mentions newer Pascal
   laptops needed them). [Not used by GP106; BLOCKERS B11.]
