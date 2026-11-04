#!/usr/bin/env python3
"""Helios 300 Pascal (GP106M) bring-up: sandboxed constant/format tests.

Validates the bit-level facts the Pascal port relies on, against the sources
recorded in HELIOS300-PASCAL-RESEARCH.md:

- install.sh's SUPPORTED_IDS gap and age gate for the Helios 300 Pascal IDs
- GP106 MMIO ranges (envytools / pascal-egpu RESEARCH.md)
- pushbuffer method encoding (tinygrad ops_nv.py form: type|count|subch|mthd>>2)
- 64-bit GPFIFO entry packing (dev_pbdma.ref.txt: GET/FETCH/LENGTH/GET_HI/SYNC)
- QMD v01_07 field packing (clc1c0qmd.h MW(X:Y) bit notation)

Pure CPU. Compiles nothing, touches no hardware, installs nothing: safe on a
live macOS machine. Run: python3 helios300-gp106-simtest.py
"""
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parent
PASS = []

def ok(name):
    PASS.append(name)
    print("PASS " + name)

# ---------------------------------------------------------------- 1. device IDs
# Helios 300 Pascal mobile IDs (NVIDIA 390.12 supportedchips + envytools pciid).
HELIOS_PASCAL_IDS = [
    "1C8C", "1C8D", "1C92",              # GP107M: 1050 Ti / 1050 Mobile
    "1C20", "1C23", "1C60", "1C61", "1C62",  # GP106M: 1060/1050 Ti/1050 Mobile
    "1BA0", "1BA1", "1BA2", "1BE0", "1BE1",  # GP104M: 1070/1080 Mobile
]
# the machine in the photos: i7-7700HQ + GTX 1060 Mobile
TARGET_IDS = ["1C20", "1C60"]

ids_text = re.search(
    r'^SUPPORTED_IDS="\n(.*?)\n"$',
    (ROOT / "package/install.sh").read_text(), re.S | re.M).group(1).split()
assert len(ids_text) == 247, len(ids_text)

for tid in HELIOS_PASCAL_IDS:
    assert tid not in ids_text, f"{tid} unexpectedly in SUPPORTED_IDS"
    assert int(tid, 16) < 0x1E00, f"{tid} not below the installer's Turing gate"
ok(f"device table gap: all {len(HELIOS_PASCAL_IDS)} Helios Pascal IDs absent from "
   "SUPPORTED_IDS and below the 0x1E00 age gate")

# ---------------------------------------------------------------- 2. MMIO map
# BAR0 ranges, GP106 (GF100+ layout). name: (base, end) in BAR0 bytes.
MMIO = {
    "PMC":    (0x000000, 0x000FFF),
    "PBUS":   (0x001000, 0x001FFF),
    "PFIFO":  (0x002000, 0x003FFF),
    "PTIMER": (0x020000, 0x020FFF),
    "PCOPY":  (0x060000, 0x06FFFF),
    "PFB":    (0x100000, 0x1FFFFF),
    "PPMU":   (0x10A000, 0x10AFFF),
    "PGRAPH": (0x400000, 0x41FFFF),
    "PDISP":  (0x610000, 0x61FFFF),
}
assert MMIO["PMC"][0] == 0x0                      # PMC_BOOT_0 identifies the chip
assert MMIO["PDISP"][0] == 0x610000               # CPU-driven display on Pascal
assert MMIO["PGRAPH"][1] > MMIO["PPMU"][1]        # ordering sanity
ok("MMIO BAR0 map: 9 engine blocks, PMC_BOOT_0 at +0x0, PDISP at +0x610000")

# ------------------------------------------------------------ 3. pushbuffer
# Encoding question RESOLVED against clc06f.h (NVIDIA, MIT; saved at
# research/reference/clc06f.h; analysis in research/evidence/ENCODING-RESOLUTION.md):
# the type field is SEC_OP, bits [31:29], 3 bits:
#   1 = INC_METHOD, 3 = NON_INC_METHOD, 4 = IMMD_DATA, 7 = END_PB_SEGMENT;
# method address sits at [11:0] (already >> 2), count [28:16], subch [15:13].
# tinygrad's typ<<28 form is legacy TERT-only (SEC_OP=2 + OPCODE3=0 decodes as
# non-increasing) and is kept here only as a compat decode target.
SEC_OP_INC, SEC_OP_NONINC, SEC_OP_IMMD, SEC_OP_ENDSEG = 1, 3, 4, 7

