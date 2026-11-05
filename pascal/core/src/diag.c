// SPDX-License-Identifier: GPL-2.0-only
// pascal/core/src/diag.c — first-contact failure decoder + liveness probe.
//
// Sources (read-only references; see POSTED-DETECTION.md for the full list):
//   - research/prior-art/pascal-egpu/FINDINGS.md — the "What Doesn't Work"
//     symptom→return-value→cause table (lines 29-44) and the diagnostic
//     register captures (lines 45-74), measured on a desktop GP106 eGPU.
//     The submodule lives at research/prior-art/pascal-egpu in this repo.
//   - third_party/open-gpu-kernel-modules
//     src/common/inc/swref/published/maxwell/gm107/dev_pri_ringstation_sys.h
//     lines 28-42 — the NVIDIA-documented PRI error codes; a failed PRI read
//     returns (NV_PPRIV_SYS_PRI_ERROR_CODE << 8), code in bits [31:8].
//   - nouveau (linux master) nvkm/subdev/devinit/gf100.c gf100_devinit_preinit
//     — the 0x02240c bit-1 "devinit already ran" readback; gm200.c wires the
//     same function to every GP10x chipset.
//
// Everything in the failure table is a PRIOR-ART-DERIVED HEURISTIC: a
// generalization from ONE desktop GP106 in ONE (eGPU) configuration. It is
// not nouveau behavior and not guaranteed on GP106M — the meaning strings
// below keep pascal-egpu's own wording for exactly that reason.

#include <pascal/diag.h>
#include <pascal/mmio.h>
#include <pascal/pmc.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

// ------------------------------------------------------------------
// PRI error-code table: bits [31:8] of a failed readback.
// ------------------------------------------------------------------

// NVIDIA-documented codes. Exact values and names from
// open-gpu-kernel-modules maxwell/gm107/dev_pri_ringstation_sys.h:28-42
// (the Maxwell/GM107 sys priv-ring station header; GM200+ shares the
// encoding — no Pascal-specific ringstation header exists in the SDK,
// searched src/common/inc/swref/published/pascal/).
static const char *
sdk_pri_code(uint32_t code, const char **source)
{
	switch (code) {
	case 0xbad001u:
		*source = "open-gpu-kernel-modules gm107/dev_pri_ringstation_sys.h:29";
		return "host PRI timeout (access got no ack from the ring)";
	case 0xbad00fu:
		*source = "open-gpu-kernel-modules gm107/dev_pri_ringstation_sys.h:28";
		return "host FECS error (FECS raised a privilege error)";
	case 0xbad0b0u:
		*source = "open-gpu-kernel-modules gm107/dev_pri_ringstation_sys.h:30";
		return "host FB ack timeout (framebuffer never acknowledged)";
	case 0xbadf10u:
		*source = "open-gpu-kernel-modules gm107/dev_pri_ringstation_sys.h:31";
		return "FECS PRI timeout (FECS-side access timed out)";
	case 0xbadf11u:
		*source = "open-gpu-kernel-modules gm107/dev_pri_ringstation_sys.h:32";
		return "FECS PRI decode (FECS could not decode the access)";
	case 0xbadf12u:
		*source = "open-gpu-kernel-modules gm107/dev_pri_ringstation_sys.h:33";
		return "FECS PRI reset (access rejected during reset)";
	case 0xbadf13u:
		*source = "open-gpu-kernel-modules gm107/dev_pri_ringstation_sys.h:34";
		return "FECS PRI floorsweep (access targeted floorswept hardware)";
	case 0xbadf14u:
		*source = "open-gpu-kernel-modules gm107/dev_pri_ringstation_sys.h:35";
		return "FECS PRI stuck ack";
	case 0xbadf15u:
		*source = "open-gpu-kernel-modules gm107/dev_pri_ringstation_sys.h:36";
		return "FECS PRI unexpected/missing expected ack";
	case 0xbadf16u:
		*source = "open-gpu-kernel-modules gm107/dev_pri_ringstation_sys.h:37";
		return "FECS PRI fence error";
	case 0xbadf17u:
		*source = "open-gpu-kernel-modules gm107/dev_pri_ringstation_sys.h:38";
		return "FECS PRI subid error";
	case 0xbadf20u:
		*source = "open-gpu-kernel-modules gm107/dev_pri_ringstation_sys.h:39";
		return "FECS PRI orphan";
	case 0xbadf30u:
		*source = "open-gpu-kernel-modules gm107/dev_pri_ringstation_sys.h:40";
		return "FECS dead ring";
	case 0xbadf40u:
		*source = "open-gpu-kernel-modules gm107/dev_pri_ringstation_sys.h:41";
		return "FECS trap";
	case 0xbadf50u:
		*source = "open-gpu-kernel-modules gm107/dev_pri_ringstation_sys.h:42";
		return "FECS PRI client error";
	default:
		return 0;
	}
}

