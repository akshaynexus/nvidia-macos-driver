// SPDX-License-Identifier: GPL-2.0-only
// pascal/tests/test_devinit_real.c — DEVINIT interpreter against a REAL
// VBIOS init script.
//
// Phase 1 gate, real-ROM edition (HANDOFF-PASCAL.md §6 Phase 1): takes the
// smallest committed GP10x image (research/firmware/vbios/, a real desktop
// GP106 dump), resolves its init script 0 (pre-POST, the first script
// nvbios_post runs — bios-init/init.c:2331) through the BIT parser, and
// drives the EXISTING interpreter (pg106_init_run, core/src/devinit.c —
// not modified here) with a deterministic mock bus, mirroring
// tests/test_devinit.c's vtable.
//
// Acceptance: the run terminates with PG106_INIT_OK, or with a DOCUMENTED
// LOUD error — PG106_INIT_ERR_OPCODE naming the opcode (real scripts may
// legitimately need buses this simulator does not model: I2C/AUX/PLL/TMDS;
// devinit.h refuses to silently skip). Either way:
//   - the trace size and first/last 5 records are printed as evidence,
//   - the script is run TWICE and the traces must diff clean byte-for-byte
//     (the determinism part of the Phase 1 gate, now on real ROM bytes),
//   - when stopped by an opcode, the error names it and the partial trace
//     up to that point is proven deterministic.
//
// Bus-state stand-ins (both documented in devinit.h and reported in the
// output): MMIO/IO reads return the mock store's deterministic contents
// (fresh zero state), and the RAM-restrict strap selection is
// ram_restrict_groups=1 / ramcfg_index=0 because the strap parser does not
// exist yet.
//
// THIS IS SIMULATION, NOT HARDWARE EVIDENCE (HANDOFF-PASCAL.md §7).

#include <pascal/devinit.h>
#include <pascal/vbios_bit.h>

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

// Smallest committed image (0xf000): NVIDIA GTX 1060 3GB (GP106), TPU id
// 208872, sha256 in research/firmware/vbios/README.md. Also the ROM with
// the deepest script table (5 nonzero scripts, indexes 0..4).
#define ROM_FILE "../research/firmware/vbios/nvidia-gtx1060-3gb-tpu208872.rom"
#define ROM_MAX (1u << 19)
#define TRACE_CAP 65536 // real scripts write a lot; 1 MiB static is fine

static uint8_t image[ROM_MAX];
static struct pg106_init_trace_rec trace1[TRACE_CAP], trace2[TRACE_CAP];

// ---------------------------------------------------------------------------
// Deterministic mock bus (structure mirrors tests/test_devinit.c: sparse
// MMIO store, indexed-IO ports, direct ports, accumulating clock). Fresh
// zero state — no presets — so every read is deterministic.
// ---------------------------------------------------------------------------

#define MAX_REGS 4096
struct fake_reg { uint32_t addr, val; };
static struct fake_reg mmio[MAX_REGS];
static unsigned mmio_len;

#define MAX_IOPORTS 16
struct fake_io_port { uint16_t port; uint8_t idx[256]; };
static struct fake_io_port ioports[MAX_IOPORTS];
static unsigned ioports_len;

static uint8_t direct_port[65536];
static uint64_t fake_clock;
static unsigned gpio_resets;

static struct fake_io_port *
ioport(uint16_t port)
{
	for (unsigned i = 0; i < ioports_len; i++)
		if (ioports[i].port == port)
			return &ioports[i];
	REQUIRE(ioports_len < MAX_IOPORTS);
	struct fake_io_port *p = &ioports[ioports_len++];
	p->port = port;
	memset(p->idx, 0, sizeof(p->idx));
	return p;
}

static uint32_t
t_rd32(struct pg106_init_env *env, uint32_t addr)
{
	(void)env;
	for (unsigned i = 0; i < mmio_len; i++)
		if (mmio[i].addr == addr)
			return mmio[i].val;
	return 0;
}

