# BRIDGE-NVK-REVIEW — Phase 4 interface review: NVK's Pascal backend over the pg106 bridge

Phase 4 gate, second half (`HANDOFF-PASCAL.md` §6: "bridge code reviewed against
`nvk/nvk-macos.patch`'s interface points"). The first half, the uapi subset with test
vectors, is `NOUVEAU-UAPI.md`.

Status 2026-11-07. Everything below was built and run on Linux only. The
evidence is the pg106 backend source (`nvk/pascal/src/nvkmd/pg106/`), the bridge it
links (`pascal/bridge`, ABI 0.2, `266d3c5`), and `nvk/pascal/scripts/linux-check.sh`:
Mesa's real nvkmd core and util code, the bridge mock, and a loopback that executes host
semaphore methods. Result: 221 checks, 0 failures, also under ASan/UBSan.
**SIMULATION.** No NVK entry point, kext, GPU or macOS was involved.

Sources, as in `NOUVEAU-UAPI.md` §0:
- Mesa `17ca6174` + `nvk/nvk-macos.patch`. This is the tree the backend compiles
  against; the line numbers come from `nvk-pascal.patch`'s base.
- Mesa `759935be` (`third_party/mesa`) for the upstream `nvkmd/nouveau` file:line cites.
- Linux 7.4-rc1 for the nouveau kernel.
- `nvk-macos.patch:LINE` is a line of the patch file.

## 1. Every nvkmd op the Pascal backend implements

Ops are listed in `nvkmd.h` order. Columns:
- **pg106 op**: the backend function, in `nvk/pascal/src/nvkmd/pg106/`.
- **bridge call → wire op**: `pascal/bridge/include/pg106_br_bridge.h`.
- **nouveau semantic mirrored**: kernel side, and the upstream nvkmd backend
  that issues it.
- **test**: the `test_nvkmd_pg106.c` function that pins it.

### pdev

| nvkmd op | pg106 op | bridge call → wire op | nouveau semantic mirrored | test |
| --- | --- | --- | --- | --- |
| `nvkmd_enum_pdev` (patch adds it, `nvk-macos.patch:6693`) | `nvkmd_pg106_enum_pdev` | `pg106_br_iokit_transport_open` (IOKit class `PG106`, render type 1) → `pg106_br_dev_open` → HANDSHAKE | `nvkmd_nouveau_try_create_pdev` (`nvkmd_nouveau_pdev.c`): open the render node, refuse below drm `0x01000301` (`nouveau_device.c:504-505`). The bridge gates on ABI 0.2 and the drm floor in the handshake. | `test_enum_and_dev_info`, `test_enum_rejects`, `test_no_transport` |
| (pdev device facts) | `nvkmd_pg106_create_pdev` | GETPARAM `PCI_VENDOR`, `PCI_DEVICE`, `EXEC_PUSH_MAX`, `GRAPH_UNITS`, `VRAM_BAR_SIZE`, plus bridge-private `VRAM_SIZE` (`0x10000001`), `BOOT0` (`0x10000002`), `READY` (`0x10000003`) | `nouveau_ws_device_new`: GETPARAMs + NVIF device info. Chipset comes from BOOT_0 `[28:20]` (`0x136`) in place of NVIF `chipset`. VRAM size comes from the private param in place of NVIF `ram_user`. READY stands in for "the kernel has a GR context". | same |
| `pdev_ops.get_vram_used` | `nvkmd_pg106_pdev_get_vram_used` | GETPARAM `VRAM_USED` | `nouveau_ws_device_vram_used` | `test_enum_and_dev_info` |
| `pdev_ops.create_dev` | `nvkmd_pg106_create_dev` | new transport + `pg106_br_dev_open` + `pg106_br_vm_init(2^39, 2^39)` | `nvkmd_nouveau_create_dev` opens its own `nouveau_ws_device` (own fd), and VM_INIT reserves `[2^39, 2^40)` (`nouveau_device.c:508-515`). `-EBUSY` (v0: one render client holds the channel) becomes a clean `VK_ERROR_INITIALIZATION_FAILED`. | `test_dev_and_mem`, `test_enum_rejects` (EBUSY) |
| `pdev_ops.get_drm_primary_fd` | NULL | — | no DRM primary node on macOS | — |

### dev

| nvkmd op | pg106 op | bridge call → wire op | nouveau semantic mirrored | test |
| --- | --- | --- | --- | --- |
| `dev_ops.get_gpu_timestamp(_checked)` (`_checked` added by patch, `:6754`) | `nvkmd_pg106_dev_get_gpu_timestamp_checked` | GETPARAM `PTIMER_TIME` | `nouveau_ws_device_timestamp` | `test_dev_and_mem` |
| `dev_ops.alloc_mem` | `nvkmd_pg106_alloc_mem` | `pg106_br_bo_alloc` → BO_ALLOC; then `nvkmd_dev_alloc_va` + VM_BIND MAP over the whole BO | `nvkmd_nouveau_alloc_tiled_mem` → `create_mem_or_close_bo`: GEM_NEW, then VA + bind of the whole BO. Flags go through 1:1 (`static_assert`ed). `COHERENT` is always set (the discrete-GPU rule). The patch's `NVKMD_MEM_BAR` (`:6738`) is dropped. | `test_dev_and_mem` |
| `dev_ops.alloc_tiled_mem` | `nvkmd_pg106_alloc_tiled_mem` | — (`VK_ERROR_FEATURE_NOT_PRESENT` for kind/tile ≠ 0) | `has_alloc_tiled` false: NVK puts the PTE kind on the VA bind, as nouveau without `HAS_VMA_TILEMODE` does. The patch's compressed-image path falls back to a plain BO. | `test_dev_and_mem` |
| `dev_ops.import_dma_buf` | refuses | — | `has_dma_buf` false (no dma-buf on macOS) | `test_dev_and_mem` |
| `dev_ops.import_host_mem` (patch, `:6763`) | `nvkmd_pg106_import_host_mem` | `pg106_br_bo_import_host` → BO_IMPORT_HOST; then VA + VM_BIND MAP | No nouveau counterpart. It mirrors the patch's NVRM `NV01_MEMORY_SYSTEM_OS_DESCRIPTOR` import (`:9935`). The kext wires the pages for the BO's lifetime (`NOUVEAU-UAPI.md` §6.1.4). CPU map = the caller's pointer. | `test_host_import` |
| `dev_ops.alloc_va` | `nvkmd_pg106_alloc_va` | none for a plain VA (userspace `util_vma_heap`); `MAP\|SPARSE` handle 0 for a sparse one | `nvkmd_nouveau_alloc_va` (`nvkmd_nouveau_va.c:144-151`), the same two heaps | `test_sparse_va` |
| `dev_ops.create_ctx` | `nvkmd_pg106_create_ctx` | `pg106_br_ctx_create(channel)` | `nvkmd_nouveau_create_ctx`: exec ctx = CHANNEL_ALLOC, bind ctx = none. Here an exec ctx asks for the next channel id and shares channel 0 on `-ENOENT` (§3). | `test_exec_and_sync` |

### mem

| nvkmd op | pg106 op | bridge call → wire op | nouveau semantic mirrored | test |
| --- | --- | --- | --- | --- |
| `mem_ops.free` | `nvkmd_pg106_mem_free` | VM_BIND UNMAP (via `nvkmd_va_free`), then `pg106_br_bo_free` → BO_FREE | `nvkmd_nouveau_mem_free`. Unbind before release (`NOUVEAU-UAPI.md` §6.1.3): the bridge refuses `-EBUSY` otherwise. | `test_dev_and_mem`, `test_host_import` |
| `mem_ops.map` | `nvkmd_pg106_mem_map` | `pg106_br_bo_map` → BO_MAP + `map_cookie` (`IOConnectMapMemory64`) | `nvkmd_nouveau_mem_map` (mmap of the GEM offset). The internal and client maps share one bridge mapping, because the kext allows one per BO. Imported BOs return the host pointer. | `test_dev_and_mem` |
| `mem_ops.unmap` | `nvkmd_pg106_mem_unmap` | BO_UNMAP when the last user goes | munmap | `test_dev_and_mem` |
| `mem_ops.overmap` / `export_dma_buf` / `sync_*` | NULL | — | `has_overmap`/`has_dma_buf` false; maps are COHERENT, so the core never calls `sync_*` | — |
| `mem_ops.log_handle` | the bridge handle | — | GEM handle | — |

### va

| nvkmd op | pg106 op | bridge call → wire op | nouveau semantic mirrored | test |
| --- | --- | --- | --- | --- |
| `va_ops.bind_mem` | `nvkmd_pg106_va_bind_mem` | `pg106_br_ctx_bind` + `pg106_br_ctx_flush` → VM_BIND MAP `{handle, addr, bo_offset, range, flags = pte_kind}` | `nvkmd_nouveau_va_bind_mem` (`:210-232`). The PTE kind goes in `flags[7:0]` (`nouveau_uvmm.c:1399`). | `test_sparse_va` |
| `va_ops.unbind` | `nvkmd_pg106_va_unbind` | VM_BIND **UNMAP** | **Deliberate difference.** `nvkmd_nouveau_va_unbind` (`:235-249`) sends a handle-0 non-sparse MAP, which the kernel answers with `-ENOENT` (`nouveau_uvmm.c:1278-1283`; `NOUVEAU-UAPI.md` §5.2). UNMAP is what the op means; inside a sparse region it leaves the region sparse. | `test_sparse_va` |
| `va_ops.free` | `nvkmd_pg106_va_free` | UNMAP over the range, then UNMAP\|SPARSE for a sparse VA | `nvkmd_nouveau_va_free` (`:183-200`). The range leaks if unbinding fails, as on nouveau. | `test_sparse_va` |

### ctx

| nvkmd op | pg106 op | bridge call → wire op | nouveau semantic mirrored | test |
| --- | --- | --- | --- | --- |
| exec `ctx_ops.exec` | `nvkmd_pg106_exec_ctx_exec` | `pg106_br_ctx_exec` (EXEC push `{va, va_len, NO_PREFETCH}`) | `nvkmd_nouveau_exec_ctx_exec` (`nvkmd_nouveau_ctx.c:165-219`): same splitting at `EXEC_PUSH_MAX`, with incomplete runs kept in one EXEC. One slot per EXEC is reserved for the release push. | `test_push_splitting` |
| exec `ctx_ops.wait` | `nvkmd_pg106_exec_ctx_wait` | an extra EXEC push: host `SEMAPHOREA..D` **ACQ_GEQ** on the waited context's progress word | **Replaces** the EXEC `in_sync` syncobj (`nouveau_sched.c` `nouveau_job_add_deps`). Same ordering: the acquire precedes the job's pushes on the channel. | `test_exec_and_sync` |
| exec `ctx_ops.signal` / `flush` | `nvkmd_pg106_exec_ctx_signal` / `_flush` | final EXEC push: `WFI` + `MEM_OP_C/D SYS_MEMBAR` + `SEMAPHORE` **RELEASE** of sequence n; then `pg106_br_ctx_flush` → EXEC frame | **Replaces** the EXEC `out_sync` (`nouveau_job_fence_attach`). The signal fires once all work on the channel is done (WFI) and visible in system memory (membar, cf. Linux `2cb66ae604`). The sync records (ctx timeline, n) → value. | `test_exec_and_sync`, `test_device_lost` |
| exec `ctx_ops.sync` | `nvkmd_pg106_exec_ctx_sync` | flush + CPU poll of the progress word, bounded by `NVK_PG106_TIMEOUT_MS` (default 10 s) | `exec_ctx_sync` (`:242-292`): signal own syncobj, `drmSyncobjWait`, empty EXEC for errors. A timeout or a refused EXEC marks the context lost (`VK_ERROR_DEVICE_LOST`, OFFLOAD-DESIGN §2.5). | `test_device_lost` |
| bind `ctx_ops.bind` | `nvkmd_pg106_bind_ctx_bind` | `pg106_br_ctx_bind` (queued, ≤ `PG106_BR_MAX_BINDS`) | `nvkmd_nouveau_bind_ctx` op build (`:391-400`) | `test_bind_ctx` |
| bind `wait` / `signal` / `flush` / `sync` | `nvkmd_pg106_bind_ctx_*` | CPU wait on the syncs → one VM_BIND frame → CPU signal | The kernel orders the bind job after its in_syncs and signals out_syncs when applied (`nouveau_sched.c`). The bridge applies a job before returning, so CPU ordering gives the same result. | `test_bind_ctx` |

### vk_sync (the pdev's only `sync_types` entry)

`nvkmd_pg106_sync`: binary and timeline in one type, like `vk_drm_syncobj`.
- **State.** A value known on the CPU, plus up to 16 (timeline, sequence) → value
  points.
- **Features.** BINARY, TIMELINE, GPU_WAIT, GPU_MULTI_WAIT, CPU_WAIT,
  CPU_RESET, CPU_SIGNAL, WAIT_ANY and **WAIT_PENDING**. The runtime's
  ASSISTED timeline mode asserts WAIT_PENDING on every GPU-waitable type
  (`vk_device.c` `get_timeline_mode`); that is why it is advertised.
- **Device loss.** A lost context's point makes `get_value`/`wait`
  return `VK_ERROR_DEVICE_LOST`.

Tests: `test_exec_and_sync` covers pending/complete/any waits and binary
reset; `test_device_lost` covers loss.

## 2. Why the backend does not use the uapi syncobjs

The bridge offers syncobjs (`pg106_br_syncobj_*`, `drm_nouveau_sync` on EXEC/VM_BIND).
The backend does not use them. Pascal's PBDMA executes host semaphore methods on its
own (`clc06f.h:75-108`). A 64 KiB GART BO per context, holding one progress word and
a ring of control pushes, then gives three things:
1. completion without an interrupt or kext event path, so the v0 kext needs none;
2. GPU-side cross-context waits without kext involvement;
3. the "mapped GART word + bounded CPU poll" scheme OFFLOAD-DESIGN §2.3 already
   specifies, and the NVRM backend runs on Turing+ (`nvRmSemSurf.c`, `nvk-macos.patch:7581`).

The cost is CPU polling (spin, then 10 µs → 1 ms backoff, the EVENT-WAIT policy) instead of a
blocking wait. When the kext gets an event wait selector, `nvkmd_pg106_timeline_wait` is the
one place to change. The 32-bit word is compared wrap-aware; ring slots are reused only after
the flush that used them has completed.

## 3. Channels

NVK opens one exec context per VkQueue and one for its upload queue
(`nvk_upload_queue.c:26`, copy only). The v0 kext provisions one channel, and only one
render connection may hold it: a second process gets `-EBUSY` from VM_INIT (kdev,
2026-11-07). The backend asks for channel ids 0, 1, … and shares channel 0 on `-ENOENT`.
- **Correct:** sharing makes submission order global. That is stronger than
  Vulkan needs, and safe for the upload queue (subchannel 4, copy class only)
  next to one 3D/compute queue.
- **Not correct:** two VkQueues that both set 3D state on one channel.
  NVK's queue init pushes its context state once per queue, so a second 3D
  queue would clobber the first. Apps that use one queue (the Metal plugin
  uses one, `plugin/nvmtl_vk.c` `g_queue`) are fine.
- **Fix:** real CHANNEL_ALLOC/multiple channels in the kext. Ids stay
  0..n-1 (kdev), so the backend needs no change.

## 4. `nvk-macos.patch` interface points and what the Pascal path does with each

GSP/NVRM assumptions in the existing patch, numbered by patch line (the transport, escape and
submission points are detailed in `NOUVEAU-UAPI.md` §4.1–4.3). None of them is modified for
Turing+; the Pascal backend sits beside them.

| # | Interface point | Patch evidence | Pascal path |
| --- | --- | --- | --- |
| 1 | Backend selection: `nvkmd_enum_pdev` → `nvkmd_nvrm_enum_pdev` only | `:6693-6700` | **Hooked.** `nvk-pascal.patch` asks NVRM, then pg106. `NVK_KMD=nvrm\|pg106` limits it to one. The plugin sets `NVK_KMD=pg106` on Pascal. |
| 2 | IOKit transport to NVRM/NVAccel, selectors GO/ESC/GETFD/BINDFD, `/dev/null` fd trick | `nvrm_xnu.c` `:11658-11860`, `:11681`, `:11729-11852` | **Replaced** by `pg106_br_iokit_transport_open`: class `PG106`, render type 1, `SEL_SUBMIT` frames. No fds. The plugin's `_open` rebinding (`nvmtl_icd_open`) is skipped on Pascal. |
| 3 | RM escapes (`NV_ESC_RM_ALLOC/CONTROL/MAP_MEMORY_DMA/DUP_OBJECT…`) | `nvRmApi.c` `:7118-7454` | **Bypassed.** GETPARAM / VM_INIT / BO_* / VM_BIND / EXEC frames. |
| 4 | Device facts from RM controls (GR_GET_INFO SM_VERSION, FB_GET_INFO, class list) | `nvkmd_nvrm_pdev.c` `:10097…`, SM_VERSION `:10592`, classes `:10215-10240` | **Replaced.** A fixed GP106 table plus GETPARAMs (§1 pdev). Classes are fixed: PASCAL_B / PASCAL_COMPUTE_B / PASCAL_DMA_COPY_B / KEPLER_INLINE_TO_MEMORY_B / FERMI_TWOD_A / PASCAL_CHANNEL_GPFIFO_A. |
| 5 | Channel `KEPLER_CHANNEL_GPFIFO_C`, TSG, ctxshare, GPFIFO schedule controls | `:6893`, `:8775-8840` | **Bypassed.** The kext owns the channel; `PASCAL_CHANNEL_GPFIFO_A` is reported as `cls_gpfifo`. |
| 6 | Userspace-built GPFIFO + usermode doorbell (`NVC361_NOTIFY_CHANNEL_PENDING`) | `:9050`, `:9222`; usermode alloc `:10679-10687` | **Bypassed.** EXEC carries push VAs; the kext packs GPFIFO entries and kicks GP_PUT at USERD+0x8c (`PBDMA-DOORBELL.md`). Pascal has no usermode class. |
| 7 | Global submit slots, `nvkmd_nvrm_slots[]`, gsub/defer machinery, `NVRM_610_*` conf | `:7972`, `:8207`, `:8092` | **Not used.** One timeline per exec context (§2). |
| 8 | Event waits (`NVRM_EVENT_WAIT`, `nvrm_xnu_wait_event`) | `:8347`, `:8381` | **Not used.** Bounded CPU poll (§2). A kext event selector can replace it later. |
| 9 | Sync = mapped semaphore surface per sync (`nvkmd_nvrm_sync.c`, `nvRmSemSurf`) | `:10877…`, `:7581` | **Replaced** by `nvkmd_pg106_sync` with per-context progress words: same mechanism, no RM semaphore-surface objects. |
| 10 | Notifier errors (`NV_CHANNELGPFIFO_NOTIFICATION_TYPE_ERROR`) | `:8261`, `:9090-9099` | **Replaced.** A refused EXEC (`-ENODEV`, killed channel, `nouveau_exec.c:373-381`) or a timeout → `DEVICE_LOST`. |
| 11 | Host-memory import via OS descriptor + `has_host_import` probe | `:9935`, `:10738`, `:6746`, `:6763` | **Replaced** by BO_IMPORT_HOST. `has_host_import` = `PG106_BR_HAS_BO_IMPORT_HOST`. |
| 12 | IOSurface family-VRAM import (`nvmtl_nvk_stage_import`, `NVRM_ESC_SURF_VRAM` 0xE2) | `:6822`, `:9647`, `:11612` | **Not used on Pascal.** The plugin's `nvmtl_vk_surface_share_on` is off on a headless GPU, and Pascal is always headless. |
| 13 | Surface-dirty / WindowServer completion mark (`nvmtl_nvk_surface_dirty` → `NVRM_ESC_SURF_DIRTY` 0xE3) | `:4338`, `:6832-6833` | **Guarded.** `nvk-pascal.patch` returns `0x80000002` (not supported) on any backend but NVRM. Before, `nvkmd_nvrm_surface_dirty` would `container_of` a pg106 device. The plugin then copies out (§5). |
| 14 | `NVKMD_MEM_BAR` placement (VRAM through BAR1) | `:6738` | **Dropped** by `nvkmd_pg106_bo_flags`: VRAM with `CAN_MAP` is BAR1-mapped by the kext. |
| 15 | `get_gpu_timestamp_checked` | `:6754` | Implemented (GETPARAM `PTIMER_TIME`). |
| 16 | Sample-depth contract gated on `nvkmd_nvrm_pdev_ops` && SM 120 | `:4514-4517` | False on Pascal (other pdev ops), as intended. |
| 17 | Raw sm_120 SASS dispatch (`nvmtl_nvk_sass.c`) and NAK `sm == 120` paths | `:2803`, `:11918`; `:156`, `:636`, `:1598-1628` | Unreachable. The plugin gates on the device name "GeForce RTX 50xx" (`plugin/nvmtl_sass_gate.h`) and NAK on `sm == 120`; Pascal is SM 61. |
| 18 | `nvmtl_nvk_sparse_bind_v1` export (the plugin enables `sparseBinding` if present) | `:9416-9417` | Exported regardless of backend. The pg106 bind ctx implements sparse binds with CPU ordering (§1 ctx). |
| 19 | `PERF_BOOST` RM control | `:10708` | **Not applicable.** No PMU firmware on Pascal, so no reclocking (`HANDOFF-PASCAL.md` §4). |
| 20 | Build: the `nvrm_files` list, IOKit/CoreFoundation frameworks on darwin, OGKM include dirs | `meson.build` hunk `:2773-2912` | `nvk-pascal.patch` adds `pg106_files` (backend + `bridge.c` + `uapi.c` + `transport_iokit.c`) and three include dirs. It reuses the darwin framework deps. |

## 5. The offload consumer (plugin) and what it expects of this backend

The plugin's headless path (`plugin/nvmtl_vk.c` `nvmtl_gpu_headless`, `NVMTLObjects.m`
`nvmtlSurfaceOutDirtied:`) does three things per GPU write:
1. locks the IOSurface;
2. reads the private image into the surface's pages, preferring a
   copy-engine write into those pages imported as host memory
   (`nvmtl_vk_image_read_host`, i.e. `import_host_mem` → BO_IMPORT_HOST);