// Codes pascal-egpu observed on real GP106 that the SDK table does not name.
// File+line citations are into research/prior-art/pascal-egpu/FINDINGS.md in
// this repo's submodule.
static const char *
prior_art_pri_code(uint32_t code, const char **source)
{
	switch (code) {
	case 0xbad002u:
		*source = "pascal-egpu FINDINGS.md:31";
		return "PMC scratch domain clock-gated (FINDINGS.md:52,54: the "
		       "same code appears on PMC scratch and PBUS_PRI_TIMEOUT_SAVE_0)";
	case 0xbad0dau:
		*source = "pascal-egpu FINDINGS.md:32";
		return "PMU falcon in HS mode, access gated by security";
	case 0xbad0acu:
		*source = "pascal-egpu FINDINGS.md:33";
		return "FBP priv-ring stations not registered (low 8 bits are an "
		       "incrementing per-access counter)";
	default:
		return 0;
	}
}

// ------------------------------------------------------------------
// Decoder
// ------------------------------------------------------------------

struct pg106_diag_read
pg106_diag_decode_readback(uint32_t addr, uint32_t value)
{
	struct pg106_diag_read r = { .code = 0, .meaning = 0, .source = 0 };

	// Security rejection first: unambiguous 0xdead prefix (top 16 bits).
	// The exact value pascal-egpu captured is 0xdead5ec1 on PMU falcon
	// IMEM writes in HS mode (FINDINGS.md:119; same value in the
	// FINDINGS.md:32 row). (The task brief's "0xdead5c81?" is a misquote;
	// the submodule records 0xdead5ec1.)
	if ((value & 0xffff0000u) == 0xdead0000u) {
		r.cls = PG106_DIAG_SEC_REJECT;
		r.meaning = "falcon security rejection: priv-rejected access, "
			    "expected while UCODE_LEVEL=3 latch holds";
		r.source = "pascal-egpu FINDINGS.md:32,119";
		return r;
	}

	// PRI error codes: 0xbad prefix (top 12 bits), code in [31:8] per the
	// ringstation header field NV_PPRIV_SYS_PRI_ERROR_CODE (bits 31:8).
	if ((value & 0xfff00000u) == 0xbad00000u) {
		const char *src = 0;
		uint32_t code = value >> 8;
		const char *m = sdk_pri_code(code, &src);
		if (!m)
			m = prior_art_pri_code(code, &src);
		r.cls = PG106_DIAG_PRI_ERRCODE;
		r.code = code;
		if (m) {
			r.meaning = m;
			r.source = src;
		} else {
			// A 0xbad* code neither NVIDIA nor pascal-egpu names:
			// still a hard PRI error, still decode-and-log.
			r.meaning = "unregistered PRI error code (not in the "
				    "SDK ringstation table nor pascal-egpu's captures)";
			r.source = "generic";
		}
		return r;
	}

	// All-ones: SEC2/GPCCS reads returning 0xffffffff = engine powered
	// off (FINDINGS.md:33,34); dead priv-ring stations read 0xffffffff
	// (FINDINGS.md:72-73); a fully collapsed priv ring leaves the whole
	// GPU returning 0xffffffff (FINDINGS.md:122,202).
	if (value == 0xffffffffu) {
		r.cls = PG106_DIAG_ALL_FF;
		r.meaning = "reads all-ones: device absent, engine powered off, "
			    "or dead/collapsed priv-ring station";
		r.source = "pascal-egpu FINDINGS.md:33,72-73,122,202";
		return r;
	}

	// All-zero, generic heuristic — NOT a pascal-egpu signature. Many
	// registers legitimately read 0, so only the wording is cautious;
	// the one special case we decode as OK is PMC_BOOT_1, whose
	// little-endian value IS zero (pascal/pmc.h PG106_PMC_BOOT_1_LE,
	// nouveau base.c nvkm_device_endianness).
	if (value == 0x00000000u && addr != PG106_PMC_BOOT_1) {
		r.cls = PG106_DIAG_ALL_ZERO;
		r.meaning = "generic heuristic: reads zero — block unclocked or "
			    "address not decoded; also the legitimate value of "
			    "some registers, check the expected reset value";
		r.source = "generic heuristic (not a pascal-egpu signature)";
		return r;
	}

	r.cls = PG106_DIAG_OK;
	r.meaning = "no failure signature in the readback value";
	r.source = "generic";
	return r;
}