static void
t_wr32(struct pg106_init_env *env, uint32_t addr, uint32_t val)
{
	(void)env;
	for (unsigned i = 0; i < mmio_len; i++) {
		if (mmio[i].addr == addr) {
			mmio[i].val = val;
			return;
		}
	}
	REQUIRE(mmio_len < MAX_REGS);
	mmio[mmio_len].addr = addr;
	mmio[mmio_len].val = val;
	mmio_len++;
}

static uint8_t
t_rd8(struct pg106_init_env *env, uint16_t port, uint8_t index)
{
	(void)env;
	return ioport(port)->idx[index];
}

static void
t_wr8(struct pg106_init_env *env, uint16_t port, uint8_t index, uint8_t val)
{
	(void)env;
	ioport(port)->idx[index] = val;
}

static uint8_t
t_rdport(struct pg106_init_env *env, uint16_t port)
{
	(void)env;
	return direct_port[port];
}

static void
t_wrport(struct pg106_init_env *env, uint16_t port, uint8_t val)
{
	(void)env;
	direct_port[port] = val;
}

static uint32_t
t_rd32_io(struct pg106_init_env *env, uint16_t port)
{
	(void)env; (void)port;
	return 0;
}

static uint64_t
t_time(struct pg106_init_env *env, uint32_t delay_us)
{
	(void)env;
	fake_clock += delay_us;
	return fake_clock;
}

static void
t_gpio_reset(struct pg106_init_env *env)
{
	(void)env;
	gpio_resets++;
}

static const struct pg106_init_vt test_vt = {
	t_rd32, t_wr32, t_rd8, t_wr8, t_rdport, t_wrport, t_rd32_io,
	t_time, t_gpio_reset,
};

// Reset the mock bus to the fresh state both runs see.
static void
reset_bus(void)
{
	mmio_len = 0;
	ioports_len = 0;
	memset(direct_port, 0, sizeof(direct_port));
	fake_clock = 0;
	gpio_resets = 0;
}

// ---------------------------------------------------------------------------
// Env wiring from the REAL image: the BIT 'I' init-data table's u16
// subtable pointers (init.c:399-406) and entry counts derived from the
// pointer deltas — the tables are back-to-back in the VBIOS data region,
// so the next-higher canonical pointer bounds each table (nouveau needs no
// counts; the simulator does — devinit.h). No higher pointer (shared/
// trailing pointer) -> generous bound of 255, still deterministic.
// ---------------------------------------------------------------------------

struct sub_tables {
	uint16_t script, macro, condition, io_condition, io_flag, xlat;
};

static uint8_t
delta_count(const struct sub_tables *t, uint16_t ptr, unsigned entry_size)
{
	uint16_t pts[6] = { t->script, t->macro, t->condition, t->io_condition,
		t->io_flag, t->xlat };
	uint16_t next = 0;
	for (unsigned i = 0; i < 6; i++)
		if (pts[i] > ptr && (next == 0 || pts[i] < next))
			next = pts[i];
	uint32_t n = next ? (next - ptr) / entry_size : 255;
	return (uint8_t)(n > 255 ? 255 : n);
}

