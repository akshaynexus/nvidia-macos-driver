#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Extract the Helios 300 GTX 1060 Mobile VBIOS from Acer's BIOS package.

The Acer Predator G3-571 (Helios 300) has no dGPU ROM chip; the VBIOS lives
in the system BIOS and is served to the OS through ACPI _ROM
(research/evidence/HELIOS300-HW-PROBE.md). Acer's BIOS V1.22 package
(BIOS_Acer_1.22_A_A.zip -> Windows/C5PRH122.exe, the Insyde flasher with
the BIOS image inside) carries five VBIOS images in one LZMA-compressed
firmware volume: three GP107 variants and two GP106 (1c20) revisions. This
picks the GP106 image whose version matches the one the NVIDIA driver
reports on the machine (86.06.4F.00.08) and cuts it to RVBS = 0x29400 bytes.

Usage:
    extract_helios_vbios.py C5PRH122.exe [out.rom]

Download the package from Acer's support page for the Predator G3-571
(BIOS 1.22, 2019/04/01, 6.4 MB) and unzip it first. Acer's site blocks
non-browser clients, so the download is a manual step. Only the stdlib is
used (lzma), so nothing third-party runs on the input.
"""
import hashlib
import lzma
import re
import sys

# EFI_GUID_DEFINED_SECTION GUID for LZMA (EE4E5898-3914-4259-9D6E-DC7BD79403CF).
LZMA_GUID = bytes.fromhex("98584eee143959429d6edc7bd79403cf")
RVBS = 0x29400  # VBIOS size the ACPI _ROM method reports on the machine
WANT_DEVICE = 0x1C20
WANT_VERSION = b"86.06.4F.00.08"
WANT_SHA256 = "de95ee90eaebed9df9bf87ac4a61a2021af2e78512df53e9edb8ae0cca8a0d33"


def lzma_sections(buf, depth=0):
    """Yield every decompressed LZMA GUID-defined section, recursively."""
    yield buf
    if depth > 3:
        return
    for m in re.finditer(re.escape(LZMA_GUID), buf):
        sec = m.start() - 4  # common section header precedes the GUID
        if sec < 0:
            continue
        data_off = int.from_bytes(buf[m.start() + 16:m.start() + 18], "little")
        try:
            out = lzma.LZMADecompressor(format=lzma.FORMAT_ALONE).decompress(
                buf[sec + data_off:])
        except lzma.LZMAError:
            continue
        yield from lzma_sections(out, depth + 1)


def main():
    if len(sys.argv) not in (2, 3):
        sys.exit(__doc__)
    data = open(sys.argv[1], "rb").read()
    out = sys.argv[2] if len(sys.argv) == 3 else "helios300-gp106.rom"
    for buf in lzma_sections(data):
        for m in re.finditer(rb"\x55\xaa", buf):
            img = buf[m.start():m.start() + RVBS]
            if len(img) < 0x200:
                continue
            pcir = int.from_bytes(img[0x18:0x1a], "little")
            if img[pcir:pcir + 4] != b"PCIR":
                continue
            dev = int.from_bytes(img[pcir + 6:pcir + 8], "little")
            if dev != WANT_DEVICE or WANT_VERSION not in img[:0x1000]:
                continue
            digest = hashlib.sha256(img).hexdigest()
            if digest != WANT_SHA256:
                sys.exit(f"found 1c20 {WANT_VERSION.decode()} but sha256 "
                         f"{digest} != expected {WANT_SHA256}")
            open(out, "wb").write(img)
            print(f"wrote {out}: {len(img)} bytes, 10de:{dev:04x}, "
                  f"{WANT_VERSION.decode()}, sha256 {digest}")
            return
    sys.exit("no 10de:1c20 86.06.4F.00.08 image found")


if __name__ == "__main__":
    main()