// Bounded append helpers — the portable core carries no <stdio.h> (kext
// symbol audit, pascal/kext/scripts/symbol-audit.sh). These cover the only
// specifiers this module formats: fixed-width lowercase hex and strings.
static size_t
put_hex(char *dst, size_t cap, size_t o, uint32_t v, int width)
{
	char tmp[8];
	int n = 0;

	do {
		tmp[n++] = "0123456789abcdef"[v & 0xf];
		v >>= 4;
	} while (v != 0);
	while (width > n && n < (int)sizeof(tmp))
		tmp[n++] = '0';
	while (n > 0 && o + 1 < cap)
		dst[o++] = tmp[--n];
	return o;
}

static size_t
put_str(char *dst, size_t cap, size_t o, const char *s)
{
	while (*s != '\0' && o + 1 < cap)
		dst[o++] = *s++;
	return o;
}

int
pg106_diag_format(char *buf, uint32_t cap, uint32_t addr, uint32_t value,
		  const struct pg106_diag_read *r)
{
	const char *block = pg106_mmio_block_name(addr);
	if (!block)
		block = "(unmapped)";
	uint32_t code = (r->cls == PG106_DIAG_PRI_ERRCODE) ? r->code : 0;
	size_t o = 0;

	o = put_str(buf, cap, o, "0x");
	o = put_hex(buf, cap, o, addr, 6);
	o = put_str(buf, cap, o, " (");
	o = put_str(buf, cap, o, block);
	o = put_str(buf, cap, o, ") = 0x");
	o = put_hex(buf, cap, o, value, 8);
	if (r->cls == PG106_DIAG_PRI_ERRCODE) {
		o = put_str(buf, cap, o, " — PRI error 0x");
		o = put_hex(buf, cap, o, code, 6);
		o = put_str(buf, cap, o, ": ");
	} else {
		o = put_str(buf, cap, o, " — ");
	}
	o = put_str(buf, cap, o, r->meaning);
	o = put_str(buf, cap, o, " [");
	o = put_str(buf, cap, o, r->source);
	o = put_str(buf, cap, o, "]");
	buf[(o < cap) ? o : (cap ? cap - 1 : 0)] = '\0';
	return (int)o;
}

// ------------------------------------------------------------------
// Falcon execution verdict (UCODE_LEVEL latch warning)
// ------------------------------------------------------------------

struct pg106_diag_falcon
pg106_diag_falcon_exec(uint32_t ctlctl, uint32_t sctl, uint32_t tracepc)
{
	struct pg106_diag_falcon f = { .verdict = PG106_FALCON_HALTED,
				       .ucode_level3_latched = false,
				       .note = 0 };

	f.ucode_level3_latched = PG106_FALCON_SCTL_UCODE_LEVEL(sctl) == 3;

	if (!(ctlctl & PG106_FALCON_CPUCTL_STARTCPU)) {
		// HALTED with no STARTCPU pending is the documented clean
		// rest state (FECS CPUCTL 0x10, FINDINGS.md:57).
		f.note = "no STARTCPU pending — idle/halted cleanly";
		return f;
	}

	if (!(ctlctl & PG106_FALCON_CPUCTL_HALTED)) {
		f.verdict = PG106_FALCON_RUNNING;
		f.note = "started and not halted";
		return f;
	}

	// STARTCPU stuck set AND HALTED: this is the documented false
	// positive. pascal-egpu: "The HALTED state is a security exception,
	// not a real halt. The Falcons silently fault on first instruction
	// fetch when SCTL has UCODE_LEVEL=3" (FINDINGS.md:167; TRACEPC stuck
	// at 0x101 and dead MAILBOX writes, FINDINGS.md:164-166). The latch
	// cannot be cleared from the host (FINDINGS.md:169-180), so the only
	// path past it is the signed ACR chain.
	f.verdict = PG106_FALCON_SECURITY_FAULT;
	f.note = "STARTCPU stuck + HALTED: security exception, not a run; "
		 "unsigned execution is impossible while UCODE_LEVEL=3 "
		 "(SCTL writes silently dropped) — use the signed ACR path";
	(void)tracepc; // TRACEPC (e.g. stuck 0x101) is logged by the caller
	return f;
}

// ------------------------------------------------------------------
// Liveness probe
// ------------------------------------------------------------------