static void
setup_env(struct pg106_init_env *env, struct pg106_init_trace_rec *buf,
	  const struct pg106_vbios_bit *vb,
	  const struct pg106_vbios_bit_entry *bit_i, uint16_t script_off)
{
	struct sub_tables t = { 0 };

	reset_bus();
	pg106_init_env_init(env, &test_vt, image, sizeof(image), buf,
			    TRACE_CAP);
	// Deterministic stand-in until the strap parser exists (devinit.h).
	env->ram_restrict_groups = 1;
	env->ramcfg_index = 0;

	REQUIRE(pg106_vbios_rd16(vb, bit_i->offset + 0x00, &t.script) == 0);
	REQUIRE(pg106_vbios_rd16(vb, bit_i->offset + 0x04, &t.macro) == 0);
	REQUIRE(pg106_vbios_rd16(vb, bit_i->offset + 0x06, &t.condition) == 0);
	REQUIRE(pg106_vbios_rd16(vb, bit_i->offset + 0x08,
				 &t.io_condition) == 0);
	REQUIRE(pg106_vbios_rd16(vb, bit_i->offset + 0x0a,
				 &t.io_flag) == 0);
	REQUIRE(pg106_vbios_rd16(vb, bit_i->offset + 0x10, &t.xlat) == 0);

	env->table_script = t.script;
	env->table_macro = t.macro;
	env->table_condition = t.condition;
	env->table_io_condition = t.io_condition;
	env->table_io_flag_condition = t.io_flag;
	env->table_xlat = t.xlat;
	env->macro_count = t.macro ? delta_count(&t, t.macro, 8) : 0;
	env->condition_count = t.condition ? delta_count(&t, t.condition, 12) : 0;
	env->io_condition_count =
		t.io_condition ? delta_count(&t, t.io_condition, 5) : 0;
	env->io_flag_condition_count =
		t.io_flag ? delta_count(&t, t.io_flag, 9) : 0;

	printf("   env subtables: script @0x%04x, macro @0x%04x (%u entries), "
	       "condition @0x%04x (%u), io_cond @0x%04x (%u), io_flag "
	       "@0x%04x (%u), xlat @0x%04x\n",
	       t.script, t.macro, env->macro_count, t.condition,
	       env->condition_count, t.io_condition, env->io_condition_count,
	       t.io_flag, env->io_flag_condition_count, t.xlat);
	printf("   script 0 @0x%04x\n", script_off);
}

static void
print_rec(const char *tag, unsigned i, const struct pg106_init_trace_rec *r)
{
	printf("  %s[%u] @0x%04x op 0x%02x (%s): addr 0x%08x val 0x%08x "
	       "idx 0x%02x kind %u\n", tag, i, r->script_offset, r->opcode,
	       pg106_init_opcode_name(r->opcode), r->address, r->value,
	       r->index, r->kind);
}

// Opcode histogram over a trace — the "which opcodes real scripts actually
// use" evidence. Side-effect opcodes only (conditions/jumps don't trace).
static void
print_histogram(const struct pg106_init_trace_rec *buf, size_t n)
{
	unsigned counts[256] = { 0 };
	for (size_t i = 0; i < n; i++)
		counts[buf[i].opcode]++;
	printf("   opcodes in trace:\n");
	for (unsigned op = 0; op < 256; op++)
		if (counts[op])
			printf("     0x%02x %-32s x%u\n", op,
			       pg106_init_opcode_name((uint8_t)op), counts[op]);
}

static void
dump_edges(const struct pg106_init_trace_rec *buf, size_t n)
{
	size_t head = n < 5 ? n : 5;
	size_t tail = n < 5 ? 0 : n - 5;
	for (size_t i = 0; i < head; i++)
		print_rec("first", (unsigned)i, &buf[i]);
	if (tail)
		printf("   ... %zu record(s) omitted ...\n", tail - head);
	for (size_t i = tail; i < n; i++)
		print_rec("last", (unsigned)i, &buf[i]);
}

