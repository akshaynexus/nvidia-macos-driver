// SPDX-License-Identifier: GPL-2.0-only
// pascal/core/src/devinit_pmu.c — GP106 POST model (gm200 VBIOS-PMU
// DEVINIT). SIMULATION; see devinit_pmu.h. Line references: linux master
// nvkm/subdev/devinit/gm200.c.

#include <pascal/devinit_pmu.h>

#include <errno.h>
#include <string.h>

struct post_ctx {
	const struct pg106_vbios_bit *vb;
	const struct pg106_pmu_bus *bus;
	struct pg106_pmu_step *trace;
	size_t cap;
	struct pg106_post_result *res;
	uint8_t phase;
};

static void
rec(struct post_ctx *c, uint8_t kind, uint32_t addr, uint32_t val)
{
	if (c->trace && c->res->steps < c->cap) {
		struct pg106_pmu_step *s = &c->trace[c->res->steps];
		memset(s, 0, sizeof(*s));
		s->kind = kind;
		s->phase = c->phase;
		s->addr = addr;
		s->val = val;
	} else if (c->trace) {
		c->res->trace_truncated = true;
	}
	c->res->steps++;
}

static void
wr32(struct post_ctx *c, uint32_t addr, uint32_t val)
{
	c->bus->wr32(c->bus->user, addr, val);
	rec(c, PG106_PMU_STEP_WR, addr, val);
	if (addr == PG106_PMU_FALCON_IMEMD0)
		c->res->imem_words++;
	else if (addr == PG106_PMU_FALCON_DMEMD0)
		c->res->dmem_words++;
}

static uint32_t
rd32(struct post_ctx *c, uint32_t addr)
{
	uint32_t v = c->bus->rd32(c->bus->user, addr);
	rec(c, PG106_PMU_STEP_RD, addr, v);
	return v;
}

// nvkm_mask: read, then always write (temp & ~mask) | value.
static uint32_t
mask32(struct post_ctx *c, uint32_t addr, uint32_t m, uint32_t v)
{
	uint32_t t = rd32(c, addr);

	wr32(c, addr, (t & ~m) | v);
	return t;
}

static void
udelay(struct post_ctx *c, uint32_t us)
{
	if (c->bus->time)
		c->bus->time(c->bus->user, us);
}

// gm200_flcn_reset_wait_mem_scrubbing (falcon/gm200.c:140-151). The poll
// reads are recorded once per wait (first and last value) so a trace of
// a slow scrub stays small; res->scrub_* keep the counts.
static int
scrub_wait(struct post_ctx *c)
{
	uint32_t i, v = 0;

	mask32(c, PG106_PMU_FALCON_MAILBOX, 0, 0);
	for (i = 0; i < PG106_PMU_SCRUB_POLLS; i++) {
		v = c->bus->rd32(c->bus->user, PG106_PMU_FALCON_DMACTL);
		if (i == 0 || !(v & PG106_PMU_SCRUB_BUSY))
			rec(c, PG106_PMU_STEP_RD, PG106_PMU_FALCON_DMACTL, v);
		if (!(v & PG106_PMU_SCRUB_BUSY))
			break;
		udelay(c, 1);
	}
	if (i + 1 > c->res->scrub_polls_max)
		c->res->scrub_polls_max = i + 1;
	if (v & PG106_PMU_SCRUB_BUSY) {
		c->res->scrub_stuck = v;
		rec(c, PG106_PMU_STEP_POLL, PG106_PMU_FALCON_DMACTL, i);
		return -ETIMEDOUT;
	}
	return 0;
}

// gp102_flcn_reset_eng (falcon/gp102.c:66-82); gp102_pmu_flcn has no
// reset_prep.
static int
reset_eng(struct post_ctx *c)
{
	mask32(c, PG106_PMU_FALCON_ENGCTL, 0x00000001, 0x00000001);
	udelay(c, 10);
	mask32(c, PG106_PMU_FALCON_ENGCTL, 0x00000001, 0x00000000);
	return scrub_wait(c);
}

