// SPDX-License-Identifier: GPL-2.0-only
// pascal/tests/test_devinit_real.c — DEVINIT interpreter against EVERY
// boot script of EVERY committed real VBIOS (interpreter-coverage evidence).
//
// SCOPE (corrected): this is NOT the GP10x POST path. On GP106 nouveau
// does not host-interpret the BIT 'I' boot scripts — gm200_devinit_post
// (devinit/gm200.c:123-176) uploads them to the VBIOS PMU DEVINIT app
// (tests/test_devinit_pmu.c models that). Running them here is opcode
// coverage on real firmware bytes: every opcode they use must decode with
// upstream semantics, and the run must be deterministic.
//
// For each ROM: resolve the env exactly as nvbios_init would
// (pg106_init_env_from_bios: 'I' sub-tables via init_table_, 'C' PLL limits
// incl. the u32 v2 pointer, 'M' ramcfg count, image0/imaged remap, DCB for
// conn/GPIO), walk script indices until a zero entry, run every script
// twice on a fresh mock bus and require byte-identical traces. Accepted
// outcomes: PG106_INIT_OK, or PG106_INIT_ERR_OPCODE (a byte absent from
// nvkm's table — upstream nvbios_exec stops there with -EINVAL too).
// Missing tables now degrade with warnings, as upstream.
//
// Stand-ins: MMIO/IO reads come from a fresh zeroed mock store; the ramcfg
// strap INDEX is 0 (nvbios_ramcfg_index needs hardware); crystal 27 MHz;
// no I2C/AUX hooks (master's adap == NULL paths).
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

static const char *const roms[] = {
	"../research/firmware/vbios/asus-gtx1050ti-tpu188459.rom",
	"../research/firmware/vbios/gigabyte-gtx1060-6gb-tpu186364.rom",
	"../research/firmware/vbios/msi-gtx1050-tpu193426.rom",
	"../research/firmware/vbios/msi-gtx1050ti-mobile-tpu241139.rom",
	"../research/firmware/vbios/nvidia-gtx1060-3gb-tpu208872.rom",
	"../research/firmware/vbios/helios300-gp106.rom", // target machine
};
#define ROM_COUNT (sizeof(roms) / sizeof(roms[0]))

#define ROM_MAX (1u << 18) // largest committed image is 160 KiB
#define TRACE_CAP 65536    // real scripts write a lot; 512 KiB static is fine
#define MAX_SCRIPTS 32     // script-table walk bound (largest seen: 5)

static uint8_t image[ROM_MAX];
static struct pg106_init_trace_rec trace1[TRACE_CAP], trace2[TRACE_CAP];

// ---------------------------------------------------------------------------
// Deterministic mock bus (structure mirrors tests/test_devinit.c: sparse
// MMIO store, indexed-IO ports, direct ports, accumulating clock). Fresh
// zero state — no presets — so every read is deterministic. No I2C hooks:
// the interpreter then reproduces master's no-adapter path exactly.
// ---------------------------------------------------------------------------

#define MAX_REGS 8192
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
	NULL, NULL, NULL, NULL, // no I2C bus: master's adap == NULL degradation
	NULL, NULL, NULL,       // no AUX channel / gpio func hook
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
// Env wiring: nvbios_init semantics via pg106_init_env_from_bios.
// ---------------------------------------------------------------------------

static void
setup_env(struct pg106_init_env *env, struct pg106_init_trace_rec *buf,
	  const struct pg106_vbios_bit *vb)
{
	reset_bus();
	pg106_init_env_init(env, &test_vt, image, vb->size, buf, TRACE_CAP);
	REQUIRE(pg106_init_env_from_bios(env, vb) == 0);
	env->ramcfg_index = 0;     // strap stand-in (devinit.h)
	env->crystal_khz = 27000;  // nvkm device->crystal stand-in
}

// ---------------------------------------------------------------------------
// Per-script evidence: opcode histogram over the trace, result row.
// ---------------------------------------------------------------------------

