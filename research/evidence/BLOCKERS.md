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

**Status 2026-11-08:** the kext no longer depends on the answer. devboot
reads 0x2240c on every boot and POSTs an unposted GPU. A GPU that kept its
power (system sleep that did not cut it) is recognised by the WPR the last ACR
boot left locked (`gm200_acr_wpr_check`, 0x100cd4; `pascal/core/suspend.h`)
and power-cycled through PG00 before the boot. The bit itself across D3cold or
S3 on macOS is still unobserved (sleep/wake checklist,
`tools/macos/README.md`).

`devinit_pmu` skips POST when 0x02240c bit 1 is set. Whether the bit survives
an Optimus/ACPI power-off of the dGPU (D3cold via `_OFF`/`_PS3`) on the Helios
is undocumented (`POSTED-DETECTION.md`, "Unknown"). Needs: the liveness probe
log (`pg106_diag_probe_liveness`) at attach, and again after the dGPU has been
powered down and back up. Unblocks: deciding whether the kext must POST.

## B6. First macOS compile and link of the kext (Phase 3)

**ANSWERED differently 2026-11-08:** the kext is cross-built on Linux with Apple
ld64 (`pascal/kext/scripts/cross-build-macos.sh`, KPI audit) and checked against
the real 25G332 collections (`pascal/scripts/kc-preflight.sh`); the cross-built
binary loaded and reached READY on the Mac. The Xcode project still has never
been opened.

`PG106.xcodeproj` is hand-written and has never been opened in Xcode. Linux
proves only `clang -fsyntax-only` against MacKernelSDK `3f750085` (9 TUs) and
a symbol audit. Needs: `pascal/kext/scripts/build-macos.sh` output on the Mac
(full log), plus `kmutil print-diagnostics -p <PG106.kext>`. Unblocks: any
claim that the kext builds or links.

## B7. Kext signing, SIP and AuxKC approval state

**ANSWERED by the macOS boots (2026-11-07/08):** PG106 is OpenCore-injected
(`Kernel->Add`, ad-hoc or unsigned both fine for injection) and loaded. PG106FB
and PG106Accel cannot be injected (IOGraphicsFamily / IOAcceleratorFamily2 are
only in SystemKernelExtensions.kc), so they go to /Library/Extensions: ad-hoc
signed, root:wheel, in the auxiliary collection, approved once in System
Settings (`tools/macos/install-gpu.sh`). PG106FB 94f086ec attached that way.
The SIP value itself was not recorded in the repository; `install-gpu.sh`
prints `csrutil status` in its dry run.

`build-macos.sh` ad-hoc signs. Whether this install accepts it depends on the
owner's SIP/AMFI settings and the load path (OpenCore `Kernel->Add` vs
`kmutil load` + approval + AuxKC rebuild; `OWNER-RUNBOOK.md` Step 1). Needs:
`csrutil status`, the OpenCore config's kernel section and the macOS version,
recorded in the Step 0 baseline. Unblocks: choosing 1A or 1B.

## B8. dGPU visibility: ACPI disable and the `_OFF` method

**ANSWERED by the macOS boots (2026-11-07/08):** the GP106 is visible
(`pci10de,1c20` with `acpi-path` under PEG0, HDA function `pci10de,10f1` at
`PEG0@10000/HDAU@1`); boot-args `-v debug=0x100 keepsyms=1 -amfipassbeta
-igfxblt -vi2c-force-polling revpatch=sbvmm`, no `-wegnoegpu` in the working
configuration (a boot with `-wegnoegpu` + PG106/PG106FB in /L/E hung). BAR0
answered without an ACPI power-on, and PG106 reached READY. The `_OFF` body
is in SSDT11 (`PG00` under PEG0); calling it under macOS is still B14.

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

**RESOLVED 2026-11-08 by embedding:** kernelmanagerd answered all 17
requests of the OpenCore-injected kext with "Unable to create bundle at path
/Library/Extensions/PG106.kext". The 17 files are now built into the binary
(`pascal/kext/scripts/gen-firmware-embed.py`, sha256-pinned in
`pascal/kext/firmware.sha256`); `PG106FirmwareEmbedded` = 17 and READY on the
Mac. The text below is the original question.

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

