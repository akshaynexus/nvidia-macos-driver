# Resolution: pushbuffer method-header type field (the §5 open question)

Date: 2026-11-05. Status: **resolved against primary sources.**

## The question

`helios300-gp106-simtest.py` flagged that tinygrad's `nvm()` encodes the pushbuffer
type field as `typ << 28`, while the documented layout says bits [31:29]/[31:30].
Which is correct for GP106 submission?

## Answer: the type field is SEC_OP, bits [31:29], three bits wide

Authoritative source: `third_party/open-gpu-kernel-modules:src/common/sdk/nvidia/inc/class/clc06f.h` — NVIDIA's own class header for
`PASCAL_CHANNEL_GPFIFO_A` (0xC06F), MIT-licensed, fetched from
`NVIDIA/open-gpu-kernel-modules` `src/common/sdk/nvidia/inc/class/clc06f.h`
(saved 2026-11-05). That header states its GPFIFO/method documentation comes from
`dev_pbdma.ref` / `dev_ram.ref`, i.e. it *is* the dev_pbdma.ref-derived encoding.

Fields (all names quoted verbatim from clc06f.h):

```
NVC06F_DMA_SEC_OP            31:29   (3 bits, not 2)
  0 = GRP0_USE_TERT   (TERT_OP at 17:16 selects legacy op)
  1 = INC_METHOD      (count at 28:16, address at 11:0, data words follow)
  2 = GRP2_USE_TERT   (legacy: OPCODE3 17:16 = 0 means NON_INC_METHOD)
  3 = NON_INC_METHOD
  4 = IMMD_DATA_METHOD (immediate data at 28:16)
  5 = ONE_INC
  7 = END_PB_SEGMENT
NVC06F_DMA_METHOD_ADDRESS    11:0    (method >> 2 goes here in the new format)
NVC06F_DMA_METHOD_ADDRESS_OLD 12:2   (legacy layout; bit 12 unused in new format)
NVC06F_DMA_METHOD_COUNT      28:16
NVC06F_DMA_METHOD_SUBCHANNEL 15:13
```

Canonical Pascal method word (increasing):

```
word = (1 << 29) | (count << 16) | (subch << 13) | (mthd >> 2)
```

## Why tinygrad's `typ << 28` still "works"

`typ = 2` gives `2 << 28 == 1 << 29`, i.e. exactly SEC_OP = 1 (`INC_METHOD`) with
the COUNT field at [28:16] untouched (count < 0x2000 never reaches bit 28). So
tinygrad's `typ=2` form happens to *be* the canonical increasing-method word.
`typ = 1` (`1 << 28`) is different: SEC_OP = 0 selects the legacy TERT path where
`TERT_OP` [17:16] aliases the low two bits of COUNT — with count = 1 that reads
as TERT_OP = 1 (`SET_SUB_DEV_MASK`), a corrupt word. Conclusion unchanged: emit
only canonical SEC_OP values; treat tinygrad's shorthand as an INC-only spelling.

## GPFIFO entry (64-bit), same source

```
ENTRY0: FETCH 0:0, GET 31:2                -> dword address of pushbuffer segment
ENTRY1: GET_HI 7:0, PRIV 8:8, LEVEL 9:9,
        LENGTH 30:10, SYNC 31:31           -> absolute bits 39:32 .. 63:42
LEVEL_MAIN = 0, LEVEL_SUBROUTINE = 1       (NVC06F_GP_ENTRY1_LEVEL_*)
SYNC_PROCEED = 0, SYNC_WAIT = 1
```

**This corrects the existing simtest**, which asserted `LEVEL_MAIN → bit == 1`
(1 is SUBROUTINE). `helios300-gp106-simtest.py` is updated accordingly, and now
also checks the canonical SEC_OP encodings (1/3/4) and the tinygrad-compat
legacy decoding, so the resolution is regression-locked.

## Consequence for the port

The Phase 0 pushbuffer encoder (`pascal/core`) implements the canonical format:
SEC_OP at [31:29], address at [11:0], and never emits the legacy TERT forms.
Golden vectors are in `pascal/tests/test_pushbuf.c`, each annotated with the
`clc06f.h` macro it is derived from.
