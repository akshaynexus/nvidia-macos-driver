# UAPI-ORACLE.md — uapi oracle + tinygrad differential (Phase 4 evidence)

Status: **oracle evidence — live probing was NOT possible on this box; differential
encoding evidence is complete.** Branch: `pascal-gp106`. Date: 2026-11-05.

Deliverables:

| File | License | Role |
| --- | --- | --- |
| `pascal/scripts/uapi_probe.sh` | MIT | Read-only oracle probe (sysfs/DRM-version/GETPARAM only) |
| `pascal/tests/test_uapi_oracle.c` | MIT (same as `pascal/core` uapi.{c,h}) | ABI-layout oracle (compile-time pins vs system `nouveau_drm.h`) + live GETPARAM battery when a nouveau node exists |
| `pascal/tests/test_tinygrad_compat.c` | MIT | Differential test of pushbuffer/GPFIFO/QMD encodings vs tinygrad `ops_nv.py` vectors |

Safety: everything here is read-only. No MMIO/BAR access, no module
load/unload/rebind, no sysfs writes, no DRM master, no mode setting. The only
ioctls issued are `DRM_IOCTL_VERSION` and `NOUVEAU_GETPARAM` on render nodes —
the discovery calls any normal app makes.

## 1. PART A finding: nouveau is NOT bound — live oracle unavailable

This box's NVIDIA GPU is a **GA104 (RTX 3070 Ti Laptop, `10de:24a0`) — Ampere**,
and it runs the **proprietary `nvidia` driver**. Nouveau is not loaded, so there
is no nouveau render node and the live GETPARAM oracle could not run. Per the
mission rules we did NOT attempt to switch drivers. Raw probe output
(`pascal/scripts/uapi_probe.sh`, 2026-11-05):

```
== uapi_probe.sh 2026-11-04T23:13:49Z ==

--- /dev/dri ---
total 0
drwxr-xr-x  2 root root        120 Oct 31 18:56 by-path
crw-rw----+ 1 root video  226,   0 Nov  5 03:50 card0
crw-rw----+ 1 root video  226,   1 Nov  5 03:50 card1
crw-rw-rw-  1 root render 226, 128 Nov  5 03:50 renderD128
crw-rw-rw-  1 root render 226, 129 Nov  5 03:50 renderD129

--- render node owners ---
/sys/class/drm/renderD128 pci=0000:00:02.0 driver=i915
/sys/class/drm/renderD129 pci=0000:01:00.0 driver=nvidia

--- loaded kernel modules ---
nouveau NOT loaded
(proprietary NVIDIA modules loaded:)
nvidia_drm
nvidia_uvm
nvidia_modeset
nvidia

--- lspci class 0x03 kernel drivers (read-only) ---
0000:00:02.0 VGA compatible controller: Intel Corporation Alder Lake-P GT2 [Iris Xe Graphics] (rev 0c)
	Kernel driver in use: i915
0000:01:00.0 VGA compatible controller: NVIDIA Corporation GA104 [Geforce RTX 3070 Ti Laptop GPU] (rev a1)
	Kernel driver in use: nvidia

--- oracle test binary ---
== uapi oracle (see research/evidence/UAPI-ORACLE.md) ==
oracle headers: system <drm/nouveau_drm.h> present, ABI pins active
PASS sizeof(struct pg106_uapi_getparam) == 16
PASS sizeof(struct pg106_uapi_sync) == 16
PASS sizeof(struct pg106_uapi_vm_init) == 16
PASS sizeof(struct pg106_uapi_vm_bind_op) == 40
PASS sizeof(struct pg106_uapi_vm_bind) == 40
PASS sizeof(struct pg106_uapi_exec_push) == 16
PASS sizeof(struct pg106_uapi_exec) == 40
ORACLE /dev/dri/renderD129: drm version 0.0.0 driver="nvidia-drm" date="0"
CONTROL /dev/dri/renderD129: GETPARAM on non-nouveau driver -> rc=-1 errno=2 (REJECTED-AS-EXPECTED), expected failure
ORACLE /dev/dri/renderD128: drm version 1.6.0 driver="i915" date="0"
CONTROL /dev/dri/renderD128: GETPARAM on non-nouveau driver -> rc=-1 errno=13 (REJECTED-AS-EXPECTED), expected failure
SKIP live oracle: no nouveau render node found (this box binds the NVIDIA GPU to the proprietary nvidia driver; recorded in UAPI-ORACLE.md)
test_uapi_oracle: 0 failure(s)
```

