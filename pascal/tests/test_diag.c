// SPDX-License-Identifier: GPL-2.0-only
// pascal/tests/test_diag.c — diag decoder + liveness probe unit tests.
//
// Pure CPU: no hardware. Golden vectors are the EXACT values pascal-egpu
// measured on a real desktop GP106 (research/prior-art/pascal-egpu/
// FINDINGS.md, in this repo as a submodule) plus the NVIDIA-documented PRI
// error codes from third_party/open-gpu-kernel-modules
// src/common/inc/swref/published/maxwell/gm107/dev_pri_ringstation_sys.h.
// Liveness classes run against synthetic buses. Prints one "PASS <name>"
// line per group (simtest style), exits nonzero on any failed assertion.

#include <pascal/diag.h>
#include <pascal/mmio.h>
#include <pascal/pmc.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;

#define REQUIRE(cond)                                                        \
	do {                                                                 \
		if (!(cond)) {                                               \
			printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
			failures++;                                          \
		}                                                            \
	} while (0)

static void
ok(const char *name)
{
	printf("PASS %s\n", name);
}

// ---------------------------------------------------------------
// Golden vectors: decoder rows
// ---------------------------------------------------------------

static void
expect(struct pg106_diag_read r, enum pg106_diag_class cls, uint32_t code,
       const char *src_hint)
{
	REQUIRE(r.cls == cls);
	REQUIRE(r.meaning != NULL);
	REQUIRE(r.source != NULL);
	if (cls == PG106_DIAG_PRI_ERRCODE)
		REQUIRE(r.code == code);
	if (src_hint)
		REQUIRE(strstr(r.source, src_hint) != NULL);
}