// nvkm_falcon_reset (falcon/base.c:243-252) with gp102_pmu_flcn's hooks
// (pmu/gp102.c:28-42): gm200_flcn_disable then gm200_flcn_enable.
static int
falcon_reset_regs(struct post_ctx *c)
{
	int ret;

	// gm200_flcn_disable (gm200.c:194-215): no select, no reset_pmc.
	mask32(c, PG106_PMU_FALCON_RM, 0x00000003, 0x00000000);
	wr32(c, PG106_PMU_FALCON_IRQMCLR, 0xffffffff);
	if ((ret = reset_eng(c)))
		return ret;
	// gm200_flcn_enable (gm200.c:153-180).
	if ((ret = reset_eng(c)))
		return ret;
	if ((ret = scrub_wait(c)))
		return ret;
	wr32(c, PG106_PMU_FALCON_BOOT0, rd32(c, 0x00000000));
	return 0;
}

int
pg106_pmu_falcon_reset(const struct pg106_pmu_bus *bus,
		       struct pg106_pmu_step *trace, size_t cap, size_t *steps,
		       uint8_t phase, struct pg106_post_result *res)
{
	struct pg106_post_result local;
	struct post_ctx c;
	int ret;

	if (!bus || !bus->rd32 || !bus->wr32)
		return -EINVAL;
	if (!res) {
		memset(&local, 0, sizeof(local));
		res = &local;
	}
	c = (struct post_ctx){ NULL, bus, trace, cap, res, phase };
	res->steps = steps ? *steps : 0;
	ret = falcon_reset_regs(&c);
	res->resets++;
	if (steps)
		*steps = res->steps;
	return ret;
}

size_t
pg106_devinit_preinit(const struct pg106_pmu_bus *bus,
		      struct pg106_pmu_step *trace, size_t cap)
{
	struct pg106_post_result res;
	struct post_ctx c = { NULL, bus, trace, cap, &res,
			      PG106_POST_PH_PREINIT };

	memset(&res, 0, sizeof(res));
	if (!bus || !bus->rd32 || !bus->wr32)
		return 0;
	// nvkm_pci_rom_shadow(pci, false) then (pci, true) around the PROM
	// read (bios/shadow.c, pci/base.c:66-74).
	mask32(&c, PG106_PCI_MIRROR_0x50, 0x00000001, 0x00000000);
	mask32(&c, PG106_PCI_MIRROR_0x50, 0x00000001, 0x00000001);
	// nvkm_intr_unarm -> gp100_mc_intr_unarm: both leaves blocked.
	wr32(&c, PG106_MC_INTR_BLOCK(0), 0xffffffff);
	wr32(&c, PG106_MC_INTR_BLOCK(1), 0xffffffff);
	return res.steps;
}

// pmu_code (gm200.c:32-50). ROM words via nvbios_rd32 semantics (remap,
// 0 on OOB).
static void
pmu_code(struct post_ctx *c, uint32_t pmu, uint32_t img, uint32_t len,
	 bool sec)
{
	uint32_t i;

	wr32(c, PG106_PMU_FALCON_IMEMC0,
	     0x01000000u | (sec ? 0x10000000u : 0) | pmu);
	for (i = 0; i < len; i += 4) {
		if ((i & 0xff) == 0)
			wr32(c, PG106_PMU_FALCON_IMEMT0, (pmu + i) >> 8);
		wr32(c, PG106_PMU_FALCON_IMEMD0,
		     pg106_vbios_nv_rd32(c->vb, img + i));
	}
	while (i & 0xff) {
		wr32(c, PG106_PMU_FALCON_IMEMD0, 0x00000000);
		i += 4;
	}
}

// pmu_data (gm200.c:52-62).
static void
pmu_data(struct post_ctx *c, uint32_t pmu, uint32_t img, uint32_t len)
{
	uint32_t i;

	wr32(c, PG106_PMU_FALCON_DMEMC0, 0x01000000u | pmu);
	for (i = 0; i < len; i += 4)
		wr32(c, PG106_PMU_FALCON_DMEMD0,
		     pg106_vbios_nv_rd32(c->vb, img + i));
}

