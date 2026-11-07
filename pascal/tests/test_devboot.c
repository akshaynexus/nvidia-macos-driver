// SPDX-License-Identifier: GPL-2.0-only
// pascal/tests/test_devboot.c — the bring-up sequence (core/src/devboot.c)
// that the kext runs at start() and after every D3cold power-up.
//
// SIMULATION. The bus is a register file with two behaviours: a PMU that
// raises 0x10a040 bit 13 when DEVINIT is started (or never, to test the
// timeout), and PRI-error answers where a test asks for them. The ACR and
// GR stages run in full against their own mocks in test_acr_boot.c and
// test_gr_boot.c; here they are exercised up to their failure paths, which
// is what the kext's "every failure stops and leaves the GPU idle" rule is
// about. Real inputs: the Helios VBIOS (POST path; PASS-SKIP without it)
// and the committed linux-firmware gp106 blobs.

#include <pascal/devboot.h>
#include <pascal/pmc.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail;

#define CHECK(c)                                                         \
	do {                                                             \
		if (!(c)) {                                              \
			printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
			g_fail++;                                        \
		}                                                        \
	} while (0)
#define CHECK_EQ(a, b)                                                   \
	do {                                                             \
		long long a_ = (long long)(a), b_ = (long long)(b);      \
		if (a_ != b_) {                                          \
			printf("FAIL %s:%d: %s == %s (%lld vs %lld)\n",  \
			       __FILE__, __LINE__, #a, #b, a_, b_);      \
			g_fail++;                                        \
		}                                                        \
	} while (0)

#define NREG 8192
struct regs {
	uint32_t addr[NREG], val[NREG];
	size_t n;
	bool pmu_answers;
	uint32_t pri_error_addr; // reads return 0xbadf1100
	size_t writes;
	size_t writes_after_fail;
	bool failed;
	uint64_t t_us;
	// events
	char log[64][96];
	unsigned nlog;
	uint8_t fail_stage;
	int fail_ret;
};

static uint32_t *
slot(struct regs *r, uint32_t a, bool mk)
{
	size_t i;

	for (i = 0; i < r->n; i++)
		if (r->addr[i] == a)
			return &r->val[i];
	if (!mk || r->n == NREG)
		return NULL;
	r->addr[r->n] = a;
	r->val[r->n] = 0;
	return &r->val[r->n++];
}

static uint32_t
get(struct regs *r, uint32_t a)
{
	uint32_t *p = slot(r, a, false);
	return p ? *p : 0;
}

static void
set(struct regs *r, uint32_t a, uint32_t v)
{
	uint32_t *p = slot(r, a, true);
	if (p)
		*p = v;
}

static uint32_t
rd(void *u, uint32_t a)
{
	struct regs *r = u;

	if (r->pri_error_addr && a == r->pri_error_addr)
		return 0xbadf1100u;
	return get(r, a);
}

static void
wr(void *u, uint32_t a, uint32_t v)
{
	struct regs *r = u;

	r->writes++;
	if (r->failed)
		r->writes_after_fail++;
	// gm200.c:163-168: mailbox 0x5000, start; DEVINIT signals bit 13.
	if (a == PG106_PMU_FALCON_CPUCTL && v == 2 && r->pmu_answers &&
	    get(r, PG106_PMU_FALCON_MAILBOX) == 0x5000u) {
		set(r, PG106_PMU_FALCON_MAILBOX, 0x5000u | PG106_PMU_DEVINIT_DONE);
		return;
	}
	set(r, a, v);
}

static void
dly(void *u, uint32_t us)
{
	((struct regs *)u)->t_us += us;
}

static void
on_event(void *u, const struct pg106_devboot_event *e)
{
	struct regs *r = u;

	if (r->nlog < 64)
		snprintf(r->log[r->nlog++], 96, "%s %s %d", pg106_devboot_stage_name(e->stage),
			 pg106_devboot_ev_name(e->ev), e->ret);
	if (e->ev == PG106_DB_EV_FAIL) {
		r->failed = true;
		r->fail_stage = e->stage;
		r->fail_ret = e->ret;
	}
}

static bool
logged(const struct regs *r, const char *s)
{
	unsigned i;

	for (i = 0; i < r->nlog; i++)
		if (strcmp(r->log[i], s) == 0)
			return true;
	return false;
}

static uint8_t *
load(const char *path, size_t *n)
{
	FILE *f = fopen(path, "rb");
	uint8_t *b;
	long sz;

	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	sz = ftell(f);
	fseek(f, 0, SEEK_SET);
	b = malloc(sz > 0 ? (size_t)sz : 1);
	if (b && fread(b, 1, (size_t)sz, f) != (size_t)sz) {
		free(b);
		b = NULL;
	}
	fclose(f);
	*n = (size_t)sz;
	return b;
}

static struct regs *
fresh(uint32_t boot0, bool posted)
{
	struct regs *r = calloc(1, sizeof(*r));

	set(r, PG106_PMC_BOOT_0, boot0);
	set(r, PG106_DEVINIT_POSTED_REG, posted ? PG106_DEVINIT_POSTED_BIT : 0);
	return r;
}

static struct pg106_hw_bus
bus_of(struct regs *r)
{
	return (struct pg106_hw_bus){ r, rd, wr, dly, NULL, 0 };
}

#define BOOT0_GP106 0x136000a1u

static void
test_identify_posted(void)
{
	struct regs *r = fresh(0xffffffffu, true);
	struct pg106_hw_bus b = bus_of(r);
	struct pg106_devboot_cfg c = { .bus = &b, .event = on_event,
				       .event_user = r,
				       .stop_after = PG106_DB_READY };
	struct pg106_devboot db;

	// Powered-off / not decoding GPU: refused before any write.
	CHECK_EQ(pg106_devboot_run(&db, &c), -ENODEV);
	CHECK_EQ(db.stage, PG106_DB_IDENTIFY);
	CHECK_EQ(r->writes, 0);
	CHECK(logged(r, "identify FAIL -19"));
	pg106_devboot_fini(&db);
	free(r);

	// A GP104 is refused too.
	r = fresh(0x134000a1u, true);
	b = bus_of(r);
	c.bus = &b;
	c.event_user = r;
	CHECK_EQ(pg106_devboot_run(&db, &c), -ENODEV);
	pg106_devboot_fini(&db);
	free(r);

	// Safe mode (identify + posted only): reads, no writes.
	r = fresh(BOOT0_GP106, true);
	b = bus_of(r);
	c.bus = &b;
	c.event_user = r;
	c.stop_after = PG106_DB_POSTED;
	CHECK_EQ(pg106_devboot_run(&db, &c), 0);
	CHECK(db.posted_before);
	CHECK_EQ(r->writes, 0);
	CHECK(logged(r, "identify ok 0") && logged(r, "posted ok 0"));
	pg106_devboot_fini(&db);
	free(r);

	// 0x2240c answering with a PRI error is not "unposted".
	r = fresh(BOOT0_GP106, false);
	r->pri_error_addr = PG106_DEVINIT_POSTED_REG;
	b = bus_of(r);
	c.bus = &b;
	c.event_user = r;
	CHECK_EQ(pg106_devboot_run(&db, &c), -EIO);
	CHECK_EQ(db.stage, PG106_DB_POSTED);
	pg106_devboot_fini(&db);
	free(r);
	printf("PASS identify/posted: unreadable and non-GP106 BOOT_0 refused with 0 "
	       "writes; safe mode writes nothing; PRI-error posted bit refused\n");
}

static void
test_post(void)
{
	size_t n = 0;
	uint8_t *rom = load("../research/firmware/vbios/helios300-gp106.rom", &n);
	struct regs *r = fresh(BOOT0_GP106, false);
	struct pg106_hw_bus b = bus_of(r);
	struct pg106_devboot_cfg c = { .bus = &b, .event = on_event,
				       .event_user = r,
				       .stop_after = PG106_DB_POST };
	struct pg106_devboot db;

	// Not posted and no VBIOS: POST cannot run.
	CHECK_EQ(pg106_devboot_run(&db, &c), -ENOENT);
	CHECK_EQ(db.stage, PG106_DB_POST);
	CHECK_EQ(r->writes, 0);
	pg106_devboot_fini(&db);
	free(r);

	// Already posted: POST skipped.
	r = fresh(BOOT0_GP106, true);
	b = bus_of(r);
	c.bus = &b;
	c.event_user = r;
	CHECK_EQ(pg106_devboot_run(&db, &c), 0);
	CHECK(logged(r, "post skip 0"));
	CHECK(!db.posted);
	pg106_devboot_fini(&db);
	free(r);

	if (!rom) {
		printf("PASS-SKIP POST with the Helios VBIOS: ROM not present\n");
		return;
	}
	// After D3cold (posted bit clear): gm200_devinit_post with the real
	// ROM; the mock PMU signals DEVINIT done.
	r = fresh(BOOT0_GP106, false);
	r->pmu_answers = true;
	b = bus_of(r);
	c.bus = &b;
	c.event_user = r;
	c.vbios = rom;
	c.vbios_size = (uint32_t)n;
	CHECK_EQ(pg106_devboot_run(&db, &c), 0);
	CHECK(db.posted);
	CHECK(db.post.have_devinit_app);
	CHECK(db.post.imem_words > 0 && db.post.dmem_words > 0);
	CHECK(logged(r, "post ok 0"));
	printf("PASS POST on the real Helios VBIOS: %zu bus steps, %u IMEM + %u "
	       "DMEM words, done after %u poll(s)\n",
	       db.post.steps, db.post.imem_words, db.post.dmem_words,
	       db.post.polls);
	pg106_devboot_fini(&db);
	free(r);

	// The PMU never answers: -ETIMEDOUT after nouveau's 2 s budget, and
	// nothing is written after the failing poll.
	r = fresh(BOOT0_GP106, false);
	b = bus_of(r);
	c.bus = &b;
	c.event_user = r;
	c.stop_after = PG106_DB_READY;
	CHECK_EQ(pg106_devboot_run(&db, &c), -ETIMEDOUT);
	CHECK_EQ(db.stage, PG106_DB_POST);
	CHECK_EQ(r->writes_after_fail, 0);
	CHECK(r->t_us >= 1999u * 1000u);
	CHECK(!logged(r, "top ok 0"));
	printf("PASS POST timeout: -ETIMEDOUT after %llu us simulated, no later "
	       "stage ran\n", (unsigned long long)r->t_us);
	pg106_devboot_fini(&db);
	free(r);
	free(rom);
}

struct fwset {
	uint8_t *buf[32];
	struct pg106_devboot_fw fw;
};

static int
load_fw(struct fwset *s)
{
	size_t i;

	memset(s, 0, sizeof(*s));
	for (i = 0; i < pg106_devboot_fw_count; i++) {
		const struct pg106_fw_name *nm = &pg106_devboot_fw_names[i];
		struct pg106_fw_blob *b =
			(struct pg106_fw_blob *)((uint8_t *)&s->fw + nm->offset);
		char path[256];
		size_t n = 0;

		snprintf(path, sizeof path, "../research/firmware/%s", nm->path);
		s->buf[i] = load(path, &n);
		b->data = s->buf[i];
		b->size = s->buf[i] ? n : 0;
		if (!s->buf[i] && nm->required_for == PG106_DB_ACR)
			return -1;
	}
	return 0;
}

static void
free_fw(struct fwset *s)
{
	size_t i;

	for (i = 0; i < 32; i++)
		free(s->buf[i]);
}

static void
test_names(void)
{
	size_t i, j;

	CHECK_EQ(pg106_devboot_fw_count, 14);
	for (i = 0; i < pg106_devboot_fw_count; i++) {
		const struct pg106_fw_name *a = &pg106_devboot_fw_names[i];

		CHECK(strncmp(a->resource, "gp106-", 6) == 0);
		CHECK(strchr(a->resource, '/') == NULL); // flat bundle names
		CHECK(a->offset + sizeof(struct pg106_fw_blob) <=
		      sizeof(struct pg106_devboot_fw));
		for (j = 0; j < i; j++) {
			CHECK(strcmp(a->resource, pg106_devboot_fw_names[j].resource));
			CHECK(a->offset != pg106_devboot_fw_names[j].offset);
		}
	}
	printf("PASS firmware name table: 14 unique flat resource names, one per field\n");
}

static void
test_acr_gr_failures(void)
{
	struct fwset s;
	struct regs *r;
	struct pg106_hw_bus b;
	struct pg106_devboot_cfg c;
	struct pg106_devboot db;

	if (load_fw(&s)) {
		printf("FAIL: research/firmware/gp106 not found\n");
		g_fail++;
		free_fw(&s);
		return;
	}
	// No firmware at all: ACR refuses before touching SEC2.
	r = fresh(BOOT0_GP106, true);
	b = bus_of(r);
	memset(&c, 0, sizeof(c));
	c.bus = &b;
	c.event = on_event;
	c.event_user = r;
	c.stop_after = PG106_DB_READY;
	CHECK_EQ(pg106_devboot_run(&db, &c), -ENOENT);
	CHECK_EQ(db.stage, PG106_DB_ACR);
	CHECK_EQ(r->writes, 0);
	// PTOP empty: the measured fallback base was used.
	CHECK_EQ(db.sec2_base, PG106_ACRBOOT_SEC2_BASE_GP106);
	pg106_devboot_fini(&db);
	free(r);

	// Real firmware, but SEC2 answers with a PRI error (engine gated).
	r = fresh(BOOT0_GP106, true);
	r->pri_error_addr = PG106_ACRBOOT_SEC2_BASE_GP106 + PG106_FALCON_MEM_CAPS;
	b = bus_of(r);
	c.bus = &b;
	c.event_user = r;
	c.fw = &s.fw;
	CHECK_EQ(pg106_devboot_run(&db, &c), -ENODEV);
	CHECK_EQ(db.stage, PG106_DB_ACR);
	CHECK_EQ(r->writes, 0);
	pg106_devboot_fini(&db);
	free(r);

	// HWCFG readable but no memory-object writer: the plan builds from
	// the real blobs and stops at its first WPR write (-ENOSYS), having
	// issued only the oneinit reads before it.
	r = fresh(BOOT0_GP106, true);
	set(r, PG106_ACRBOOT_SEC2_BASE_GP106 + PG106_FALCON_MEM_CAPS, 0x400e0100u);
	b = bus_of(r);
	c.bus = &b;
	c.event_user = r;
	c.mem = (struct pg106_devboot_mem){ 0x100000000ull, 0x0f0000000ull, 0,
					     0x10000000ull };
	CHECK_EQ(pg106_devboot_run(&db, &c), -ENOSYS);
	CHECK_EQ(db.stage, PG106_DB_ACR);
	CHECK(db.acr_plan.nsteps > 100);
	CHECK(db.exec.why && db.exec.why[0]);
	CHECK_EQ(r->writes, 0);
	printf("PASS ACR stage: missing firmware, gated SEC2 and missing mem_write "
	       "all stop before the first write (plan %zu steps from the real blobs)\n",
	       db.acr_plan.nsteps);
	pg106_devboot_fini(&db);
	free(r);

	// GR stage alone: no sw_nonctx -> -ENOENT before any GR write.
	r = fresh(BOOT0_GP106, true);
	b = bus_of(r);
	c.bus = &b;
	c.event_user = r;
	memset(&db, 0, sizeof(db));
	db.sec2_base = PG106_ACRBOOT_SEC2_BASE_GP106;
	s.fw.sw_nonctx.data = NULL;
	s.fw.sw_nonctx.size = 0;
	CHECK_EQ(pg106_devboot_gr(&db, &c), -ENOENT);
	CHECK_EQ(r->writes, 0);
	pg106_devboot_fini(&db);
	// With a pack but 0x409604 = 0 GPCs: refused (-ENODEV).
	{
		static const uint8_t pack[8] = { 0 };

		s.fw.sw_nonctx.data = pack;
		s.fw.sw_nonctx.size = sizeof(pack);
		memset(&db, 0, sizeof(db));
		CHECK_EQ(pg106_devboot_gr(&db, &c), -ENODEV);
		CHECK_EQ(r->writes, 0);
		pg106_devboot_fini(&db);
		s.fw.sw_nonctx.data = NULL;
	}
	printf("PASS GR stage: missing sw_nonctx.bin and an empty GPC count stop "
	       "before any write\n");
	free(r);
	free_fw(&s);
}

int
main(void)
{
	printf("== devboot: SIMULATION — bring-up sequence control flow ==\n");
	test_names();
	test_identify_posted();
	test_post();
	test_acr_gr_failures();
	if (g_fail) {
		printf("%d failure(s)\n", g_fail);
		return 1;
	}
	printf("PASS test_devboot\n");
	return 0;
}
