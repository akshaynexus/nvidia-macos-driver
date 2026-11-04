// SPDX-License-Identifier: GPL-2.0-only
// pascal/core/src/top.c — GP106 nvkm_top table reader.
//
// Port of nouveau nvkm/subdev/top/ (linux v5.15, fetched 2026-11-05):
//   gk104_top_oneinit  (nvkm/subdev/top/gk104.c:26-101)  -> pg106_top_scan
//   nvkm_top_addr      (nvkm/subdev/top/base.c:44-53)    -> pg106_top_find
//   nvkm_top_reset     (nvkm/subdev/top/base.c:55-67)    ->
//                        pg106_top_reset_mask
// Field defaults follow nvkm_top_device_new (base.c:26-42). Layout constants
// and their per-line citations live in pascal/top.h; nothing here invents a
// magic value.
//
// Deviations from upstream (also documented in top.h at pg106_top_scan):
// the bus read goes through the caller's callback (errors propagate instead
// of crashing), and the append is bounds-checked even though the static
// entry bound equals the upstream loop bound.

#include <pascal/top.h>

#include <errno.h>
#include <string.h>

int
pg106_top_scan(pg106_top_rd32_fn rd32, void *userdata,
	       struct pg106_top_table *out)
{
	// The pending entry under construction — upstream keeps a freshly
	// nvkm_top_device_new()-ed `info` (gk104.c:31,36-41) with the same
	// defaults (base.c:29-38).
	struct pg106_top_device pending;
	bool have_pending = false;
	// ENGINE_TYPE seen for the pending entry (gk104.c:32,39 `type = ~0`;
	// a row can only encode values <= PG106_TOP_TYPE_MASK >> 2, so -1 is
	// a safe "not seen" sentinel) and the DATA-row instance
	// (gk104.c:40 `inst = 0`).
	int type = -1;
	int inst = 0;
	uint32_t i;

	if (!rd32 || !out)
		return -EINVAL;

	memset(out, 0, sizeof(*out));