static void
test_decoder_golden(void)
{
	struct pg106_diag_read r;

	// Healthy identity: no failure signature.
	r = pg106_diag_decode_readback(PG106_PMC_BOOT_0, 0x136000a1u);
	expect(r, PG106_DIAG_OK, 0, NULL);

	// PMC scratch domain clock-gated — the exact value FINDINGS.md:31
	// records for "PMC scratch writes".
	r = pg106_diag_decode_readback(0x000220u, 0xbad00200u);
	expect(r, PG106_DIAG_PRI_ERRCODE, 0xbad002u, "FINDINGS.md:31");
	REQUIRE(strstr(r.meaning, "clock-gated") != NULL);

	// PBUS_PRI_TIMEOUT_SAVE_0 captured the same 0xbad00200 code
	// (FINDINGS.md:54) — the decoder must not care which address.
	r = pg106_diag_decode_readback(0x00137cu, 0xbad00200u);
	expect(r, PG106_DIAG_PRI_ERRCODE, 0xbad002u, "FINDINGS.md:31");

	// PMU in HS mode, writes gated by security (FINDINGS.md:32).
	r = pg106_diag_decode_readback(0x10a000u, 0xbad0da00u);
	expect(r, PG106_DIAG_PRI_ERRCODE, 0xbad0dau, "FINDINGS.md:32");

	// FBP priv-ring stations not registered; the low byte is an
	// incrementing counter (FINDINGS.md:33) — code is [31:8], any XX.
	r = pg106_diag_decode_readback(0x100c70u, 0xbad0ac00u);
	expect(r, PG106_DIAG_PRI_ERRCODE, 0xbad0acu, "FINDINGS.md:33");
	r = pg106_diag_decode_readback(0x100c70u, 0xbad0ac07u);
	expect(r, PG106_DIAG_PRI_ERRCODE, 0xbad0acu, "FINDINGS.md:33");

	// Falcon priv-reject: the exact value is 0xdead5ec1 (FINDINGS.md:119,
	// PMU IMEM write in HS mode). Note: the task brief's "0xdead5c81?"
	// is a misquote; 0xdead5ec1 is what the submodule records.
	r = pg106_diag_decode_readback(0x10a180u, 0xdead5ec1u);
	expect(r, PG106_DIAG_SEC_REJECT, 0, "FINDINGS.md:32,119");

	// Any 0xdead* prefix classifies as a security reject (generic).
	r = pg106_diag_decode_readback(0x10a184u, 0xdeadbeefu);
	expect(r, PG106_DIAG_SEC_REJECT, 0, NULL);

	// PFB_CFG0 = 0xbadf1100 "FB controller not initialized"
	// (FINDINGS.md:53) — [31:8] = 0xbadf11 = the SDK's FECS_PRI_DECODE
	// (gm107/dev_pri_ringstation_sys.h:32). Address unrecorded in
	// FINDINGS; the decoder is a function of the value.
	r = pg106_diag_decode_readback(0x100200u, 0xbadf1100u);
	expect(r, PG106_DIAG_PRI_ERRCODE, 0xbadf11u,
	       "dev_pri_ringstation_sys.h:32");
	REQUIRE(strstr(r.meaning, "decode") != NULL);

	// MSVLD/NVDEC priv-rejected with 0xbadf1002 (FINDINGS.md:158) =
	// FECS_PRI_TIMEOUT (dev_pri_ringstation_sys.h:31).
	r = pg106_diag_decode_readback(0x084000u, 0xbadf1002u);
	expect(r, PG106_DIAG_PRI_ERRCODE, 0xbadf10u,
	       "dev_pri_ringstation_sys.h:31");

	// NVIDIA-documented codes, one representative per family
	// (gm107/dev_pri_ringstation_sys.h:28-42): 0xBAD00F HOST_FECS_ERR,
	// 0xBAD001 HOST_PRI_TIMEOUT, 0xBAD0B0 HOST_FB_ACK_TIMEOUT,
	// 0xBADF30 FECS_DEAD_RING, 0xBADF50 FECS_PRI_CLIENT_ERR.
	r = pg106_diag_decode_readback(0x000000u, 0xbad00f00u);
	expect(r, PG106_DIAG_PRI_ERRCODE, 0xbad00fu, "ringstation_sys.h:28");
	r = pg106_diag_decode_readback(0x000000u, 0xbad00100u);
	expect(r, PG106_DIAG_PRI_ERRCODE, 0xbad001u, "ringstation_sys.h:29");
	r = pg106_diag_decode_readback(0x000000u, 0xbad0b000u);
	expect(r, PG106_DIAG_PRI_ERRCODE, 0xbad0b0u, "ringstation_sys.h:30");
	r = pg106_diag_decode_readback(0x000000u, 0xbadf3000u);
	expect(r, PG106_DIAG_PRI_ERRCODE, 0xbadf30u, "ringstation_sys.h:40");
	r = pg106_diag_decode_readback(0x000000u, 0xbadf5000u);
	expect(r, PG106_DIAG_PRI_ERRCODE, 0xbadf50u, "ringstation_sys.h:42");

	// SEC2 reads 0xffffffff = engine powered off (FINDINGS.md:33); GPCCS
	// ditto with GPC stations dead (FINDINGS.md:34); the whole GPU reads
	// 0xffffffff after the priv-ring collapse (FINDINGS.md:202).
	r = pg106_diag_decode_readback(0x087000u, 0xffffffffu);
	expect(r, PG106_DIAG_ALL_FF, 0, "FINDINGS.md:33");
	REQUIRE(strstr(r.meaning, "all-ones") != NULL);

	// PMC_BOOT_1 legitimately reads zero on a little-endian GPU
	// (pascal/pmc.h PG106_PMC_BOOT_1_LE, nouveau base.c
	// nvkm_device_endianness): must NOT be flagged as a failure.
	r = pg106_diag_decode_readback(PG106_PMC_BOOT_1, 0x00000000u);
	expect(r, PG106_DIAG_OK, 0, NULL);

	// Any other register reading zero stays a (cautious) generic warning.
	r = pg106_diag_decode_readback(0x100c70u, 0x00000000u);
	expect(r, PG106_DIAG_ALL_ZERO, 0, NULL);
	REQUIRE(strstr(r.meaning, "heuristic") != NULL);

	// A nonzero non-signature value (e.g. a real WPR2/sticky word or a
	// counter) is just OK — no signature, no drama.
	r = pg106_diag_decode_readback(PG106_DIAG_WPR2_ADDR_HI, 0xccccfcfcu);
	expect(r, PG106_DIAG_OK, 0, NULL);

	// A 0xbad* code NVIDIA and pascal-egpu both leave unnamed still
	// classifies as a PRI error with code extracted.
	r = pg106_diag_decode_readback(0x12345678u & 0x001fffffu,
				       0xbad01234u);
	expect(r, PG106_DIAG_PRI_ERRCODE, 0xbad012u, "generic");

	ok("decoder: pascal-egpu golden values, SDK error codes, generic classes");
}

