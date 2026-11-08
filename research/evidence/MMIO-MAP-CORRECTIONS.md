# MMIO-MAP-CORRECTIONS — GP106 BAR0 map rebuilt from SDK swref + nouveau

Date: 2026-11-05. Trigger: PORT-AUDIT follow-up + diag-agent finding that
`pascal/core/include/pascal/mmio.h` (and `helios300-gp106-simtest.py`
section 2) carried an envytools **NV50-era** BAR0 map. On GF100+ several
ranges MOVED and one range (a fixed "PCOPY" window) does not exist at all.

Authorities, and who wins where they disagree:

1. **NVIDIA SDK swref headers** (this repo's submodule
   `third_party/open-gpu-kernel-modules`, `src/common/inc/swref/published/…`)
   — highest authority for absolute register addresses. No Pascal
   `dev_top.h` exists in the tree, so PTOP is cited from GA100 (the block
   persisted) + nouveau's GF100+ literals.
2. **nouveau** (torvalds/linux, per-file citations below) — second
   authority; wins for bases the SDK headers do not name (PMU falcon base,
   top-table index, devinit latch, EVO window).
3. **envytools MMIO map** — deprecated for GF100+: correct for NV50-era
   addressing only. Where envytools disagreed with 1/2 above, 1/2 won and
   the old value is recorded in the table as WRONG.
4. **test_top.c golden** (real GP106 top-table capture) — confirms GR
   0x400000 / SEC2 0x3c0000 engine bases on actual silicon.
   [Correction 2026-11-07: the SEC2 golden was not a silicon value. The SEC2
   base measured on a real GP106 is 0x087000 (pascal-egpu `FINDINGS.md:160`);
   `af5c7a5` fixed the golden and `f8fc91e` the acr_boot fallback.]

Policy: no code copied into the repo; every constant cited to SDK header
file:line (macro) or a nouveau file.

## Old vs new map

| Block | Old (envytools/NV50-era) | New (GP106) | Status | Authority |
| --- | --- | --- | --- | --- |
| PMC | 0x000000–0x000fff | 0x000000–0x000fff | unchanged | swref `maxwell/gm107/dev_pmc.h:27` `NV_PMC_BOOT_0 = 0x00000000`; `:38` `NV_PMC_ENABLE = 0x00000200` |
| PBUS | 0x001000–0x001fff | 0x001000–0x001fff | unchanged (comment corrected) | swref `maxwell/gm107/dev_bus.h:26` `NV_PBUS_INTR_0 = 0x1100`; `:43` `NV_PBUS_BAR0_WINDOW = 0x1700`; `:63` `NV_PBUS_BAR2_BLOCK = 0x1714` — PBUS stayed in the 0x001000 page on GF100+ |
| PFIFO | 0x002000–0x003fff | 0x002000–0x003fff | unchanged | swref `maxwell/gm107/dev_fifo.h:26` `NV_PFIFO_FB_IFACE = 0x26f0`; nouveau `nvkm/engine/fifo/gk104.c:446-447` runlist `0x2270/0x2274`, `:749` USERD `0x2254`, `:718` intr `0x2100`, `:647` kick `0x2a00` |
| PTIMER | 0x020000–0x020fff | **0x009000–0x009fff** | **MOVED** | swref `maxwell/gm107/dev_timer.h:26` `NV_PTIMER_TIME_0 = 0x00009400` (the triggering example); `:27` TIME_1 `0x9410`; `:26-40` block registers span 0x9100–0x9480; also `kepler/gk104/dev_timer.h:35` |
| PCOPY | 0x060000–0x06ffff | — | **REMOVED** | envytools NV50-era artifact. GF100+ has no fixed PCOPY/PDI window: the CEs are engines whose bases come from the PTOP device-info table (nouveau `nvkm/engine/top/gk104.c:43,76-79` reads `0x022700+i*4`, type 1/2/3 → CE). Real GP106 top capture (test_top.c golden) confirms engine bases are runtime data. A 0x06xxxx literal on GF100+ is unenumerated |
| PTOP | (absent — 0x022xxx unmapped) | **0x022000–0x022fff** | **ADDED** | nouveau `nvkm/subdev/devinit/gf100.c:101` posted/devinit-ran latch `rd32(0x2240c)`; `:70` disable flags `rd32(0x022500)`; `nvkm/engine/top/gk104.c:43` top table `0x022700+(i*4)`; swref `ampere/ga100/dev_top.h:28-29` names the block: `NV_PTOP_DEVICE_INFO2 = 0x22800`, `NV_PTOP_DEVICE_INFO_CFG = 0x224fc` |
| PFB | 0x100000–0x1fffff | 0x100000–0x1fffff | unchanged | swref `pascal/gp102/dev_fb.h:26` `NV_PFB_PRI_MMU_LOCAL_MEMORY_RANGE = 0x00100CE0`; `maxwell/gm107/dev_fb.h:26` NISO_FLUSH `0x100C10`, `:29` INVALIDATE_PDB `0x100CB8`; nouveau `nvkm/subdev/acr/gm200.c:57` WPR-check `0x100cd4` |
| PPMU | 0x10a000–0x10afff | 0x10a000–0x10afff | unchanged (verified) | nouveau `nvkm/subdev/pmu/base.c:145` `nvkm_falcon_ctor(…, 0x10a000, &pmu->falcon)` — hardcoded for gm107+. No `dev_pmmb.h`/PMU block header exists in the SDK tree for Pascal, so nouveau's literal is the authority; the research-doc value agreed |
| PGRAPH | 0x400000–0x41ffff | 0x400000–0x41ffff | unchanged (re-homed to top-table authority) | test_top.c golden (real GP106 top row: GR type 0 addr 0x400000); nouveau `nvkm/engine/top/gk104.c:76` type 0 → GR; GR falcons inside the window: `nvkm/engine/gr/gf100.c:2648-2649` gpccs `0x41a000` (and fecs `0x418000`) |
| PDISP | 0x610000–0x61ffff | 0x610000–0x61ffff | unchanged (see conflict note) | swref `disp/v04_02/dev_disp.h:26` `NV_PDISP = 0x006F1FFF:0x00610000`; SOR power `0x61c004` (nouveau `nvkm/engine/disp/nv50.c` `nv50_sor_power`) |
| EVO | (absent — 0x640000 unmapped) | **0x640000–0x64ffff** | **ADDED** | nouveau `nvkm/engine/disp/gf119.c:429` head readback `0x640414 + hoff`; `:593/:609` channel windows `0x640000 + ctrl*0x1000`; `:962/:976` core-channel `suspend_put` at `0x640000`. This is bring-up finding F1 |

