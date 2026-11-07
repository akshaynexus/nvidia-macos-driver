# BLOCKERS — items only the owner can resolve

Format: what is needed, why, how to provide, what unblocks.

## B1. Helios 300 VBIOS dump — RESOLVED 2026-11-07

Resolved: the VBIOS was extracted from Acer's BIOS V1.22 package
(`research/firmware/vbios/README.md`, `pascal/scripts/extract_helios_vbios.py`).
It is 10de:1c20, 86.06.4F.00.08, 0x29400 bytes, the same version the
machine's driver reports. It completes the PMU DEVINIT upload in
`test_devinit_pmu`. What is still open for Phase 1 is a known-good POST
register trace from hardware, which needs the first live run. The history
of this item is kept below.

### History: the full flash (Phase 1)

Phase 1's gate is a register-write trace for **this machine's** VBIOS.
Without it the POST model runs against substitutes.

2026-11-07: GP106 POSTs through the VBIOS **PMU DEVINIT application**
(`gm200_devinit_post`; `POSTED-DETECTION.md` §2b), which lives in the type-0xe0
images after image 0. The fetched GP106/GP107 desktop substitutes end at the
PCIR image length (0xf000) and lack them; `nvidia-gtx1060-3gb` is an old-style
image (BIT 70.26, `I` 0x12 bytes) and not a credible GP106 reference. Only the
GP107M dump completes the upload. **Dump every image, not just image 0.**

2026-11-07 probe of the real machine (`HELIOS300-HW-PROBE.md`):
- The sysfs `rom` read returns **EIO**. The board has no dGPU ROM chip, so
  the first route below does not work on this machine.
- The VBIOS is served by ACPI `_ROM` (SSDT11) from a RAM copy at 0x7AE16018,
  and its size `RVBS` = **0x29400** (full image, VBIOS 86.06.4F.00.08).
- `/dev/mem` is blocked (`STRICT_DEVMEM`).
- Remaining routes, owner's choice:
  - extract the VBIOS from Acer's BIOS V1.22 update with UEFITool;
  - temporarily load a self-built `acpi_call` (`make LLVM=1` against the
    running clang/LTO kernel) and read `_ROM` in ≤0x200-byte chunks. The
    module's 4 KiB result buffer must be raised; 4 KiB chunks return empty;
  - boot nouveau once and copy `vbios.rom` from debugfs.

How to provide (any one):
- From Linux on the Helios 300: `echo 1 > /sys/bus/pci/devices/0000:01:00.0/rom`,
  `cat /sys/bus/pci/devices/0000:01:00.0/rom > helios300-gp106.rom`, then
  `echo 0 > .../rom` (`lspci -d 10de:` gives the address). Check the size: if
  it is 0xf000 or less, the dump stopped at image 0.
- From Linux with nouveau bound: `/sys/kernel/debug/dri/<N>/vbios.rom` is
  the image nouveau shadowed (from PRAMIN/PROM/ACPI `_ROM`, whichever it
  accepted). On an Optimus laptop the dGPU ROM may live in the system BIOS
  and be reachable only through ACPI `_ROM`, so this is the most likely
  source of a complete image.
- From GPU-Z on Windows: save the BIOS file; or `nvflash --save`.
Drop it at `research/firmware/vbios/helios300-gp106.rom` (gitignored; do not
commit board dumps).

Unblocks: a POST trace for the real ROM (`test_devinit_pmu` +
`test_devinit_real`), the DCB/connector table for the HDMI head (the probe found no DP connector on the dGPU).

## B2. macOS-only verification (Phases 3-6, standing)

All kext compilation (MacKernelSDK), IOKit probing, NVK-on-bridge behavior and
any display work can only be verified on the owner's Mac / Helios 300.
Owner-run build scripts and runbooks are produced instead; per handoff §3 the
owner's hardware is never touched by automation.

## B3. ACR/SEC2/GR firmware (informational, corrected 2026-11-07)

The earlier text said linux-firmware "ships no `nvidia/gp106/acr/` directory"
and that the gp102 blobs were a substitution. That was wrong:
linux-firmware's WHENCE links `nvidia/gp106/acr/*` and `sec2/*` to
`nvidia/gp102/`, and most of `gr/` to gp102/gm200 (`research/firmware/gp106/README.md`
has the table). These are the files nouveau loads on GP106. The vendored set
(linux-firmware `703a4d95` and `6d6bffb1`, NVIDIA licence in `licenses/`) is
the installed layout under the GP106 names; `gr/{fecs,gpccs}_{data,sig}.bin`
are GP106-specific. Nothing to provide unless upstream changes the links.

