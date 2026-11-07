# nvk/pascal — NVK's Pascal (GP106) backend over the pg106 bridge

`nvk/nvk-macos.patch` gives NVK one kernel backend, `nvkmd/nvrm/`, which drives
NVIDIA's RM through GSP firmware (Turing and newer). Pascal has no GSP. This
directory adds a second backend, `nvkmd/pg106/`, that implements NVK's `nvkmd`
interface (`src/nouveau/vulkan/nvkmd/nvkmd.h`) on top of the pg106 bridge
(`pascal/bridge`, `pg106_br_*`). The bridge carries the nouveau CS uapi subset
(GETPARAM, VM_INIT, VM_BIND, EXEC) plus BO lifecycle to the PG106 kext.

Everything here is MIT, like Mesa. It links only the MIT parts of `pascal/`:
`pascal/bridge/src/bridge.c` and `pascal/core/src/uapi.c`.

Status: **source complete. Built as part of the full NVK (NAK included):
cross-compiled for macOS x86_64 into `libvulkan_nouveau.dylib` (Mach-O,
every import exported by the macOS 26.5 SDK stubs) and built natively on
Linux, where the step 3–4 programs run through NVK + NAK against the
bridge's mock kext. Never run on macOS, a kext or a GPU.** See
[CROSS-BUILD.md](CROSS-BUILD.md) for the full build and what it verified.

## Layout

```
src/nvkmd/pg106/        the backend (canonical source; copied into Mesa by the patch)
  nvkmd_pg106.h           types, VA layout, control-ring constants
  nvkmd_pg106_pdev.c      transport selection, chip table, nv_device_info, enumeration
  nvkmd_pg106_dev.c       one kext connection + VM_INIT per nvkmd_dev, channels
  nvkmd_pg106_mem.c       BO alloc/map/free/host import, flags 1:1 with the bridge
  nvkmd_pg106_va.c        util_vma_heap VA, synchronous VM_BIND per call
  nvkmd_pg106_ctx.c       exec ctx (EXEC + semaphore control pushes), bind ctx
  nvkmd_pg106_sync.c      vk_sync type (binary + timeline, WAIT_PENDING)
nvk-pascal.patch        Mesa diff: the files above + three hook edits (generated)
nvk-build-fixes.patch   fixes a from-scratch build needs (nv_push.h in OpenCL C)
icd/nvk_icd.json        Vulkan ICD manifest (Pascal NVK reports Vulkan 1.3)
CROSS-BUILD.md          full NVK builds: macOS cross build, Linux native, NAK checks
scripts/prepare-mesa.sh base tarball + the three patches + class headers + bridge links + crates
scripts/mkpatch.py      regenerates nvk-pascal.patch from src/ (and --check)
scripts/linux-check.sh  Linux build + unit test of the backend alone (described below)
scripts/build-nvk-macos.sh       full NVK on the Mac (runbook step 3a)
scripts/cross-build-nvk-macos.sh full NVK for macOS x86_64, cross-compiled on Linux
scripts/macho-check.py           Mach-O + SDK .tbd symbol check of the result
scripts/build-nvk-linux.sh       native build: mesa_clc for the cross build, sim ICD
scripts/nak-sm61-check.py        NAK's SM 6.1 output vs nvdisasm -b SM61
scripts/build-nak-nouveau.sh     upstream NVK + this NAK: NAK tests, bundle for the Helios
scripts/run-nouveau.sh           runs that bundle on the Helios under nouveau
tests/                  test_nvkmd_pg106.c, loopback_gpu.c (plays GPU for host methods),
                        sim_icd.c (makes a Linux NVK build an ICD over the mock)
```

## How it slots into the existing NVK build

`nvk-pascal.patch` applies on top of Mesa `17ca6174` + `nvk/nvk-macos.patch`. It
adds `src/nouveau/vulkan/nvkmd/pg106/` and changes three files the macOS patch
already owns:

| file | change |
| --- | --- |
| `nvkmd/nvkmd.c` | `nvkmd_enum_pdev` asks NVRM, then pg106. `NVK_KMD=nvrm` or `NVK_KMD=pg106` limits it to one backend. |
| `meson.build` | builds `nvkmd/pg106/*.c`, `bridge.c`, `uapi.c` and the bridge's IOKit transport `transport_iokit.c`. Off macOS that file builds stubs that return `-ENOSYS`. |
| `nvk_device_memory.c` | the plugin's surface-dirty export answers "not supported" on any backend but NVRM. NVRM is the only backend that has those escapes. |