3. unlocks, which bumps the seed.

That is OFFLOAD-DESIGN §2.1–2.3 (host-page-backed surfaces, completion before the
seed/mark) using only ops reviewed above. On Pascal the plugin is always on this path
(`38beec8`).

## 6. Licensing check (2026-11-07)

- **Backend and test programs.** `nvk/pascal/**` and `pascal/tests-mac/**`
  are MIT (SPDX header in every source file; scripted check). They include
  only Mesa headers, `pascal/bridge/include/*.h` (MIT) and
  `pascal/core/include/pascal/uapi.h` (MIT).
- **What NVK links from `pascal/`.** Exactly `bridge.c`, `uapi.c` and
  `transport_iokit.c`, all MIT (`pascal/bridge/README.md` "Licensing").
- **plugin/ and kexts/.** `grep` finds no include of `pascal/`, `pg106*`
  or nouveau/nvkm headers. The plugin only *names* the IOKit classes `PG106`
  and `PG106Accel` (`plugin/nvmtl_vk.c`). The only `nvkm*` hits in `kexts/`
  are NVIDIA's pre-existing `nvkms*` headers from open-gpu-kernel-modules,
  unrelated to nouveau.
- **Kext boundary.** The GPL-2.0 kext is reached only through the IOKit
  user-client ABI (`IOConnectCallStructMethod` / `IOConnectMapMemory64`).
  No kext header is included by NVK, the bridge or the plugin.

