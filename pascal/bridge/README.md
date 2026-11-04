# pascal/bridge — the `pg106_br_` userspace bridge backend

Userspace-side skeleton of the library our NVK build would link: a backend
shaped like Mesa's `nvkmd` interface
(`third_party/mesa/src/nouveau/vulkan/nvkmd/nvkmd.h`, Collabora, MIT) that
transports the nouveau CS uapi subset (GETPARAM / VM_INIT / VM_BIND / EXEC,
documented in `research/evidence/NOUVEAU-UAPI.md`) over an **abstract
channel** instead of DRM ioctls. Today the channel is a recording mock; the
real one is the PG106 kext `IOUserClient` (`pascal/kext/`, Phase 3+).

Everything here is C11 + libc. Status: **skeleton — wire format, op flow and
validation are real; hardware-facing behaviour is intentionally missing**
(listed at the bottom). No hardware was touched (SIMULATION evidence only).

## Layout

```
include/pg106_br_wire.h      byte format: frame header + uapi payloads
include/pg106_br_bridge.h    the pg106_br_* public API
include/pg106_br_mock.h      recording mock transport (test stand-in for the kext)
src/bridge.c                 the ops: validate -> serialize -> transport
src/mock.c                   mock "kext" side: handshake, handles, GETPARAM, faults
tests/test_bridge.c          mock tests incl. wire-format byte-goldens
demo/pg106_br_demo.c         NVK-shaped sequence: alloc, bind, exec, wait
Makefile                     `make test`, `make demo`
```

## API surface (and the nvkmd mapping)

Each op does (a) validate through the `pg106_uapi_*` validators in
`pascal/core` (plus bridge-side state: BO handles, channel, VM_INIT), (b)
serialize to the wire structs pinned by `NOUVEAU-UAPI.md` §2, (c) hand one
`pg106_br_frame` + payload to the transport vtable. A validator rejection
returns a negative errno and **never reaches the transport**.

| `pg106_br_` API | nvkmd counterpart (mesa `17ca6174`) | notes |
| --- | --- | --- |
| `pg106_br_dev_open/close` | `nvkmd_pdev_create_dev` / `nvkmd_dev_destroy` | open = version handshake; gates on drm ≥ 0x01000301 like `nouveau_device.c:500-505` |
| `pg106_br_getparam` | winsys `nouveau_ws_param` (`nouveau_device.c:362`) | GETPARAM transport |
| `pg106_br_vm_init` | winsys `nouveau_device.c:510-515` | remembers the kernel-managed region for bind validation |
| `pg106_br_bo_alloc/free` | `nvkmd_dev_ops.alloc_mem` (`nvkmd.h:168`), `nvkmd_mem_ops.free` | GEM-handle analogue |
| `pg106_br_bo_map/unmap` | `nvkmd_mem_ops.map/unmap` (`nvkmd.h:213-221`) | via transport `map_cookie` hook = future `IOConnectMapMemory64` |
| `pg106_br_va_alloc/free` | `nvkmd_dev_ops.alloc_va`, `util_vma_heap` | bump allocator, no reuse (see below) |
| `pg106_br_ctx_create/destroy` | `nvkmd_dev_ops.create_ctx` | one ctx = one channel |
| `pg106_br_ctx_bind` (+`bind_wait`/`bind_signal`) | `nvkmd_va_ops.bind_mem/unbind`, batched bind ctx (`nvkmd_nouveau_ctx.c:306-365`) | queued, flushed as one VM_BIND frame |
| `pg106_br_ctx_exec` (+`exec_wait`/`exec_signal`) | `nvkmd_ctx_ops.exec` (`nvkmd.h:329`), `nvkmd_nouveau_exec_ctx_exec` (`nvkmd_nouveau_ctx.c:166-217`) | queued pushes + syncs |
| `pg106_br_ctx_flush/sync` | `nvkmd_ctx_ops.flush/sync` (`nvkmd.h:345-350`) | VM_BIND frame, then EXEC frame; sync-only empty exec like `nvkmd_nouveau_ctx.c:269-276` |

Wire format (`pg106_br_wire.h`): every frame is a 16-byte `pg106_br_frame`
header + payload. The uapi ops are the **verbatim `pg106_uapi_*` structs**
(byte-identical to `include/uapi/drm/nouveau_drm.h`); the Linux `*_ptr` user
pointers are sent as 0 and the arrays are flattened inline, in the kernel's
`u_memcpya()` order:

- VM_BIND: header, `vm_bind` (ptrs 0), `ops[]`, **waits[]**, **sigs[]**
- EXEC: header, `exec` (ptrs 0), **waits[]**, **sigs[]**, `pushes[]`