**Assessment 2026-11-08 (G4): not yet safe to turn on by default; keep it
opt-in (`pg106_d3cold=1`).** What changed: every BAR0/BAR1 access is refused
while the GPU is off (`fHwOff`), PG106Accel is told (`PG106_KIF_MSG_GPU_OFF` /
`_ON`) and stops its BAR1 copies, PG106FB gets WILL_OFF/DID_ON, and render
connections from before the power-off are stale (device lost). What did not:
none of the three questions below has been answered on the Mac. Also, while
PG106FB is attached it holds a display session, so the GPU never goes idle and
D3cold never triggers: with the HDMI stack installed the boot-arg has no
effect. The first test is therefore headless (no PG106FB), after the
sleep/wake checklist passes: one boot with `pg106_d3cold=1`, wait > 5 s idle,
`PG106: power: PG00._OFF done (D3cold)` and `pm_state 6` (off) from
`pg106_probe`, then a render session (`pg106_offload`) must power it back on
and reach READY. A surprise-removal (PG106 terminated) or a hang ends the test;
`-pg106off` recovers.

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

**ANSWERED 2026-11-08:** the full sequence below ran on the Helios under macOS
26.7.2 (PG106 b72926bd and 3a8ac383): `PG106Stage` ready, `PG106Ready` Yes,
CE and GR self-tests ret 0 with `pg106_selftest=2`, and from Linux userspace
before that (B18). Kept for the register-level reference.

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

## B17. Render channel: the GR golden context — RESOLVED on the GPU (grctx-run6)