## 7. Open items and owner decisions

1. **Who may open the RENDER user client — decided by the owner,
   2026-11-07: every local user.** kdev's split (2026-11-07):
   - Type 0 DEVICE: root-only diagnostics + SUBMIT, used by `pg106_probe`.
   - Type 1 RENDER: SUBMIT, GET_VERSION and GET_STATUS, with
     per-connection VM/BOs/handles.

   RENDER is open to every local user by default (`8374730`), which is what
   NVK inside ordinary apps needs. Boot-arg `pg106_render_users=0` (or
   Info.plist `PG106RenderUsers = false`) restricts it to administrators;
   DEVICE stays admin-only. Runbook steps 3–4 run without `sudo`. `-EACCES`
   from the transport now means the restriction is set; the backend logs it
   and offers no device. The accepted trade-off: any local process can reach
   the kext's request parser and wake the dGPU, with isolation only as good
   as the kext's per-connection checks.
2. **PG106Accel.** A non-GPL `IOGraphicsAccelerator2` that publishes
   `MetalPluginName` for the GP106 is required before Metal apps see a
   Pascal `MTLDevice` (OFFLOAD-DESIGN §3.3). It is not written. The Vulkan
   path and runbook step 4 `--vulkan` do not need it.
3. **One render client at a time** (v0 kext, one channel): a second
   process gets `VK_ERROR_INITIALIZATION_FAILED` from `vkCreateDevice`.