// PTIMER tick check, shaped like nouveau's stable-read (nvkm/subdev/timer/
// nv04.c nv04_timer_read samples TIME_1/TIME_0 with a stable-hi retry).
// Two independent samples must differ for the timer to be declared ticking.
static bool
timer_ticks(void *ctx, pg106_diag_rd32_fn rd32)
{
	uint32_t hi1 = rd32(ctx, PG106_DIAG_PTIMER_TIME_1);
	uint32_t lo1 = rd32(ctx, PG106_DIAG_PTIMER_TIME_0);
	uint32_t hi2 = rd32(ctx, PG106_DIAG_PTIMER_TIME_1);
	uint32_t lo2 = rd32(ctx, PG106_DIAG_PTIMER_TIME_0);

	// All-ones timer = the PTIMER block is not answering at all.
	if (hi1 == 0xffffffffu && hi2 == 0xffffffffu)
		return false;
	return hi1 != hi2 || lo1 != lo2;
}

// fmt carries at most two "%0Nx" specifiers, filled from a then b; any
// other text (including other specifiers) is copied verbatim so tests catch
// misuse. No <stdio.h> in the portable core (kext symbol audit).
static void
set_note(struct pg106_diag_liveness *L, const char *fmt, uint32_t a, uint32_t b)
{
	size_t o = 0;
	uint32_t vals[2] = { a, b };
	int vi = 0;
	const char *p = fmt;

	while (*p != '\0' && o + 1 < sizeof(L->note)) {
		if (p[0] == '%' && (p[1] == '0' || p[1] == 'x')) {
			int width = 8;
			const char *q = p + 1;
			if (*q == '0') {
				width = (q[1] - '0') * 10 + (q[2] - '0');
				q += 3;	// skip "0NNx"
			} else {
				q += 1;	// bare "%x"
			}
			if (*q == 'x' && vi < 2) {
				o = put_hex(L->note, sizeof(L->note), o,
					    vals[vi++], width);
				p = q + 1;
				continue;
			}
		}
		L->note[o++] = *p++;
	}
	L->note[(o < sizeof(L->note)) ? o : sizeof(L->note) - 1] = '\0';
}