// ---------------------------------------------------------------
// Falcon execution verdict (UCODE_LEVEL latch)
// ---------------------------------------------------------------

static void
test_falcon_verdicts(void)
{
	struct pg106_diag_falcon f;

	// pascal-egpu's documented false positive (FINDINGS.md:36,164-167):
	// CPUCTL 0x52 = STARTCPU stuck + HALTED, SCTL 0x3000 (UCODE_LEVEL=3),
	// TRACEPC stuck at 0x101. Must be a SECURITY FAULT, not a success.
	f = pg106_diag_falcon_exec(0x52u, 0x3000u, 0x101u);
	REQUIRE(f.verdict == PG106_FALCON_SECURITY_FAULT);
	REQUIRE(f.ucode_level3_latched);
	REQUIRE(strstr(f.note, "security exception") != NULL);

	// PMU capture variant: SCTL 0x3002 = HSMODE + UCODE_LEVEL=3
	// (FINDINGS.md:68) — latch detected the same way.
	f = pg106_diag_falcon_exec(0x12u, 0x3002u, 0x0u);
	REQUIRE(f.verdict == PG106_FALCON_SECURITY_FAULT);
	REQUIRE(f.ucode_level3_latched);

	// Clean rest state: CPUCTL 0x10 = HALTED with no STARTCPU
	// (FINDINGS.md:57) — not a fault.
	f = pg106_diag_falcon_exec(0x10u, 0x3000u, 0x0u);
	REQUIRE(f.verdict == PG106_FALCON_HALTED);

	// Started and still running: not halted, not a fault.
	f = pg106_diag_falcon_exec(0x42u, 0x0000u, 0x5u);
	REQUIRE(f.verdict == PG106_FALCON_RUNNING);
	REQUIRE(!f.ucode_level3_latched);

	ok("falcon verdict: HALTED-after-STARTCPU is a security fault, not a run");
}

// ---------------------------------------------------------------
// Synthetic buses for the liveness probe
// ---------------------------------------------------------------

struct bus_entry {
	uint32_t addr;
	uint32_t value;
};

struct fake_bus {
	const struct bus_entry *rom;
	unsigned n;
	uint32_t timer_count; // incremented on every TIME_0 read when ticking
	int ticking;
};

static uint32_t
bus_rd(void *ctx, uint32_t addr)
{
	struct fake_bus *b = ctx;

	if (b->ticking && addr == PG106_DIAG_PTIMER_TIME_0) {
		// PTIMER_TIME_0 counts in 32ns units (SDK kepler/gk104/
		// dev_timer.h:31-35: field [31:5] of 0x9400, TIME_1 = 0x9410).
		return (b->timer_count += 0x20u);
	}
	for (unsigned i = 0; i < b->n; i++) {
		if (b->rom[i].addr == addr)
			return b->rom[i].value;
	}
	// Unlisted addresses read zero (a neutral default, unlike ff).
	return 0x00000000u;
}

// Real-GP106 identity (pascal/tests/test_pmc.c vector; FINDINGS.md:15).
#define GP106_BOOT0 0x136000a1u