waits-before / signals-after is the wire encoding of the scheduler contract
(all waits complete before any push reaches the PBDMA, all signals fire after
the job's done fence — `nouveau_sched.c`, `NOUVEAU-UAPI.md` §2.4); the byte
positions are asserted in the tests.

## Pinning rule (P0-1)

The bridge enforces the **unbind-before-release** half of the page-pinning
rule documented in `NOUVEAU-UAPI.md` §6: it keeps a binding table (one entry
per applied MAP), refuses a second MAP that overlaps an active binding
(`-EINVAL`, the `bind_validate_map_common` rule), and refuses
`pg106_br_bo_free` while the BO still has bindings (`-EBUSY` — a BO whose
pages are bound into the GPU VM must not be released; UNMAP first). The
actual page wiring (IOKit pinning for the bind lifetime, who unpins) is
kext-side and specified in that same section.

## How this slots into the NVK build

`NOUVEAU-UAPI.md` §4 shows that `nvk/nvk-macos.patch` (base mesa `17ca6174`)
adds a **second `nvkmd` backend**, `src/nouveau/vulkan/nvkmd/nvrm/`, beside
`nvkmd/nouveau/`; the backend selection point is `nvkmd.c` +
`nvk_device.c`'s pdev creation (`nvkmd_pdev_create_dev`), i.e. whichever
`nvkmd_*_pdev` module is linked in. This bridge is the third option: a
backend with the same *call map* as `nvkmd/nouveau/` (see the table above)
but whose transport is IOKit instead of `drmIoctl`.

Integration shape (not wired yet):

1. Replace the `nvkmd/nouveau/` files' ioctl bodies with `pg106_br_*` calls:
   `nvkmd_nouveau_va.c` `vm_bind` (:98-112) → `pg106_br_ctx_bind`;
   `nvkmd_nouveau_ctx.c` `exec`/`flush` (:137-217) → `pg106_br_ctx_exec` +
   `pg106_br_ctx_flush`; winsys `nouveau_bo.c` GEM calls → `pg106_br_bo_*`.
   No `nvkmd.h` type changes: the flags/enums here mirror
   `enum nvkmd_mem_flags` (`nvkmd.h:35-58`) 1:1.
2. Link order: NVK (MIT) → `libpg106_bridge.a` (MIT + one
   syscall-note-excepted core object, see licensing) → IOKit
   `IOConnectCallMethod` (system) → GPL kext. The bridge links exactly one
   `pascal/core` object — `src/uapi.c`, the `pg106_uapi_*` validators, whose
   header is GPL-2.0 WITH the Linux-syscall-note (unrestricted userspace use
   of the ABI shapes) — and **no other core or kext code**.

**Binary separation story:** NVK links the bridge; the bridge talks to the
GPL kext *at arm's length* through the IOKit user-client ABI
(`IOConnectCallMethod` / `IOConnectMapMemory64`, shaped by
`pascal/kext/src/pg106_uc_abi.h` — a separate program, not a linked
derivative). The GPL-2.0 kext and the MIT userspace exchange only the
syscall-note-shaped structs plus bridge-owned fixed layouts
(`pg106_br_frame`, handshake, BO alloc/map), so neither side incorporates
the other's copyrighted expression — the same split the Linux kernel/DRM
drivers exist in. When the UserClient transport lands it must keep this
property: no kext header or link-dependency in bridge or NVK binaries, only
the stable selector/ABI numbers.

## Licensing boundary

- `pascal/bridge/**` is **MIT** (it is Mesa-side code; NVK is MIT).
- `libpg106_bridge.a` **contains one object from the GPL side**:
  `pascal/core/src/uapi.c`, the `pg106_uapi_*` validators (compiled by this
  Makefile as `core-uapi.o`). That file carries
  **GPL-2.0 WITH the `Linux-syscall-note` exception**: the note permits
  unrestricted userspace use of the syscall ABI, which is exactly how
  NVK/Linux already consumes these shapes (`drm-uapi/nouveau_drm.h` is
  vendored into Mesa). The header it goes with (`pascal/uapi.h`) is used by
  the bridge under the same note. Everything the core contains is pure C11
  validation of host-memory arrays - no kext code is involved.
- `pascal/core`'s other sources, and `pascal/kext/**`, are **GPL-2.0-only**
  and are *never* linked into the bridge or NVK.

## What is intentionally missing (skeleton boundary)

- **Channel lifecycle.** `CHANNEL_ALLOC`/`CHANNEL_FREE`/NVIF SCLASS are not
  in the uapi subset the core models; the mock provisions channel 0 at the
  handshake and `pg106_br_ctx_create` rejects anything else (`-ENOENT`).
- **VA reuse.** `pg106_br_va_alloc` is a bump allocator; `pg106_br_va_free`
  is a no-op. Mesa uses `util_vma_heap`; porting it is mechanical once the
  backend is real.
- **Asynchronous completion.** `pg106_br_ctx_sync` == flush; a real
  transport blocks on the kext event wait selector
  (`NOUVEAU-UAPI.md` §4.1 "Event wait"). The mock completes inline.
- **Sparse binds, region-overlap bookkeeping.** Per-client region state is
  kext-side (`NOUVEAU-UAPI.md` §2.3 "Stateless-only").
- **BO import/export** (IOSurface import, the offload path's
  `NVRM_ESC_SURF_VRAM` analogue): no `pg106_br_bo_import` yet — see
  `NOUVEAU-UAPI.md` §6 for the pinning rules it must follow.
- **The real UserClient transport.** `pg106_br_transport` is the seam;
  `src/mock.c` is the reference for what it must implement.

## Tests

```
cd pascal/bridge && make test    # mock transport suite, byte-goldens included
make demo                        # alloc -> bind -> exec -> wait trace
```

Byte-goldens embed full expected frames (header + payload, little-endian
x86-64) for HANDSHAKE, VM_BIND and EXEC, so an accidental wire change fails
the build rather than a future kext.