struct pg106_diag_liveness
pg106_diag_probe_liveness(void *ctx, pg106_diag_rd32_fn rd32)
{
	struct pg106_diag_liveness L;
	struct pg106_pmc_probe id;
	struct pg106_diag_read d;

	// Everything zeroed, then step 1: PMC identity.
	L.state = PG106_DIAG_ABSENT;
	L.boot0 = rd32(ctx, PG106_PMC_BOOT_0);
	L.boot1 = rd32(ctx, PG106_PMC_BOOT_1);
	L.boot0_ok = false;
	L.timer_ticked = false;
	L.bus_window = 0;
	L.devinit_done = 0;
	L.devinit_latch_readable = false;
	L.devinit_ran = false;
	L.wpr2_lo = 0;
	L.wpr2_hi = 0;
	L.pstraps = 0;
	set_note(&L, "probe not started", 0, 0);

	// Branch 1 — identity dead or rejected: any bus where BOOT_0 reads
	// all-ones is the whole-GPU-unresponsive state pascal-egpu ended in
	// (FINDINGS.md:202: "PMC_BOOT_0 returns 0xffffffff"); a 0xdead* on
	// BOOT_0 means even the host port is security-gated. No further
	// reads can help; classify ABSENT and stop before touching anything
	// else. Rationale: pascal-egpu could not recover this state in-band,
	// only by power-cycle (FINDINGS.md:200-210).
	d = pg106_diag_decode_readback(PG106_PMC_BOOT_0, L.boot0);
	if (d.cls == PG106_DIAG_ALL_FF || d.cls == PG106_DIAG_SEC_REJECT) {
		L.state = PG106_DIAG_ABSENT;
		set_note(&L, "BOOT_0 dead (0x%08x): device absent or priv ring "
			     "collapsed; no in-band recovery", L.boot0, 0);
		return L;
	}

	// Branch 2 — the PRI port answers with error codes instead of data:
	// the host-side ring is alive but the addressed domain is gated
	// (0xbad00200 = PMC scratch domain clock-gated, FINDINGS.md:31).
	// Classify CLOCK_GATED; do not attempt engine reads.
	if (d.cls == PG106_DIAG_PRI_ERRCODE) {
		L.state = PG106_DIAG_CLOCK_GATED;
		set_note(&L, "BOOT_0 read returned PRI error 0x%06x; PRI port "
			     "up but domain clock-gated", d.code, 0);
		return L;
	}

	// Branch 3 — identity must decode as GP106 (pascal/pmc.h probe,
	// nouveau base.c nvkm_device_ctor decode). Anything else is not our
	// GPU or the BAR is aliased: ABSENT for this driver.
	id = pg106_pmc_probe(L.boot0);
	if (d.cls == PG106_DIAG_ALL_ZERO || id.status != PG106_PMC_OK ||
	    !id.is_gp106) {
		L.state = PG106_DIAG_ABSENT;
		set_note(&L, "BOOT_0 0x%08x does not decode as GP106", L.boot0, 0);
		return L;
	}
	L.boot0_ok = true;

	// Branch 4 — bus-domain sanity: PBUS_BAR0_WINDOW (dev_bus.h:43) is a
	// harmless read-only readback on any healthy PBUS. If it error-codes,
	// the PBUS domain itself is gated: CLOCK_GATED.
	L.bus_window = rd32(ctx, PG106_DIAG_PBUS_BAR0_WINDOW);
	d = pg106_diag_decode_readback(PG106_DIAG_PBUS_BAR0_WINDOW, L.bus_window);
	if (d.cls == PG106_DIAG_PRI_ERRCODE) {
		L.state = PG106_DIAG_CLOCK_GATED;
		set_note(&L, "PBUS window read returned PRI error 0x%06x: "
			     "PBUS domain gated", d.code, 0);
		return L;
	}

	// Branch 5 — timer: a ticking PTIMER distinguishes a powered,
	// clocked GPU from a merely enumerable one. Recorded, never gated
	// on (a correct devinit decision below must not depend on it).
	L.timer_ticked = timer_ticks(ctx, rd32);

	// Branch 6 — the POSTED decision. nouveau's evidence bit: 0x02240c
	// bit 1, "set by devinit ... flips back to 0 on suspend" (gf100.c
	// gf100_devinit_preinit, used by every GP10x chipset via gm200_devinit
	// .preinit). Bit set => firmware already ran devinit => our devinit
	// interpreter must be skipped. Bit clear => cold GPU, devinit allowed.
	L.devinit_done = rd32(ctx, PG106_DIAG_DEVINIT_DONE);
	d = pg106_diag_decode_readback(PG106_DIAG_DEVINIT_DONE, L.devinit_done);
	if (d.cls == PG106_DIAG_PRI_ERRCODE || d.cls == PG106_DIAG_ALL_FF ||
	    d.cls == PG106_DIAG_SEC_REJECT) {
		// The latch region itself is unreadable: identity and timer
		// look fine, but we cannot know whether devinit ran. This is
		// exactly the case where blindly running devinit is the
		// documented lockup risk (PRIOR-ART-AUDIT.md P0-3): refuse.
		L.state = PG106_DIAG_ALIVE_UNVERIFIED;
		L.devinit_latch_readable = false;
		set_note(&L, "devinit latch unreadable (0x%08x): do NOT run "
			     "devinit; collect logs", L.devinit_done, 0);
	} else {
		L.devinit_latch_readable = true;
		L.devinit_ran = (L.devinit_done & PG106_DIAG_DEVINIT_DONE_BIT) != 0;
		L.state = L.devinit_ran ? PG106_DIAG_ALIVE_POSTED
					: PG106_DIAG_ALIVE_UNPOSTED;
		if (L.devinit_ran)
			set_note(&L, "devinit latch bit set (0x%08x): firmware "
				     "already ran devinit at POST; skip devinit",
				 L.devinit_done, 0);
		else
			set_note(&L, "devinit latch clear (0x%08x): cold GPU; "
				     "devinit may run", L.devinit_done, 0);
	}

	// Branch 7 — corroboration record, never gating: WPR2 sticky value
	// (FINDINGS.md:54: 0xccccfcfc, does not clear on reset) and board
	// straps (PLAN.md:149). Logged so a hardware session can diff them.
	L.wpr2_lo = rd32(ctx, PG106_DIAG_WPR2_ADDR_LO);
	L.wpr2_hi = rd32(ctx, PG106_DIAG_WPR2_ADDR_HI);
	L.pstraps = rd32(ctx, PG106_DIAG_PSTRAPS);
	return L;
}

const char *
pg106_diag_state_name(enum pg106_diag_state state)
{
	switch (state) {
	case PG106_DIAG_ALIVE_POSTED:
		return "ALIVE_POSTED";
	case PG106_DIAG_ALIVE_UNPOSTED:
		return "ALIVE_UNPOSTED";
	case PG106_DIAG_ALIVE_UNVERIFIED:
		return "ALIVE_UNVERIFIED";
	case PG106_DIAG_CLOCK_GATED:
		return "CLOCK_GATED";
	case PG106_DIAG_ABSENT:
		return "ABSENT";
	}
	return "UNKNOWN";
}
