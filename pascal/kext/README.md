<!-- SPDX-License-Identifier: GPL-2.0-only -->
# pascal/kext — GP106 XNU kext (GPL-2.0-only tree)

The XNU kernel extension for the GP106 (GTX 1060 Mobile) in the Helios 300.
It identifies the GPU, fetches the VBIOS through ACPI, loads the firmware from
its bundle and runs the bring-up sequence from `pascal/core` on its work loop.
It serves the userspace bridge (`pascal/bridge`) through `PG106UserClient` and
applies the D3cold idle policy.

**It has never been loaded on a Mac.** What has been checked:

- On Linux it compiles against MacKernelSDK (`-fsyntax-only`).
- `scripts/cross-build-macos.sh` compiles, links an `MH_KEXT_BUNDLE` with
  Apple ld64 against the macOS 26.5 SDK, and audits every import against the
  KPIs that `Info.plist` declares: 30 TUs, 369/369 imports resolve
  (2026-11-08).
- Every decision the kext delegates to `pascal/core` runs under Linux unit
  tests, against mocks.

None of that shows the kext loads, or that any GPU register behaves as
modelled (`HANDOFF-PASCAL.md` §7).

## Licensing — read before touching anything else

- **This tree is `GPL-2.0-only`** (SPDX header on every file, full text in
  `LICENSE`). It links nouveau-derived `pascal/core` objects (nvkm is
  GPL-2.0-only).
- **It must never be merged with, compiled into, or shipped inside the repo's
  PolyForm-NC trees** (`kexts/`, `plugin/`, `translator/`). The NVRM kext
  under `kexts/NVRM/` is a reference for shapes only; no code is copied.
- Userspace that talks to it is a separate program and may use any licence.
  The MIT bridge includes only the MIT wire header
  (`pascal/bridge/include/pg106_br_wire.h`). This tree includes that header
  too, and `PG106UserClient.cpp` static-asserts that both sides agree on the
  selector and type numbers.
- The non-GPL `PG106Accel` accelerator (`kexts/PG106Accel`) links nothing
  from this tree. `IOGraphicsAccelerator2::start` accepts only an
  `IOPCIDevice` provider, so PG106Accel attaches to the GP106's
  `IOPCIDevice` in `IOMatchCategory` `IOAccelerator`, beside PG106. It
  finds PG106 through publish/terminate notifications on
  `serviceMatching("PG106")`, keeping the one whose provider is the same
  PCI device.
- Interfaces PG106Accel depends on, which must stay stable:
  - the class name `PG106`;
  - PG106 being a client of the GP106's `IOPCIDevice`;
  - the personality property `PG106RenderUsers` and the
    `pg106_render_users` boot-arg, whose policy PG106Accel applies to its
    own user clients;
  - the registry property `PG106Ready` (boolean on the PG106 service, true
    only while bring-up has reached READY and the GPU is up), which
    PG106Accel can use to hold back `MetalPluginName`.

## What happens at load

Each step below logs a `PG106:` line. `research/evidence/OWNER-RUNBOOK.md`
step 1 lists the lines in order, with the values to expect on the Helios.