// pmu_args (gm200.c:64-71).
static uint32_t
pmu_args(struct post_ctx *c, uint32_t argp, uint32_t argi)
{
	wr32(c, PG106_PMU_FALCON_DMEMC0, argp);
	wr32(c, PG106_PMU_FALCON_DMEMC0, rd32(c, PG106_PMU_FALCON_DMEMD0) + argi);
	return rd32(c, PG106_PMU_FALCON_DMEMD0);
}

// pmu_exec (gm200.c:73-80).
static void
pmu_exec(struct post_ctx *c, uint32_t init_addr)
{
	wr32(c, PG106_PMU_FALCON_BOOTVEC, init_addr);
	wr32(c, PG106_PMU_FALCON_DMACTL, 0x00000000);
	wr32(c, PG106_PMU_FALCON_CPUCTL, 0x00000002);
}

// pmu_load (gm200.c:82-112).
static int
pmu_load(struct post_ctx *c, uint8_t type, bool post, bool has_pmu,
	 struct pg106_vbios_pmu_app *app, uint32_t *init_addr_pmu,
	 uint32_t *args_addr_pmu)
{
	int ret;

	if (pg106_vbios_pmu_app(c->vb, type, app))	// gm200.c:91-92
		return -EINVAL;
	if (!post || !has_pmu)				// gm200.c:94-95
		return 0;

	// nvkm_falcon_reset (gm200.c:97): the hook when the caller models it,
	// else the GP102 PMU register sequence.
	if (c->bus->falcon_reset) {
		ret = c->bus->falcon_reset(c->bus->user);
	} else {
		ret = falcon_reset_regs(c);
		c->res->resets++;
	}
	rec(c, PG106_PMU_STEP_RESET, 0x0010a000u, (uint32_t)ret);
	if (ret)
		return ret;

	pmu_code(c, app->boot_addr_pmu, app->boot_addr, app->boot_size, false);
	pmu_code(c, app->code_addr_pmu, app->code_addr, app->code_size, true);
	pmu_data(c, app->data_addr_pmu, app->data_addr, app->data_size);

	if (init_addr_pmu) {
		*init_addr_pmu = app->init_addr_pmu;
		*args_addr_pmu = app->args_addr_pmu;
		return 0;
	}
	pmu_exec(c, app->init_addr_pmu);
	return 0;
}

int
pg106_devinit_gm200_post(const struct pg106_vbios_bit *vb, bool post,
			 bool has_pmu, const struct pg106_pmu_bus *bus,
			 struct pg106_pmu_step *trace, size_t cap,
			 struct pg106_post_result *res)
{
	struct post_ctx c = { vb, bus, trace, cap, res, 0 };
	uint32_t exec = 0, args = 0;
	int ret;

	if (!res)
		return -EINVAL;
	memset(res, 0, sizeof(*res));
	res->post = post;
	if (!vb || !bus || !bus->rd32 || !bus->wr32) {
		res->ret = -EINVAL;
		res->error = "bad arguments";
		return res->ret;
	}

	// gm200.c:134-138.
	if (pg106_vbios_bit_entry(vb, 'I', &res->bit_I) ||
	    res->bit_I.version != 1 || res->bit_I.length < 0x1c) {
		res->ret = -EINVAL;
		res->error = "VBIOS PMU init data not found";
		return res->ret;
	}

	// gm200.c:140-145.
	c.phase = PG106_POST_PH_DEVINIT_LOAD;
	ret = pmu_load(&c, PG106_VBIOS_PMU_TYPE_DEVINIT, post, has_pmu,
		       &res->devinit, &exec, &args);
	if (ret) {
		res->ret = ret;
		res->error = ret == -ETIMEDOUT ?
			"PMU falcon reset: IMEM/DMEM scrub (0x10a10c & 6) did "
			"not finish in 10 ms" : "VBIOS PMU/DEVINIT not found";
		return ret;
	}
	res->have_devinit_app = true;
	res->exec_addr = exec;
	res->args_addr = args;

