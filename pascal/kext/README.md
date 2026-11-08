<!-- SPDX-License-Identifier: GPL-2.0-only -->
# pascal/kext — GP106 XNU kext (GPL-2.0-only tree)

The XNU kernel extension for the GP106 (GTX 1060 Mobile) in the Helios 300.
It identifies the GPU, fetches the VBIOS through ACPI, loads the firmware from
its bundle and runs the bring-up sequence from `pascal/core` on its work loop.
It serves the userspace bridge (`pascal/bridge`) through `PG106UserClient` and
applies the D3cold idle policy.

**What has run on macOS (26.7.2 25G332, OpenCore-injected, 2026-11-08):**
PG106 loads and attaches, the bring-up reaches READY (`PG106Ready` Yes,
`PG106Stage` ready, firmware built in: `PG106FirmwareEmbedded` 17), and with
`pg106_selftest=2` both self-tests pass (CE copy and GR dispatch, ret 0)
(research/hardware/helios300/macos-runs/README.md). **Nothing beyond that
bring-up and the self-tests has been verified on macOS**: not sleep/wake,
not D3cold, not the HDMI output, not PG106Accel, the Metal plugin or NVK.

What else has been checked:

- On Linux it compiles against MacKernelSDK (`-fsyntax-only`).
- `scripts/cross-build-macos.sh` compiles, links an `MH_KEXT_BUNDLE` with
  Apple ld64 against the macOS 26.5 SDK, and audits every import against the
  KPIs that `Info.plist` declares: 31 TUs, 385/385 imports resolve
  (2026-11-08).
- `../scripts/kc-preflight.sh` against the owner's real 25G332 collections:
  imports, class sizes, vtables (PASS).
- Every decision the kext delegates to `pascal/core` runs under Linux unit
  tests, against mocks.

**The hardware path reaches READY from D3cold. This was shown on Linux, not
on macOS.** On 2026-11-08 the same `pascal/core` bring-up that the kext
runs (`pg106_gpu_boot`, through the same `pg106_hw_pmu_bus`/`flcn_bus`
adapters) ran on the Helios GP106 from Linux userspace with
`pascal/tools/pg106_hwtest` (hw-run6). It started from a GPU woken out of
D3cold, so unposted and with no WPR lock, and covered:
- POST: PMU falcon reset, then VBIOS DEVINIT;
- nouveau's mc init;
- FB, BAR2/BAR1 and FIFO, with the CE copy self-test (0 mismatches);
- the HS ACR and the SEC2 RTOS;
- BOOTSTRAP_FALCON(FECS, GPCCS), then GR (FECS ctx 0x87500), then READY.