## Known conflicts / deliberate choices

- **PDISP end 0x61ffff vs SDK device span 0x6F1FFF.** The SDK defines the
  whole display device as 0x006F1FFF:0x00610000. The map deliberately splits
  that envelope into PDISP (direct regs, 0x610000–0x61ffff) and the new EVO
  method window (0x640000–0x64ffff, `gf119.c:593` striding 0x1000 per
  channel). 0x620000–0x63ffff and 0x650000–0x6f1fff stay unmapped gaps — no
  code in this repo touches them. `test_bringup.c` (unchanged, out of scope)
  asserts PDISP-range write accounting that depends on this split.
- **GR/SEC2/CE as engines, not fixed ranges.** On GF100+ engine blocks are
  runtime-discovered via the PTOP table (`pascal/top.h` `PG106_TOP_INDEX`
  0x022700; gk104.c:43). Only PGRAPH keeps a fixed convenience range because
  its top-table address is invariant (0x400000). SEC2's 0x3c0000 stays a
  documented sim stand-in (`acr_boot.h`); on silicon it resolves to an
  unenumerated gap until the top table is read, exactly as `test_bringup.c`
  finding O1 states. [Superseded 2026-11-07: the fallback is now 0x087000,
  measured on GP106; the top-table read on the Helios is BLOCKERS B4.]
- **PTOP block bounds.** Only 0x0224fc/0x022800 (GA100 SDK) and the
  0x02240c/0x022500/0x022700 GF100+ literals pin the block; the 0x022000–
  0x022fff page extent is an inference from those (all within one 4 KiB
  page). On Pascal the PTOP register block itself sits inside this page.
- **PPMU end 0x10afff.** Nouveau only pins the base; the falcon register
  file spans well under 4 KiB (flcn v1 layout), so the 0x1000 extent is a
  convenience, same as the old map.

## Remains envytools-only

Nothing. After this correction every range in `pg106_mmio_map` is pinned to
an SDK swref macro or a nouveau literal. The envytools page is retained in
`/tmp/pascal-ref/envytools-mmio.html` purely as NV50-era background.

## Files changed

- `pascal/core/include/pascal/mmio.h` — map rebuilt (PTIMER moved, PCOPY
  removed, PTOP + EVO added; API shape unchanged).
- `helios300-gp106-simtest.py` — section 2 dict + asserts + PASS line;
  docstring line 8 provenance note. All other sections byte-identical.
- `pascal/tests/test_pmc.c` — MMIO assertions only.
- `pascal/core/include/pascal/diag.h` — comment-only: the old note claimed
  mmio.h mapped PTIMER at 0x020000 (now false); citations updated to
  gm107/dev_timer.h. No constants or logic touched. (diag.h was outside the
  strict file list; the comment-only fix was required to keep the header
  truthful — flagged to the parent session.)
- Dependents audited, **no constant changes needed**: `diag.c`/
  `test_diag.c` already used 0x9400/0x9410, 0x1700, 0x02240c, 0x10a000;
  `acr_boot.c`/`test_acr_boot.c` WPR 0x100cd4 stays PFB; `gr_boot.c`/
  `test_gr_boot.c` PFB 0x100800-0x100ccc + PGRAPH 0x40xxxx stay;
  `disp_mode.c`/`test_disp_mode.c` SOR 0x61c004 stays PDISP and EVO methods
  are offsets (the 0x640000 window is now a named block);
  `test_bringup.c` (out of scope, untouched) assertions re-checked against
  the new map and all still hold.

## Cross-check log (evidence used)

- `NV_PTIMER_TIME_0 = 0x00009400` — gm107/dev_timer.h:26 (diag agent's
  verified example; re-verified in-tree).
- GF100+ devinit reads at 0x2240c/0x22500 — nouveau devinit/gf100.c:70,101
  (fetched live from torvalds/linux master).
- PMU falcon 0x10a000 — nouveau pmu/base.c:145 (fetched live).
- EVO 0x640000 windows — nouveau disp/gf119.c:429,593-609,962-976 (fetched
  live).
- NV_PDISP 0x006F1FFF:0x00610000 — disp/v04_02/dev_disp.h:26 (in-tree
  swref; v04_02 is the GP10x display block generation).
- NV_PTOP DEVICE_INFO2 0x22800 / CFG 0x224fc — ampere/ga100/dev_top.h:28-29
  (no Pascal dev_top.h in tree; block persisted from GF100+, nouveau
  literals at 0x022xxx).
- PBUS/PFIFO/PFB/PMC in the 0x001000/0x002000/0x100000/0x000000 pages —
  in-tree swref gm107/gp102 headers cited per line above.