One `libvulkan_nouveau.dylib` therefore carries both backends. A Turing+ Mac
finds its GPU through NVRM; the Helios 300 finds its GP106 through PG106. The
ICD manifest is the same `nvk_icd.json` the plugin already loads
(`plugin/nvmtl_vk.c` `nvmtl_vk_icd_json`).

`prepare-mesa.sh` links `pascal/bridge` and `pascal/core` into
`nvkmd/pg106/{bridge,core}`, the way the NVRM backend expects
`nvkmd/nvrm/open-gpu-kernel-modules`. On the Mac, the full NVK build is then
`build/build263.sh` pointed at that tree, or directly:

```sh
sh nvk/pascal/scripts/prepare-mesa.sh ~/nvmtl-build/pascal
meson setup ~/nvmtl-build/pascal/build ~/nvmtl-build/pascal/mesa -Dvulkan-drivers=nouveau \
   -Dgallium-drivers= -Dplatforms= ...      # same options as build/build263.sh
ninja -C ~/nvmtl-build/pascal/build src/nouveau/vulkan/libvulkan_nouveau.dylib
```

## Design in one page

Mapping of every nvkmd op to the bridge and the kernel uapi:
`research/evidence/BRIDGE-NVK-REVIEW.md`.

- **Device.** One PG106 per machine. The pdev opens a probe connection
  (`pg106_br_iokit_transport_open`, IOKit class `PG106`) and handshakes
  (bridge ABI 0.2: the major must match and the minor must be at least 2).
  It reads GETPARAMs, including the bridge-private `PG106_BR_GETPARAM_VRAM_SIZE`
  and `PG106_BR_GETPARAM_BOOT0` (whose chipset field must be `0x136`), and
  looks the PCI id up in a table. The table holds the seven GP106 mobile ids the kext gate accepts
  (`pascal/kext/src/pg106_device_gate.c`) plus the two desktop ids the mock
  reports. `nv_device_info` is filled the way the nouveau winsys fills it for
  chipset `0x136`: SM 6.1, PASCAL_B (`0xC197`), PASCAL_COMPUTE_B (`0xC1C0`,
  QMD v02_01), PASCAL_DMA_COPY_B (`0xC1B5`), KEPLER_INLINE_TO_MEMORY_B,
  FERMI_TWOD_A, PASCAL_CHANNEL_GPFIFO_A (`0xC06F`), 96 KiB shared memory per
  SM, 48 KiB per workgroup. No GSP, no RM objects, no usermode doorbell.
  Until its ACR/GR bring-up has finished, the kext serves only HANDSHAKE and
  GETPARAM. The probe therefore sends one VM_INIT on its own connection; on
  `-ENODEV`, enumeration finds no device. If the user client refuses the
  process (`-EACCES`; only when `pg106_render_users=0` limits the render
  client to administrators), that is logged and no device is offered. A failing
  connection hides the GP106 but never fails `vkEnumeratePhysicalDevices`
  for other GPUs.
- **Memory and VA.** Every nvkmd_dev opens its own kext connection and does
  VM_INIT for `[2^39, 2^40)`, the same split as nouveau. BOs are bridge
  handles with a VA bound over the whole BO, as in `create_mem_or_close_bo`.
  VA is userspace-managed with nouveau's two heaps. Each bind is one
  synchronous VM_BIND, sent through a VA context. Unbinds send `UNMAP`.
  nouveau sends a handle-0 `MAP` there, which the kernel refuses
  (`NOUVEAU-UAPI.md` §5.2).
- **Submission.** NVK's pushes go to EXEC unchanged. Each exec context owns a
  64 KiB GART BO. Offset 0 holds a 32-bit progress word; a ring of 256-byte
  control slots follows. Every flush ends with a control push (WFI,
  SYSMEMBAR, then a 4-byte `SEMAPHORE` release of the next sequence number).
  A GPU wait on another context is an `ACQ_GEQ` on that context's word. EXEC
  never carries kernel syncobjs, so the kext needs no syncobj support.
- **Sync.** One `vk_sync` type covers binary and timeline. It holds a value
  plus (context, sequence) → value points, and supports `WAIT_PENDING`,
  which the runtime's assisted timeline mode requires. CPU waits poll the
  word with backoff. A wait longer than `NVK_PG106_TIMEOUT_MS` (default 10 s)
  is device loss, and so is a refused EXEC.
