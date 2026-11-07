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

	ret = c->bus->falcon_reset ? c->bus->falcon_reset(c->bus->user) : 0;
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
		res->error = "VBIOS PMU/DEVINIT not found";
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
	default:                         return "?";
	}
}