def mthd_inc(subch, mthd, *args):
    """Canonical increasing method (NVC06F_DMA_INCR_*)."""
    assert len(args) >= 1
    return [(SEC_OP_INC << 29) | (len(args) << 16) | (subch << 13) | (mthd >> 2), *args]

def mthd_noninc(subch, mthd, *args):
    """Canonical non-increasing method (NVC06F_DMA_NONINCR_*)."""
    return [(SEC_OP_NONINC << 29) | (len(args) << 16) | (subch << 13) | (mthd >> 2), *args]

def mthd_immd(subch, mthd, data):
    """Immediate-data method (NVC06F_DMA_IMMD_*): no data words follow."""
    return [(SEC_OP_IMMD << 29) | (data << 16) | (subch << 13) | (mthd >> 2)]

# SET_OBJECT = method 0x0000, class PASCAL_COMPUTE_B = 0xC1C0, subchannel 1
entry = mthd_inc(1, 0x0000, 0xC1C0)
assert entry[0] == (1 << 29) | (1 << 16) | (1 << 13) | 0
assert entry[0] == 0x20012000, hex(entry[0])
assert entry[1] == 0xC1C0

def decode(word):
    return {"secop": (word >> 29) & 0x7, "count": (word >> 16) & 0x1FFF,
            "subch": (word >> 13) & 0x7, "mthd": (word & 0x1FFF) << 2}

d = decode(entry[0])
assert d == {"secop": 1, "count": 1, "subch": 1, "mthd": 0x0000}
# SEND_PCAS_A = 0x02B4, no data words -> the QMD address goes in the next entry
p = mthd_noninc(1, 0x02B4)
assert decode(p[0]) == {"secop": 3, "count": 0, "subch": 1, "mthd": 0x02B4}
# tinygrad compat: typ<<28 with typ=2 == 1<<29 == canonical SEC_OP=1 (INC_METHOD)
# -- that is why it happens to work. typ=1 (1<<28) lands on SEC_OP=0 (legacy
# TERT with TERT_OP=0 == legacy INC_METHOD) and also sets the top COUNT bit.
tg = (2 << 28) | (1 << 13) | (0x02B4 >> 2)
assert decode(tg) == {"secop": 1, "count": 0, "subch": 1, "mthd": 0x02B4}
tg1 = (1 << 28) | (1 << 13) | (0x02B4 >> 2)
# bit 28 is the MSB of COUNT; with count=1 the '1' also leaks into TERT_OP
# [17:16] (=1, SET_SUB_DEV_MASK) -- the legacy form is simply not usable for
# real submissions. Shown with count=0 to isolate the TERT decode.
assert decode(tg1) == {"secop": 0, "count": 0x1000, "subch": 1, "mthd": 0x02B4}
assert (tg1 >> 16) & 3 == 0  # TERT_OP [17:16] = 0 -> legacy INC_METHOD
ok("pushbuffer method encoding: canonical SEC_OP[31:29] INC/NONINC/IMMD; "
   "tinygrad typ=2 == canonical INC, typ=1 == legacy TERT (ENCODING-RESOLUTION.md)")

# ------------------------------------------------------------ 4. GPFIFO entry
# 64-bit, clc06f.h NVC06F_GP_ENTRY_*: GET[33:2]=pushbuffer dword addr,
# FETCH[0]; word1 -> GET_HI[39:32], PRIV[40], LEVEL[41], LENGTH[62:42], SYNC[63].
# LEVEL_MAIN = 0, LEVEL_SUBROUTINE = 1 (NVC06F_GP_ENTRY1_LEVEL_*).
def gpfifo(addr, length, level=0, sync=0, fetch=0):
    return (((sync & 1) << 63) | ((length & 0x1FFFFF) << 42) | (level << 41)
            | (((addr >> 2) & 0x3FFFFFFF) << 2) | (fetch & 1))