int
main(void)
{
	printf("devinit REAL-ROM test — SIMULATION ONLY, not hardware "
	       "evidence (real GP106 ROM, research/firmware/vbios/)\n");

	// --- load + parse the real image ----------------------------------
	size_t size = 0;
	FILE *f = fopen(ROM_FILE, "rb");
	if (f) {
		size = fread(image, 1, ROM_MAX, f);
		fclose(f);
	}
	if (size == 0) {
		// PASS-skip: CI-safe when the ROM checkout is absent.
		printf("PASS-SKIP devinit real ROM: " ROM_FILE " not present\n");
		return failures ? EXIT_FAILURE : EXIT_SUCCESS;
	}
	REQUIRE(size >= 0x1000);
	printf("-- " ROM_FILE ": %zu bytes\n", size);

	struct pg106_vbios_bit vb;
	REQUIRE(pg106_vbios_bit_parse(image, (uint32_t)size, &vb) == 0);
	struct pg106_vbios_bit_entry bit_i;
	REQUIRE(pg106_vbios_bit_entry(&vb, 'I', &bit_i) == 0);

	// Script 0 = pre-POST init, the first script nvbios_post runs.
	uint16_t script0 = 0;
	REQUIRE(pg106_vbios_init_script(&vb, &bit_i, 0, &script0) == 0);
	REQUIRE(script0 != 0 && script0 < size);

	// --- run 1 ----------------------------------------------------------
	static struct pg106_init_env env1;
	setup_env(&env1, trace1, &vb, &bit_i, script0);
	int rc = pg106_init_run(&env1, script0);
	printf("   run 1: status %d (%s), steps %u, trace %zu records, "
	       "clock %llu us, gpio_resets %u\n",
	       rc, pg106_init_strerr(rc), env1.steps, env1.trace_len,
	       (unsigned long long)fake_clock, gpio_resets);

	// Acceptance: OK, or a documented loud stop naming the opcode. A
	// real script hitting an unmodelled bus (I2C/AUX/PLL/TMDS/DCB) is a
	// FINDING for the port, recorded below — not a test failure.
	if (rc == PG106_INIT_OK) {
		ok("devinit real: script 0 ran to INIT_DONE");
	} else if (rc == PG106_INIT_ERR_OPCODE) {
		const char *name = pg106_init_opcode_name(env1.error_opcode);
		printf("   LOUD STOP: opcode 0x%02x (%s) @0x%04x: %s\n",
		       env1.error_opcode, name, env1.error_offset,
		       env1.error_msg);
		REQUIRE(env1.error_opcode != 0);
		REQUIRE(strstr(env1.error_msg, name) != NULL);
		REQUIRE(!pg106_init_opcode_implemented(env1.error_opcode));
		printf("   FINDING for the port: real GP106 devinit needs %s "
		       "(0x%02x) — unimplemented by design, stops loudly\n",
		       name, env1.error_opcode);
		ok("devinit real: script 0 stopped loudly at a named opcode");
	} else {
		// OVERRUN/TRACE_FULL/NO_TABLE/DEPTH/STEPS on a real ROM is
		// still reportable evidence, but not one of the two accepted
		// outcomes the Phase 1 gate defines.
		printf("FAIL real script ended with status %d (%s): %s\n", rc,
		       pg106_init_strerr(rc), env1.error_msg);
		failures++;
	}
	REQUIRE(env1.trace_len > 0);

	dump_edges(trace1, env1.trace_len);
	print_histogram(trace1, env1.trace_len);

	// --- run 2 from identical inputs, diff clean ------------------------
	static struct pg106_init_env env2;
	setup_env(&env2, trace2, &vb, &bit_i, script0);
	int rc2 = pg106_init_run(&env2, script0);
	REQUIRE(rc2 == rc);
	REQUIRE(env2.trace_len == env1.trace_len);
	size_t n = env1.trace_len < env2.trace_len ? env1.trace_len
						   : env2.trace_len;
	int differs = 0;
	for (size_t i = 0; i < n; i++) {
		if (memcmp(&trace1[i], &trace2[i], sizeof(trace1[i])) != 0) {
			printf("FAIL determinism at record %zu:\n", i);
			print_rec("run1", (unsigned)i, &trace1[i]);
			print_rec("run2", (unsigned)i, &trace2[i]);
			failures++;
			differs = 1;
			break;
		}
	}
	REQUIRE(!differs);
	if (!differs)
		ok("devinit real: second run diffs clean against the first "
		   "(determinism on real ROM bytes)");

	if (failures) {
		printf("%d check(s) FAILED\n", failures);
		return EXIT_FAILURE;
	}
	printf("all checks passed\n");
	return EXIT_SUCCESS;
}
