// SPDX-License-Identifier: GPL-2.0-only
// pascal/core/include/pascal/top.h — GP106 nvkm_top engine-discovery table
// reader (port of nouveau nvkm/subdev/top/, linux v5.15).
//
// Derived from (reference copies, torvalds/linux tag v5.15, fetched
// 2026-11-05; the same paths in master carry a byte-equivalent reader —
// the only textual diff is the NVJPG row, master gk104.c:92 I_ vs v5.15
// gk104.c:92 O_):
//   nvkm/subdev/top/gk104.c   gk104_top_oneinit — the runtime MMIO table
//                             reader: 64 u32 rows at index 0x022700
//                             (gk104.c:35 loop bound, :43 base), row
//                             format :45-67, chain bit :69-70, ENGINE_TYPE
//                             -> engine map :75-95.
//   nvkm/subdev/top/base.c    nvkm_top_device_new field defaults
//                             (base.c:26-42), nvkm_top_addr lookup
//                             (base.c:44-53), nvkm_top_reset
//                             (base.c:55-67).
//   nvkm/falcon/base.c:143-147  the consumer this port exists for:
//                             falcon->addr = nvkm_top_addr(device, type,
//                             inst) at falcon oneinit — the SEC2 falcon
//                             base used by the ACR boot comes from the
//                             table, never from a constant (this is
//                             research/evidence/PORT-AUDIT.md item 1).
//
// Cross-check vs the NVIDIA SDK (third_party/open-gpu-kernel-modules): the
// published NV_PTOP headers (src/common/inc/swref/published/<chip>/dev_top
// .h) exist only for Ampere and newer and describe a different format
// (NV_PTOP_DEVICE_INFO2 at 0x00022800, ga100/dev_top.h:28). There is NO
// GM10x-format NV_PTOP header in the SDK, so every layout constant below
// stands on the nouveau sources cited per line. The SDK does corroborate
// the CHAIN bit semantics: ROW_CHAIN_MORE = 1 / ROW_CHAIN_LAST = 0
// (gb100/dev_top.h:34-36) matches gk104.c:69-70 finalizing an entry on the
// first row with bit 31 CLEAR.
//
// SIMULATION, NOT HARDWARE PROOF: pg106_top_scan() walks a MODELED bus
// through the caller-provided rd32 callback (the unit test feeds a
// hand-built table image). On real hardware the table must be read from
// BAR0 0x022700 before its values are trusted (PORT-AUDIT.md item 1).
//
// C11, libc only, single-threaded, no allocation (the caller owns the
// table storage). Errors are errno-style (0 or negative). All symbols are
// prefixed pg106_top_.

#ifndef PASCAL_TOP_H
#define PASCAL_TOP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Table geometry + raw row values. Every constant cites its upstream line.
// ---------------------------------------------------------------------------

// NV_PTOP table index (first row address in BAR0) — the `0x022700` literal
// of nvkm_rd32(device, 0x022700 + (i * 0x04)), gk104.c:43.
#define PG106_TOP_INDEX 0x022700u
// Row stride — `(i * 0x04)`, gk104.c:43.
#define PG106_TOP_ROW_STRIDE 0x4u
// Iteration bound — `for (i = 0; i < 64; i++)`, gk104.c:35. There is no
// global terminator: the fixed 64-row window IS the termination condition
// (each device's rows end at its own chain-last row, see pg106_top_scan).
#define PG106_TOP_MAX_ROWS 64

// Row-type field, bits [1:0] — switch (data & 0x00000003), gk104.c:45.
#define PG106_TOP_ROW_NOT_VALID 0x0u // gk104.c:46-47 (skip, keeps the
				     // pending entry open)
#define PG106_TOP_ROW_DATA 0x1u      // gk104.c:48-53
#define PG106_TOP_ROW_ENUM 0x2u      // gk104.c:54-63
#define PG106_TOP_ROW_ENGINE_TYPE 0x3u // gk104.c:64-66

// Chain field, bit 31 — `if (data & 0x80000000) continue;` gk104.c:69-70.
// SET = more rows follow for this device (CHAIN_MORE); CLEAR = last row of
// the device, the entry is finalized (CHAIN_LAST; cross-checked against the
// SDK's NV_PTOP_*_ROW_CHAIN_{MORE,LAST}, gb100/dev_top.h:34-36).
#define PG106_TOP_ROW_CHAIN_MORE 0x80000000u

