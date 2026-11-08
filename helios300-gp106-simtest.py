#!/usr/bin/env python3
"""Helios 300 Pascal (GP106M) bring-up: sandboxed constant/format tests.

Validates the bit-level facts the Pascal port relies on, against the sources
recorded in HELIOS300-PASCAL-RESEARCH.md:

- install.sh's SUPPORTED_IDS gap and age gate for the Helios 300 Pascal IDs
- GP106 MMIO ranges (NVIDIA SDK swref headers + nouveau; was envytools)
- pushbuffer method encoding (tinygrad ops_nv.py form: type|count|subch|mthd>>2)
- 64-bit GPFIFO entry packing (dev_pbdma.ref.txt: GET/FETCH/LENGTH/GET_HI/SYNC)
- QMD v02_01 field packing (clc1c0qmd.h MW(X:Y) bit notation)

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
# BAR0 ranges, GP106 (GF100+ layout), from the NVIDIA SDK swref headers
# (third_party/open-gpu-kernel-modules src/common/inc/swref/published/...)
# + nouveau — the old envytools NV50-era map was wrong on GF100+ (PTIMER
# moved to 0x9400; there is no fixed PCOPY window). Full old-vs-new table:
# research/evidence/MMIO-MAP-CORRECTIONS.md. name: (base, end) in BAR0 bytes.
MMIO = {
    # gm107/dev_pmc.h:27 NV_PMC_BOOT_0 = 0x00000000, :38 NV_PMC_ENABLE 0x200
    "PMC":    (0x000000, 0x000FFF),
    # gm107/dev_bus.h:26 NV_PBUS_INTR_0 0x1100, :43 BAR0_WINDOW 0x1700,
    # :63 BAR2_BLOCK 0x1714 — PBUS stayed in the 0x001000 page on GF100+
    "PBUS":   (0x001000, 0x001FFF),
    # gm107/dev_fifo.h:26 NV_PFIFO_FB_IFACE 0x26f0; nouveau fifo/gk104.c
    # runlist 0x2270/0x2274, intr 0x2100, runlist kick 0x2a00
    "PFIFO":  (0x002000, 0x003FFF),
    # MOVED from 0x020000 (NV50-era): gm107/dev_timer.h:26
    # NV_PTIMER_TIME_0 = 0x9400 (TIME_1 0x9410; block spans 0x9100-0x9480)
    "PTIMER": (0x009000, 0x009FFF),
    # NEW: PTOP/device-info page. ga100/dev_top.h:28 NV_PTOP_DEVICE_INFO2
    # 0x22800, :29 DEVICE_INFO_CFG 0x224fc; top table 0x022700+i*4
    # (nouveau top/gk104.c:43); devinit-ran latch 0x02240c, disable flags
    # 0x022500 (nouveau devinit/gf100.c:70,101)
    "PTOP":   (0x022000, 0x022FFF),
    # gp102/dev_fb.h:26 NV_PFB_PRI_MMU_LOCAL_MEMORY_RANGE 0x100ce0;
    # gm107/dev_fb.h:26 0x100c10, :29 0x100cb8; WPR check 0x100cd4
    # (nouveau acr/gm200.c:57)
    "PFB":    (0x100000, 0x1FFFFF),
    # PMU falcon base, nouveau pmu/base.c:145
    # nvkm_falcon_ctor(..., 0x10a000, &pmu->falcon)
    "PPMU":   (0x10A000, 0x10AFFF),
    # top-table GR base 0x400000 (test_top.c golden; type 0 = GR in
    # top/gk104.c:76); GPCCS falcon 0x41a000 caps the window
    # (nouveau gr/gf100.c:2649)
    "PGRAPH": (0x400000, 0x41FFFF),
    # disp/v04_02/dev_disp.h:26 NV_PDISP = 0x006F1FFF:0x00610000; split
    # here into direct regs (SOR power 0x61c004, nv50.c nv50_sor_power)
    "PDISP":  (0x610000, 0x61FFFF),
    # Display channel USER/PUT window (0x640000+ctrl*0x1000, gp102.c
    # gp102_disp_core_init) + ARMED-state mirror (gf119.c:429 readback
    # 0x640414+hoff). Not a method window: methods come from the DMA pushbuffer.
    "PEVO":   (0x640000, 0x64FFFF),
    # PBDMA i at 0x040000 + i*0x2000 (nouveau fifo/gk104.c:311-330); live
    # count PFIFO 0x002004[7:0] (fifo/gm200.c:31); window for 16 units
    "PBDMA":  (0x040000, 0x05FFFF),
    # per-GPC GR units: GPC_UNIT 0x500000 + gpc*0x8000, GPC_MAX 32
    # (nouveau gr/gf100.h:36,43-45). GR does not end at 0x41FFFF.
    "PGPC":   (0x500000, 0x5FFFFF),
    # gm107/dev_ram.h:26 NV_PRAMIN = 0x007FFFFF:0x00700000
    "PRAMIN": (0x700000, 0x7FFFFF),
    # PCCSR 0x800000 + chid*8, 12-bit chid (nouveau fifo/gk104.c:44-77,313)
    "PCCSR":  (0x800000, 0x807FFF),
}
assert MMIO["PMC"][0] == 0x0                      # PMC_BOOT_0 identifies the chip
assert MMIO["PTIMER"][0] == 0x009000              # GF100+ PTIMER at 0x9400, NOT 0x020000
assert MMIO["PTOP"][0] == 0x022000                # 0x02240c latch + 0x022700 top table live here
assert MMIO["PEVO"][0] == 0x640000                # display USER/ARMED window (F1, resolved)
assert MMIO["PDISP"][0] == 0x610000               # CPU-driven display on Pascal
assert MMIO["PGRAPH"][1] > MMIO["PPMU"][1]        # ordering sanity
assert MMIO["PGPC"][0] == 0x500000                # GR continues past 0x41FFFF
_spans = sorted(MMIO.values())
assert all(a[1] < b[0] or (a[0] <= b[0] and b[1] <= a[1])  # disjoint or nested
           for a, b in zip(_spans, _spans[1:]))
ok("MMIO BAR0 map: 14 blocks (SDK swref/nouveau): PTIMER@0x9000 not 0x20000, "
   "PTOP@0x022000 new, PCOPY removed, PEVO@0x640000 new; PMC_BOOT_0 at +0x0, "
   "PDISP at +0x610000")

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
            "subch": (word >> 13) & 0x7, "mthd": (word & 0xFFF) << 2}  # clc06f.h:233 ADDRESS 11:0

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

# ------------------------------------------------------------ 5. QMD v02_01
# Every Pascal userspace builds QMD v02_01 (major 2, minor 1): NAK
# picks Qmd2_1 for cls >= PASCAL_COMPUTE_A (mesa nak/qmd.rs:942-943),
# gallium fills NVC0C0_QMDV02_01 in gp100_compute_setup_launch_desc
# (nvc0/nve4_compute.c:637). Positions: clc1c0qmd.h NVC1C0_QMDV02_01_*.
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
mw(qmd, 583, 580, 2)      # QMD_MAJOR_VERSION 2
mw(qmd, 579, 576, 1)      # QMD_VERSION 1
mw(qmd, 378, 378, 1)      # API_VISIBLE_CALL_LIMIT = NO_CHECK (NAK qmd_init!)
mw(qmd, 134, 134, 1)      # SM_GLOBAL_CACHING_ENABLE (NAK, gallium)
mw(qmd, 366, 366, 1)      # RELEASE_MEMBAR_TYPE = FE_SYSMEMBAR (gallium)
mw(qmd, 369, 368, 1)      # CWD_MEMBAR_TYPE = L1_SYSMEMBAR (gallium)
mw(qmd, 287, 256, 0x00A00000)  # PROGRAM_OFFSET (bytes from program region)
mw(qmd, 415, 384, 129)    # CTA_RASTER_WIDTH  (grid X)
mw(qmd, 431, 416, 64)     # CTA_RASTER_HEIGHT (grid Y)
mw(qmd, 463, 448, 1)      # CTA_RASTER_DEPTH  (grid Z) - moved in v02_01
mw(qmd, 607, 592, 8)      # CTA_THREAD_DIMENSION0 (block X)
mw(qmd, 623, 608, 8)      # CTA_THREAD_DIMENSION1
mw(qmd, 639, 624, 1)      # CTA_THREAD_DIMENSION2
mw(qmd, 561, 544, 1024)   # SHARED_MEMORY_SIZE (per block, 0x100 multiple)
mw(qmd, 991, 984, 168)    # REGISTER_COUNT per thread
mw(qmd, 959, 955, 1)      # BARRIER_COUNT
mw(qmd, 951, 928, 32)     # SHADER_LOCAL_MEMORY_LOW_SIZE (per thread)
mw(qmd, 1015, 992, 0x800) # SHADER_LOCAL_MEMORY_CRS_SIZE (gallium 0x800)
mw(qmd, 138, 138, 1)      # SEMAPHORE_RELEASE_ENABLE0 - RELEASE0 is inert without it
mw(qmd, 767, 736, 0xDEAD0000)  # RELEASE0_ADDRESS_LOWER (semaphore)
mw(qmd, 799, 799, 1)      # RELEASE0_STRUCTURE_SIZE = ONE_WORD
mw(qmd, 831, 800, 42)     # RELEASE0_PAYLOAD
mw(qmd, 1055, 1024, 0x12345600)  # CONSTANT_BUFFER_ADDR_LOWER(0)
mw(qmd, 1087, 1075, 256 // 16)   # CONSTANT_BUFFER_SIZE_SHIFTED4(0)
mw(qmd, 640, 640, 1)      # CONSTANT_BUFFER_VALID(0)
assert read_mw(qmd, 583, 580) == 2 and read_mw(qmd, 579, 576) == 1
assert (read_mw(qmd, 415, 384), read_mw(qmd, 431, 416),
        read_mw(qmd, 463, 448)) == (129, 64, 1)
assert read_mw(qmd, 991, 984) == 168 and read_mw(qmd, 959, 955) == 1
# raw words: the goldens pascal/tests/test_qmd.c checks
assert qmd[4]  == (1 << 10) | (1 << 6)                # RELEASE_ENABLE0 | SM_GLOBAL_CACHING
assert qmd[11] == (1 << 26) | (1 << 16) | (1 << 14)   # CALL_LIMIT | CWD_MEMBAR | RELEASE_MEMBAR
assert qmd[14] == 1                                   # CTA_RASTER_DEPTH
assert qmd[18] == (8 << 16) | (2 << 4) | 1            # dim0 | major 2 | version 1
assert qmd[20] == 1                                   # CONSTANT_BUFFER_VALID(0)
assert qmd[24] == 1 << 31                             # RELEASE0_STRUCTURE_SIZE ONE_WORD
assert qmd[29] == (1 << 27) | 32                      # BARRIER_COUNT | SLM low
assert qmd[30] == 168 << 24                           # REGISTER_COUNT
assert qmd[31] == 0x800                               # CRS size, SASS_VERSION 0
assert qmd[33] == 16 << 19                            # CB0 SIZE_SHIFTED4
ok("QMD v02_01 packing: launch + release + cbuf fields round-trip through MW(X:Y)")

print("PASS all %d GP106 constant/format checks" % len(PASS))
