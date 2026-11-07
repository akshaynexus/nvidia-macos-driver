# pascal/ — GP106 (GTX 1060 Mobile) driver work for the Helios 300

Three trees, specified by `HANDOFF-PASCAL.md`:

- `core/` — portable C11 library (libc only): the nvkm GP106 subset plus
  NVIDIA-header/Mesa-derived encoders. Builds and tests on plain Linux.
- `kext/` — XNU kext source that links part of the core (Phase 3). Built only
  on macOS by the owner; Linux runs stub tests, a symbol audit and a
  `-fsyntax-only` check against MacKernelSDK.
- `bridge/` — userspace `nvkmd`-shaped backend that speaks the nouveau CS uapi
  subset over an abstract transport (Phase 4): a mock, an in-process
  loopback (tests run it into the kext's dispatcher, `test_kdisp`), and the
  IOKit transport to the kext (macOS).

**Everything here is simulation or unit-test evidence, except where a
section says "hardware".** The kext cross-builds to a linked `MH_KEXT_BUNDLE`
on Linux (`kext/scripts/cross-build-macos.sh`) but has never been loaded; the
Linux hardware harness (`tools/pg106_hwtest.c`) has run on the owner's GP106
(hw-run6: READY; grctx-run6: the GR self-test, `research/hardware/helios300/hw-runs`); no NVK build uses
the bridge yet (`HANDOFF-PASCAL.md` §7; gate status in
`research/evidence/ROADMAP.md`).

## Licensing (per file, SPDX header on each)

| Files | License | Why |
| --- | --- | --- |
| `core/` nouveau-derived: `acr`, `acr_boot`, `devboot`, `devinit`, `devinit_pmu`, `diag`, `dispatch`, `disp_core`, `disp_hw`, `disp_host.h`, `disp_mode`, `disp_vbios`, `fifo`, `gpu`, `gr_boot`, `hwexec`, `intr`, `kdisp`, `mmio.h`, `mmu`, `pm`, `pmc`, `top`, `vbios_bit`, `vbios_fetch`, `vmm`, `vram` and their tests; `tests/mockgpu.h`; `tools/pg106_hwtest.c` | GPL-2.0-only | nvkm is GPL-2.0-only |
| `core/` `ce`, `edid`, `pbdma`, `pushbuf`, `qmd`, `uapi` and their tests; `test_mesa_diff`, `test_tinygrad_compat`, `test_uapi_oracle` | MIT | written from NVIDIA class headers (MIT), the VESA/CTA specs, Mesa (MIT) and `nouveau_drm.h` + the nouveau uapi files (MIT) |
| `kext/` | GPL-2.0-only (`kext/LICENSE`) | links GPL core objects |
| `fb/` (PG106FB.kext, the HDMI IOFramebuffer) | GPL-2.0-only | links GPL core objects (`disp_hw` …); talks to PG106Accel only through the MIT `bridge/include/pg106_kif.h` |
| `bridge/` | MIT | links only MIT core objects (`uapi.c`; the demo also `pushbuf.c`) |

GPL code must never reach the PolyForm-NC trees (`kexts/`, `plugin/`,
`translator/`; `HANDOFF-PASCAL.md` §3). Upstream code is not copied into the
repo: Mesa, open-gpu-kernel-modules and tinygrad are submodules under
`third_party/`; nouveau was read from a sparse torvalds/linux clone
(d48723711bae for the 2026-11-07 round). Citations name file and line.

## core/ modules

Header in `core/include/pascal/`, implementation in `core/src/`, test in
`tests/test_<name>.c` (plus the cross-module tests below).

| Module | What it models | Main upstream |
| --- | --- | --- |
| `pmc` | PMC_BOOT_0 decode, GP10x family, GP106 = 0x136, NV04/05 legacy path | nvkm `device/base.c` |
| `mmio.h` | BAR0 block map (14 blocks: PMC, PBUS, PFIFO, PTIMER 0x9000, PTOP, PFB, PPMU, PGRAPH, PDISP, the 0x640000 display channel window, PBDMA, PGPC, PRAMIN, PCCSR), address→block lookup | SDK swref headers + nouveau literals; old envytools NV50 map replaced (`research/evidence/MMIO-MAP-CORRECTIONS.md`) |
| `top` | PTOP device-info table reader (engine bases, reset/intr lines) | nvkm `top/gk104.c` |
| `mmu` | MMU v2 page tables: PDE3→PDE0 (dual) → PTE, 4K/64K PTEs, per-PDE refcounts, walk/PA decode | nvkm `vmm*.c`, SDK `dev_mmu.h` |
| `pushbuf` | method headers (SEC_OP, 12-bit address) and GPFIFO entries | `clc06f.h` |
| `qmd` | QMD v02_01 builder (what NAK and gallium use for Pascal) | `clc0c0qmd.h`/`clc1c0qmd.h`, Mesa |
| `dispatch` | channel instance (VMM join + RAMFC), runlist entry, NVK's compute program + SEND_PCAS, USERD+0x8c GP_PUT kick | nvkm `fifo/`, `nvif/chan506f.c`, NVK `nvk_cmd_dispatch.c` |
| `pbdma` | consumer-side model: GPFIFO walk, method decode, error bits | `dev_pbdma.ref`, `clc06f.h` |
| `ce` | PASCAL_DMA_COPY_B memcpy/2D + semaphore release on the last chunk | `clc1b5.h`, tinygrad |
| `acr` | ACR HS ucode, WPR/LSB header, LS image and signature parsers | nvkm `acr/`, `falcon/fw.c` |
| `acr_boot` | ACR HS boot plan, SEC2 RTOS boot, cmdq/msgq, `BOOTSTRAP_FALCON` for FECS/GPCCS | nvkm `acr/`, `falcon/`, `sec2/gp102.c` |
| `gr_boot` | post-ACR GR init (RMW steps, ZBC, FECS size polls) | nvkm `gr/gf100.c`, `gm200.c`, `gp100.c`, `gp102.c` |
| `gr_ctx` | GR engine context: topology/tiles/SM ids, the global buffers, the firmware register lists, the golden image through FECS (`gf100_grctx_generate` for `gp104_grctx`), per-channel context (mmio list, golden copy, inst+0x210); `pg106_gpu_gr_selftest` (compute dispatch + QMD semaphore + store kernel) | nvkm `gr/gf100.c`, `gk20a.c`, `gm200.c`, `ctxgf100.c`, `ctxgf117.c`, `ctxgk104.c`, `ctxgm107.c`, `ctxgm200.c`, `ctxgp100.c`, `ctxgp102.c`, `ctxgp104.c`, `fifo/gk104.c` |
| `vbios_bit` | image chain (PCIR/NPDE), BIT tokens, image0/imaged remap, `I`/`p` tables, DCB/conn/GPIO | nvkm `bios/` |
| `devinit_pmu` | GP106 POST: posted bit 0x2240c, gm200 PMU DEVINIT upload + run | nvkm `devinit/gm200.c`, `gf100.c` |
| `devinit` | host init-script interpreter, scoped to what GP10x uses it for (DCB encoder, IED, DP scripts) | nvkm `bios/init.c` |
| `diag` | 0xbad*/0xdead* readback decoder, liveness probe, falcon fault verdict, WPR window read | pascal-egpu FINDINGS, SDK, nvkm |
| `edid` | E-EDID base + all CTA-861 blocks | VESA E-EDID 1.4, CTA-861 |
| `disp_mode` | mode → core917d/base907c method streams in commit order | nvkm `disp/`, nouveau `dispnv50/` |
| `disp_core` | display engine BAR0 side: init, DMA channels + PUT, SOR route, HDMI, supervisor 1/2/3, DP link training vs a simulated sink | nvkm `disp/gf119.c`, `gp102.c`, `dp.c` |
| `disp_vbios` | DCB CCB/I2C entries, connector HPD → GPIO function + polarity, BIT 'U' IEDT/ocfg/oclk IED script selection | nvkm `bios/i2c.c`, `conn.c`, `gpio.c`, `disp.c`, `disp/nv50.c` |
| `disp_hw` | the HDMI display driver over `disp_host.h`: display inst + RAMHT + ctxdmas in VRAM, core/base DMA + cursor PIO channels, supervisors (VPLL + IED via `devinit`), DDC bit-bang EDID, GPIO HPD, AVI/ELD, modes, vblank, flips, cursor. **Hardware code path, run only against `test_disp_hw`'s mock display engine**; init diffed against nouveau's Helios mmiotrace (`scripts/disp_trace_diff.py`) | nvkm `disp/`, `dma/usergf119.c`, `core/ramht.c`, `i2c/`, `gpio/`; `dispnv50/` |
| `uapi` | nouveau GETPARAM/VM_INIT/VM_BIND/EXEC structs and kernel-matching validators | `nouveau_drm.h`, `nouveau_uvmm.c`, `nouveau_exec.c` |
| `vbios_fetch` | VBIOS assembly from ACPI `_ROM` (0x1000-byte reads) or the PCI ROM, image-chain driven, PCIR id gate | nvkm `bios/shadow.c`, `shadowacpi.c` |
| `hwexec` | executes acr_boot/gr_boot step lists on a rd32/wr32/delay bus (time-budget polls, stop at first failure), sw_nonctx pack, PRAMIN VRAM writer | nvkm `instmem/nv50.c`, `gr/gk20a.c`, `gr/gf100.c` |
| `devboot` | the bring-up sequence the kext runs: identify, posted, POST, PTOP, ACR, GR + SEC2 bootstrap; VRAM size | nvkm `devinit/gf100.c`, `gm200.c`, `gr/gf100.c`, `fb/ramgf100.c`, `ramgm200.c`, `ramgp100.c` |
| `pm` | D3cold idle policy state machine (sessions, idle timer, quiesce/off/on/re-boot) | `nouveau_drm.c` runtime PM |
| `kdisp` | kernel side of the bridge wire protocol: handles, VM table + mmu, GPFIFO + fence, syncobjs; SYNCOBJ_WAIT sleeps through `env->wait_event` | `nouveau_uvmm.c`, `nouveau_exec.c`, `nvif/chan506f.c`, `nvc0_fence.c` |
| `vram` | VRAM allocator (nvkm_mm head/tail, `rsvd_tail`), PRAMIN access, gm200/gp102 FB init (MMU debug buffers, remapper off, per-instance big page, VPR check), generic range allocator | nvkm `core/mm.c`, `fb/ram.c`, `fb/gm200.c`, `fb/gp100.c`, `fb/gp102.c`, `instmem/nv50.c` |
| `vmm` | VMMs with page tables in VRAM: core `mmu` + dirty-range upload (`pg106_mmu_sync`), TLB invalidate (0x100cb8/0x100cec/0x100cbc, 0x100c80 polls), instance-block join (0x200/0x208); BAR1/BAR2 bind (0x1704/0x1714), wait (0x1710), flush (0x70000), map | nvkm `mmu/vmmgf100.c`, `vmmgp100.c`, `bar/gf100.c`, `gm107.c`, `g84.c` |
| `fifo` | gp100 host FIFO: PBDMA init, USERD (VRAM, BAR1, 0x2254), one TSG per channel on runlists from PTOP, double-buffered runlist commit (0x2270/0x2274, 0x2284 wait), PCCSR bind/start/stop (0x800000, bit 10/11), preempt (0x2634), GP_PUT kick through BAR1, PFIFO intr + non-stall | nvkm `fifo/gp100.c`, `gk104.c`, `gk110.c`, `gk208.c`, `gm107.c`, `gf100.c`, `nv50.c`, `base.c` |
| `intr` | PMC interrupt routing: gp100 allow 0x160 / block 0x180, unarm/rearm for a filter + work-loop split, unhandled bits blocked | nvkm `mc/gp100.c` |
| `gpu` | the GPU as one object: devboot twice (POST, then everything with the ACR memory in VRAM), BAR2/BAR1, FIFO, interrupts; `pg106_gpu_selftest` = host semaphore release + 64 KiB CE copy on the copy engine's runlist | the above |

Cross-module tests: `test_bringup` (one shared mock bus, probe → devinit →
ACR → MMU → display → submit, run twice), `test_mesa_diff` (encoders vs
Mesa's generated emitters, needs `tests/mesa_ref/`), `test_tinygrad_compat`,
`test_uapi_oracle` (live nouveau oracle, skips without a nouveau render node),
`test_vbios_real`/`test_devinit_real` (fetched ROMs, skip when absent),
`test_edid_corpus` (36 real EDIDs, compared with `edid-decode`),
`test_disp_hw` (the display driver against a register-semantics mock display
engine; with the gitignored Helios ROM also the DCB/IED/VPLL path).

`tests/mockgpu.h` is the register-semantics GPU model behind `test_vmm`,
`test_fifo`, `test_intr`, `test_gpu` and the ACR-memory part of
`test_acr_boot`. It has sparse VRAM whose unwritten words read as poison, and
PRAMIN. Its MMU v2 walker shares no code with `core/src/mmu.c` and reads the
page tables out of the mock VRAM. It has a TLB that only 0x100cbc
invalidates, and BAR1/BAR2 binds. Its runlist parser and PCCSR state machine
feed a PBDMA that walks the GPFIFO through the channel VMM after a BAR1
GP_PUT kick. The PBDMA executes SET_OBJECT, SEMAPHOREA-D, NON_STALL_INTERRUPT
and PASCAL_DMA_COPY_B copies, and PMC interrupt lines are modelled.
`test_acr_boot`'s toy ACR fetches the HS image by falcon DMA: BINDINST, then
the instance block, then a page walk.

## Hardware reference values (owner's nouveau mmiotrace of this GPU)

Register writes nouveau made on the Helios 300's GP106M, read from the
owner-approved trace (`research/hardware/helios300/raw/posttrace2-2026-11-07`,
gitignored). These are register-level only: nothing reconstructs memory
contents from the trace. Where this code computes the same word, a test
checks it.

| What | Trace | Here |
| --- | --- | --- |
| SEC2 BINDINST | 0x4017fe84 (inst block VRAM 0x17fe84000, target VRAM) | `(1 << 30) \| inst >> 12` (`acr_boot.c`, `devboot` ACR memory); `test_acr_boot` checks the encoding and the bound inst |
| ACR inst / page directory | 0x17fe84000 / 0x17fe83000 | ours land higher in the same gap above the WPR (tail first fit), because nouveau allocates other objects there first; a hardware run logs ours (`ACR memory: a = inst, b = page directory`) |
| WPR | 0x17fe40000–0x17fe80000, shadow at 0x17fe00000 | the same alloc size 0x80000 (WPR 0x40000) from the same firmware (`test_acr_boot`), and with nouveau's init order (FB, BAR2, BAR1, then ACR) the same address: `test_gpu` |
| SEC2 reset | PMC_ENABLE bit 14 (0x4000) | PTOP reset line 14 (`test_top` hardware vector) |
| BAR2 / BAR1 bind | 0x1714 = 0x8017febf, 0x1704 = 0x8017feaa | `pg106_gpu_boot` writes exactly these words in simulation: MMU debug buffers (2 × 128 KiB), BAR2 inst, BAR2 tables (booted: 4 + 16 pages for 32 MiB), BAR1 inst, top-down from 6 GiB − 1 MiB (`test_gpu`; `test_vmm` checks the encoding) |
| FIFO channel enable | 0x800004 + chid*8 bit 10 | `gk104_chan_start` mask (`fifo.c`, `test_fifo`) |
| runlist submit | 0x2270 / 0x2274 | `gk104_runl_commit` (`fifo.c`, `test_fifo`) |
| GR golden context | nouveau's `gf100_grctx_generate` window (BIND_POINTER .. WFI_GOLDEN_SAVE): 3628 PGRAPH writes; FECS image size 0x87500 | `gr_ctx.c` makes the same 3628 writes in order, values equal except buffer/instance addresses (`scripts/compare_grctx_trace.py` on `test_grctx`'s `PG106_GRCTX_DUMP`) |

## Linux hardware harness (`tools/pg106_hwtest.c`)

`make -C pascal harness` builds `$BUILD_DIR/pg106_hwtest`. It runs the same
`pg106_gpu` bring-up on the real GPU through
`/sys/bus/pci/devices/0000:01:00.0/resource{0,1,3}` mmaps, with no kernel
driver bound, and ends with GPU work: `pg106_gpu_selftest`, a host
semaphore release and then a CE copy, both VRAM to VRAM. The HS image is
in VRAM, so no host DMA is involved.

- `--probe` only reads registers.
- `--go --fw research/firmware [--vbios rom]` runs everything.
- `--gr` adds, after READY, the GR self-test (`pg106_gpu_gr_selftest`):
  the golden context through FECS, a channel on GR's runlist with its
  context, an EXIT-kernel dispatch whose QMD releases a semaphore, a host
  semaphore, and a store kernel (NAK's SM61 words). **Hardware:** passed
  on the Helios GP106 in grctx-run6 (`gr selftest: ret 0`, store
  0x6a5ec0de).
- It refuses when the function is not 10de:1c20 or a driver is bound, and
  it never writes without `--go`.
- `make test` runs it only against a fake sysfs tree
  (`scripts/hwtest_smoke.sh`).

**Hardware runs** (`research/hardware/helios300/hw-runs`): hw-run6 reached
READY, grctx-run6 passed the GR self-test. Each run needs the coordinator's
explicit go; `scripts/stage-hw-run.sh` stages one, `scripts/hw-run.sh` runs
it from D3cold and restores nvidia + GDM.

`fb/` is PG106FB.kext, the macOS IOFramebuffer for the HDMI port (`fb/README.md`).
`make test` cross-builds it when the macOS SDK tools are configured.

## Build and test

Needs cc (gcc or clang), make, python3. `BUILD_DIR` defaults to `build/` and
may be absolute.

    make -C pascal test BUILD_DIR=/tmp/pg106     # everything below
    make -C pascal all  BUILD_DIR=/tmp/pg106     # core test binaries only
    make -C pascal coverage                      # scripts/coverage.sh
    make -C pascal clean BUILD_DIR=/tmp/pg106

`make test` runs every core test binary, then `kext/` (`test_device_gate`,
`test_core_shim`, `symbol-audit.sh`, `kext-syntax-check.sh`) into
`$BUILD_DIR/kext`, then `bridge/` (`test_bridge`) into `$BUILD_DIR/bridge`,
then `helios300-gp106-simtest.py`. The kext syntax check prints `SKIP`
without clang or MacKernelSDK; `PG106_SDK_FETCH=1` clones the pinned SDK into
`$BUILD_DIR`.

Inputs that are not committed:

| Input | How to get it | Without it |
| --- | --- | --- |
| `tests/mesa_ref/` | `scripts/gen_mesa_ref.sh` (from `third_party/mesa`, else GitLab at the pinned commit); `make all` runs it when missing | `make` stops |
| `research/firmware/vbios/*.rom` | `scripts/fetch_vbios.sh` (Wayback captures, sha256-checked) | real-ROM tests PASS-SKIP |
| MacKernelSDK | `PG106_SDK_FETCH=1` or `PG106_MACKERNELSDK=<path>` | syntax check SKIP |

Committed inputs: `research/firmware/gp106/` (linux-firmware ACR/GR/SEC2,
NVIDIA licence, see its README) and `research/reference/edid/`.

Other scripts: `scripts/coverage.sh` writes
`research/evidence/PHASE0-COVERAGE.{txt,csv}` from a private temp build
(`core/src` only; bridge and kext are not instrumented),
`scripts/edid_corpus_diff.sh` (EDID diff against `edid-decode`),
`scripts/uapi_probe.sh` (read-only nouveau oracle probe).
