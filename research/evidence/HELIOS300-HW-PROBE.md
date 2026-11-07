# HELIOS300-HW-PROBE — first look at the real target machine

Date: 2026-11-07. The machine's owner asked for this probe and authorised it.
It ran over SSH on the LAN, against the running CachyOS install, and was
read-only: no GPU registers were written, nothing was installed, and no driver
was changed. Raw outputs (ACPI tables, `lspci -vvv`, `nvidia-smi -q`, dmesg)
are kept locally in `research/hardware/helios300/raw/`, which is gitignored
because the ACPI tables are Acer firmware.

## Machine

| | |
|---|---|
| DMI | Acer Predator G3-571 (Helios 300), board `Sienta_KLS`, BIOS V1.22 (2019-03-15) |
| CPU / iGPU | Kaby Lake-H, HD Graphics 630 `8086:591b` (00:02.0) |
| dGPU | GP106M GeForce GTX 1060 Mobile, `10de:1c20` rev a1, subsystem `1025:118b`, part `1C20-750-A1` (01:00.0) |
| dGPU audio | `10de:10f1` (01:00.1) |
| VBIOS | **86.06.4F.00.08** (as reported by the NVIDIA driver) |
| OS during probe | CachyOS, kernel 7.3.1 (built with clang, ThinLTO), NVIDIA 580.196.07 bound |

## PCI

- BAR0 is 16 MiB at 0xa3000000 (MMIO). BAR1 is 256 MiB prefetchable (VRAM
  aperture), BAR3 is 32 MiB prefetchable, and BAR5 is 128 I/O ports.
- The device has a 512 KiB expansion ROM BAR, but **reading the PCI ROM fails
  (`sysfs rom` returns EIO)**. Like most Optimus laptops, the board has no
  dGPU ROM chip of its own.
- The link is rated 8 GT/s x16 and was running at 2.5 GT/s x16 at idle (power
  state P8).
- Runtime PM is set to `auto` and the device was in D0 during the probe.

## Display wiring (confirms the no-MUX assumption, corrects the port list)

- The NVIDIA DRM device exposes **one connector, `HDMI-A-1`** (disconnected
  during the probe).
- The Intel device exposes **`eDP-1`**, the internal panel.
- **No DisplayPort/miniDP connector exists on the dGPU.** The research doc
  assumed HDMI + miniDP on the dGPU; on this unit only HDMI is wired. The DP
  model in `disp_core` is therefore not needed for this machine. (The HDA
  function always lists four HDMI/DP PCMs whatever the board wiring, so they
  are not evidence of DP.)

## Where the VBIOS lives

- The VBIOS comes from **ACPI `_ROM`** on `\_SB.PCI0.PEG0.PEGP`, in SSDT11
  (OEM `ACRSYS`/`ACRPRDCT`).
- `_ROM` reads a firmware-filled RAM copy, OperationRegion `VBOR` at physical
  0x7AE16018 (in ACPI NVS). It holds the dword `RVBS` followed by eight 32 KiB
  fields.
- **`RVBS` = 0x29400 (168,960 bytes).** This is the full multi-image VBIOS,
  not the 0xf000 first image the substitute dumps were cut to. It should
  therefore include the PMU DEVINIT application that
  `gm200_devinit_post` needs (DISP/POSTED findings).
- **Obtained 2026-11-07** by extracting it from Acer's BIOS V1.22 package.
  See `research/firmware/vbios/README.md` and
  `pascal/scripts/extract_helios_vbios.py`.
  - The extracted image is 10de:1c20, 86.06.4F.00.08, exactly 0x29400 bytes.
  - Its first 16 bytes equal the ones read live through `_ROM` on the
    machine.
  - What the substitute dumps lacked is present: image0 0xf200, two 0xe0
    images and a 0x70 image.
  - The PMU DEVINIT application is present: `test_devinit_pmu` runs the full
    `gm200_devinit_post` upload (9,643 bus steps, PRE_OS app present).
  - The DCB (v4.1) has **one** active output: TMDS on OR mask 0x2,
    sublink A, connector 2, I2C 6. Its 11 other entries are unused
    templates. This is the firmware's own confirmation that the dGPU drives
    HDMI only.
- Live reading from the machine was not completed (no longer needed):
  - `/dev/mem` is blocked (`CONFIG_STRICT_DEVMEM=y`, `CONFIG_IO_STRICT_DEVMEM=y`).
  - The `acpi_call` route was stopped before the dump; see BLOCKERS B1.
- Ways to obtain it, least to most intrusive:
  1. Extract it from Acer's BIOS V1.22 update image with UEFITool. Nothing
     runs on the laptop.
  2. Temporarily load `acpi_call` (built against the running kernel with
     `LLVM=1`), call `_ROM` in chunks, then unload it.
  3. Boot nouveau once and copy `/sys/kernel/debug/dri/*/vbios.rom`. nouveau
     fetches the VBIOS through the same `_ROM` method.

## Power (relevant to Phase 6 and the "no runtime D3" limit)

From SSDT11, under `PEG0`/`PEGP`:

- Power resource `PG00` with `_ON`/`_OFF`, referenced from `_PR0` and `_PR3`.
  This is a **platform-controlled D3cold**: ACPI cuts the dGPU's power.
- `HGON`/`HGOF` (hybrid-graphics on/off), `SGPO` (GPIO helper),
  `_PS0`/`_PS3`.
- `NVOP` (Optimus `_DSM`), `GPS` (GPU power steering), and `GC6I`/`GC6O`
  (GC6 entry/exit).

The handoff's accepted limit "no runtime D3" assumed dGPU power-off needs PMU
firmware. On this machine a full power-off is an **ACPI** operation (`_PR3` →
`PG00._OFF`), which a macOS driver can request through IOACPIPlatformDevice
without any NVIDIA firmware. Reclocking still needs the PMU (that limit
stands). This changes the Phase 6 power design: the dGPU can be fully powered
off while idle, and it then needs a re-POST (`gm200_devinit_post`) on power-up.

## What this settles / opens

- **Settled:**
  - The device ID and subsystem match the kext's personality (`10de:1c20`).
  - The VBIOS version is known.
  - There is no dGPU ROM chip (the VBIOS comes through ACPI `_ROM`).
  - The full VBIOS is 0x29400 bytes.
  - Only HDMI is wired to the dGPU.
  - ACPI D3cold is available.
- **Open:**
  - On macOS, the kext must fetch the VBIOS through ACPI `_ROM` (or a
    shipped copy). It cannot use the PCI ROM BAR.
  - Whether OpenCore's ACPI patches on this machine leave `PG00`/`_ROM`
    intact.