static void
print_histogram(const struct pg106_init_trace_rec *buf, size_t n)
{
	unsigned counts[256] = { 0 };
	for (size_t i = 0; i < n; i++)
		counts[buf[i].opcode]++;
	printf("   opcodes in trace:");
	for (unsigned op = 0; op < 256; op++)
		if (counts[op])
			printf(" %s x%u", pg106_init_opcode_name((uint8_t)op),
			       counts[op]);
	printf("\n");
}

static void
dump_edges(const struct pg106_init_trace_rec *buf, size_t n)
{
	size_t head = n < 5 ? n : 5;
	size_t tail = n < 5 ? 0 : n - 5;
	for (size_t i = 0; i < head; i++)
		printf("   first[%zu] @0x%04x op 0x%02x (%s): addr 0x%08x "
		       "val 0x%08x kind %u\n", i, buf[i].script_offset,
		       buf[i].opcode, pg106_init_opcode_name(buf[i].opcode),
		       buf[i].address, buf[i].value, buf[i].kind);
	if (tail > head)
		printf("   ... %zu record(s) omitted ...\n", tail - head);
	for (size_t i = tail; i < n; i++)
		printf("   last[%zu] @0x%04x op 0x%02x (%s): addr 0x%08x "
		       "val 0x%08x kind %u\n", i, buf[i].script_offset,
		       buf[i].opcode, pg106_init_opcode_name(buf[i].opcode),
		       buf[i].address, buf[i].value, buf[i].kind);
}

// Accepted outcomes: ran to INIT_DONE, or stopped loudly on an opcode/
// subtable problem (the error always names the opcode byte).
static int
accepted_status(int rc)
{
	return rc == PG106_INIT_OK || rc == PG106_INIT_ERR_OPCODE;
}