	// gm200.c:147-153: init tables.
	if (post) {
		c.phase = PG106_POST_PH_TABLES;
		res->tables_dmem = pmu_args(&c, args + 0x08, 0x08);
		res->tables_img = pg106_vbios_nv_rd16(vb, res->bit_I.offset + 0x14u);
		res->tables_len = pg106_vbios_nv_rd16(vb, res->bit_I.offset + 0x16u);
		pmu_data(&c, res->tables_dmem, res->tables_img, res->tables_len);
	}

	// gm200.c:155-161: boot scripts.
	if (post) {
		c.phase = PG106_POST_PH_SCRIPTS;
		res->scripts_dmem = pmu_args(&c, args + 0x08, 0x10);
		res->scripts_img = pg106_vbios_nv_rd16(vb, res->bit_I.offset + 0x18u);
		res->scripts_len = pg106_vbios_nv_rd16(vb, res->bit_I.offset + 0x1au);
		pmu_data(&c, res->scripts_dmem, res->scripts_img,
			 res->scripts_len);
	}

	// gm200.c:163-172: execute DEVINIT and wait for bit 13.
	if (post) {
		uint32_t i, v = 0;

		c.phase = PG106_POST_PH_EXEC;
		wr32(&c, PG106_PMU_FALCON_MAILBOX, 0x00005000);
		pmu_exec(&c, exec);
		for (i = 0; i < PG106_DEVINIT_POST_POLLS; i++) {
			v = bus->rd32(bus->user, PG106_PMU_FALCON_MAILBOX);
			if (v & PG106_PMU_DEVINIT_DONE)
				break;
			if (bus->time)
				bus->time(bus->user, 1000);
		}
		res->polls = i < PG106_DEVINIT_POST_POLLS ? i + 1 : i;
		rec(&c, PG106_PMU_STEP_POLL, PG106_PMU_FALCON_MAILBOX,
		    res->polls);
		if (!(v & PG106_PMU_DEVINIT_DONE)) {
			// What the falcon looks like after 2 s, for the log.
			res->have_diag = true;
			res->diag_mbox0 = v;
			res->diag_cpuctl = rd32(&c, PG106_PMU_FALCON_CPUCTL);
			res->diag_mbox1 = rd32(&c, PG106_PMU_FALCON_MAILBOX1);
			res->diag_dmactl = rd32(&c, PG106_PMU_FALCON_DMACTL);
			res->diag_engctl = rd32(&c, PG106_PMU_FALCON_ENGCTL);
			res->diag_pmc_enable = rd32(&c, 0x00000200);
			res->diag_posted = rd32(&c, PG106_DEVINIT_POSTED_REG);
			res->ret = -ETIMEDOUT;
			res->error = "PMU DEVINIT did not signal 0x10a040 bit 13";
			return res->ret;
		}
	}

	// gm200.c:114-121, :174: PRE_OS, result ignored.
	c.phase = PG106_POST_PH_PREOS;
	res->have_preos_app = pmu_load(&c, PG106_VBIOS_PMU_TYPE_PRE_OS, post,
				       has_pmu, &res->preos, NULL, NULL) == 0;
	return 0;
}

const char *
pg106_post_phase_name(uint8_t phase)
{
	switch (phase) {
	case PG106_POST_PH_DEVINIT_LOAD: return "pmu_load(DEVINIT)";
	case PG106_POST_PH_TABLES:       return "tables->DMEM";
	case PG106_POST_PH_SCRIPTS:      return "scripts->DMEM";
	case PG106_POST_PH_EXEC:         return "exec+poll";
	case PG106_POST_PH_PREOS:        return "preos";
	case PG106_POST_PH_PREINIT:      return "preinit";
	default:                         return "?";
	}
}