**Resolved 2026-11-08 by hardware run grctx-run6** (7853fd0, `pg106_hwtest --gr`): golden context generated through FECS, a GR channel on that context, PASCAL_COMPUTE_B bound, a QMD dispatch whose RELEASE0 landed, a host semaphore with WFI, and a store kernel that wrote 0x6a5ec0de through its constant buffer, all with GR_INTR 0 and no MMU/PBDMA errors (research/hardware/helios300/hw-runs/grctx-run6). grctx-run7 (b731e0b, `pg106_hwtest --render`) then ran the kext's render path with IOKit taken out on the GPU: the bridge (NVK's backend) over the loopback into kdisp (PG106UserClient's dispatcher) with a VRAM/BAR1 environment, `chan_bind` = `PG106::claimChannel`; VM_INIT bound the channel, BOs were allocated, mapped and bound, two EXECs ran with kernel fences (`render: ret 0`, store 0x6a5ec0de, fence seqno 2, GR_INTR 0). grctx-run8 (bad826d) added a copy-engine copy on the same channel (PASCAL_DMA_COPY_B on subchannel 4, a GRCE on runlist 0): 64 KiB VRAM -> VRAM, 0 mismatched words through BAR1, fence seqno 3, so NVK's copies work on the render channel. What remains is macOS: IOKit (IODMACommand, BAR1 client mappings, the interrupt thread, the user client) and the NVK dylib on top; nothing has run there.

NVK's channel needs GR (PASCAL_COMPUTE_B / PASCAL_A) on GR's runlist with
a context at inst+0x210 (`gk104_ectx_bind`). Ported 2026-11-08 (grctx,
commits adf69b6 + f13a5d4): `pascal/core/gr_ctx.c` is nouveau's
`gf100_gr_oneinit` + `gk20a_gr_load_sw` + `gf100_grctx_generate` for
`gp104_grctx` (the golden image through FECS BIND_POINTER /
WFI_GOLDEN_SAVE, in a private VMM) and `gf100_gr_chan_new` /
`gf100_gr_chan_bind` / `gk104_ectx_ctor` (per-channel context). The FIFO
takes an engine-context pointer (`pg106_chan_create_desc`), kdisp maps the
host's context objects into the client VM (`pg106_kd_map_vram`) and places
`PG106_BR_BO_VRAM` BOs in VRAM, and `PG106::claimChannel` binds a real
channel: VM_INIT -> channel on GR's runlist, EXEC -> GP_PUT.

Cross-check against the owner's nouveau mmiotrace (posttrace2): nouveau's
own `gf100_grctx_generate` on this GPU, from FECS BIND_POINTER to
WFI_GOLDEN_SAVE, is 3628 PGRAPH writes; ours (test_grctx with the Helios
topology, `PG106_GRCTX_DUMP` + `scripts/compare_grctx_trace.py`) are the
same 3628 writes in the same order, values equal except the buffer
addresses and the instance block (placed by our VMM). FECS's replies in
that trace: image 0x87500, zcull 0x50400, pm 0x2300.

Evidence so far is simulation: `tests/test_grctx.c` runs the real GR stage
and the port against a register-semantics FECS in `tests/mockgpu.h`
(mailbox methods, golden save through the bound instance block, context
load + mmio list on the first kick, a QMD's RELEASE0), end to end from
the bridge. The mock FECS does not know what the real ucode saves.

Hardware so far (`pg106_hwtest --gr`, research/hardware/helios300/hw-runs):
- grctx-run4 (bbdbca6): the golden context generated on the GPU, FECS
  image 0x87500 as in nouveau's trace; the first dispatch raised a GR
  trap and the channel stalled.
- grctx-run5 (dfed358, NAK-encoded kernels, three submissions): step 1
  (context load, SET_OBJECT, compute state, host semaphore) completed;
  step 2's launch was rejected by SKED: 0x407020 = 0x1000
  TOTAL_TEMP_SIZE at SEND_SIGNALING_PCAS_B. Cause: the QMD asked for CRS
  0x800 (gallium's value, backed there by a TLS allocation) on a channel
  with no SET_SHADER_LOCAL_MEMORY_*. Fixed in 09fa5f3: SLM 0 + CRS 0, the
  configuration NAK's hardware tests ran on this GPU (NVK run 3), plus
  INVALIDATE_SKED_CACHES.
- grctx-run6 (7853fd0): `gr selftest: ret 0`: QMD release 0x6a5e0001 after
  10 µs, host sem 0x6a5e0002, store 0x6a5ec0de, GP_GET 3, GR_INTR 0.
  Unblocked: EXEC for NVK through the kext (on macOS, untested).

## B18. Linux hardware harness (pg106_hwtest): RESOLVED, READY on the real GPU

**Resolved 2026-11-08 by hardware run 6.** `pascal/tools/pg106_hwtest.c`
runs the portable bring-up from Linux userspace through sysfs BAR mmaps,
with no driver bound. On the Helios GP106 it reached `ready ok bring-up
complete` from a GPU woken out of D3cold:
- POST: PMU falcon reset, then VBIOS DEVINIT;
- nouveau's mc init;
- FB, BAR2/BAR1 and FIFO, with the host-semaphore and copy-engine self-test
  (64 KiB, 0 mismatches);
- the HS ACR, the SEC2 RTOS, and BOOTSTRAP_FALCON(FECS, GPCCS);
- GR up: FECS ctx size 0x87500, the same as nouveau got on this GPU.

The logs and per-run table are in `research/hardware/helios300/hw-runs/`
(README.md there).

Runs 1-5 each failed at a different point. Every fix was a place where we
differed from nouveau, found by comparing nouveau's register trace of this
GPU (posttrace2):

| Run | Failure | Fix |
| --- | --- | --- |
| 1 | POST timeout: PMU falcon never reset | 39df02f |
| 2 | ACR plan asserted SEC2 ALIAS_EN = 0 | 0a56594 (FALCON_START, decided at run time) |
| 3 | 0x502608 = 0x40005 clamped to 0xff | 386e794 |
| (found in the diff) | tile map, zcull words and 0x260 differed from nouveau | eeb41e6 |
| 4 | GPC registers read 0xbadf1200 without nouveau's mc init | c86d5e2 |
| 5 | SEC2 reply wait too short in real time | bbdbca6 |

How to run it again:
1. Stage: `pascal/scripts/stage-hw-run.sh NAME [COMMIT]` builds a static
   binary from a clean export of the commit, plus the firmware, the VBIOS
   and SHA256SUMS.
2. Run: `pascal/scripts/hw-run.sh`, as the staged `run.sh` piped through
   hb. Stages A (POST), B (FIFO + CE selftest) and C (full) each get a
   D3cold cycle first. It stops at the first failure and always restores
   nvidia + gdm.
3. Never run it without the owner's or coordinator's go.

What it answered:
- A GPU that nvidia had driven before needs no special boot. With no driver
  bound it runtime-suspends to D3cold, and waking it gives an unposted GPU
  with no WPR lock.
- The CE executes our pushes.
- The VPR question did not come up: nothing in the path failed on it.

Resolved on the GPU: the GR golden context and a compute dispatch (B17,
`pg106_hwtest --gr`, grctx); everything on macOS (the kext has never been
loaded, B6/B7).

## B16. HDMI display (PG106FB + disp_hw): what only the Mac and the GPU can answer

**First HDMI run, in order (2026-11-08).** One change per boot; stop at the
first step that fails and keep its logs.
0. *No monitor, now:* install PG106FB alone (`ONLY=PG106FB
   tools/macos/install-gpu.sh`), boot, `MODE=status`. `PG106FBSelfCheck` must
   read: display engine up, HPD GPIO 27 (active low) sense 0x00005000 (0xd67c,
   bit 14 set, as in the Linux no-monitor trace) ->
   nothing connected, EDID absent, 1 mode from the fallback (CTA VIC 16),
   aperture BAR1, interrupts routed, power events registered; and
   `PG106FBConnected` No. The Intel panel must be untouched. (Items 1, 4
   without a sink; 6.)
1. Item 1 (disp_host on the Mac) is covered by step 0.
2. Item 4 (HPD polarity): plug a 1080p60 monitor; `hotplug: connected`,
   `PG106FBConnected` Yes.
3. Item 3 (DDC): `EDID: ... bytes` in the log, `PG106FBModeCount` > 1.
4. Item 2 (the link): pick the 1080p60 mode; `setDisplayMode ... -> 0` and a
   picture. The `modeset #N` block is the evidence either way.
5. Item 6 (aperture) and 5 (WindowServer on an unaccelerated framebuffer,
   `pg106fb_accel=0`) only if 4 lit but the desktop does not draw.
6. Item 7 (cursor blend): look at the pointer edges.
7. Item 9 (audio): `audio: on` and the AppleHDA pairing line; play a sound.
8. Item 8 (HDMI 2.0): a 4K60 or 1440p144 sink; `HDMI: ... SCDC written`,
   `Scrambling_Status 01`.
9. Then PG106Accel (accel checklist), then a sleep with the monitor attached
   (`DID_ON` must restore the mode).

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
8. **HDMI 2.0 (> 340 MHz), added 2026-11-08 (disp3).** Implemented after
   nouveau (DISP-PLAN.md §12) and run only against the mock's SCDC sink and
   7 real HDMI 2.0 EDIDs; no scrambled link has been driven on hardware.
   Unknowns in first-run order:
   - SCDC over the bit-banged DDC at 0x54. Symptom: `SCDC not used ...
     err -5` in the `HDMI:` log line; a 4K60/1440p144 modeset then fails
     with -EIO, and ≤ 340 MHz modes light without HDMI packets.
   - The sink locking to the scrambled stream. The `HDMI:` line prints
     `Scrambling_Status`; `01` means locked. `00` plus a `WARNING:` line
     and no picture means the SOR side (0x61c5bc = 3, SOR clock speed 0x14
     + div2) or the 594 MHz IED script (0x5aac, chosen by the ROM's BIT
     'U' table on the mock) is wrong.
   - Reduced-blanking DTDs whose hblank is < 74 (LG HDR 4K: 4K60 at 527.85
     MHz, hblank 70). These get MAX_AC_PACKET 0, which is nouveau's value
     for hblank 74..105, instead of nouveau's u32 wrap. Whether AVI/audio
     packets fit is a hardware question.
   - Monitor power-cycle without an HPD drop at 4K60: `hotplug: SCDC
     re-arm 1` should appear and the picture should return.
   - Not implemented: YCbCr 4:2:0 (the 4:2:0-only VICs of 300-MHz-class
     TVs are not offered; nouveau's dispnv50 has no 4:2:0 output either),
     deep colour (10/12 bpc: needs a 2101010 scanout format through
     WindowServer), FRL/HDMI 2.1 (not on GP106).
9. **HDMI audio, added 2026-11-08 (disp3).** Implemented (DISP-PLAN.md
   §13) but nothing has run on a Mac. In first-run order:
   - **The HDA function is hidden.** The owner's Linux dmesg has
     `01:00.0: Enabling HDA controller` (quirk_nvidia_hda). The firmware
     leaves 0x488 bit 25 clear, and SSDT11's `_ON`/`_PS0` set it (`MLTF`)
     only when GPIO 0x01080001 reads high (most likely HDMI-present). On
     macOS, 01:00.1 then exists only if (a) HDMI was plugged at boot, (b)
     `SSDT-PG106-HDAU.aml` ran (iasl + acpiexec only), or (c) PG106 set
     the bit and its root-port rescan (`kernelRequestProbe`, never run)
     found the function. Only (a) and (b) give NMHDA a function to count
     when AppleHDAController loads. With (c) alone NMHDA reports `no
     NVIDIA HD Audio function found` and audio stays off until the next
     boot.
   - **Owner of the bit: PG106** (kdev4, deedcd5, as kdev3 advised). PG106
     sets it at start and in `doPowerOn` right after `restoreDeviceState`,
     before the bring-up and DID_ON (Linux's resume_early quirk). Its rescan
     runs off the gate after a gated check that the GPU is up
     (`pg106_hda=0` disables it). PG106FB only reads the bit and logs when
     it is clear, so the bus is scanned once per power-on.
   - **The pairing.** AppleHDA has to pick up PG106FB's `hda-gfx` /
     `audio-codec-info` (codec 0, AFG 1, pin NID 7 on the Helios, because
     only SOR3 is HDA-capable). The property names follow NVRMFB, whose
     submitter validated them on an RTX 5080 Laptop. Nobody has checked
     them with an NVIDIA GP106 codec (0x10de0084) on macOS 26. If the pin
     is wrong, override it with `PG106FBAudioCodecInfo`.
   - **N/CTS and the audio infoframe.** Nothing is programmed on the SOR.
     GM200/GP100 SORs have no `hdmi->audio` hook in nouveau, so the HDA
     codec and its HDMI hardware generate them, and AppleHDA writes the
     audio infoframe through the codec's DIP verbs. Whether AppleHDA does
     that for this codec is the open question.
   - `kConnectionEnableAudio` / `kConnectionAudioStreaming`: who calls
     them on macOS 26 is not known; PG106FB honours them.

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

Needs: steps 4e.0, 4e.0b (HDMI 2.0 modes + audio, items 8–9) then 4e of
OWNER-RUNBOOK.md, with their logs (the diagnostic block and the `HDMI:` /
`audio:` lines in particular). Then the HDMI mmiotrace above once a
monitor is available. A 4K60 monitor would also give the first trace of
nouveau's SCDC writes to diff against §12.

## Note: render client access (owner decision, 2026-11-07)

Render connections (user-client type 1, what NVK opens) are open to every
local user by default, so NVK runs in unprivileged app processes. The
decision was relayed by the coordinating session. Trade-off: any local
process can reach an unreviewed kernel parser and wake the dGPU.
`pg106_render_users=0` (boot-arg) or Info.plist `PG106RenderUsers` = false
restricts render connections to administrators. The DEVICE type (raw
identity) is always administrator-only.