| Step | Where | What |
| --- | --- | --- |
| probe | `PG106::probe` | Wide plist match, software gate on the seven GP106M/GP106BM ids (`pg106_device_gate.c`). |
| start | `PG106::start` | Own `IOWorkLoop` with an `IOCommandGate` and an `IOTimerEventSource` (idle timer); PCI memory decode and bus mastering on; BAR0 mapped uncached, BAR1 size recorded; BOOT_0 must decode as GP106; `registerService()`. |
| acpi | `findAcpi` | Finds the PCI function's ACPI companion through the `acpi-device` property, else `acpi-path` + `IORegistryEntry::fromPath`. On the Helios that is `\_SB.PCI0.PEG0.PEGP`; its `IOACPIPlane` parent is PEG0, which holds power resource `PG00`. Unverified on this machine (BLOCKERS B12). |
| vbios | `fetchVbios` | Tries ACPI `_ROM(offset, 0x1000)` first, then the PCI expansion ROM BAR (enabled only for the read). Chunk assembly and validation are `pascal/core/vbios_fetch.c`: nouveau's `shadow_image` walk plus a check that image 0's PCIR vendor/device match this PCI function. The real Helios ROM through a fake 0x1000-capped `_ROM` takes 42 reads and comes back byte-identical (`test_vbios_fetch`). |
| fw | `requestFirmware` | One `OSKextRequestResource` per entry of `pg106_devboot_fw_names` (14 linux-firmware files, flat names like `gp106-acr-bl.bin`). The callbacks copy the data and hand it to the gate. Requests are answered only once kextd/kernelmanagerd runs (`OSKextLib.h`; BLOCKERS B13). |
| intr | `setupInterrupts` | `IOFilterInterruptEventSource` on the MSI source (legacy index 0 if none). The filter (primary context) checks PMC_INTR_0 against what is routed and blocks all of it (`gp100_mc_intr_unarm`, 0x180). The action (work loop) runs the routed handlers (FIFO non-stall, display), then re-arms (0x160). |
| boot | `doBoot` → `pg106_gpu_boot` (`pascal/core/gpu.c`) | Pass 1: identify → posted bit `0x2240c` → `gm200_devinit_post` (VBIOS PMU DEVINIT app) if not posted. VRAM size (`pg106_fb_vram_size`) → VRAM allocator over `[0, size − 1 MiB)`. Pass 2: PTOP; FB init; ACR memory nouveau's way: WPR+shadow (tail, 0x40000), ACR inst block + VMM in VRAM, the HS image in IODMACommand-mapped host pages mapped at VA 0 with 4 KiB PTEs, join, TLB flush, BINDINST; the ACR plan; SEC2 RTOS; GR plan; SEC2 `BOOTSTRAP_FALCON(FECS, GPCCS)`; rest of GR. Then BAR2, BAR1, FIFO (runlists from PTOP) and the FIFO interrupt. Every stage logs `PG106: boot <stage> <ev> ret=… a=… b=… <detail>`, ACR memory included (`a = inst, b = page directory`; `WPR: a = shadow start, b = WPR end`). The first failure stops the sequence. |
| pm | `pmPerform` | `pascal/core/pm.c`. Bring-up at start; sessions counted per render connection. Idle with D3cold enabled: quiesce → `saveDeviceState` → `PG00._OFF` on PEG0. Next session: `PG00._ON` → 100 ms → `restoreDeviceState` → BOOT_0 check → full boot again (POST included). A failed quiesce or power-off keeps power on and disables D3cold for the load. A failed boot or power-on refuses sessions until reload. |

### Boot-args

| boot-arg | effect | default |
| --- | --- | --- |
| `pg106_stop_after=N` | stop after devboot stage N (0 identify, 1 posted: no GPU writes, 2 POST, 3 PTOP, 4 ACR); also forces D3cold off | run everything |
| `pg106_d3cold=1` | idle power-off through ACPI `PG00._OFF` | off: the path has never run under macOS (B5, B8, B14) |
| `pg106_idle_ms=N` | idle time before D3cold | 5000 (nouveau's autosuspend delay, `nouveau_drm.c:686`) |
| `pg106_render_users=0` | restrict RENDER connections to administrators (also Info.plist `PG106RenderUsers` = false) | open to all local users |
| `pg106_selftest=1` | after bring-up, run GPU work: a channel on the copy engine's runlist does a host semaphore release and a 64 KiB CE copy (`pg106_gpu_selftest`); logged as `PG106: selftest: …` | off |
| `pg106_selftest=2` | also the GR self-test (`pg106_gpu_gr_selftest`, gr_ctx.h): golden context, a GR channel, an EXIT-kernel dispatch with a QMD semaphore, a store kernel; logged as `PG106: gr selftest: …` | off |

## What is real, what is stubbed

"Real" here means implemented and exercised on Linux against mocks or real
inputs. None of it has run on the GPU or under macOS.

- VBIOS fetch and validation (ACPI and PCI sources).
- The POST model on the real Helios ROM.
- PTOP scan, and the ACR plan built from the real linux-firmware blobs.
- **ACR memory, nouveau's way** (`devboot` with `cfg->vram`):
  - FB init;
  - WPR+shadow allocated from the top of VRAM (alloc 0x80000, the same as
    nouveau's on this GPU in the owner's trace);
  - the ACR instance block and VMM in VRAM;
  - the HS image in host pages from IODMACommand, mapped at VA 0;
  - join, TLB invalidate, BINDINST (VRAM target).

  `test_acr_boot`'s toy ACR fetches the image by falcon DMA through
  BINDINST, the instance block and a page walk in mock VRAM.
- SEC2 RTOS init, BOOTSTRAP_FALCON (FECS, GPCCS), the GR plan with the
  `sw_nonctx` pack.
- **Memory and MMU** (`vram`, `vmm`):
  - VRAM allocator;
  - page tables in VRAM, uploaded leaves-first through PRAMIN;
  - gp100 TLB invalidate;
  - BAR2/BAR1 VMMs bound at 0x1714/0x1704, with BAR1 mapped uncached for
    the kernel.
- **Host FIFO** (`fifo`):
  - PBDMA init;
  - USERD in VRAM via BAR1 (0x2254);
  - a TSG plus channel on runlists from PTOP;
  - runlist commit 0x2270/0x2274;
  - PCCSR bind/enable;
  - GP_PUT kick through BAR1;
  - preempt and teardown.
- **Copy-engine work**: `pg106_gpu_selftest` (boot-arg `pg106_selftest=1`)
  and PG106Accel's channels (`PG106_kif.cpp`) on the CE2 runlist. The CE
  needs no engine context.
- **Interrupts** through `IOFilterInterruptEventSource`. FIFO non-stall
  wakes fence waiters (`fenceWait`) and PG106Accel. Display bits are routed
  to PG106FB.
- **The display host table** (`PG106_disp.cpp`, `disp_host.h` ABI 1.0) and
  **PG106Accel's interface** (`PG106_kif.cpp`, `pg106_kif.h` ABI 1.0,
  `PG106KifABI` published).
