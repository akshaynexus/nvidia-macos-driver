# NOUVEAU-UAPI.md — Phase 4 gate: the nouveau CS uapi subset NVK needs

Status: **documented uapi subset with test vectors — SIMULATION evidence only** (host-side
sources and unit tests; no hardware was touched, nothing here is a hardware claim).
Branch: `pascal-gp106`. Date: 2026-11-05.

This is the Phase 4 gate document (HANDOFF-PASCAL.md §6 Phase 4: "documented uapi subset
with test vectors; bridge code reviewed against `nvk/nvk-macos.patch`'s interface points").

## 0. Sources (all fetched 2026-11-05 into `/tmp/pascal-ref/`)

| Source | Origin | Local copy |
| --- | --- | --- |
| `include/uapi/drm/nouveau_drm.h` | torvalds/linux master | `uapi/nouveau_drm.h` |
| `drivers/gpu/drm/nouveau/nouveau_exec.{c,h}`, `nouveau_dma.h`, `nouveau_uvmm.c`, `nouveau_sched.c`, `nouveau_drm.c` | torvalds/linux master | `linux-nouveau/` |
| Mesa NVK kernel backend `src/nouveau/vulkan/nvkmd/nouveau/*` | mesa `17ca6174` (= `nvk/MESA_BASE`, the base of `nvk/nvk-macos.patch`) | `mesa-nvkmd_nouveau_*` |
| Mesa winsys `src/nouveau/winsys/*`, queue `src/nouveau/vulkan/nvk_queue.c` | mesa `17ca6174` | `mesa-winsys-*`, `mesa-nvk_queue.c` |

Mesa citations below give file:line **in the fetched copies** (upstream path +
function name so they survive rebases). The Linux kernel backend at mesa `17ca6174`
is `nvkmd/nouveau/` (NVK's kernel-abstractor `nvkmd`), which is exactly the layer the
macOS patch duplicates as `nvkmd/nvrm/` — so that directory, not the winsys alone, is
the authoritative call map.

**Licensing.** `pascal/core/{include/pascal/uapi.h,src/uapi.c}` copy struct layouts and
constants from `include/uapi/drm/nouveau_drm.h`, which is GPL-2.0 WITH the
`Linux-syscall-note` exception (the note permits unrestricted userspace use of the ABI).
The core files carry `SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note`: GPL-2.0
for the kext tree, the syscall-note so MIT-licensed NVK userspace stays free to speak
this shape through the bridge.

## 1. ioctl coverage — what NVK actually issues

NVK (mesa `17ca6174`) issues exactly this set through the `nvkmd/nouveau/` backend and
the winsys:

| DRM ioctl | Where NVK issues it (mesa `17ca6174`) | Purpose |
| --- | --- | --- |
| `DRM_IOCTL_NOUVEAU_GETPARAM` | winsys `nouveau_device.c` `nouveau_ws_param` (:362); callers :519, :555, :560, :564, :651, :666, :676; `nouveau_device.c:459` (GET_ZCULL_INFO uses its own ioctl) | device discovery + limits |
| `DRM_IOCTL_NOUVEAU_VM_INIT` | winsys `nouveau_device.c:510-515` | reserves kernel-managed VA; success ⇒ `has_vm_bind` |
| `DRM_IOCTL_NOUVEAU_CHANNEL_ALLOC` / `_CHANNEL_FREE` | winsys `nouveau_context.c:131-148` (3D), :221-225 (video), :116-124 (free) | one channel per queue |
| `DRM_IOCTL_NOUVEAU_NVIF` | winsys `nouveau_context.c:31` (SCLASS query), :56 (SCLASS), :112 (object DEL) | enumerate subchannel classes, drop legacy objects |
| `DRM_IOCTL_NOUVEAU_VM_BIND` | `nvkmd_nouveau_va.c` `vm_bind` (:98-112), `alloc_va` sparse MAP (:145-149), `va_free` UNMAP (:184-197), `va_bind_mem` MAP (:223-244); batched queue path `nvkmd_nouveau_ctx.c:306-365` | GPU VA space management |
| `DRM_IOCTL_NOUVEAU_EXEC` | `nvkmd_nouveau_ctx.c` `nvkmd_nouveau_exec_ctx_exec` (:166-217) → `flush` (:137-165, ioctl at :148); sync-only "empty" exec (:269-276) | pushbuffer submission |
| `DRM_IOCTL_NOUVEAU_GEM_NEW` / `_GEM_INFO` / `_GEM_CPU_PREP` | winsys `nouveau_bo.c:126`, :192, :291-293; prime export :299 | BO lifecycle |

`NOUVEAU_NEWOBJ` does not exist in the current uapi (there is no such ioctl in
`nouveau_drm.h`; object creation goes through NVIF/CHANNEL_ALLOC paths above).

### GETPARAMs the bridge must answer

`PCI_VENDOR (3)`, `PCI_DEVICE (4)`, `GRAPH_UNITS (13)`, `PTIMER_TIME (14)`,
`EXEC_PUSH_MAX (17)`, `VRAM_BAR_SIZE (18)`, `VRAM_USED (19)`, `HAS_VMA_TILEMODE (20)`
(nouveau_drm.h:36-76; mesa query sites listed in the table row above).
`EXEC_PUSH_MAX` must equal `gpfifo_ring/2 - 1` — the same number
`pg106_uapi_exec_push_max()` computes (kernel rule: `nouveau_exec.h:45-52`).
Mesa refuses to run at all below drm version `0x01000301`
(`nouveau_device.c:500-505`) — the bridge's drm_version must report ≥ that.

## 2. Modelled subset (what `pascal/core` implements)

Everything below is modelled as byte-identical C11 structs in
`pascal/core/include/pascal/uapi.h` with `_Static_assert` size/offset pins, and
validated by `pascal/core/src/uapi.c`. Test vectors: `pascal/tests/test_uapi.c`.

### 2.1 `struct drm_nouveau_sync` → `struct pg106_uapi_sync` (nouveau_drm.h:230-251)

`{u32 flags; u32 handle; u64 timeline_value}`, 16 B. Flags bits [3:0] select the type:
`SYNCOBJ (0x0)` or `TIMELINE_SYNCOBJ (0x1)`. Kernel rejects any other type with
`-EOPNOTSUPP` (`nouveau_sched.c` ~:145-148, `nouveau_job_add_deps` path). Implemented:
`pg106_uapi_sync_check`.

### 2.2 `struct drm_nouveau_vm_init` → `pg106_uapi_vm_init` (nouveau_drm.h:263-274)

`{u64 kernel_managed_addr; u64 kernel_managed_size}`. Kernel checks
`check_add_overflow(addr, size)` and `addr+size ≤ NOUVEAU_VA_SPACE_END`
(`nouveau_uvmm.c:1908-1916`); VA space is 2^47 (`nouveau_uvmm.c:35-37`). NVK reserves
the low region and gives UMD the rest (`nouveau_device.c:510-515`, `TOP = 1<<40`).
Implemented: `pg106_uapi_vm_init_validate`.

### 2.3 `struct drm_nouveau_vm_bind(+_op)` → `pg106_uapi_vm_bind(+_op)` (nouveau_drm.h:282-378)

Op queue: `{op: MAP(0)/UNMAP(1), flags: SPARSE(1<<8), handle, pad, addr, bo_offset,
range}`; request: `{op_count, flags: RUN_ASYNC(0x1), wait/sig counts+ptrs, op_ptr}`.
Kernel rules implemented in `pg106_uapi_vm_bind_validate` /
`pg106_uapi_vm_bind_op_check_map`:

- per-op `addr`, `range` page-aligned, `range > 0` (`nouveau_uvmm_validate_range`,
  `nouveau_uvmm.c:1008-1017` — kernel checks `& ~PAGE_MASK`; `range == 0` never comes
  from NVK and is rejected by the bridge);
- ops stay inside the 2^47 VA space and out of the kernel-managed region
  (`drm_gpuvm_range_valid` bounds installed by `nouveau_uvmm.c:1943-1948`);
- MAP: `handle != 0`, `bo_offset` page-aligned, `bo_offset < bo_size`,
  `range ≤ bo_size - bo_offset` (`bind_validate_op`, `nouveau_uvmm.c:1048-1065`);
- sync waits/signals only with `RUN_ASYNC` (`nouveau_sched.c:43-46, 56-59` — sync jobs
  carrying syncs are `-EINVAL`);
- sync type check per §2.1;
- bridge-strict (kernel today ignores these): unknown `vm_bind.flags` bits, unknown op
  `flags` bits, `pad != 0` (uapi doc: "should be 0", nouveau_drm.h:316-319). The bridge
  rejects so a confused UMD cannot smuggle future-flag bits; NVK sends only documented
  values.

**Stateless-only:** the sparse-region bookkeeping (UNMAP+SPARSE must match a sparse
region exactly; unmap/sparse overlaps against queued jobs — `nouveau_uvmm.c:34-56` doc
comment, `bind_validate_map_sparse`/`bind_validate_map_common` ~:1067-1140) needs the
per-client region table and lives in the kext bridge, not the portable core. Same for
`handle → BO` existence (`-ENOENT`).

### 2.4 `struct drm_nouveau_exec(+_push)` → `pg106_uapi_exec(+_push)` (nouveau_drm.h:386-434)

Push: `{u64 va; u32 va_len; u32 flags: NO_PREFETCH(0x1)}`; request:
`{channel, push_count, wait/sig counts+ptrs, push_ptr}`. Kernel rules implemented in
`pg106_uapi_exec_validate`:

- `push_count ≤ ring/2 - 1` (`nouveau_exec_ioctl_exec`, `nouveau_exec.c:384-390`;
  formula `nouveau_exec.h:45-52`);
- per push `va_len ≤ 0x7fffff` = `NV50_DMA_PUSH_MAX_LENGTH` (`nouveau_dma.h:48`,
  checked `nouveau_exec.c:211-219`);
- `va_len > 0`; `va` and `va_len` 4-byte aligned ("the hardware limit on all current
  GPUs" — Mesa asserts exactly this, `nvkmd_nouveau_ctx.c:201`; PBDMA fetches dwords);
- push flags only `NO_PREFETCH` (bridge-strict; kernel ignores unknown bits);
- sync type check per §2.1.

**Semantics the bridge must reproduce** (not statelessly validatable): EXEC runs
asynchronously; all waits complete before any push reaches the PBDMA and all signals
fire after the job's done fence (`nouveau_sched.c` `nouveau_job_submit` →
`nouveau_job_add_deps` / `nouveau_job_fence_attach` ~:246-267). The channel must have
been created through this uapi (`-ENOENT` unknown channel, `-ENODEV` killed,
`nouveau_exec.c:373-381`).

### 2.5 Everything the core does *not* model

`GEM_NEW/GEM_INFO/CPU_PREP` (plain handle bookkeeping on the bridge side),
`CHANNEL_ALLOC` (RM object tree on the kext side), `NVIF` SCLASS/DEL (the bridge
answers with the fixed Pascal class list: `PASCAL_COMPUTE_B 0xC1C0`,
`PASCAL_CHANNEL_GPFIFO_A 0xC06F`, CE — see HELIOS300-PASCAL-RESEARCH.md §3.1),
`GET_ZCULL_INFO` (mesa queries it, `nouveau_device.c:459`; values are fixed
GP106 constants for the bridge).

## 3. Test vectors (`pascal/tests/test_uapi.c`)

Golden vectors, all cross-cited in-file:

1. **Wire layout**: sizes 16/16/16/40/40/16/40 and key `offsetof`s (`sync.handle@4`,
   `vm_bind.op_ptr@32`, `exec.push_ptr@32`) against `nouveau_drm.h`; all flag constants
   verbatim.
2. **`pg106_uapi_exec_push_max`**: 0→0, 1→0, 2→0, 8→3, 940→469 (`nouveau_exec.h:45-52`).
3. **VM_INIT**: accept {1 GiB @ 1 GiB}; reject overflow, reject end > 2^47.
4. **NVK-shaped VM_BIND sequence**: MAP 64 KiB at first UMD VA + UNMAP, RUN_ASYNC, one
   timeline wait + one plain syncobj signal → accept; sync-mode with syncs → `-EINVAL`;
   unknown op/sync types → rejected; per-rule rejects (unaligned addr/range/bo_offset,
   empty range, bad op, `pad != 0`, handle 0, kernel-region overlap incl. straddle,
   past VA-space end, BO cross-check `range > bo_size - offset`, pointer/count
   agreement, no VM_INIT).
5. **NVK-shaped EXEC sequence (the layering vector)**: the pushbuffer words are built
   with the **existing** `pg106_pb_*` core API (`SET_OBJECT` PASCAL_COMPUTE_B subch 4 →
   `0x20014000`/`0xC1C0`, `SEND_PCAS_A` non-inc → `0x600060AD`, IMMD, `END_PB_SEGMENT`;
   5 words = `va_len 20`) — the same encoders the kext bridge will run — then wrapped in
   `pg106_uapi_exec_push` and accepted by `pg106_uapi_exec_validate`. Includes the
   sync-only "empty exec" flush NVK uses (`nvkmd_nouveau_ctx.c:269-276`). Per-rule
   rejects: `va_len > 0x7fffff`, unaligned `va`/`va_len`, empty push, unknown flags,
   `push_count` over the ring limit (accepted again on a bigger ring), bad sync type,
   NULL-array/count mismatch.

Run: `cd pascal && make test` — `test_uapi: 0 failure(s)` (69 PASS lines); full suite
green 2026-11-05 including `helios300-gp106-simtest.py`.

## 4. Bridge interface points in `nvk/nvk-macos.patch`

The patch (base mesa `17ca6174`) adds a **second `nvkmd` backend**,
`src/nouveau/vulkan/nvkmd/nvrm/`, that speaks Apple **NVRM RM ioctls** instead of the
nouveau CS uapi. Every interface point below cites `nvk/nvk-macos.patch:LINE` (patch
line numbers; each `+` line inside the file block). The replacement surface is our
Pascal kext UserClient + the uapi shape of §2.

### 4.1 Transport (patch `nvrm_xnu.c`, :11658-11860)

| Interface point | Patch evidence | Replacement surface |
| --- | --- | --- |
| IOKit UserClient open: `IOServiceOpen(IOServiceMatching("NVAccel"/"NVRM"), type)` | :11735-11749 | Pascal GPL kext's `IOUserClient` (NVRMUserClient shape, research doc §8), type-gated on GP10x IDs |
| Selector table `{SEL_GO=0, SEL_ESC=1, SEL_GETFD=2, SEL_BINDFD=3}` | :11681 | UserClient selector map; SEL_GO carries the uapi-shaped requests |
| RM escape pass-through `IOConnectCallMethod(conn, SEL_ESC, &cmd, …)` | :11772 | Not needed: GETPARAM/VM_BIND/EXEC become first-class selectors instead of opaque NVOS* blobs |
| fd↔connection binding `IOConnectCallScalarMethod(SEL_BINDFD)` + `/dev/null` fd trick | :11729, :11755, :11846-11852 | One UserClient connection per NVK client; no fd emulation needed |
| Event wait `IOConnectCallScalarMethod(conn, 6, {seen,timeout_us})` | :11760-11766 | UserClient wait selector (or mach semaphore / IOSyncEvent) |
| Mappings `IOConnectMapMemory64(conn, 0, …, kIOMapAnywhere[, WC])` | :11797-11799 | UserClient memory-map selector for BOs + one user doorbell page |

### 4.2 RM escape set (patch `nvRmApi.c`, :7118-7454; trace switch :11777-11786)

| NV escape (call site) | Patch line | uapi replacement |
| --- | --- | --- |
| `NV_ESC_CARD_INFO` | :7448 | `GETPARAM PCI_VENDOR/PCI_DEVICE` + kext device probe |
| `NV_ESC_RM_ALLOC` (`NVOS21`) | :7168 (used for device :10747?, TSG :8775, channel :8811, `NV50_MEMORY_VIRTUAL` :11509) | CHANNEL_ALLOC (channel/TSG) + VM_BIND MAP (VA objects); GETPARAMs for device facts |
| `NV_ESC_RM_ALLOC_MEMORY` (`NVOS02`) | :7185 (`NV01_MEMORY_SYSTEM_OS_DESCRIPTOR` :9935) | GEM_NEW-equivalent UserClient alloc |
| `NV_ESC_RM_FREE` (`NVOS00`) | :7201 | implicit via UserClient object lifecycle |
| `NV_ESC_RM_CONTROL` (`NVOS54`) | :7217 — GPFIFO schedule `NVA06F_CTRL_CMD_GPFIFO_SCHEDULE` :8825, `NVC36F_…_SET_WORK_SUBMIT_TOKEN_NOTIF_INDEX` :8832, `NVC36F_…_GET_WORK_SUBMIT_TOKEN` :8840 | folded into EXEC/doorbell design: scheduler owns the GPFIFO; no per-channel RM controls |
| `NV_ESC_RM_MAP_MEMORY_DMA` / `UNMAP_MEMORY_DMA` (`NVOS46/47`) | :7236 / :7254 | **VM_BIND MAP/UNMAP ops** (`pg106_uapi_vm_bind_op`) — this is the direct analogue of the patch's `nvkmd_nvrm_va.c` (:11509 alloc `NV50_MEMORY_VIRTUAL`, :11569 bind via MAP_MEMORY_DMA) |
| `NV_ESC_RM_MAP_MEMORY` / `UNMAP_MEMORY` (`NVOS33/34`) | :7304 / :7368 (CPU maps; usermode doorbell page :10679, :10687) | UserClient map selector for CPU-visible BOs + doorbell page |
| `NV_ESC_RM_DUP_OBJECT` (`NVOS55`) | :7424 (IOSurface import :9890-9905) | UserClient import selector (IOSurface → BO handle) |
| `NV_ESC_REGISTER_FD`, `ALLOC/FREE_OS_EVENT` | :7380, :7393, :7406 | UserClient event allocation/wait selector (§4.1) |
| `NVRM_ESC_SURF_VRAM` 0xE2 / `SURF_DIRTY` 0xE3 (private escapes, `nvrm_surfshare_abi.h`) | :11612-11613, used :9890, :9662 | offload path (Phase 6): IOSurface handoff stays on the Intel side; dGPU renders into IOSurface-backed BOs |

### 4.3 Submission path (patch `nvkmd_nvrm_ctx.c`, :8125-9456)

Patch mechanism | Patch evidence | uapi replacement |
| --- | --- | --- |
| Channel class `KEPLER_CHANNEL_GPFIFO_C 0xA26F` (`class/cla26f.h`, vendored from OGKM 570 SDK :6850) | :6893, :8811 | `PASCAL_CHANNEL_GPFIFO_A 0xC06F` via CHANNEL_ALLOC (Phase 0 classes) |
| GPFIFO entries + push built in userspace (`NVA16F_GP_ENTRY1_SYNC_*` style) | :8640-8660 (SYNC bit pick) | **EXEC pushes**: `pg106_uapi_exec_push` array; entry encoding stays in the kernel-side core (`pg106_gpfifo_pack`) |
| Semaphore acquire/release as push methods (`NVC36F SEM_ADDR_LO/HI, PAYLOAD, SEM_EXECUTE`) | :8697-8720 | `drm_nouveau_sync` waits/signals on EXEC/VM_BIND (§2.1, §2.4) |
| Doorbell: write submit token to `usermodeMap + NVC361_NOTIFY_CHANNEL_PENDING` | :9050-9051, :9222-9223 | bridge kicks its GPFIFO after applying EXEC; NVK never sees a doorbell |
| Progress/error via mapped notifiers (`NV_CHANNELGPFIFO_NOTIFICATION_TYPE_ERROR/WORK_SUBMIT_TOKEN`) | :8261, :9090-9099 | syncobj signal on job completion + UserClient event wait |
| Sync objects = 4 KiB mapped GART semaphore each (`nvkmd_nvrm_sync.c`) | :10936 (alloc_mapped_mem in sync_init) | drm-syncobj-like handles managed by the bridge; `pg106_uapi_sync` on the wire |

**Net effect:** the patch's whole RM-escape surface (§4.2) collapses to GETPARAM +
VM_INIT + CHANNEL_ALLOC(+NVIF class query) + GEM lifecycle + VM_BIND + EXEC (§1), with
the doorbell/notifier machinery moving behind the bridge's EXEC implementation. That is
precisely the "second backend shaped like the nouveau CS uapi" the architecture section
calls for (HANDOFF-PASCAL.md §4; HELIOS300-PASCAL-RESEARCH.md §3.3).

## 5. Uncertainties / open questions

1. **Kernel leniency vs bridge strictness.** Linux does not reject unknown
   `vm_bind.flags`/push-flag bits today; our validators do (documented above). If NVK
   ever sends new flag bits after a Mesa rebase, the bridge — not the core constant
   table — must be updated first.
2. **`range == 0` VM_BIND op.** I could not point to an explicit kernel rejection of an
   empty range (`nouveau_uvmm_validate_range`'s PAGE_MASK check passes 0); it is
   rejected here as bridge policy, on the evidence that NVK never sends one
   (`alloc_va` guarantees a positive range).
3. **PAGE_SIZE assumption.** Alignment rules are modelled at 4 KiB
   (`PG106_UAPI_PAGE_SHIFT 12`), matching the reference x86-64 kernel. The macOS side
   uses 16 KiB pages on Apple Silicon — irrelevant for x86-64 Helios 300 targets, but
   the constant is named so a port can revisit it.
4. **drm_version floor.** Mesa requires drm ≥ `0x01000301` before touching any ioctl
   (`nouveau_device.c:500-505`). The bridge must fake a nouveau drm_version; the exact
   `version_patchlevel` reporting needs a decision when the UserClient lands (Phase 3/4
   integration, `BLOCKERS.md` candidate).
5. **GET_ZCULL_INFO.** Queried by the winsys but only consumed for graphics zcull
   setup; whether NVK Pascal paths require real values (vs zeros) is untested here —
   no hardware evidence either way (SIMULATION).
6. **Patch line numbers drift.** All `nvk-macos.patch:LINE` cites are against the
   current file in-repo (12298 lines); any rebase of the patch invalidates exact
   line numbers (the function/escape names remain stable anchors).

## 6. Page pinning and IOSurface lifetime (closes PRIOR-ART-AUDIT P0-1)

Added 2026-11-05, same SIMULATION-only status as the rest of this document.
PRIOR-ART-AUDIT.md §D P0-1: the zero-copy VM-bind path
(OFFLOAD-DESIGN.md §2.1 "the IOSurface's host pages are bound into the GPU's
MMU v2 address space") never stated the page-pinning rule. This section is
that rule.

### 6.1 The rule

1. **Wired for the bind lifetime.** Every page range bound into the GPU VM
   through a VM_BIND MAP op — whether a bridge-allocated BO or an imported
   IOSurface — must be *wired* (page-pinned: IOKit
   `IOMemoryDescriptor::prepare()` on the backing memory, or the xnu
   `vm_map_wire` equivalent) from the moment the MAP is applied until the
   matching UNMAP. Resolving physical addresses once at creation and dropping
   the wiring is forbidden (the VedCloud anti-pattern, §6.2).
2. **EXEC never pins.** EXEC references GPU VAs, not pages; protection is
   owned entirely by the VM_BIND region, so the submit path needs no
   per-submission pin/unpin (matching the uapi: `drm_nouveau_exec_push`
   carries only `va`/`va_len`, and Linux pins GEM pages at
   `nouveau_uvmm`-bind time, not at EXEC time).
3. **Unbind before release — who unpins.** The kext bridge unpins, never the
   userspace caller: the UNMAP handler (and the BO-free path that implies it)
   drops the wiring after the PTEs are torn down. Ordering is mandatory —
   unmap → TLB/pTE invalidate → unpin → free — the same
   "put_pages revokes before free" ordering Linux guarantees
   (`drm_gem_shmem`/`ttm` put_pages) and that a userspace caller cannot be
   trusted to run. The bridge enforces the userspace-visible half already:
   `pg106_br_bo_free` on a BO with active bindings is `-EBUSY` (unbind first;
   `pascal/bridge/src/bridge.c`, `test_pinning_rule` in
   `pascal/bridge/tests/test_bridge.c`), and a MAP overlapping an applied
   binding is `-EINVAL` (`bind_validate_map_common` semantics).
4. **Imported IOSurfaces pin on import.** An IOSurface imported as a BO
   (the offload path's `NVRM_ESC_SURF_VRAM`/`DUP_OBJECT` analogue,
   §4.2) takes its wire at import and holds it for the surface's GPU
   residency; it must not be re-resolved per EXEC even though WindowServer
   may mutate surface contents. The wire protects *placement*, not *contents*
   (cross-process sync is IOSurface busy-tags/fences per
   OFFLOAD-DESIGN.md §2.3, out of scope here).
5. **Teardown order.** On client exit the bridge must drain in the reverse
   order: wait for in-flight EXEC → UNMAP all regions → unpin → free BOs,
   matching OFFLOAD-DESIGN.md §2.4's surface drain (bounded wait on the final
   frame) with the pin release inserted after the unmaps.

### 6.2 The VedCloud traps (research/prior-art/meteor-lake-xe)

- **Lifetime-pin omission (kernels 1.0.10→1.0.11).**
  `meteor-lake-xe/ROADMAP.md:381-393`: the port's
  `lkpi_unmap_mapping_range()` is deliberately empty and "safe only because
  mappings are supposed to be pinned for their lifetime"; Linux's
  `put_pages` revokes user PTEs before the backing is freed, but the port
  cannot revoke — so an idle *mapped* object's pages could be reclaimed
  underneath a live user mapping. Kernel 1.0.11 added the mapping-lifetime
  pin (`nmx_ttm_pin` for the TTM path; the i915 `vm_ops_cpu` WB/WC/UC path
  never had it). Two lessons recorded there, both binding on us: the pin is
  *necessary*, and (their Phase-8 finding) a real defect is not automatically
  *the* cause of any given corruption — the 1.0.11 lifetime pin alone did not
  fix their overwrite, so pinning must be paired with the revoke-on-unmap
  ordering of §6.1.3, not treated as a complete story by itself.
- **Resolve-once-then-drop (the anti-pattern this rule forbids).**
  `meteor-lake-xe/MyIntelGPU/MyIntelGEMBuffer.cpp:415-477`
  (`gemBufferResolvePages`): per page it wraps the kernel VA in an
  `IOMemoryDescriptor`, calls `prepare()`, snapshots
  `getPhysicalSegment()` into `pagesPhys[]` — and then immediately
  `complete()`s and `release()`s the descriptor. The PTEs point at a
  one-time snapshot of physical addresses with no wiring held; if the pager
  moves or reclaims a page afterwards, the GPU DMAs into whatever is there
  now. Correct shape: keep the memory descriptor *prepared* (wired) for the
  whole bind lifetime and `complete()` it only in the unmap path (§6.1.1,
  §6.1.3).
- **Stolen-memory bookkeeping trap.**
  `meteor-lake-xe/MyIntelGPU/MyIntelGPU.cpp:1434-1441` ("Phase 4.2 REMOVED"):
  writing a framebuffer-derived size into `fStolenBase/fStolenSize` shrank the
  reported VRAM to 7.91 MB; the real carve-out comes from
  `detectStolenMemory()` (DSMBASE/GMS). And
  `meteor-lake-xe/MyIntelGPU/MASTER-KNOWLEDGE.md:388`: BIOS stolen memory is
  ~8 MB contiguous (PTE[0..2024]) with a 256 MB max-mappable aperture. For us
  the analogue is: never derive GP106 VRAM/GART extents from framebuffer
  usage — BAR1/VRAM sizes come from PCI config and VBIOS
  (research/firmware/vbios), and GETPARAM `VRAM_BAR_SIZE`/`VRAM_USED`
  (§1) must report the real ones, or NVK's `nvkmd` heap sizing inherits a
  silently wrong aperture.

### 6.3 xnu specifics (open question)

- **Which wire primitive for user pages.** For kext-owned memory,
  `IOMemoryDescriptor::prepare()`/`complete()` is the documented wire pair
  (VedCloud's per-page flow above shows the mechanism). For *user-task* pages
  (an app-created IOSurface imported into the bridge) the candidate is a
  `IOMemoryDescriptor::withAddressOffset(task,…)` + `prepare()`, or
  `mach_make_memory_entry_64` + `vm_wire`-style protection; which one holds
  against an IOSurface that legitimately migrates its backing pages
  (IOSurface supports page migration — PRIOR-ART-AUDIT.md §D P0-1) needs a
  decision before the UserClient transport lands. Until then the rule stands
  in this order of preference: (a) wire via the surface's existing kernel
  backing object if one is handed to the kext; (b) wire the task range for
  the bind lifetime; (c) fall back to the offload design's CE-copy path
  (OFFLOAD-DESIGN.md §2.1 fallback), which needs no pinning at all.
- **Page size.** Wiring granularity is 16 KiB on Apple Silicon vs the 4 KiB
  the validators assume (§5.3, `PG106_UAPI_PAGE_SHIFT`); on the x86-64
  Helios 300 target both are 4 KiB, so the pin accounting can reuse the bind
  ranges verbatim, but a port must re-check.
