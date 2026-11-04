# vbios — real GP10x VBIOS ROM images

Real NVIDIA VBIOS images for the Phase 1 gate: proving the BIT parser
(`pascal/core/src/vbios_bit.c`) and the DEVINIT interpreter
(`pascal/core/src/devinit.c`) against actual NVIDIA ROM code rather than the
synthetic image of `pascal/tests/test_vbios.c`.

## Provenance and licensing

Every file is an unmodified VBIOS image from the TechPowerUp VBIOS database
(https://www.techpowerup.com/vgabios/ — public ROM dumps submitted by card
owners), fetched 2026-11-05 through the Internet Archive's raw captures
(`web.archive.org/web/<ts>id_/<original-url>`) because TechPowerUp's direct
download endpoint is gated behind a browser-fingerprint proof-of-work bot
check that rejects non-interactive clients. The Wayback captures serve the
bytes TPU itself served at capture time; each capture was verified against
the TPU database listing (card name + version string) before carving, see
"Carving" below.

These are proprietary NVIDIA firmware. They are committed here under the
same justification as the `research/firmware/gp106/` GSP/ACR blobs: NVIDIA's
linux-firmware license permits redistribution of the firmware as part of a
driver package; the VBIOSes play the same role for the devinit
interpreter (interoperability research, HANDOFF-PASCAL.md §6 Phase 1).
They are NOT GPL, NOT PolyForm — never link or embed them in the kext tree.

## What is committed

| file | card (TPU listing) | PCI ID (PCIR) | chip | size | sha256 |
|------|--------------------|---------------|------|------|--------|
| nvidia-gtx1060-3gb-tpu208872.rom | NVIDIA GeForce GTX 1060 3 GB | 10de:1c02 | GP106 | 0xf000 | `956a83578de785e19b40cc3d197d47e3e3e2edc1e22c040fb3b96a49c187837e` |
| gigabyte-gtx1060-6gb-tpu186364.rom | Gigabyte GTX 1060 6 GB | 10de:1c03 | GP106 | 0xf000 | `680decacefcda1a19738ffdafc382d5748ad520a57cd3d26a7561328b3594f1c` |
| asus-gtx1050ti-tpu188459.rom | ASUS GTX 1050 Ti 4 GB | 10de:1c82 | GP107 | 0xf000 | `ac47f97f7e2080e08305349fac03fa12d24d78f5ace3e7eda1b1e035ba636248` |
| msi-gtx1050-tpu193426.rom | MSI GTX 1050 2 GB | 10de:1c81 | GP107 | 0xf000 | `634237a26b5f6366ab3341dab8cab98d6de727b7dd883763744814b75556cd3e` |
| msi-gtx1050ti-mobile-tpu241139.rom | MSI GTX 1050 Ti (TPU listing; PCIR says **GP107M mobile**, 10de:1c8c = GTX 1050 Ti Mobile per pci.ids) | 10de:1c8c | GP107M | 0x28000 | `8e226bd93e442070b7d276cc6be8a81f725f3cdf0ff4665efcaaaec92176f328` |

Source URLs (TPU database page = the id; the .rom link is what the Wayback
capture preserved):

| file | TPU page | captured .rom URL |
|------|----------|-------------------|
| nvidia-gtx1060-3gb-tpu208872.rom | https://www.techpowerup.com/vgabios/208872/208872 | https://www.techpowerup.com/vgabios/208872/NVIDIA.GTX10603GB.3072.181212.rom |
| gigabyte-gtx1060-6gb-tpu186364.rom | https://www.techpowerup.com/vgabios/186364/186364 | https://www.techpowerup.com/vgabios/186364/Gigabyte.GTX1060.6144.160815.rom |
| asus-gtx1050ti-tpu188459.rom | https://www.techpowerup.com/vgabios/188459/188459 | https://www.techpowerup.com/vgabios/188459/Asus.GTX1050Ti.4096.161020.rom |
| msi-gtx1050-tpu193426.rom | https://www.techpowerup.com/vgabios/193426/193426 | https://www.techpowerup.com/vgabios/193426/MSI.GTX1050.2048.170109.rom |
| msi-gtx1050ti-mobile-tpu241139.rom | https://www.techpowerup.com/vgabios/241139/241139 | https://www.techpowerup.com/vgabios/241139/MSI.GTX1050Ti.4096.171123.rom |

Wayback capture timestamps used (`id_` = raw original bytes):
208872 @ 2019-05-13, 186364 @ 2022-07-11, 188459 @ 2019-08-21,
193426 @ 2023-03-14, 241139 @ 2022-06-14.

### Substitution note (Helios 300 GP106M)

The Helios 300's exact image (GTX 1060 Mobile, 10de:1c20/1c60) was NOT
obtainable: no GTX 1060 Mobile capture exists in the Wayback corpus
(checked all `application/octet-stream` captures of
`techpowerup.com/vgabios/*` with HTTP 200 — desktop 1060/1050/1050 Ti and
RTX-mobile only), and TPU's live download endpoint rejects scripted
clients. The committed set therefore substitutes the same chip families:
two desktop GP106 images (same die as GP106M), two GP107, and one genuine
GP107M mobile image. This replaces BLOCKERS.md B1's synthetic substitute
for the *parser/interpreter* gate; the owner's own dump remains the final
device evidence (HANDOFF-PASCAL.md §7 — simulation evidence either way,
until run on the real card).

### Carving

Four of the five Wayback captures are full SPI flash dumps with a 0xa00-byte
`NVGI` header in front of the expansion ROM (TPU-era serving format for
these uploads). The committed files are the expansion-ROM region carved out
at the first `0x55aa`, sized by the PCIR image-length field
(`u16 at pcir+0x10` × 512), exactly the region nouveau's shadow code
selects. Every carved image self-verifies:

- `0x55aa` at offset 0,
- `PCIR` structure in range, vendor `10de`, device id matching the card,
- byte-sum over the PCIR image size == 0 (x86 expansion-ROM checksum),
- `ff b8 'B' 'I' 'T'` signature present.

`nvidia-gtx1060-3gb-tpu208872.rom` needed no carving (already a plain
0xf000 expansion ROM). The `NVGI` wrappers and flash tails were discarded.

### Version strings (BIT 'i' token, nvkm order major.chip.minor.micro.patch)

| file | version |
|------|---------|
| nvidia-gtx1060-3gb-tpu208872.rom | 70.26.23.00.01 |
| gigabyte-gtx1060-6gb-tpu186364.rom | 86.06.0e.00.c2 |
| asus-gtx1050ti-tpu188459.rom | 86.07.22.00.6a |
| msi-gtx1050-tpu193426.rom | 86.07.39.00.85 |
| msi-gtx1050ti-mobile-tpu241139.rom | 86.07.2b.00.00 |

## Consumers

- `pascal/tests/test_vbios_real.c` — ROM probe + BIT token directory +
  init-script resolution for every file, cross-ROM invariants.
- `pascal/tests/test_devinit_real.c` — runs the smallest image's init
  script 0 through the real DEVINIT interpreter (`pg106_init_run`) with the
  mock bus, twice, and diffs the traces (Phase 1 determinism gate on a real
  ROM for the first time).