## B4. SEC2 on the real GP106M — ANSWERED from hardware 2026-11-07

Read from the owner-approved nouveau mmiotrace on the Helios 300
(`POST-TRACE-CAPTURE.md`, second capture):

- **PTOP table:** 8 engines. It is now the hardware vector in `test_top.c`.
  - SEC2 at **0x087000**, reset line **14 → PMC bit 0x00004000**. The old
    fixture value 0x100 was wrong. The model takes the mask from PTOP
    (`devboot.c` `pg106_top_reset_mask`), and `test_bringup` now uses
    0x4000. The trace shows nouveau setting bit 14 in 0x200 when it
    brings SEC2 up.
  - GR 0x400000 (reset 12), NVDEC 0x084000, NVENC 0x1c8000, four CE at
    0x104000.
- **SEC2 HWCFG (0x108) = 0x20420100:** IMEM and DMEM are both 64 KiB, the
  same sizes the mock decodes to.
- **0x408 = 0:** bit 20 is clear, so the **production** signature is the
  right one.
- **Queues after the RTOS init message** (`sec2/gp102.c:214-215`):
  - cmdq head/tail (0xa00/0xa04) were last read as 0x01000050 / 0x01000050;
  - msgq head/tail (0xa30/0xa34) were last read as 0x01000090 / 0x01000090;
  - nouveau's last cmdq head write was 0x01000060.

Still open: IRQDEST routing as `gp102_sec2_intr` takes it (the model
polls), and running this port's `acr_boot` plan itself on hardware, which
needs the kext on the Mac.

## B5. Posted-bit behaviour across power gating

`devinit_pmu` skips POST when 0x02240c bit 1 is set. Whether the bit survives
an Optimus/ACPI power-off of the dGPU (D3cold via `_OFF`/`_PS3`) on the Helios
is undocumented (`POSTED-DETECTION.md`, "Unknown"). Needs: the liveness probe
log (`pg106_diag_probe_liveness`) at attach, and again after the dGPU has been
powered down and back up. Unblocks: deciding whether the kext must POST.

## B6. First macOS compile and link of the kext (Phase 3)

`PG106.xcodeproj` is hand-written and has never been opened in Xcode. Linux
proves only `clang -fsyntax-only` against MacKernelSDK `3f750085` (9 TUs) and
a symbol audit. Needs: `pascal/kext/scripts/build-macos.sh` output on the Mac
(full log), plus `kmutil print-diagnostics -p <PG106.kext>`. Unblocks: any
claim that the kext builds or links.

## B7. Kext signing, SIP and AuxKC approval state

`build-macos.sh` ad-hoc signs. Whether this install accepts it depends on the
owner's SIP/AMFI settings and the load path (OpenCore `Kernel->Add` vs
`kmutil load` + approval + AuxKC rebuild; `OWNER-RUNBOOK.md` Step 1). Needs:
`csrutil status`, the OpenCore config's kernel section and the macOS version,
recorded in the Step 0 baseline. Unblocks: choosing 1A or 1B.

## B8. dGPU visibility: ACPI disable and the `_OFF` method

Hackintosh configs often hide the dGPU (`-wegnoegpu`, an SSDT that calls
`_OFF`, or disable properties). Needs: Step 0 of `OWNER-RUNBOOK.md` (boot-args,
`EFI/OC/ACPI` listing, `ioreg` for 10de), plus a DSDT/SSDT dump (`acpidump`
from Linux on the Helios, or OpenCore `SysReport`) showing the dGPU device's
`_OFF`/`_ON`/`_PS0`/`_PS3` bodies. Unblocks: B5, the power section of
`OFFLOAD-DESIGN.md`, and whether the kext needs an ACPI power-on before BAR0
reads.

## B9. drm_version reported by the bridge (Phase 4 decision)

Mesa refuses to run below drm 0x01000301 (`nouveau_device.c:504-505`). The
mock and the kext's dispatcher (HANDSHAKE, `pascal/core/kdisp.c`) both report
exactly that.
`NOUVEAU-UAPI.md` §5.4 leaves open which version (and patchlevel) the kext
should claim. Owner decision; it fixes what NVK paths are enabled.

## B10. Does the gp102-family firmware boot on a GTX 1060 Mobile? — RESOLVED 2026-11-07