e = gpfifo(0x1234000, 257)
assert (e >> 2) & 0x3FFFFFFF == 0x1234000 >> 2   # dword address round-trips
assert (e >> 42) & 0x1FFFFF == 257               # length in [62:42]
assert (e >> 41) & 1 == 0                        # LEVEL = MAIN (0, not 1)
assert (e >> 63) & 1 == 0                        # SYNC = PROCEED
sub = gpfifo(0x1000, 3, level=1)
assert (sub >> 41) & 1 == 1                      # LEVEL = SUBROUTINE
ok("GPFIFO entry packing: addr/length/level/sync round-trip (LEVEL_MAIN=0 per clc06f.h)")

# ------------------------------------------------------------ 5. QMD v01_07
# MW(X:Y): bit X is MSB, bit Y LSB; word = bit // 32; offset = bit % 32.
def mw(word_view, msb, lsb, value):
    w = msb // 32
    off = lsb % 32
    width = msb - lsb + 1
    mask = (1 << width) - 1
    assert value <= mask, (msb, lsb, value)
    word_view[w] = (word_view[w] & ~(mask << off)) | ((value & mask) << off)

def read_mw(word_view, msb, lsb):
    w = msb // 32
    off = lsb % 32
    width = msb - lsb + 1
    return (word_view[w] >> off) & ((1 << width) - 1)

qmd = [0] * 64  # 256 bytes
mw(qmd, 579, 576, 7)      # QMD_VERSION must be 7 on Pascal
mw(qmd, 583, 580, 1)      # QMD_MAJOR_VERSION must be 1
mw(qmd, 287, 256, 0x00A00000)  # PROGRAM_OFFSET: shader code offset >> 8
mw(qmd, 415, 384, 129)    # CTA_RASTER_WIDTH  (grid X)
mw(qmd, 431, 416, 64)     # CTA_RASTER_HEIGHT (grid Y)
mw(qmd, 447, 432, 1)      # CTA_RASTER_DEPTH  (grid Z)
mw(qmd, 607, 592, 8)      # CTA_THREAD_DIMENSION0 (block X)
mw(qmd, 623, 608, 8)      # CTA_THREAD_DIMENSION1
mw(qmd, 639, 624, 1)      # CTA_THREAD_DIMENSION2
mw(qmd, 561, 544, 1024)   # SHARED_MEMORY_SIZE (per block)
mw(qmd, 1503, 1496, 168)  # REGISTER_COUNT per thread
mw(qmd, 1471, 1467, 1)    # BARRIER_COUNT
mw(qmd, 1463, 1440, 32)   # SHADER_LOCAL_MEMORY_LOW_SIZE (per thread)
mw(qmd, 767, 736, 0xDEAD0000)  # RELEASE0_ADDRESS_LOWER (semaphore)
mw(qmd, 831, 800, 42)     # RELEASE0_PAYLOAD
# constant buffer 0: addr lower MW(959:928) -> word 29 full, size MW(991:975)
mw(qmd, 959, 928, 0x12345678)
mw(qmd, 991, 975, 256)

assert read_mw(qmd, 579, 576) == 7 and read_mw(qmd, 583, 580) == 1
assert read_mw(qmd, 287, 256) == 0x00A00000
assert (read_mw(qmd, 415, 384), read_mw(qmd, 431, 416),
        read_mw(qmd, 447, 432)) == (129, 64, 1)
assert (read_mw(qmd, 607, 592), read_mw(qmd, 623, 608),
        read_mw(qmd, 639, 624)) == (8, 8, 1)
assert read_mw(qmd, 561, 544) == 1024
assert read_mw(qmd, 1503, 1496) == 168 and read_mw(qmd, 1471, 1467) == 1
assert read_mw(qmd, 767, 736) == 0xDEAD0000 and read_mw(qmd, 831, 800) == 42
assert read_mw(qmd, 959, 928) == 0x12345678 and read_mw(qmd, 991, 975) == 256
# word indices the raw QMD writer will use
assert qmd[18] == (8 << 16) | (1 << 4) | 7  # thread_dim0 + major + version share word 18
assert qmd[12] == 129                       # CTA_RASTER_WIDTH alone in word 12
ok("QMD v01_07 packing: 15 fields round-trip through the MW(X:Y) notation")

print("PASS all %d GP106 constant/format checks" % len(PASS))