int
main(void)
{
	printf("devinit REAL-ROM all-scripts test — SIMULATION ONLY, not "
	       "hardware evidence (all committed GP10x ROMs, "
	       "research/firmware/vbios/)\n");

	unsigned total_scripts = 0, total_ok = 0, total_loud = 0;
	int pll_hit = 0;

	for (unsigned r = 0; r < ROM_COUNT; r++) {
		// --- load + parse -----------------------------------------
		size_t size = 0;
		FILE *f = fopen(roms[r], "rb");
		if (f) {
			size = fread(image, 1, ROM_MAX, f);
			fclose(f);
		}
		if (size == 0) {
			// CI-safe when the ROM checkout is absent.
			printf("PASS-SKIP %s: not present\n", roms[r]);
			continue;
		}
		REQUIRE(size >= 0x1000);
		printf("-- %s: %zu bytes\n", roms[r], size);

		struct pg106_vbios_bit vb;
		REQUIRE(pg106_vbios_bit_parse(image, (uint32_t)size, &vb) == 0);
		struct pg106_vbios_bit_entry bit_i;
		REQUIRE(pg106_vbios_bit_entry(&vb, 'I', &bit_i) == 0);

		// --- walk the script table --------------------------------
		uint32_t offsets[MAX_SCRIPTS];
		unsigned nscripts = 0;
		for (unsigned idx = 0; idx < MAX_SCRIPTS; idx++) {
			uint32_t off = 0;
			int rc = pg106_vbios_init_script(&vb, &bit_i, idx,
							 &off);
			if (rc < 0)
				break; // outside the table -> walk over
			if (off == 0)
				break; // documented "no such script"
			offsets[nscripts++] = off;
		}
		REQUIRE(nscripts > 0);
		printf("   %u script(s):", nscripts);
		for (unsigned i = 0; i < nscripts; i++)
			printf(" [%u]@0x%04x", i, offsets[i]);
		printf("\n");

		// --- run every script, twice, diff clean -------------------
		for (unsigned s = 0; s < nscripts; s++) {
			static struct pg106_init_env env1, env2;

			setup_env(&env1, trace1, &vb);
			int rc1 = pg106_init_run(&env1, offsets[s]);

			setup_env(&env2, trace2, &vb);
			int rc2 = pg106_init_run(&env2, offsets[s]);

			REQUIRE(accepted_status(rc1));
			REQUIRE(rc2 == rc1);
			REQUIRE(env2.trace_len == env1.trace_len);
			int differs = 0;
			for (size_t i = 0; i < env1.trace_len; i++) {
				if (memcmp(&trace1[i], &trace2[i],
					   sizeof(trace1[i])) != 0) {
					printf("FAIL determinism %s script "
					       "%u record %zu\n", roms[r], s,
					       i);
					failures++;
					differs = 1;
					break;
				}
			}
			REQUIRE(!differs);

			const char *result;
			if (rc1 == PG106_INIT_OK) {
				result = "OK";
				total_ok++;
			} else {
				result = "LOUD";
				total_loud++;
				REQUIRE(env1.error_opcode != 0);
				printf("   loud stop detail: opcode 0x%02x "
				       "(%s) @0x%04x: %s\n",
				       env1.error_opcode,
				       pg106_init_opcode_name(
					       env1.error_opcode),
				       env1.error_offset, env1.error_msg);
				// The error must name the opcode — either by
				// handler name or (bytes absent from nvkm's
				// dispatch, e.g. the 0xac real ROMs carry) by
				// the unknown-opcode message.
				const char *name = pg106_init_opcode_name(
					env1.error_opcode);
				REQUIRE(strstr(env1.error_msg, name) != NULL ||
					strstr(env1.error_msg,
					       "not in nvkm dispatch") !=
					NULL);
				(void)name;
				if (env1.error_opcode == 0xac)
					printf("   note: 0xac is not in nvkm's "
					       "host init_opcode[] — on GP10x these "
					       "boot scripts are executed by the "
					       "VBIOS PMU DEVINIT app "
					       "(devinit/gm200.c:155-170), not "
					       "the host interpreter\n");
			}

			printf("   script[%u] @0x%04x -> %s (%s), steps %u, "
			       "trace %zu, warnings %u, clock %llu us, "
			       "gpio_resets %u\n",
			       s, offsets[s], result, pg106_init_strerr(rc1),
			       env1.steps, env1.trace_len, env1.warnings,
			       (unsigned long long)fake_clock, gpio_resets);

			// Headline: a real script exercising the freshly
			// implemented PLL opcodes.
			for (size_t i = 0; i < env1.trace_len; i++) {
				uint8_t op = trace1[i].opcode;
				if (op == PG106_INIT_PLL ||
				    op == PG106_INIT_PLL2 ||
				    op == PG106_INIT_IO_RESTRICT_PLL ||
				    op == PG106_INIT_IO_RESTRICT_PLL2 ||
				    op == PG106_INIT_PLL_INDIRECT ||
				    op == PG106_INIT_RAM_RESTRICT_PLL)
					pll_hit = 1;
			}

			print_histogram(trace1, env1.trace_len);
			if (s == 0) {
				// Keep full first/last-5 evidence for script 0
				// of each ROM (the pre-POST script).
				dump_edges(trace1, env1.trace_len);
			}
			total_scripts++;
		}
	}

	if (total_scripts == 0) {
		printf("PASS-SKIP devinit real: no ROM under "
		       "../research/firmware/vbios/ (fetch them per its "
		       "README.md); nothing real was run\n");
		return failures ? EXIT_FAILURE : EXIT_SUCCESS;
	}
	printf("-- matrix: %u script(s) across %u ROM(s): %u ran to "
	       "INIT_DONE, %u stopped loudly on a documented "
	       "opcode/subtable\n",
	       total_scripts, (unsigned)ROM_COUNT, total_ok, total_loud);
	if (pll_hit)
		printf("HEADLINE: a real committed ROM script exercised an "
		       "INIT_PLL-family opcode through the new pll limits/"
		       "gt215_pll_calc/gf100 pll_set port\n");
	else
		printf("no committed ROM script reached an INIT_PLL-family "
		       "opcode in these runs (none emitted in the walked "
		       "scripts)\n");
	ok("devinit real: every script of every committed ROM terminated "
	   "OK or loudly, each run twice with byte-identical traces");

	if (failures) {
		printf("%d check(s) FAILED\n", failures);
		return EXIT_FAILURE;
	}
	printf("all checks passed\n");
	return EXIT_SUCCESS;
}