**Yes, on the owner's Helios 300.** The second owner-approved capture
(`POST-TRACE-CAPTURE.md`) ran nouveau with the linux-firmware
gp102/gp106 ACR, SEC2 and GR images and a surfaceless EGL client.
- Mesa reported `renderer: NV136`, OpenGL 4.3 core / ES 3.2.
- The trace shows the expected activity: SEC2 at 0x087000 (248 W / 422 R),
  FECS (57 W / 47 R, `0x409800` answering 0x2300) and GPCCS.
- So GR came up through the same ACR -> SEC2 -> FECS/GPCCS chain
  `acr_boot`/`gr_boot` model.

History:

nouveau loads the same linux-firmware files for every GP106 (B3), and the
FECS/GPCCS signatures are GP106-specific, but nothing public shows them
booting on a GP106M (1C20/1C60) rather than a desktop GP106. Only the first
SEC2/ACR run on the Helios (after B4) answers it; record the ACR mailbox and
SEC2 init message either way.

## B11. GP108 ACR variant (informational)

`HANDOFF-PASCAL.md` §5 says to keep GP108 ACR blobs available. GP106 uses
`gp102_acr_new` (B3) and never requests them, so they are not vendored.
Owner decision only if the target ever widens to GP108 (MX150/MX250) parts.

## B12. The dGPU's ACPI companion as macOS sees it (VBIOS + power)

The kext finds `\_SB.PCI0.PEG0.PEGP` through the PCI device's `acpi-device`
property, else `acpi-path` + `IORegistryEntry::fromPath`. It then evaluates
`_ROM(offset, 0x1000)` on it, and `PG00._OFF`/`PG00._ON` on its
`IOACPIPlane` parent (PEG0) (`pascal/kext/src/PG106.cpp` findAcpi,
fetchVbios, doPowerOff/On). Neither property, nor relative-path evaluation
through `IOACPIPlatformDevice::evaluateObject`, has been seen on this
machine. OpenCore ACPI patches may also rename or drop these objects
(HELIOS300-HW-PROBE.md "Open").

Needs:
- `ioreg -r -c IOPCIDevice -n PEGP -l -w0` (or the dGPU's name) with its
  `acpi-device` / `acpi-path` properties.
- The `PG106: acpi:` and `PG106: vbios:` log lines from the first load
  (OWNER-RUNBOOK step 1, safe mode `pg106_stop_after=1` is enough).

Unblocks: VBIOS on macOS (POST after D3cold depends on it), the D3cold
path.

## B13. Firmware delivery through OSKextRequestResource

The 14 firmware files are bundle resources, requested at `start()`. Requests
are only answered once kextd/kernelmanagerd runs (`libkern/OSKextLib.h`), so
under OpenCore injection the bring-up starts some seconds into boot. A render
session opened before then waits for the requests to finish (up to 30 s), and
the time this takes has not been measured.

Whether `OSKextRequestResource` serves an OpenCore-injected kext at all
(kernelmanagerd locates the bundle on disk; an injected kext's bundle lives
in the EFI partition) is unknown. If it does not:
- install the kext also in `/Library/Extensions`, or
- embed the blobs in the binary.

Needs: the `PG106: fw:` lines from step 1. Unblocks: every stage after
POSTED.

## B14. ACPI D3cold under macOS (PG00._OFF/_ON)

`pg106_d3cold=1` makes the kext save PCI state and evaluate `PG00._OFF`
when idle. The next session evaluates `PG00._ON`, waits 100 ms, restores
config space and re-runs the bring-up. Unknown on macOS:
- whether IOPCIFamily treats the link-down as a surprise removal and
  terminates the driver (PEG0 hot-plug capability);
- whether AppleACPIPlatform already manages `_PR3`/PG00 for this device and
  conflicts with the direct call;
- whether the GPU then reads unposted (B5).

Alternatives, if the direct call is wrong:
- `_PS3`/`_PS0` on PEGP;
- the Optimus `_DSM` (NVOP);
- IOPCIFamily's own power management (`changePowerStateTo` with
  `acpi-pmcap`).

Needs: steps 1–2 passing, then one idle cycle with the boot-arg, the
`PG106: power:` / `pm:` lines, `power_cycles` from `pg106_probe`, and a watt
reading. Off by default.

## B15. What the first full bring-up writes, and where it stops

