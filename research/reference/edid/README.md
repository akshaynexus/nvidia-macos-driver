# Real EDID reference data (Phase 5 gate input)

## Acer ED320QR S — 31.5" 1920x1080 gaming monitor (real dump)

| File | What it is |
|---|---|
| `acer_ed320qr_s_original_dump.txt` | Verbatim dump from the source repo, byte-for-byte as fetched. |
| `acer_ed320qr_s.hex` | Canonical 256-byte EDID (base block + CTA-861 extension), human-readable hex. |
| `acer_ed320qr_s.bin` | The same 256 bytes, binary — this is what `pascal/tests/test_edid.c` loads. |

### Provenance

- Source repository: <https://github.com/linuxhw/EDID> ("EDID repository for LCD
  monitors", the dump collection behind linux-hardware.org), file
  `Digital/Acer/ACR0000/53D1F4B469C0`, fetched 2026-11-05 from
  `https://raw.githubusercontent.com/linuxhw/EDID/master/Digital/Acer/ACR0000/53D1F4B469C0`.
- The repo is licensed CC-BY-4.0 (see its `LICENSE`); the dump is a factual
  hardware read attributed here accordingly. These EDID bytes are raw monitor
  data, not software, and are redistributed with that attribution.
- Monitor: Acer (ACR) model 0, name string `ED320QR S`, made week 8 of 2021,
  1920x1080 native, 698 mm x 393 mm, EDID 1.4, one CTA-861 rev 3 extension.
  A 1080p 165 Hz FreeSync monitor of exactly the class a Helios 300's dGPU
  HDMI/miniDP outputs would drive — which is why it was picked for the
  Phase 5 simulated mode-set work.

### Why the canonical file is 256 bytes when the dump shows 512

The original dump contains the base block + CTA extension **twice** (its own
edid-decode run flags this: "EDID specified 1 extension block(s), but found 3
extension block(s)" and "Unknown Extension Block" — a dump-side duplication
artifact, not part of the monitor's declaration). The canonical EDID is the
first instance of each block: bytes 0-127 (base block, checksum byte 0x36)
and bytes 128-255 (CTA-861 rev 3 extension, checksum byte 0xde). Blocks 2 and
3 of the dump are byte-identical duplicates of blocks 0 and 1.

### Verification performed before committing (2026-11-05)

- Base block: header `00 ff ff ff ff ff ff 00`; sum of all 128 bytes ≡ 0
  (mod 256); extension count (byte 126) = 1.
- Extension block: tag 0x02 (CTA-861), revision 3, sum ≡ 0 (mod 256).
- Structure cross-checked against the edid-decode output embedded in the
  original dump: first Detailed Timing Descriptor 1920x1080 @ 60.000 Hz,
  148.500 MHz (Hfront 88 / Hsync 44 / Hback 148, Vfront 4 / Vsync 5 /
  Vback 36); CTA video data block contains VIC 16 (1920x1080 @ 60 Hz) with
  the native bit set, plus VICs 1, 3, 5, 20, 4, 19, 31, 18, 2, 17, 63.

### Intended use

`pascal/tests/test_edid.c` loads `acer_ed320qr_s.bin` and asserts the parsed
1080p numbers; the printed timing summary is the input for the Phase 5
simulated 1080p mode-set sequence (see HANDOFF-PASCAL.md §6 Phase 5).
