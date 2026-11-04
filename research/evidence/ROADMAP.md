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
  - Coverage: `research/evidence/PHASE0-COVERAGE.txt` (best-effort, per-binary
    gcov; last-binary-wins artifact noted, merged report TODO).