4. **Full NVK build.** Not done on Linux (no bindgen/cbindgen/mesa_clc).
   NAK's SM 6.1 code generation and NVK's Pascal 3D/compute paths are
   upstream code this work does not touch, but nothing here exercised them
   either.
5. **Polling instead of event waits** (§2). Fine for bring-up. Battery and
   CPU cost under load are unmeasured.
6. **GRAPH_UNITS.** The skeleton/mock answer 0; the backend falls back to
   per-board SM counts (10 for 1C20). The kext should report the real value.

## 8. Gate assessment (for the coordinator; the ROADMAP table is not edited here)

The Phase 4 gate is "documented uapi subset with test vectors; bridge code reviewed against
`nvk/nvk-macos.patch`'s interface points".
- The uapi subset with test vectors is `NOUVEAU-UAPI.md` + `pascal/tests/test_uapi.c`.
- The review is this document: §1 covers every nvkmd op; §4 covers every
  GSP/NVRM interface point in the patch.
- Beyond the gate's wording, the bridge is now wired into NVK
  (`nvk-pascal.patch`), compiled against the real patched Mesa tree, and
  unit-tested through Mesa's nvkmd core.

**Met as simulation** in my assessment. Hardware validation is runbook step 3
(owner-only). The RENDER access policy has been decided (§7.1).

*Hardware status this round (kdev, 2026-11-07):* on the Helios, kext bring-up
stops at the ACR HS signature patch (the ACR VMM is not ported), and the kext
has no hardware channel yet, so EXEC returns `-ENODEV`. `READY` stays 0, the
backend therefore offers no device, and runbook steps 3–4 cannot pass until
both land. That is a kext prerequisite, not a backend change.
