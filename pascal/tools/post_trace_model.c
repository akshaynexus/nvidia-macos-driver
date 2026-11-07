// SPDX-License-Identifier: GPL-2.0-only
// pascal/tools/post_trace_model.c — dump the modelled gm200_devinit_post
// register writes for a VBIOS image, one line per step, so they can be
// diffed against a hardware mmiotrace (scripts/compare_post_trace.py).
//
// SIMULATION ONLY: the PMU falcon underneath is a memory model (IMEM/DMEM
// with auto-increment, mailbox, CPUCTL). The DEVINIT application itself is
// not executed; the model stops where nouveau's host-side code stops
// (upload, start, poll for 0x10a040 bit 13), so only host-issued writes
// are comparable.
//
// Build (from pascal/):
//   cc -std=c11 -O1 -Icore/include -o post_trace_model
//      tools/post_trace_model.c core/src/*.c
// Usage:  post_trace_model ROM > model.trace
// Output: "W <addr> <val> <phase>" / "R <addr> <val> <phase>" /
//         "RESET - - <phase>" / "POLL <addr> <polls> <phase>"

#include <pascal/devinit_pmu.h>
#include <pascal/vbios_bit.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MEM_BYTES (1u << 16)

static struct {
	uint8_t imem[MEM_BYTES], dmem[MEM_BYTES];
	uint32_t imem_ctl, imem_ptr, dmem_ctl, dmem_ptr, mailbox;
} F;

static void
put32(uint8_t *m, uint32_t a, uint32_t v)
{
	if (a + 4 > MEM_BYTES)
		return;
	for (int i = 0; i < 4; i++)
		m[a + i] = (uint8_t)(v >> (8 * i));
}

static uint32_t
get32(const uint8_t *m, uint32_t a)
{
	if (a + 4 > MEM_BYTES)
		return 0;
	return (uint32_t)m[a] | (uint32_t)m[a + 1] << 8 |
	       (uint32_t)m[a + 2] << 16 | (uint32_t)m[a + 3] << 24;
}

// Falcon IMEMC/DMEMC: offset in [23:2] (here [23:0] masked to dwords),
// AINCW bit 24, AINCR bit 25 (nvkm/falcon/v1.c load_imem/load_dmem).
static uint32_t
rd32(void *u, uint32_t addr)
{
	(void)u;
	if (addr == PG106_PMU_FALCON_DMEMD0) {
		uint32_t v = get32(F.dmem, F.dmem_ptr);
		if (F.dmem_ctl & 0x02000000)
			F.dmem_ptr += 4;
		return v;
	}
	if (addr == PG106_PMU_FALCON_MAILBOX)
		return F.mailbox;
	return 0;
}

static void
wr32(void *u, uint32_t addr, uint32_t val)
{
	(void)u;
	switch (addr) {
	case PG106_PMU_FALCON_IMEMC0:
		F.imem_ctl = val;
		F.imem_ptr = val & 0x00fffffc;
		break;
	case PG106_PMU_FALCON_IMEMD0:
		put32(F.imem, F.imem_ptr, val);
		if (F.imem_ctl & 0x01000000)
			F.imem_ptr += 4;
		break;
	case PG106_PMU_FALCON_DMEMC0:
		F.dmem_ctl = val;
		F.dmem_ptr = val & 0x00fffffc;
		break;
	case PG106_PMU_FALCON_DMEMD0:
		put32(F.dmem, F.dmem_ptr, val);
		if (F.dmem_ctl & 0x01000000)
			F.dmem_ptr += 4;
		break;
	case PG106_PMU_FALCON_MAILBOX:
		F.mailbox = val;
		break;
	case PG106_PMU_FALCON_CPUCTL:
		// Stand-in for the DEVINIT app reporting completion, so the
		// host-side sequence runs to its end (gm200.c:167-170).
		if (val == 2)
			F.mailbox |= PG106_PMU_DEVINIT_DONE;
		break;
	default:
		break;
	}
}

static uint8_t rom[1u << 20];
static struct pg106_pmu_step trace[1u << 18];

int
main(int argc, char **argv)
{
	if (argc != 2) {
		fprintf(stderr, "usage: %s VBIOS.rom\n", argv[0]);
		return 2;
	}
	FILE *f = fopen(argv[1], "rb");
	if (!f) {
		perror(argv[1]);
		return 1;
	}
	size_t n = fread(rom, 1, sizeof(rom), f);
	fclose(f);

	struct pg106_vbios_bit vb;
	if (pg106_vbios_bit_parse(rom, (uint32_t)n, &vb) != 0) {
		fprintf(stderr, "not a parseable VBIOS\n");
		return 1;
	}
	const struct pg106_pmu_bus bus = { rd32, wr32, NULL, NULL, NULL };
	struct pg106_post_result res;
	int rc = pg106_devinit_gm200_post(&vb, true, true, &bus, trace,
					  sizeof(trace) / sizeof(trace[0]),
					  &res);
	if (rc != 0) {
		fprintf(stderr, "gm200_devinit_post model failed: %d %s\n", rc,
			res.error ? res.error : "");
		return 1;
	}
	if (res.trace_truncated)
		fprintf(stderr, "warning: trace truncated\n");
	for (size_t i = 0; i < res.steps; i++) {
		const struct pg106_pmu_step *s = &trace[i];
		const char *ph = pg106_post_phase_name(s->phase);
		switch (s->kind) {
		case PG106_PMU_STEP_WR:
			printf("W 0x%06x 0x%08x %s\n", s->addr, s->val, ph);
			break;
		case PG106_PMU_STEP_RD:
			printf("R 0x%06x 0x%08x %s\n", s->addr, s->val, ph);
			break;
		case PG106_PMU_STEP_RESET:
			printf("RESET - - %s\n", ph);
			break;
		case PG106_PMU_STEP_POLL:
			printf("POLL 0x%06x %u %s\n", s->addr, s->val, ph);
			break;
		}
	}
	fprintf(stderr, "model: %zu steps (%u IMEM + %u DMEM data words)\n",
		res.steps, res.imem_words, res.dmem_words);
	return 0;
}