// DATA-row field masks (gk104.c:48-53), pre-shift:
#define PG106_TOP_DATA_INST 0x3c000000u // inst = bits [29:26] (>>26)
#define PG106_TOP_DATA_ADDR 0x00fff000u // addr = bits [23:12] (kept as-is:
					// the PRI base is this field's value)
#define PG106_TOP_DATA_FAULT_PRESENT 0x00000004u // bit 2
#define PG106_TOP_DATA_FAULT 0x000003f8u	 // fault = bits [9:3] (>>3)

// ENUM-row field masks (gk104.c:54-63), pre-shift:
#define PG106_TOP_ENUM_ENGINE_PRESENT 0x00000020u // bit 5
#define PG106_TOP_ENUM_ENGINE 0x3c000000u	  // engine = bits [29:26]
#define PG106_TOP_ENUM_RUNLIST_PRESENT 0x00000010u // bit 4
#define PG106_TOP_ENUM_RUNLIST 0x01e00000u	   // runlist = bits [24:21]
#define PG106_TOP_ENUM_INTR_PRESENT 0x00000008u	   // bit 3
#define PG106_TOP_ENUM_INTR 0x000f8000u		   // intr = bits [19:15]
#define PG106_TOP_ENUM_RESET_PRESENT 0x00000004u   // bit 2
#define PG106_TOP_ENUM_RESET 0x00003e00u	   // reset = bits [13:9]

// ENGINE_TYPE-row field mask — (data & 0x7ffffffc) >> 2, gk104.c:65 (bit 31
// is the chain bit, bits [1:0] the row type, leaving type = bits [30:2]).
#define PG106_TOP_TYPE_MASK 0x7ffffffcu

// ---------------------------------------------------------------------------
// ENGINE_TYPE codes (the switch cases of gk104.c:75-95, which upstream
// translates to NVKM_ENGINE_* identifiers — the raw code is kept here so
// pg106_top_find() needs no separate translation table). GP106 is a full
// GP10x engine set; the codes GP106 devices can carry are GR, CE0-2, MSENC,
// NVENC/NVDEC, SEC2, VIC, IOCTRL, GSP. Named constants exist for the whole
// v5.15 map (NVJPG per the v5.15 O_ row, gk104.c:92).
//
// Note: DISP and FIFO are NOT engine types in this table — upstream models
// them as subdevs and the gk104 map (gk104.c:75-95) has no row for them, so
// no PG106_TOP_TYPE_* constant exists for either.
// ---------------------------------------------------------------------------
enum pg106_top_type {
	PG106_TOP_TYPE_GR = 0x00,      // gk104.c:76 NVKM_ENGINE_GR
	PG106_TOP_TYPE_CE0 = 0x01,     // gk104.c:77 NVKM_ENGINE_CE,0
	PG106_TOP_TYPE_CE1 = 0x02,     // gk104.c:78 NVKM_ENGINE_CE,1
	PG106_TOP_TYPE_CE2 = 0x03,     // gk104.c:79 NVKM_ENGINE_CE,2
	PG106_TOP_TYPE_MSPDEC = 0x08,  // gk104.c:80
	PG106_TOP_TYPE_MSPPP = 0x09,   // gk104.c:81
	PG106_TOP_TYPE_MSVLD = 0x0a,   // gk104.c:82
	PG106_TOP_TYPE_MSENC = 0x0b,   // gk104.c:83
	PG106_TOP_TYPE_VIC = 0x0c,     // gk104.c:84
	PG106_TOP_TYPE_SEC2 = 0x0d,    // gk104.c:85 NVKM_ENGINE_SEC2 — the
				       // ACR-boot falcon (falcon/base.c:143)
	PG106_TOP_TYPE_NVENC_0 = 0x0e, // gk104.c:86 (I_: inst from DATA row)
	PG106_TOP_TYPE_NVENC_1 = 0x0f, // gk104.c:87
	PG106_TOP_TYPE_NVDEC = 0x10,   // gk104.c:88 (I_)
	PG106_TOP_TYPE_IOCTRL = 0x12,  // gk104.c:89 (I_)
	PG106_TOP_TYPE_CE_N = 0x13,    // gk104.c:90 (I_: inst from DATA row)
	PG106_TOP_TYPE_GSP = 0x14,     // gk104.c:91
	PG106_TOP_TYPE_NVJPG = 0x15,   // gk104.c:92
	// An entry whose row group carried no recognized ENGINE_TYPE row:
	// upstream leaves info->type = NVKM_SUBDEV_NR ("????????",
	// gk104.c:93-94 default + base.c:31). PG106_TOP_MAX_TYPE-1 is the
	// largest value a row can encode ((PG106_TOP_TYPE_MASK >> 2) fits an
	// int positive), so -1 cannot collide with a real code.
	PG106_TOP_TYPE_UNKNOWN = -1,
};