- **The render channel** (`PG106::claimChannel`, `pascal/core/gr_ctx.c`):
  at the first render client's VM_INIT the GR engine is set up (topology
  from the GR stage, the pagepool/bundle/attrib_cb buffers in VRAM, the
  `sw_ctx`/`sw_bundle_init`/`sw_method_init` lists), the golden context
  image is generated through FECS (nouveau's `gf100_grctx_generate` for
  `gp104_grctx`, in a private VMM; kept in `fGolden` across re-boots), and
  the channel gets its own context (buffers mapped privileged into the
  client VM through `pg106_kd_map_vram`, the mmio list, a golden copy,
  inst+0x210) on GR's runlist. EXEC's GP_PUT kick goes through BAR1.
  Tested on the mock FECS (`tests/test_grctx.c`); not run on the GPU
  (the harness run is staged, BLOCKERS B17).
- **The wire protocol** on the kernel side (`pascal/core/kdisp.c`):
  framing, handles, VM table, syncobjs.
  - BO memory and host imports get bus addresses from IODMACommand, so the
    path is VT-d safe. `PG106_BR_BO_VRAM` BOs come from the VRAM
    allocator (zeroed); with `CAN_MAP` they get a BAR1 window the client
    maps write-combined. `GETPARAM VRAM_USED` counts them.
  - Each connection's VM tables live in VRAM it owns (4 MiB), and every
    change is uploaded and TLB-invalidated (`vm_flush`).
  - SYNCOBJ_WAIT sleeps on the fence interrupt with the gate dropped, so
    other clients run. Closing a connection ends its sleeping waits, and a
    second thread of the same connection gets its own buffers meanwhile.
- The executor's timeouts and the power state machine.

Stubbed or missing. Each item is explicit in the code:

- **One render channel** (`fChan`): a second render connection's VM_INIT
  fails with `-EBUSY` until the first closes.
- **VRAM BOs are lost across D3cold** (so is the whole VRAM): a render
  session keeps the GPU powered, so this only matters for a client that
  outlives its session.
- **Sparse PTEs** are not written (regions are tracked only).
- **VPR scrub**: when 0x100cd0 reports VPR locked, it is logged and not
  scrubbed. nouveau also only warns without `nvdec/scrubber.bin`.
- **Quiesce is a no-op.** D3cold is only requested with zero sessions.
  Before `PG00._OFF`, PG106FB gets WILL_OFF and PG106Accel gets LOST.
  After it, all of VRAM is re-created by the next boot.
- **Errno values on the wire are XNU's** (`<sys/errno.h>`, matching macOS
  userspace), except the six `PG106_UAPI_*` aliases in `uapi.h`, which are
  Linux numbers. ENOSYS and EOPNOTSUPP differ between the two.

## Who may open the render client

| Type | Who | Selectors |
| --- | --- | --- |
| DEVICE (0) | administrators only | GET_VERSION, GET_PMC, SUBMIT, GET_STATUS (`pg106_probe`) |
| RENDER (1) | every local user; administrators only with `pg106_render_users=0` / `PG106RenderUsers` = false | GET_VERSION, SUBMIT, GET_STATUS |

The owner decided on 2026-11-07, relayed through the coordinating session,
that render connections are open to all local users, because NVK runs inside
unprivileged app processes. **Trade-off:** any local process can now reach a
kernel parser that has had no security review, and can make the dGPU power
up. `pg106_render_users=0` restores administrator-only access. Raw identity
(GET_PMC) stays on the administrator-only DEVICE type.
Each connection has its own dispatcher (VM, BOs, syncobjs, handle namespace),
and a connection can name only its own handles.