Updated 2026-11-08 (kdev3). The ACR VMM stop is gone: devboot now builds
the ACR memory the way nouveau does. Without boot-args, the kext
(`pg106_gpu_boot`) runs this on the real GPU:
1. identify, posted check;
2. POST if unposted (gm200 PMU DEVINIT from the ACPI ROM);
3. FB size (FBP walk), then FB init (before the BARs and the ACR, as in
   nouveau), BAR2 (0x1714) and BAR1 (0x1704), then PTOP. FB init is:
   - MMU debug buffers 0x100cc8/0x100ccc;
   - remapper off;
   - 0x100c80 |= 0x1800;
   - sysmem flush page 0x100c10;
   - VPR check 0x100cd0;
4. ACR memory:
   - WPR + shadow from the top of VRAM (alloc 0x80000);
   - the ACR instance block and page tables in VRAM (PRAMIN);
   - the HS image in IODMACommand host pages, mapped at VA 0;
   - join, TLB invalidate (0x100cb8/0x100cec/0x100cbc);
5. the ACR plan: SEC2 reset (PMC bit 14), signature patch, BINDINST
   (0x40000000 | inst >> 12), BL PIO, HS boot, WPR check, SEC2 RTOS start;
6. GR plan, SEC2 BOOTSTRAP_FALCON(FECS, GPCCS), rest of GR, READY;
7. FIFO init (PBDMAs, 0x2254, 0x2100/0x2140), then the PMC interrupt
   enables (0x160).

Each step has only run against mocks. The values that can be compared with
the owner's nouveau trace of this GPU are in `pascal/README.md`, "Hardware
reference values". The log lines to check against them:
- `ACR memory: a = inst, b = page directory` (trace: inst 0x17fe84000, PD
  0x17fe83000; ours will differ because our allocation order differs);
- `WPR: a = shadow start, b = WPR end` (trace: 0x17fe00000 / 0x17fe80000;
  in simulation ours is the same, `test_gpu`);
- the BAR binds 0x1714 = 0x8017febf and 0x1704 = 0x8017feaa (the same words
  in simulation);
- the BINDINST word.

Still the first time on a GP106M for:
- the POST (B1);
- the SEC2 reset (B4);
- our page tables and falcon DMA through them;
- everything from GR on.

Needs: the full `PG106: boot` log of one run (or `pg106_hwtest --go` output,
B18), and `pg106_probe` afterwards.

## B17. Render channel: the GR golden context is not ported

NVK's channel needs GR (PASCAL_COMPUTE_B / PASCAL_A) on GR's runlist. A
channel scheduled on GR without a context at inst+0x210
(`gk104_ectx_bind`) makes FECS fault the context switch. nouveau builds the
golden image once, in `gf100_grctx_generate` with the gp100/gp102 grctx
tables, and gives each channel a copy (`gf100_gr_chan_new`). None of that
is ported, so `PG106::claimChannel` keeps answering `-ENOSYS` and EXEC
answers `-ENODEV`.

What does work without it:
- copy-engine channels (no context: `gk104_engn_ce` has no ctor/bind):
  `pg106_gpu_selftest` and PG106Accel's COPY/RELEASE;
- the render client's VM in VRAM with TLB invalidates;
- syncobj waits on the non-stall interrupt.

Needs: a port of grctx generation for GP106 (ctxgp100.c / ctxgp102.c
sizes, the bundle/pagepool/attrib buffers, the FECS golden save) and its
own test against `test_gr_boot`'s FECS mock. Unblocks: EXEC for NVK.

## B18. First run of the Linux hardware harness (pg106_hwtest)

`pascal/tools/pg106_hwtest.c` runs the bring-up and the GPU-work self-test
(host semaphore release, then a 64 KiB CE copy, VRAM to VRAM) from Linux
userspace through sysfs BAR mmaps, with no driver bound. It is built and
passes `scripts/hwtest_smoke.sh` against a fake sysfs tree. **It has not
run on the laptop.**

The planned order, each step only after the coordinator's explicit go:
1. `--probe`: reads only; nvidia unbound, GDM stopped.
2. `--go --stop-after 3` (identify, posted, POST if needed, PTOP, then
   memory + FIFO): the self-test runs without ACR/GR.
3. `--go` in full.

Recovery: a reboot (the WPR lock and the FIFO state do not survive a cold
boot).

Unknowns it answers:
- whether a GPU previously driven by nvidia (WPR already locked) has to be
  power-cycled before our ACR can boot;
- whether the VPR is locked;
- whether the CE executes our pushes.

## B16. HDMI display (PG106FB + disp_hw): what only the Mac and the GPU can answer