// Caller-provided BAR0 read: return 0 and store the value, or a negative
// errno (e.g. -EIO on a bus fault) which pg106_top_scan propagates.
typedef int (*pg106_top_rd32_fn)(void *userdata, uint32_t addr,
				 uint32_t *value);

// One discovered device: the port of struct nvkm_top_device with the field
// defaults of nvkm_top_device_new (base.c:26-42: inst/fault/engine/runlist/
// reset/intr start at -1, addr at 0). `type` carries the RAW ENGINE_TYPE
// code (enum pg106_top_type) instead of upstream's translated NVKM_ENGINE_*
// enum — same information, one less table.
struct pg106_top_device {
	int type;      // enum pg106_top_type; -1 = unrecognized row group
		       // (upstream "????????", gk104.c:93-94 + base.c:31)
	int inst;      // engine instance; DATA-row bits [29:26] (I_ types,
		       // gk104.c:86,88,89,90) or the O_-fixed value (all
		       // other types, gk104.c:76-85,87,91,92); -1 = unknown
	uint32_t addr; // PRI base from the DATA row, bits [23:12] as-is
		       // (gk104.c:50); 0 when no DATA row was seen
	int fault;     // fault id, DATA-row bits [9:3] when bit 2 set
		       // (gk104.c:51-52); -1 = absent
	int engine;    // ENUM engine index, bits [29:26] (gk104.c:55-56);
		       // -1 = absent
	int runlist;   // ENUM runlist index, bits [24:21] (gk104.c:57-58);
		       // -1 = absent
	int intr;      // ENUM intr index, bits [19:15] (gk104.c:59-60);
		       // -1 = absent
	int reset;     // ENUM reset line, bits [13:9] (gk104.c:61-62);
		       // -1 = absent
	int last_row;  // index of the row that closed this entry (chain bit
		       // clear, gk104.c:69-70) — informational, for traces
};

// Scanned table: caller-allocated, filled by pg106_top_scan(). Bounded by
// construction at PG106_TOP_MAX_ROWS devices (each entry consumes at least
// one row), which upstream guarantees implicitly with the same loop bound
// (gk104.c:35) over a linked list (base.c:39); the array makes the bound
// explicit.
struct pg106_top_table {
	struct pg106_top_device dev[PG106_TOP_MAX_ROWS];
	size_t ndev;  // finalized entries (0 .. PG106_TOP_MAX_ROWS)
	size_t nrows; // rows actually read (== PG106_TOP_MAX_ROWS on a full
		      // scan, fewer only when the bus errored out)
};

// Walk the modeled top table: the direct port of gk104_top_oneinit
// (gk104.c:26-101) with two documented deviations:
//   1. rows arrive through `rd32` (modeled bus) instead of nvkm_rd32; a
//      negative return from the callback aborts the scan and is propagated
//      verbatim (upstream would oops on an unreadable device);
//   2. a full table cannot overflow `out` (static bound == loop bound) but
//      the append is still checked and returns -ERANGE rather than trusting
//      the invariant (ERANGE is one of the four errnos the kext shadow
//      defines, pascal/kext/src/klibc/errno.h).
// NOT_VALID rows are skipped WITHOUT closing the pending entry, exactly
// upstream (gk104.c:46-47 `continue`). The pending entry is appended when a
// row with bit 31 CLEAR has been decoded (gk104.c:69-95 + `info = NULL`).
// A trailing pending entry that never closes (device rows still CHAIN_MORE
// at row 63) is dropped, like upstream's un-closed list head.
// Returns 0, -EINVAL on NULL arguments, or the rd32 error.
int pg106_top_scan(pg106_top_rd32_fn rd32, void *userdata,
		   struct pg106_top_table *out);

// Find a device by (type, inst): the port of nvkm_top_addr
// (top/base.c:44-53) returning the whole entry (NULL = not present —
// callers wanting upstream's exact semantics use `found ? found->addr : 0`).
const struct pg106_top_device *
pg106_top_find(const struct pg106_top_table *table, int type, int inst);

// Reset mask for one entry: BIT(reset) like nvkm_top_reset
// (top/base.c:55-67), 0 when the entry carries no reset line.
uint32_t pg106_top_reset_mask(const struct pg106_top_device *dev);

#ifdef __cplusplus
}
#endif

#endif // PASCAL_TOP_H