## Layout

| Path | What |
| --- | --- |
| `src/PG106.hpp/.cpp` | Driver class: probe gate, start/stop, MMIO bus, ACPI, VBIOS, firmware requests, bring-up, power, idle timer. |
| `src/PG106UserClient.hpp/.cpp` | User client: types, dispatch table, SUBMIT (inline or descriptor input up to 256 KiB + 24), GET_STATUS, kdisp memory backend, `clientMemoryForType`. |
| `src/PG106_mem.cpp`, `src/pg106_dma.h` | IODMACommand-mapped host memory: the HS image, the sysmem flush page, render BOs and imports, PG106Accel's SYSMEM maps. |
| `src/PG106_disp.cpp` | `disp_host.h` provider for PG106FB. |
| `src/PG106_kif.cpp` | `pg106_kif.h` provider for PG106Accel: accelerator VM, VRAM, copy channel, fences, events. |
| `src/pg106_uc_abi.h` | ABI 0.3: types, selectors, `pg106_uc_version/pmc/status` (uint32_t-only, static-asserted). |
| `src/pg106_device_gate.c/.h` | Pure C11 id gate + BOOT_0 start decision. |
| `src/klibc/`, `src/pg106_klibc.c/.h` | calloc/realloc/free for the core on `IOMalloc`/`IOFree` (asm-label bound; no macros). |
| `tools/pg106_probe.c` | Root diagnostics: version, BOOT_0 decode, GET_STATUS block. |
| `Info.plist` | Personality, `OSBundleLibraries` (iokit, libkern, bsd, mach, unsupported, IOPCIFamily, IOACPIFamily), `PG106RenderUsers`. |
| `PG106.xcodeproj/` | Hand-written project: 30 Sources TUs (7 glue + 23 core) and a Resources phase over `build/fw-stage/*`. |
| `scripts/build-macos.sh` | Owner build on the Mac: MacKernelSDK fetch, syntax check, firmware staging, `xcodebuild`, `pg106_probe`. |
| `scripts/stage-firmware.sh` | Copies `research/firmware/<path>` to the flat resource names (`--placeholders` for missing files). |
| `scripts/cross-build-macos.sh` | Linux cross-build + link + KPI audit (needs a macOS SDK and Apple ld64; exits 77 without them). |
| `scripts/kext-syntax-check.sh` | clang `-fsyntax-only` of every Sources TU against MacKernelSDK. |
| `scripts/symbol-audit.sh` | No macOS symbols or includes in the portable core; glue uses KPI allocators and SDK headers; plist ids == gate table. |
| `stub/` | Linux harness: `test_device_gate`, `test_core_shim` (all of core through klibc), `cxx_linkage.cpp` (every core header gives C linkage under C++; a mangled `pg106_` import made the earlier kext unloadable). |

## Building

On the Mac (owner):

```
cd pascal/kext && bash scripts/build-macos.sh
# → build/xcodebuild/Release/PG106.kext (with Contents/Resources/gp106-*.bin)
#   and build/pg106_probe
```

On Linux:

```
make -C pascal test BUILD_DIR=/tmp/pg106                      # core + kext stub + audit + syntax + bridge
PG106_SDK_FETCH=1 make -C pascal/kext syntax BUILD_DIR=/tmp/k  # clone the pinned MacKernelSDK if absent
PG106_MACOS_SDK=<MacOSX26.5.sdk> PG106_LD64=<x86_64-apple-darwinNN-ld> \
    BUILD_DIR=/tmp/xb bash pascal/kext/scripts/cross-build-macos.sh   # link + KPI audit
```

The syntax check and the cross-build print `SKIP` (exit 77) when their tools
are missing, and a skip is never reported as a pass.

## Only a Mac can show

The checks above cannot show any of the following:

- That the kext loads: the AuxKC or OpenCore path, and the IOACPIFamily
  symbols the audit accepts by name.
- That `acpi-device` exists and that `_ROM` evaluates through
  `IOACPIPlatformDevice`.
- That kextd answers the resource requests.
- That the first POST, PRAMIN writes, SEC2 reset and scrub behave on the
  GP106M.
- Whether `PG00._OFF` actually powers the dGPU off without IOPCIFamily
  treating the link-down as a removal.

`research/evidence/BLOCKERS.md` B2 and B4-B8, and B12-B15, list what each
needs.