- **Channels.** The skeleton kext provisions channel 0 only. The first exec
  context takes it; later ones try channel 1, 2 … and share channel 0 on
  `-ENOENT`. Sharing is correct for NVK's copy-only upload queue next to one
  3D queue, but not for two 3D queues (`BRIDGE-NVK-REVIEW.md`, "Channels").
- **Host memory.** `VK_EXT_external_memory_host` maps to `import_host_mem`
  and then to `pg106_br_bo_import_host`. The pointer and size must be
  page-aligned. The kext wires the pages for the BO's lifetime. CPU maps
  return the caller's own pointer, and no kext mapping is made. This is the
  zero-copy IOSurface path of OFFLOAD-DESIGN.md §2.1. It is on whenever the
  bridge defines `PG106_BR_HAS_BO_IMPORT_HOST` (bridge 0.2 and later).
- **Not supported:** dma-buf, tiled/compressed BOs (`has_alloc_tiled` false;
  the macOS patch falls back to plain BOs), NVDEC, transfer queues.

## What was built and run (Linux, 2026-11-07)

`sh nvk/pascal/scripts/linux-check.sh /tmp/pg106-build-nvk`, on CachyOS
x86-64 with gcc and meson 1.x:

1. Downloads Mesa `17ca6174` (GitLab archive, SHA-256 pinned in the script)
   and applies `nvk-macos.patch` (one hunk applies at an offset of -4) and
   `nvk-pascal.patch`. Checks that `nvk-pascal.patch` matches `src/`.
2. Configures that tree with `-Dvulkan-drivers=swrast` and builds Mesa's
   util and Vulkan runtime libraries. This gives the real config defines,
   generated headers and util code. (The full NVK build, with bindgen,
   cbindgen and mesa_clc installed user-local, is in CROSS-BUILD.md.)
3. Compiles the six backend files, the patched `nvkmd.c`, `bridge.c` and
   `uapi.c` (and `transport_iokit.c`'s non-Apple stubs) with the compile flags Mesa uses for a Vulkan runtime file, plus
   `-Werror` (so `-Werror=missing-prototypes`, `-Werror=format`,
   `-Werror=incompatible-pointer-types` and the rest apply). No warnings.
4. Links them with the bridge mock, `tests/loopback_gpu.c` and Mesa's
   libraries, then runs `test_nvkmd_pg106`. Result against bridge ABI 0.2
   (`7d6a42f`): **217 checks, 0 failures**. The run is also clean under `SANITIZE=1`
   (AddressSanitizer + UBSan).

What the test covers:
- enumeration, including ABI mismatch, a non-GP106 id, a non-GP106
  BOOT_0, `-ENODEV` before bring-up, a failing GETPARAM, a ring that is too
  small, `NVK_KMD`, and no transport;
- the `nv_device_info` values;
- VM_INIT range;
- BO flags on the wire, the shared CPU mapping, VA lookup;
- UNMAP before BO_FREE;
- sparse VA (`MAP|SPARSE` with handle 0, PTE kind in flags, UNMAP for
  unbind, UNMAP + `UNMAP|SPARSE` on free);
- control-push encodings (cross-checked against the `clc06f.h` fields);
- an NVK-shaped push really executed through the VA table;
- a timeline signal reaching the CPU;
- a cross-context GPU wait emitted as `ACQ_GEQ` on the right word and
  payload;
- `WAIT_PENDING`, `WAIT_ANY`, binary reset;
- EXEC splitting at `EXEC_PUSH_MAX` with an incomplete-push run kept
  together;
- a hung GPU turning into `DEVICE_LOST` after the timeout;
- a refused EXEC;
- the sparse-bind context (CPU waits, one VM_BIND job, CPU signal);
- host import: alignment refusals, the import frame, CPU map = host pointer
  with no BO_MAP, a GPU push writing into the imported pages through the VA
  table, and UNMAP before BO_FREE.

What this unit test does not cover: NVK itself (no shader is compiled, no
Vulkan entry point is called), the IOKit transport, the kext, and any GPU
behaviour. `loopback_gpu.c` only interprets host semaphore, WFI and MEMBAR
methods. Since 2026-11-08, CROSS-BUILD.md covers the first two in part:
the step 3–4 programs run through the full NVK + NAK on Linux (sim ICD),
and `transport_iokit.c` compiles and links against the macOS SDK's IOKit
in the cross build. Neither has run on a Mac.