static struct fake_bus
bus_new(const struct bus_entry *rom, unsigned n, int ticking)
{
	struct fake_bus b = { rom, n, 0x00000000u, ticking };
	return b;
}

static void
test_liveness_posted(void)
{
	// Expected Helios 300 shape: Optimus system firmware ran devinit at
	// POST, so the 0x02240c bit-1 latch is SET and WPR2 holds the sticky
	// pascal-egpu value (FINDINGS.md:54). Straps value is illustrative.
	static const struct bus_entry rom[] = {
		{ PG106_PMC_BOOT_0, GP106_BOOT0 },
		{ PG106_PMC_BOOT_1, PG106_PMC_BOOT_1_LE },
		{ PG106_DIAG_PBUS_BAR0_WINDOW, 0x00000000u },
		{ PG106_DIAG_DEVINIT_DONE, PG106_DIAG_DEVINIT_DONE_BIT },
		{ PG106_DIAG_WPR2_ADDR_LO, 0x00000000u },
		{ PG106_DIAG_WPR2_ADDR_HI, 0xccccfcfcu },
		{ PG106_DIAG_PSTRAPS, 0x55555555u },
	};
	struct fake_bus b = bus_new(rom, 7, 1);
	struct pg106_diag_liveness L = pg106_diag_probe_liveness(&b, bus_rd);

	REQUIRE(L.state == PG106_DIAG_ALIVE_POSTED);
	REQUIRE(L.boot0_ok);
	REQUIRE(L.timer_ticked);
	REQUIRE(L.devinit_latch_readable);
	REQUIRE(L.devinit_ran);
	REQUIRE(L.wpr2_hi == 0xccccfcfcu);
	REQUIRE(L.note[0] != '\0');
	REQUIRE(strstr(L.note, "skip devinit") != NULL);
	ok("liveness: POSTed bus (latch set, timer ticking) -> ALIVE_POSTED");
}

static void
test_liveness_unposted(void)
{
	// pascal-egpu's eGPU world: cold GPU, Option ROM never ran
	// (FINDINGS.md:87) — identity fine, latch clear.
	static const struct bus_entry rom[] = {
		{ PG106_PMC_BOOT_0, GP106_BOOT0 },
		{ PG106_PMC_BOOT_1, PG106_PMC_BOOT_1_LE },
		{ PG106_DIAG_DEVINIT_DONE, 0x00000000u },
	};
	struct fake_bus b = bus_new(rom, 3, 1);
	struct pg106_diag_liveness L = pg106_diag_probe_liveness(&b, bus_rd);

	REQUIRE(L.state == PG106_DIAG_ALIVE_UNPOSTED);
	REQUIRE(L.boot0_ok);
	REQUIRE(L.timer_ticked);
	REQUIRE(L.devinit_latch_readable);
	REQUIRE(!L.devinit_ran);
	REQUIRE(strstr(L.note, "may run") != NULL);
	ok("liveness: cold bus (latch clear) -> ALIVE_UNPOSTED, devinit allowed");
}

static void
test_liveness_clock_gated(void)
{
	// PRI port answering with 0xbad00200 error codes instead of data —
	// first on BOOT_0, then on the PBUS sanity read (FINDINGS.md:31).
	static const struct bus_entry rom_gated_boot0[] = {
		{ PG106_PMC_BOOT_0, 0xbad00200u },
	};
	struct fake_bus b = bus_new(rom_gated_boot0, 1, 0);
	struct pg106_diag_liveness L = pg106_diag_probe_liveness(&b, bus_rd);
	REQUIRE(L.state == PG106_DIAG_CLOCK_GATED);
	REQUIRE(strstr(L.note, "clock-gated") != NULL);

	static const struct bus_entry rom_gated_pbus[] = {
		{ PG106_PMC_BOOT_0, GP106_BOOT0 },
		{ PG106_PMC_BOOT_1, PG106_PMC_BOOT_1_LE },
		{ PG106_DIAG_PBUS_BAR0_WINDOW, 0xbad00200u },
	};
	b = bus_new(rom_gated_pbus, 3, 1);
	L = pg106_diag_probe_liveness(&b, bus_rd);
	REQUIRE(L.state == PG106_DIAG_CLOCK_GATED);
	REQUIRE(strstr(L.note, "PBUS") != NULL);
	ok("liveness: 0xbad00200 on identity/PBUS -> CLOCK_GATED, no further reads");
}