The HDMI driver (pascal/core `disp_hw.c`, pascal/fb `PG106FB.kext`) is code
on hardware paths. So far it has run only against the register-semantics
mock in `pascal/tests/test_disp_hw.c`. Its init was diffed against nouveau's
own init on this GPU (DISP-PLAN.md §11). Open items, in the order a first
run would hit them:

1. **PG106's side of `disp_host.h`** (`pascal/kext/src/PG106_disp.cpp`,
   1daa2ad, never run). VRAM, BAR1 and interrupt routing exist only once
   PG106's memory layer is up (bring-up past PTOP); before that PG106FB
   does not attach. After D3cold every VRAM allocation is gone and BAR1 is
   rebuilt: PG106FB re-allocates and re-maps. Since c551906, disp_host's
   bar1_map allocates from the top of BAR1 (tail first-fit). PG106FB's
   scanout is its only bar1_map per boot, at the same size, so it gets the
   same offset (bar1_size − size) after every re-boot whatever PG106Accel
   mapped meanwhile, and the aperture WindowServer mapped stays valid
   (test_vmm checks it). `aperture moved` in the log would mean that
   guarantee broke. The remaining unknown is that WindowServer's mapping
   survives the GPU's D3cold at all.
2. **The link itself.** Supervisor 2 (VPLL0 via the ported gf100
   `pll_set`, the IEDT OnInt2 script 0x5a40 at 148.5 MHz, the SOR clock)
   has run only on the mock. A monitor showing "no signal" after
   `setDisplayMode … -> 0` points here. Needs: one HDMI-attached mmiotrace
   from nouveau (capture request in DISP-PLAN.md §11.3), to diff the
   modeset as the init was diffed.
3. **DDC.** Bit-banged at 0x00d0d4 through hybrid pad 0. The pad mode
   writes and the timing (1 µs rise, 5 µs hold, as nouveau) are
   unverified. Symptom: `EDID read failed`.
4. **HPD polarity.** The VBIOS says GPIO line 27 is active low (log[1] =
   2). The no-monitor trace read 0xd67c = 0x5000 (bit 14 set =
   disconnected), which is consistent, but a plugged read was never seen.
5. **WindowServer and an unaccelerated IOFramebuffer** (`pg106fb_accel=0`).
   Whether macOS 26 still composites to a non-Metal framebuffer next to an
   accelerated Intel display is unknown. 4e makes it moot if the
   accelerated pairing works.
6. **BAR1 aperture** (write-combining through PG106's BAR1 VMM) versus the
   10 Hz shadow copy. The shadow copy is a bring-up fallback only.
7. **Cursor blend.** The cursor uses nouveau's composition (ALPHA_BLEND)
   on the premultiplied ARGB WindowServer hands it. Fringes on the pointer
   would mean PREMULT_ALPHA_BLEND is needed.
8. **Audio.** The ELD is written (0x10ec00), but nothing on macOS binds the
   GP106's HDA function (01:00.1). HDMI audio is out of scope this round.
9. **> 340 MHz modes.** These need SCDC scrambling with the sink (not
   implemented), so 4K60 is not offered; 4K30 (297 MHz) is.

**Owner step, deferred (2026-11-08): the HDMI-attached mmiotrace** (item 2).
The owner has no HDMI monitor available, so the capture is skipped for now.
Facts on the machine: `modetest` is already installed (libdrm 2.4.134), and
`/sys/class/drm/card*-HDMI-A-1/status` reads `disconnected`. When a monitor
is available, run the exact command in DISP-PLAN.md §11.3 (monitor
connected and powered first), and deliver
`research/hardware/helios300/raw/posttrace-hdmi-<date>/`. Until then the
first real modeset is the one on macOS in runbook 4e.0. Its `modeset #N`
diagnostic block (supervisor stages and masks, latched mode, VPLL writes,
IED script ids and results, SOR clock, armed state and link registers)
stands in for the trace.

Needs: steps 4e.0 then 4e of OWNER-RUNBOOK.md, with their logs (the
diagnostic block in particular). Then the HDMI mmiotrace above once a
monitor is available.

## Note: render client access (owner decision, 2026-11-07)

Render connections (user-client type 1, what NVK opens) are open to every
local user by default, so NVK runs in unprivileged app processes. The
decision was relayed by the coordinating session. Trade-off: any local
process can reach an unreviewed kernel parser and wake the dGPU.
`pg106_render_users=0` (boot-arg) or Info.plist `PG106RenderUsers` = false
restricts render connections to administrators. The DEVICE type (raw
identity) is always administrator-only.