(`date -u` prints 2026-11-04T23:13Z; the session date is 2026-11-05 — clock
skew between wall clock and the working date, noted for honesty.)

### 1.1 What the oracle DID validate despite no nouveau

1. **ABI-layout oracle (the point that survives the driver swap).**
   `test_uapi_oracle.c` pins, at compile time against this machine's installed
   `include/uapi/drm/nouveau_drm.h` + `drm.h` (i.e. the REAL kernel uapi
   headers, not our copy): `drm_nouveau_{getparam,sync,vm_init,vm_bind_op,
   vm_bind,exec_push,exec}` sizes and key offsets (`sync.handle @4`,
   `sync.timeline_value @8`, `op.addr @16`, `op.bo_offset @24`,
   `vm_bind.op_ptr @32`, `exec.push.push_ptr @32`, `push.va_len @8`,
   `exec.push_ptr @32`) and every flag constant (`SYNC_SYNCOBJ 0x0`,
   `SYNC_TIMELINE_SYNCOBJ 0x1`, `OP_MAP/UNMAP`, `SPARSE 1<<8`,
   `RUN_ASYNC 0x1`, `EXEC_PUSH_NO_PREFETCH 0x1`) against the
   `pg106_uapi_*` structs in `pascal/core/include/pascal/uapi.h`.
   **All pins compile green** — our model is byte-identical to the kernel uapi
   on the machine. This is exactly the generation-stability the mission
   exploits: the uapi shapes are arch-independent, so pins validated against
   an Ampere-class host hold for the Pascal bridge (the structs carry no
   chipset-dependent fields; `CHIPSET_ID` etc. are *values*, not layout).

2. **Negative controls.** `NOUVEAU_GETPARAM` on non-nouveau DRM nodes fails as
   expected (drm core rejects the unknown driver-specific command range):
   `nvidia-drm` → `errno=2 (ENOENT)`, `i915` → `errno=13 (EACCES)`. Recorded
   so the bridge can reuse the exact behavior table.