static void
test_liveness_absent(void)
{
	// Whole-GPU-unresponsive state: every read 0xffffffff (the pascal-egpu
	// lockup end state, FINDINGS.md:202).
	static const struct bus_entry rom[] = {
		{ PG106_PMC_BOOT_0, 0xffffffffu },
		{ PG106_PMC_BOOT_1, 0xffffffffu },
	};
	struct fake_bus b = bus_new(rom, 2, 0);
	struct pg106_diag_liveness L = pg106_diag_probe_liveness(&b, bus_rd);
	REQUIRE(L.state == PG106_DIAG_ABSENT);
	REQUIRE(!L.boot0_ok);

	// Not-our-GPU identity (GM107 boot0 would be 0x117..., use a clearly
	// alien value with no modern bits): ABSENT for this driver.
	static const struct bus_entry rom2[] = {
		{ PG106_PMC_BOOT_0, 0x00000001u },
	};
	b = bus_new(rom2, 1, 0);
	L = pg106_diag_probe_liveness(&b, bus_rd);
	REQUIRE(L.state == PG106_DIAG_ABSENT);
	REQUIRE(strstr(L.note, "GP106") != NULL);
	ok("liveness: all-ones identity and foreign chip -> ABSENT");
}

static void
test_liveness_unverified(void)
{
	// Identity fine but the latch region itself is unreadable: the one
	// state where running devinit would be the documented lockup risk —
	// must refuse (PRIOR-ART-AUDIT.md P0-3).
	static const struct bus_entry rom[] = {
		{ PG106_PMC_BOOT_0, GP106_BOOT0 },
		{ PG106_PMC_BOOT_1, PG106_PMC_BOOT_1_LE },
		{ PG106_DIAG_DEVINIT_DONE, 0xbad00200u },
	};
	struct fake_bus b = bus_new(rom, 3, 1);
	struct pg106_diag_liveness L = pg106_diag_probe_liveness(&b, bus_rd);

	REQUIRE(L.state == PG106_DIAG_ALIVE_UNVERIFIED);
	REQUIRE(L.boot0_ok);
	REQUIRE(!L.devinit_latch_readable);
	REQUIRE(!L.devinit_ran);
	REQUIRE(strstr(L.note, "do NOT run devinit") != NULL);

	// All-ones latch (station dead) must also stay UNVERIFIED, not POSTED.
	static const struct bus_entry rom_ff[] = {
		{ PG106_PMC_BOOT_0, GP106_BOOT0 },
		{ PG106_PMC_BOOT_1, PG106_PMC_BOOT_1_LE },
		{ PG106_DIAG_DEVINIT_DONE, 0xffffffffu },
	};
	b = bus_new(rom_ff, 3, 1);
	L = pg106_diag_probe_liveness(&b, bus_rd);
	REQUIRE(L.state == PG106_DIAG_ALIVE_UNVERIFIED);
	ok("liveness: unreadable latch -> ALIVE_UNVERIFIED, devinit refused");
}