Evidence: `research/hardware/helios300/hw-runs/`, BLOCKERS B18. What that
does not cover: anything IOKit (PCI/ACPI/IRQ/DMA plumbing, kextd firmware
loading, the `pg106_d3cold` PG00 path), the GR golden context, and
whether the kext loads at all (`HANDOFF-PASCAL.md` §7).

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
| fw | `embedFirmware`, `requestFirmware` | The 17 linux-firmware files are **built into the binary** (`src/gen/pg106_fw_embedded.c`, generated by `scripts/gen-firmware-embed.py` from `research/firmware`, every file checked against `firmware.sha256` and embedded unmodified; `pg106_fw_embedded.h`). `OSKextRequestResource` is only the fallback for a row the table lacks, and only works for a kext with a bundle in `/Library/Extensions`: on the first macOS boot (26.7.2, OpenCore-injected) kernelmanagerd answered all 17 requests with "Unable to create bundle at path /Library/Extensions/PG106.kext". |
| bring-up thread | `scheduleBoot`, `sBootCall` | The bring-up takes seconds, so it never runs on the IOConfig thread: `start()` returns after `registerService()` and a `thread_call` takes the gate and runs `sStartBoot`. `IOLog` breadcrumbs `PG106: boot +<ms> <stage> <ev> ...` carry the time since the bring-up began; the registry shows the last stage (`PG106Stage`) and, on a failure, `PG106Error` = "stage: detail (ret)". A failed bring-up leaves the GPU idle: interrupts unarmed, `PG106Ready` = No, sessions refused. Every wait in `pascal/core` has a budget. |
| intr | `setupInterrupts` | `IOFilterInterruptEventSource` on the MSI source (legacy index 0 if none). The filter (primary context) checks PMC_INTR_0 against what is routed and blocks all of it (`gp100_mc_intr_unarm`, 0x180). The action (work loop) runs the routed handlers (FIFO non-stall, display), then re-arms (0x160). |
| boot | `doBoot` → `pg106_gpu_boot` (`pascal/core/gpu.c`) | Pass 1: identify → posted bit `0x2240c` → `gm200_devinit_post` (VBIOS PMU DEVINIT app, after nouveau's PMU falcon reset) if not posted, then nouveau's mc init (`0x200 = 0xffffffff`; GR's GPCs fault without it). VRAM size (`pg106_fb_vram_size`) → VRAM allocator over `[0, size − 1 MiB)`. Pass 2: PTOP; FB init; ACR memory nouveau's way: WPR+shadow (tail, 0x40000), ACR inst block + VMM in VRAM, the HS image in IODMACommand-mapped host pages mapped at VA 0 with 4 KiB PTEs, join, TLB flush, BINDINST; the ACR plan; SEC2 RTOS; GR plan; SEC2 `BOOTSTRAP_FALCON(FECS, GPCCS)`; rest of GR. Then BAR2, BAR1, FIFO (runlists from PTOP) and the FIFO interrupt. Every stage logs `PG106: boot <stage> <ev> ret=… a=… b=… <detail>`, ACR memory included (`a = inst, b = page directory`; `WPR: a = shadow start, b = WPR end`). The first failure stops the sequence. |
| pm | `pmPerform` | `pascal/core/pm.c`. Bring-up at start; sessions counted per render connection. Idle with D3cold enabled: quiesce → `saveDeviceState` → `PG00._OFF` on PEG0. Next session: `PG00._ON` → 100 ms → `restoreDeviceState` → BOOT_0 check → full boot again (POST included). A failed quiesce or power-off keeps power on and disables D3cold for the load. A failed boot or power-on refuses sessions until reload. |

### Boot-args

| boot-arg | effect | default |
| --- | --- | --- |
| `-x` (macOS safe mode, OpenCore Shift+Enter) | PG106, PG106FB and PG106Accel stand down in `probe` after one log line, so a bad build is bypassed from the boot menu | attach |
| `-pg106off` | kill switch: `probe` returns nothing, the kext never attaches and no GPU register is touched | attach |
| `pg106_printf=1` | also write log lines to the kernel message buffer (`sudo dmesg`), drawn on the video console under `-v`; off by default since the builds that printed by default hung or looped at boot (macos-runs README) | IOLog only |
| `pg106_stop_after=N` | stop after devboot stage N (0 identify, 1 posted: no GPU writes, 2 POST, 3 PTOP, 4 ACR); also forces D3cold off | run everything |
| `pg106_d3cold=1` | idle power-off through ACPI `PG00._OFF` | off: the path has never run under macOS (B5, B8, B14) |
| `pg106_idle_ms=N` | idle time before D3cold | 5000 (nouveau's autosuspend delay, `nouveau_drm.c:686`) |
| `pg106_render_users=0` | restrict RENDER connections to administrators (also Info.plist `PG106RenderUsers` = false) | open to all local users |
| `pg106_render_chans=N` | render channels at once, 1..8 (one per render connection; WindowServer on the HDMI screen and each Metal app are one each); 1 = the single-channel behaviour, where a second connection's VM_INIT gets `-EBUSY` | 4 |
| `pg106_selftest=1` | after bring-up, run GPU work: a channel on the copy engine's runlist does a host semaphore release and a 64 KiB CE copy (`pg106_gpu_selftest`); logged as `PG106: selftest: …` | off |
| `pg106_hda=0` | leave the HD-audio function bit alone (PG106 sets config 0x488 bit 25 at start and every power-on, then rescans the bus off the gate after a gated check; quirk_nvidia_hda) | on (off in safe mode) |
| `pg106_pm=0` | register no power driver: PG106 does not take part in system sleep (the behaviour before sleep/wake support; IOPCIFamily sleeps the device and PG106 never knows) | power driver registered |
| `pg106_sleep_d3cold=1` | at system sleep also evaluate `PG00._OFF` (D3cold), and `PG00._ON` at wake | off: the platform's S3 cuts the power |
| `pg106_selftest=2` | also the GR self-test (`pg106_gpu_gr_selftest`, gr_ctx.h): golden context, a GR channel, an EXIT-kernel dispatch with a QMD semaphore, a store kernel; logged as `PG106: gr selftest: …` | off |

## What is real, what is stubbed

"Real" here means implemented and exercised on Linux against mocks or real
inputs. The `pascal/core` bring-up it calls has run on the GPU, from Linux
userspace through `pg106_hwtest`: hw-run6 reached READY (see the top of this
file). None of the kext itself has run under macOS.

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
  Tested on the mock FECS (`tests/test_grctx.c`) and, through the same
  `pascal/core` code from the Linux harness, on the GP106: grctx-run6
  generated the golden context, bound PASCAL_COMPUTE_B, dispatched a QMD
  (RELEASE0 landed) and ran a store kernel (BLOCKERS B17, resolved). The
  kext itself has not run on macOS.
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

- **Render channels: one per connection, up to 4 at once** (2026-11-08,
  `fRc[]`, boot-arg `pg106_render_chans=1..8`; 1 is the old single-channel
  kext). Each render connection's VM_INIT gets its own channel on GR's
  runlist with its own context image; the pagepool/bundle/attrib buffers are
  shared and mapped into each client VM, as in nouveau. With more than one
  channel the FIFO timeslices them and FECS switches GR contexts: that is
  simulated (`tests/test_grctx.c`, two clients, second context loaded on its
  own kick) but has **never run on the GP106**. A connection past the limit
  gets `-EBUSY` at VM_INIT (NVK: no device). Registry: `PG106RenderChannels`
  (held now), `PG106RenderChannelsMax`, `PG106RenderChannelsPeak`,
  `PG106RenderChannelClaims`, `PG106RenderChannelRefused`.
- **VRAM BOs are lost across D3cold** (so is the whole VRAM): a render
  session keeps the GPU powered, so this only matters for a client that
  outlives its session.
- **Sparse PTEs** are not written (regions are tracked only).
- **VPR scrub**: when 0x100cd0 reports VPR locked, it is logged and not
  scrubbed. nouveau also only warns without `nvdec/scrubber.bin`.
- **Runtime quiesce is a no-op.** D3cold is only requested with zero sessions.
  Before `PG00._OFF`, PG106FB gets WILL_OFF and PG106Accel gets LOST.
  After it, all of VRAM is re-created by the next boot.
- **Errno values on the wire are XNU's** (`<sys/errno.h>`, matching macOS
  userspace), except the six `PG106_UAPI_*` aliases in `uapi.h`, which are
  Linux numbers. ENOSYS and EOPNOTSUPP differ between the two.

## Sleep and wake

SIMULATION only (tests/test_suspend.c: 10 suspend/resume cycles on the mock
GPU; tests/test_acr_boot.c: 10 ACR + SEC2 + BOOTSTRAP_FALCON wake cycles).
**PG106 has never been through a sleep on the Helios**; `pmset` keeps the
Mac from sleeping until the coordinator runs the checklist.

PG106 is a power driver (two IOPM states) under the GP106's IOPCIDevice;
PG106FB's IOFramebuffer is its power child, so it sleeps before PG106 and
wakes after it. The shape is nouveau's `nouveau_pmops_suspend` / `resume`
(pascal/core/include/pascal/suspend.h has the sources):

| Phase | Where | What |
| --- | --- | --- |
| sleep | `setPowerState(0)` (PM thread) | `messageClients(PG106_KIF_MSG_GPU_OFF)` off the gate: PG106Accel stops CPU copies through BAR1. Then on the gate: pm `SUSPEND` → `doSuspend`: PG106FB gets `WILL_OFF` (head down, VRAM objects forgotten), PG106Accel's channels `EVENT_LOST`, GR engine and render channel dropped (the golden image stays in `fGolden`), `pg106_gpu_suspend` (PMC interrupts blocked, every channel stopped + preempted, scheduling off on every runlist `0x2630`, GR idle `gf100_gr_wait_idle`, every PTOP engine idle, FIFO interrupts off; one 500 ms budget for all of it), memory layer and FIFO freed, `fHwOff`: from here `rd32`/`wr32`/BAR1 access do nothing, for every caller. The sleep is never refused. |
| power | IOPCIFamily / the platform | S3 cuts the dGPU's power. `pg106_sleep_d3cold=1` evaluates `PG00._OFF` first. |
| wake | `setPowerState(1)` | acknowledged at once (the system's wake never waits for the GPU); a thread call takes the gate: pm `RESUME` → `doResume`: `pg106_resume_probe` reads BOOT_0, the posted bit and the WPR (`gm200_acr_wpr_check`, 0x100cd4). Cold → boot. BOOT_0 all ones → `PG00._ON`, probe again. WPR still locked (power was not cut; this driver has no ACR unload ucode) → `PG00._OFF` + `_ON`, probe again. At most two power operations; then `PG106Error`. |
| boot | pm `BOOT` → `doBoot` | the ordinary bring-up: POST from the VBIOS if unposted, mc init, FB, BAR2/BAR1, ACR + SEC2 RTOS, BOOTSTRAP_FALCON, GR, FIFO; the GR engine again with the saved golden image if it was up at sleep; `DID_ON` to PG106FB (mode restored); `PG106Ready` Yes; `messageClients(PG106_KIF_MSG_GPU_ON)`. |

Registry (strings replaced whole, never mutated): `PG106LastSleep` (what
the quiesce did and how long), `PG106LastWake`, `PG106WakeMs` (setPowerState(1)
to the end of the wake), `PG106WakeResult` ("ok: ready, generation N, ..." or
"FAILED (state ...): <PG106Error>"). A failed wake leaves `PG106Ready` No and
`PG106Error` set; the next sleep and wake try again.

What a sleep costs clients: everything in VRAM. Render connections opened
before it are stale from then on (`memEpoch`): every op answers `-ENODEV`
(NVK reports device lost) and nothing of theirs is freed into the new
allocator. A stale client's user mapping of a BAR1 window is not revoked:
after the wake that window may show another object's VRAM. PG106Accel
re-attaches through the kif on `GPU_ON`; PG106FB re-initialises on `DID_ON`.
PG106 being OFF (runtime D3cold) at sleep stays OFF after the wake.

### Runtime D3cold (`pg106_d3cold=1`): opt-in, not proven

Assessment 2026-11-08 (BLOCKERS B14): keep it off by default. The register
gate, PG106Accel's GPU_OFF/GPU_ON and PG106FB's WILL_OFF/DID_ON now cover the
power-off, but `PG00._OFF` has never been evaluated under macOS, and whether
IOPCIFamily takes the link-down for a removal is unknown. With PG106FB
attached the GPU never idles (PG106FB holds a session), so the boot-arg only
matters headless. Test it headless, after sleep/wake, as B14 describes.

### Clocks (G3)

The GPU runs at the clocks the VBIOS DEVINIT sets (boot clocks). NVIDIA
ships no PMU firmware for Pascal, so there is no reclocking, as with nouveau
on GP10x: expect a fraction of the GTX 1060's rated performance.

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
| `PG106.xcodeproj/` | Hand-written project: 31 Sources TUs (7 glue + 23 core + the generated `src/gen/pg106_fw_embedded.c`) and a Resources phase over `build/fw-stage/*`. |
| `scripts/build-macos.sh` | Owner build on the Mac: MacKernelSDK fetch, syntax check, firmware embed + staging, `xcodebuild`, `pg106_probe`. |
| `scripts/gen-firmware-embed.py` | Generates `src/gen/pg106_fw_embedded.c`: the firmware table built into the binary, sha256-checked against `firmware.sha256` (fails the build on a mismatch). |
| `scripts/stage-firmware.sh` | Copies `research/firmware/<path>` to the flat resource names (`--placeholders` for missing files); the `OSKextRequestResource` fallback. |
| `firmware.sha256` | The pinned linux-firmware hashes of the 17 files the kext embeds. |
| `../scripts/kc-preflight.sh` | Pre-flight against the real macOS 26.7.2 kernel collections (local copies, never committed): imports, parent class sizes (`kKcAuditParentSizes`), vtables, with macos_helios300 `tools/kc-audit.py`. |
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