	for (i = 0; i < PG106_TOP_MAX_ROWS; i++) {
		uint32_t data;

		{
			int ret = rd32(userdata,
				       PG106_TOP_INDEX + i * PG106_TOP_ROW_STRIDE,
				       &data);

			if (ret)
				return ret; // bus fault: propagate verbatim
		}
		out->nrows++;

		if (!have_pending) {
			// gk104.c:36-41 (+ base.c:29-38 defaults).
			have_pending = true;
			pending = (struct pg106_top_device){
				.type = PG106_TOP_TYPE_UNKNOWN,
				.inst = -1,
				.addr = 0,
				.fault = -1,
				.engine = -1,
				.runlist = -1,
				.intr = -1,
				.reset = -1,
				.last_row = -1,
			};
			type = -1;
			inst = 0;
		}

		// Row decode (gk104.c:45-67). NOT_VALID keeps the pending
		// entry open and falls through to the chain check below,
		// exactly upstream's `continue` at gk104.c:47 — except we
		// must still honor bit 31 semantics, which is why the skip
		// is modeled with a goto-free restructure: upstream's
		// `continue` skips BOTH the translate step and the chain
		// check... but a NOT_VALID row finalizes nothing either way
		// (info stays pending, no finalize, chain bit of an invalid
		// row is unreachable by construction upstream because
		// `continue` runs first). We mirror the observable behavior:
		// invalid row -> pending untouched, never finalized.
		switch (data & 0x3u) {
		case PG106_TOP_ROW_NOT_VALID:
			continue; // gk104.c:46-47
		case PG106_TOP_ROW_DATA: // gk104.c:48-53
			inst = (int)((data & PG106_TOP_DATA_INST) >> 26);
			pending.addr = data & PG106_TOP_DATA_ADDR;
			if (data & PG106_TOP_DATA_FAULT_PRESENT)
				pending.fault =
					(int)((data & PG106_TOP_DATA_FAULT) >>
					      3);
			break;
		case PG106_TOP_ROW_ENUM: // gk104.c:54-63
			if (data & PG106_TOP_ENUM_ENGINE_PRESENT)
				pending.engine = (int)((data &
							PG106_TOP_ENUM_ENGINE) >>
						       26);
			if (data & PG106_TOP_ENUM_RUNLIST_PRESENT)
				pending.runlist =
					(int)((data & PG106_TOP_ENUM_RUNLIST) >>
					      21);
			if (data & PG106_TOP_ENUM_INTR_PRESENT)
				pending.intr =
					(int)((data & PG106_TOP_ENUM_INTR) >>
					      15);
			if (data & PG106_TOP_ENUM_RESET_PRESENT)
				pending.reset =
					(int)((data & PG106_TOP_ENUM_RESET) >>
					      9);
			break;
		default: // PG106_TOP_ROW_ENGINE_TYPE, gk104.c:64-66
			type = (int)((data & PG106_TOP_TYPE_MASK) >> 2);
			break;
		}

		// Chain bit set = more rows for this device: keep the entry
		// pending (gk104.c:69-70 `if (data & 0x80000000) continue;`).
		if (data & PG106_TOP_ROW_CHAIN_MORE)
			continue;

		// Chain-last row: translate the ENGINE_TYPE code to
		// (type, inst) per the gk104.c:75-95 map. I_ rows take the
		// instance from the DATA row (gk104.c:73); O_ rows fix it
		// and ignore whatever the DATA row carried (gk104.c:74,
		// whose WARN_ON(inst) we mirror by silently overriding).
		// Unrecognized codes hit the default branch (gk104.c:93-94):
		// the entry keeps type/inst = -1 ("????????", base.c:31-32).
		pending.last_row = (int)i;
		switch (type) {
		case PG106_TOP_TYPE_GR: // O_, gk104.c:76
			pending.type = type;
			pending.inst = 0;
			break;
		case PG106_TOP_TYPE_CE0: // O_, gk104.c:77
			pending.type = type;
			pending.inst = 0;
			break;
		case PG106_TOP_TYPE_CE1: // O_, gk104.c:78
			pending.type = type;
			pending.inst = 0;
			break;
		case PG106_TOP_TYPE_CE2: // O_, gk104.c:79
			pending.type = type;
			pending.inst = 0;
			break;
		case PG106_TOP_TYPE_MSPDEC: // O_, gk104.c:80
			pending.type = type;
			pending.inst = 0;
			break;
		case PG106_TOP_TYPE_MSPPP: // O_, gk104.c:81
			pending.type = type;
			pending.inst = 0;
			break;
		case PG106_TOP_TYPE_MSVLD: // O_, gk104.c:82
			pending.type = type;
			pending.inst = 0;
			break;
		case PG106_TOP_TYPE_MSENC: // O_, gk104.c:83
			pending.type = type;
			pending.inst = 0;
			break;
		case PG106_TOP_TYPE_VIC: // O_, gk104.c:84
			pending.type = type;
			pending.inst = 0;
			break;
		case PG106_TOP_TYPE_SEC2: // O_, gk104.c:85
			pending.type = type;
			pending.inst = 0;
			break;
		case PG106_TOP_TYPE_NVENC_0: // I_, gk104.c:86
			pending.type = type;
			pending.inst = inst;
			break;
		case PG106_TOP_TYPE_NVENC_1: // O_, gk104.c:87
			pending.type = type;
			pending.inst = 1;
			break;
		case PG106_TOP_TYPE_NVDEC: // I_, gk104.c:88
			pending.type = type;
			pending.inst = inst;
			break;
		case PG106_TOP_TYPE_IOCTRL: // I_, gk104.c:89
			pending.type = type;
			pending.inst = inst;
			break;
		case PG106_TOP_TYPE_CE_N: // I_, gk104.c:90
			pending.type = type;
			pending.inst = inst;
			break;
		case PG106_TOP_TYPE_GSP: // O_, gk104.c:91
			pending.type = type;
			pending.inst = 0;
			break;
		case PG106_TOP_TYPE_NVJPG: // O_, gk104.c:92 (v5.15; master's
					   // :92 is I_ — GP106 has no NVJPG)
			pending.type = type;
			pending.inst = 0;
			break;
		default:
			break; // gk104.c:93-94: leave type/inst at -1
		}

		// Finalize: upstream appends to the top->device list and
		// resets `info = NULL` (gk104.c:97+ and the nvkm_debug line).
		// The bound is identical to the loop bound (each entry needs
		// >= 1 row) so this cannot fire; the check is the explicit
		// form of the invariant. -ERANGE (not -ENOSPC: it must exist
		// in the kext errno shadow, pascal/kext/src/klibc/errno.h).
		if (out->ndev == PG106_TOP_MAX_ROWS)
			return -ERANGE;
		out->dev[out->ndev++] = pending;
		have_pending = false;
	}

	// A still-pending entry at row 64 means its device group never saw a
	// chain-last row; upstream leaks the list head in that case — we
	// simply drop it (nothing else to do with a truncated device).
	return 0;
}

const struct pg106_top_device *
pg106_top_find(const struct pg106_top_table *table, int type, int inst)
{
	size_t i;

	// nvkm_top_addr (base.c:44-53): linear scan matching type+inst.
	// Returning NULL instead of a 0-address default lets callers tell
	// "not in the table" from "row says base 0".
	if (!table)
		return NULL;
	for (i = 0; i < table->ndev; i++)
		if (table->dev[i].type == type && table->dev[i].inst == inst)
			return &table->dev[i];
	return NULL;
}

uint32_t
pg106_top_reset_mask(const struct pg106_top_device *dev)
{
	// nvkm_top_reset (base.c:55-67): BIT(info->reset) when present.
	if (!dev || dev->reset < 0 || dev->reset >= 32)
		return 0;
	return 1u << dev->reset;
}