static void
test_liveness_frozen_timer(void)
{
	// A GPU whose PTIMER does not advance (clock tree not yet alive) is
	// still classifiable by the latch — the timer is corroboration only.
	static const struct bus_entry rom[] = {
		{ PG106_PMC_BOOT_0, GP106_BOOT0 },
		{ PG106_PMC_BOOT_1, PG106_PMC_BOOT_1_LE },
		{ PG106_DIAG_DEVINIT_DONE, PG106_DIAG_DEVINIT_DONE_BIT },
		{ PG106_DIAG_PTIMER_TIME_0, 0x00000000u }, // constant
		{ PG106_DIAG_PTIMER_TIME_1, 0x00000000u },
	};
	struct fake_bus b = bus_new(rom, 5, 0);
	struct pg106_diag_liveness L = pg106_diag_probe_liveness(&b, bus_rd);

	REQUIRE(L.state == PG106_DIAG_ALIVE_POSTED);
	REQUIRE(!L.timer_ticked);
	ok("liveness: frozen PTIMER recorded but never gates the decision");
}

static void
test_determinism(void)
{
	// Same bus state -> identical decode and identical probe result,
	// byte for byte (note buffer included). Run the decoder twice and
	// the probe over two identically-initialized buses.
	struct pg106_diag_read a = pg106_diag_decode_readback(0x100c70u,
							      0xbad0ac03u);
	for (int i = 0; i < 100; i++) {
		struct pg106_diag_read r =
			pg106_diag_decode_readback(0x100c70u, 0xbad0ac03u);
		REQUIRE(r.cls == a.cls && r.code == a.code);
		REQUIRE(strcmp(r.meaning, a.meaning) == 0);
		REQUIRE(strcmp(r.source, a.source) == 0);
	}

	static const struct bus_entry rom[] = {
		{ PG106_PMC_BOOT_0, GP106_BOOT0 },
		{ PG106_PMC_BOOT_1, PG106_PMC_BOOT_1_LE },
		{ PG106_DIAG_DEVINIT_DONE, PG106_DIAG_DEVINIT_DONE_BIT },
		{ PG106_DIAG_WPR2_ADDR_HI, 0xccccfcfcu },
	};
	struct fake_bus b1 = bus_new(rom, 4, 1);
	struct fake_bus b2 = bus_new(rom, 4, 1);
	struct pg106_diag_liveness L1 = pg106_diag_probe_liveness(&b1, bus_rd);
	struct pg106_diag_liveness L2 = pg106_diag_probe_liveness(&b2, bus_rd);

	REQUIRE(L1.state == L2.state);
	REQUIRE(memcmp(&L1, &L2, sizeof(L1)) == 0);
	REQUIRE(pg106_diag_state_name(L1.state) != NULL);
	ok("determinism: 100 decode repeats and twin probes byte-identical");
}

static void
test_format_lines(void)
{
	char buf[256];
	struct pg106_diag_read r = pg106_diag_decode_readback(0x000220u,
							      0xbad00200u);
	int n = pg106_diag_format(buf, sizeof(buf), 0x000220u, 0xbad00200u, &r);

	REQUIRE(n > 0 && (unsigned)n < sizeof(buf));
	REQUIRE(strstr(buf, "0x000220") != NULL);
	REQUIRE(strstr(buf, "PMC") != NULL); // mmio.h block context
	REQUIRE(strstr(buf, "0xbad00200") != NULL);
	REQUIRE(strstr(buf, "FINDINGS.md:31") != NULL);

	// Truncation must stay NUL-terminated and not overflow.
	char tiny[8];
	(void)pg106_diag_format(tiny, sizeof(tiny), 0x000220u, 0xbad00200u, &r);
	REQUIRE(tiny[sizeof(tiny) - 1] == '\0');
	ok("format: human-readable line with block name, value and citation");
}

int
main(void)
{
	test_decoder_golden();
	test_falcon_verdicts();
	test_liveness_posted();
	test_liveness_unposted();
	test_liveness_clock_gated();
	test_liveness_absent();
	test_liveness_unverified();
	test_liveness_frozen_timer();
	test_determinism();
	test_format_lines();

	if (failures) {
		printf("%d check(s) FAILED\n", failures);
		return EXIT_FAILURE;
	}
	printf("all checks passed\n");
	return EXIT_SUCCESS;
}