3. **Live battery is armed, not lost.** If nouveau ever appears on this box,
   the same binary (re)runs the full GETPARAM set documented in
   NOUVEAU-UAPI.md §1: `PCI_VENDOR(3)`, `PCI_DEVICE(4)`, `BUS_TYPE(5)`,
   `CHIPSET_ID(11)`, `GRAPH_UNITS(13)`, `PTIMER_TIME(14)`,
   `EXEC_PUSH_MAX(17)`, `VRAM_BAR_SIZE(18)`, `VRAM_USED(19)`,
   `HAS_VMA_TILEMODE(20)`, and checks `sizeof(drm_nouveau_getparam) == 16`
   against a live kernel. Expected values on THIS box would be
   `PCI_VENDOR=0x10de`, `PCI_DEVICE=0x24a0`, `CHIPSET_ID` in the GA10x family
   (`0xa4` per nouveau's chipset coding) — Ampere, **not** Pascal `0x13`
   (GP106). That asymmetry is fine: the oracle validates the *protocol/ABI*,
   not Pascal-specific values (which come from NOUVEAU-UAPI.md's Mesa/NVK
   source citations).

## 2. PART B: tinygrad differential (`test_tinygrad_compat.c`)

Sources, fetched 2026-11-05: tinygrad master `tinygrad/runtime/ops_nv.py`
(blob `0a1bfa77`, copy at `/tmp/pascal-tg/ops_nv.py`; pip install into
`/tmp/pascal-tg/venv` for autogen introspection — CPU-only, never imported a
device) and tinygrad `v0.9.1` (May 2024) for history; plus torvalds/linux
`nvif/chan506f.c`, `nvif/chanc36f.c`, `include/nvif/push906f.h` and NVIDIA
OGKM `cl906f.h`/`clc36f.h`/`clc1b5.h`/`clc6c0.h` as kernel-side cross-checks.

### 2.1 Agreements (asserted in the test, all green)

| What | tinygrad evidence | Our side | Result |
| --- | --- | --- | --- |
| Method-header layout: `SEC_OP [31:29]`, `COUNT [28:16]`, `SUBCH [15:13]`, `ADDR [11:0] = mthd>>2` | `nvm()` = `(typ<<28)\|(cnt<<16)\|(subc<<13)\|(mthd>>2)`, `typ=2` everywhere (master :43-45; v0.9.1 :67 — identical) | `pg106_pb_mthd_word` | 7 vectors byte-identical (`0x200120AD` SEND_PCAS_A, `0x20050017` sem-run, `0x20048100`/`0x20018106`/`0x200180c0` CE copy, `0x20012000` SET_OBJECT, `0x200125a6` INVAL_SHADER_CACHES) |
| `typ=2` aliases canonical INC_METHOD | `(2<<28) == (1<<29)` | `PG106_SEC_OP_INC_METHOD == 1` | **Confirmed** — ENCODING-RESOLUTION.md's analysis is exact; our decoder reads tinygrad words back with the right fields |
| `typ=1` is NOT an INC word | never emitted by tinygrad (no `typ=` non-default in the file) | decodes as `SEC_OP=0 GRP0_USE_TERT` (legacy TERT; `TERT_OP` aliases COUNT[17:16]) | Divergence locked as a test: we never emit, never read as INC |
| GPFIFO entry: `GET 33:2`, `GET_HI 39:32`, `LENGTH` = dwords at `62:42`, `LEVEL` bit 41 | `gpentry = addr \| (bytes//4 << 42) \| (1<<41)` (master :97; v0.9.1 :143 identical) | `pg106_gpfifo_pack` | 4 vectors identical at `LEVEL=SUBROUTINE`, `SYNC=PROCEED`, `FETCH=UNCONDITIONAL` |
| `1<<41` means LEVEL=SUBROUTINE — and that is what real submitters do | kernel `nvif_chan506f_gpfifo_push`: `main ? 0 : BIT(9)`; EXEC segments call it with `main=false` | `PG106_GP_LEVEL_SUBROUTINE == 1` | **Resolved**: tinygrad's bit-41 set is not a bug; it matches kernel-nouveau practice for user EXEC segments. Kernel-owned segments (its own control push) use MAIN. Our bridge should do the same split |
| `SYNC` bit = no-prefetch | kernel: `no_prefetch ? BIT(31) : 0` (bit 63); tinygrad always 0 (prefetch allowed) | `PG106_GP_SYNC_WAIT == 1` at bit 63 | identical bit position; our `NO_PREFETCH`→SYNC_WAIT mapping matches the kernel exactly |
| Channel semaphore methods `0x5c/0x60/0x64/0x68/0x6c` (`NVC56F_SEM_ADDR_LO..SEM_EXECUTE`) | one INC run, count=5, subc=0 (:111-114); autogen method numbers | `pg106_pb_inc` | header `0x20050017` + 5 data words identical; run spans exactly `0x5c..0x6c` |
| Compute `SEND_PCAS_A = 0x2b4`, data `qmd_addr >> 8` | master :180 | `PG106_NVC1C0_SEND_PCAS_A == 0x2b4` (clc1c0.h:374) | identical method AND shift; end-to-end word pair identical |
| CE copy methods `0x240/0x300/0x400/0x418` | `NVC6B5` autogen | `NVC1B5` (OGKM clc1b5.h) — same numbers | CE method addresses are generation-stable Maxwell→Blackwell |
| Kernel-side method-header macro | `push906f.h`: `NVVAL(DMA,METHOD_ADDRESS,(m)>>2)\|SUBCHANNEL\|COUNT\|OPCODE` with `INCR=1/NONINCR=3/IMMD=4/ONEINCR=5` at `31:29` (cl906f.h:188-212) | our SEC_OP table | third independent vote for the [31:29] layout |

### 2.2 Divergences (documented, not forced into tests)

| # | Divergence | Detail | Consequence for us |
| --- | --- | --- | --- |
| D1 | `typ<<28` is a shorthand, not the layout | Only `typ=2` (INC) is ever emitted; `typ=1` would be a corrupt legacy-TERT word | Emit canonical SEC_OP only (already our rule). Verified: tinygrad has no other `typ` call sites |
| D2 | tinygrad's GPFIFO always sets LEVEL=SUBROUTINE | `1<<41`; kernel matches for user EXEC segments, uses MAIN for its own segments | Our bridge: kernel-side (bridge) segments at MAIN, EXEC user segments at SUBROUTINE — encode both; `pg106_gpfifo_pack` already takes `level` |
| D3 | tinygrad never emits `END_PB_SEGMENT` (SEC_OP=7) | Its channels are RM-managed (`KEPLER_CHANNEL_GROUP_A` + GPFIFO classes); segment termination is a nouveau-uapi/bridge concern | Keep `pg106_pb_endseg` for our uapi bridge; no tinygrad vector exists — documented, untested against tinygrad |
| D4 | Doorbell: RM usermode write, not nouveau userd | tinygrad: `gpu_mmio[0x90//4] = workSubmitToken` (master :101; v0.9.1 :145); kernel nouveau: `userd 0x8c = GPPut` (`chan506f_gpfifo_kick`), GET read from `userd 0x88` | Our bridge's doorbell is the nouveau-uapi style (userd PUT write). The 0x90-token mechanism is NV-RM-specific and must NOT be ported |
| D5 | GET/PUT sync semantics | tinygrad keeps PUT as a host counter, writes `(put+1) % n` into the USERD `GPPut` register (`AmpereAControlGPFifo.GPPut`, master :650-652, :97-100), then rings the doorbell; kernel nouveau reads GET from userd 0x88 and wraps `cur` at `max` | Same shape as our EXEC model (POST increments PUT after entries are visible). No wire difference — semantics only |
| D6 | Semaphore release style | tinygrad: ONE 5-word INC run, `PAYLOAD_SIZE=64BIT`, then a separate `NON_STALL_INTERRUPT` write for non-timestamp releases (master :111-121); kernel `chanc36f.c`: two runs (`SEM_ADDR_LO/HI,PAYLOAD_LO` + `SEM_EXECUTE`), 32-bit payload | Both are legal spellings of the same 5 methods; our encoder emits whatever the bridge asks. Documented, not asserted |

### 2.3 QMD: tinygrad has NO Pascal path (v03_00/v05_00 only)

tinygrad's class ladder starts at Ampere — `AMPERE_COMPUTE_B 0xc7c0`,
`AMPERE_CHANNEL_GPFIFO_A 0xc56f`, `AMPERE_DMA_COPY_B 0xc7b5` (autogen; the
`next(c for c in [BLACKWELL..., AMPERE...])` selections in `setup_usermode`,
master :404-408). Its QMD versions are:

- `NVCEC0_QMDV05_00` — Blackwell (`0x60` words), `qmd_major_version=5`
- `NVC6C0_QMDV03_00` — Ada/Ampere-class (`0x40` words), `qmd_major_version=3`
  (master :50-54, :287, :293-299; v0.9.1 used the same v03_00)

**There is no Maxwell/Pascal QMD (v01_07 or v00_02) anywhere in tinygrad, past
or present** (checked master and v0.9.1; the NV backend was Ampere-only from
its first commit). So per the mission this is documented, not tested: a
v03_00 QMD image cannot be replayed on Pascal hardware — Pascal wants the
v01_07 identity `QMD_VERSION=7, QMD_MAJOR_VERSION=1` (clc1c0qmd.h).
[Corrected 2026-11-07: Pascal userspace builds v02_01, major 2 minor 1
(`clc0c0qmd.h`/`clc1c0qmd.h`; NAK `qmd.rs`, gallium `nve4_compute.c`).]

[Historical, 2026-11-07: `qmd.c` now builds **v02_01**, the version NAK and
gallium use for Pascal (`bb07dd5`); v01_07 is the Kepler-era layout. The
table below compares the old v01_07 builder and was not redone for v02_01.]

Field mapping v01_07 (ours, `pascal/qmd.h`) vs v03_00 (tinygrad's autogen
`(msb,lsb)` tuples), the bits that matter to a port:

| Field | v01_07 (Pascal) | v03_00 (tinygrad/Ada) | Status |
| --- | --- | --- | --- |
| `QMD_VERSION` | 579:576 (value **7**) | 579:576 (value **0** — tinygrad leaves it clear) | position shared, **value differs** |
| `QMD_MAJOR_VERSION` | 583:580 (value **1**) | 583:580 (value **3**) | position shared, **value differs** |
| `CTA_RASTER_WIDTH` | 415:384 | 415:384 | shared |
| `CTA_RASTER_HEIGHT` | 431:416 | 431:416 | shared |
| `CTA_RASTER_DEPTH` | 447:432 | **463:448** | **moved** (Ada inserted `CTA_RASTER_DEPTH_EN` below it) |
| `CTA_THREAD_DIMENSION0..2` | 607:592 / 623:608 / 639:624 | same | shared |
| `SHARED_MEMORY_SIZE` | 561:544 | 561:544 | shared |
| program address | `PROGRAM_OFFSET` 287:256 (`addr>>8`) | `PROGRAM_PREFETCH_ADDR_LOWER_SHIFTED` 287:256 (`addr>>8`) **plus** `PROGRAM_ADDRESS_LOWER` 1567:1536 (`addr>>4`) and `PROGRAM_PREFETCH_SIZE` 1649:1641 | shared origin, **split fields** on Ada |
| `REGISTER_COUNT` | 1503:1496 | `REGISTER_COUNT_V` **656:648** | **moved** |
| `BARRIER_COUNT` | 1471:1467 | **767:763** | **moved** |
| `RELEASE0_*` | `ADDRESS_LOWER` 767:736, `PAYLOAD` 831:800 | `RELEASE0_ADDRESS_LOWER` **799:768**, `PAYLOAD_LOWER` 863:832 | **moved** |
| const-buffer i | `CONSTANT_BUFFER_ADDR_LOWER(i)` unshifted byte addr | v3: same unshifted; v4+/Blackwell: `*_SHIFTED6` (`addr>>6`) | shared at v3; tinygrad's `set_constant_buf_addr` branches on `ver >= 4` |
| dependent QMD chaining | (no v01_07 equivalent used by us) | `DEPENDENT_QMD0_POINTER` 511:480, `ENABLE` 512:512 — tinygrad chains same-kernel relaunches (master :182) | Pascal-side unused; noted for completeness |

`test_tinygrad_compat.c` locks the shared positions and the moved positions as
compile-checked constants so this table cannot silently rot.

## 3. Test outputs (`pascal/Makefile` targets, 2026-11-05)

```
$ ./build/test_uapi_oracle
test_uapi_oracle: 0 failure(s)            # 21 compile-time ABI pins + 7 runtime CHECKs (§1)

$ ./build/test_tinygrad_compat
test_tinygrad_compat: 0 failure(s)        # 61 CHECKs: headers, e2e words, decode
                                          # round-trips, typ alias table, GPFIFO
                                          # vectors, QMD shared/divergent positions
```

Full-suite note: `make test` is currently red from **concurrent sibling work**,
not from these changes — `tests/test_mesa_diff.c` (new, compile error in its
own macro plumbing), `tests/test_edid_corpus.c` (new, corpus divergences),
`tests/test_disp_mode` (2 failing checks) all belong to other agents editing
`tests/` and `core/` concurrently; `core/src/*.c` was not touched by this
task. Direct verification: every pre-existing binary plus both new binaries
runs green against the current tree (per-binary rc=0 loop, simtest PASS).

## 4. Uncertainties / open questions

1. **No live nouveau oracle.** Everything in §1 is layout-level (compile-time)
   plus negative controls. The armed GETPARAM battery has never run against a
   live kernel; if it ever does, the one thing to watch is `CHIPSET_ID`
   (`0xa4`-family on GA104) and `EXEC_PUSH_MAX` (kernel rule
   `gpfifo_ring/2 - 1`, `nouveau_exec.h:45-52`) — the bridge must compute the
   same number for its ring size.
2. **LEVEL=SUBROUTINE for user EXEC segments** rests on two userspace
   references (tinygrad v0.9.1 + master) and one kernel reference
   (`nvif_chan506f_gpfifo_push` `main=false` for `nvif_chan_gpfifo_push`, the
   EXEC path, `nouveau_exec.c:146`). We could not fetch the exact nouveau
   kernel commit history for *why* (the PBDMA main/subroutine level
   interaction with `gpfifo.post` control methods is the likely reason — the
   kernel writes jump/END_PB_SEGMENT control words into its MAIN-level
   segment around user segments). Pascal hardware behavior is asserted only
   by clc06f.h field documentation; first real submissions (Phase 5, owner
   hardware) should start with MAIN-level entries everywhere — the safe
   subset — and only adopt SUBROUTINE if bridge segment interleaving needs it.
3. **Method-number drift across compute classes.** `SEND_PCAS_A` (0x2b4) is
   stable Pascal→Ada; `SEND_PCAS_B`/signaling moved (Pascal
   `SEND_PCAS_B 0x2b8`, `SEND_SIGNALING_PCAS_B 0x2bc` → Ada
   `SEND_SIGNALING_PCAS2_B 0x2c0`). Locked in the test. If NVK's Pascal path
   ever cites a PCAS2 method, re-check against `clc1c0.h`, not the Ada
   header.
4. **QMD v01_07 ↔ v03_00 table** is derived from clc1c0qmd.h (ours) and
   nv_570 autogen tuples (tinygrad); the autogen is generated from OGKM 570
   SDK headers, so it is authoritative for Ada, but no Pascal silicon was
   consulted (none exists in reach). The moved-field list is complete for the
   fields the simtest/`qmd.h` cover, not for all ~200 QMD fields.
5. **tinygrad revision pinning.** Line numbers cite master blob `0a1bfa77`
   (2026-11-05) and v0.9.1; tinygrad moves fast (the file was heavily
   rewritten between 0.9.1 and master). The *formulas* (`nvm`, gpentry) are
   byte-identical across both revisions, which is the evidence that they are
   ABI-stable, but future tinygrad changes can invalidate line citations
   while keeping the encodings.
