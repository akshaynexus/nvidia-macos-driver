// SPDX-License-Identifier: GPL-2.0-only
// pascal/tools/pg106_hwtest.c — Linux userspace hardware harness: runs the
// portable bring-up (pascal/core gpu.c: devboot with the ACR memory in
// VRAM, BAR2/BAR1, FIFO) on the real GP106 through
// /sys/bus/pci/devices/<bdf>/resource{0,1,3} mmaps, with no kernel driver
// bound, and ends with GPU work: a host semaphore release and a
// copy-engine copy (pg106_gpu_selftest).
//
//   pg106_hwtest --probe [--dev 0000:01:00.0]          read-only
//   pg106_hwtest --go --fw <research/firmware> [--vbios rom] [--stop-after N]
//
// Safety rules it enforces before touching anything:
//   - the function must be 10de:1c20 (GTX 1060 Mobile; --any-gp106 widens
//     it to the kext's gate list);
//   - no driver may be bound (<dev>/driver must not exist): nvidia or
//     nouveau would be fighting it;
//   - --probe only reads registers (BOOT_0, 0x2240c, PTOP, the FBP walk,
//     BAR binds, PMC_ENABLE, SEC2 HWCFG);
//   - every write needs --go; it may enable the function's memory decode
//     (<dev>/enable = 1) and nothing else in PCI config space.
// The HS ACR image is placed in VRAM (no host DMA is needed, so VT-d and
// bus mastering do not matter); the copy runs VRAM to VRAM.
//
// Recovery if the GPU wedges: stop the display manager first (the dGPU is
// not driving a panel on the Helios), unbind nvidia before the run, and a
// reboot restores the GPU (the ACR WPR lock and the FIFO state do not
// survive a cold boot). NOT run on the laptop without the coordinator's go.
//
// Status: builds and runs locally against a fake sysfs tree
// (scripts/hwtest_smoke.sh); NOT run on hardware.

#define _GNU_SOURCE
#include <pascal/devboot.h>
#include <pascal/gpu.h>
#include <pascal/pmc.h>
#include <pascal/top.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

struct bar {
	volatile uint8_t *p;
	uint64_t len;
};

struct hw {
	struct bar bar0, bar1, bar3;
	uint64_t writes, reads;
	bool go;
};

static struct hw g_hw;

static uint32_t
h_rd32(void *u, uint32_t a)
{
	struct hw *h = u;

	if ((uint64_t)a + 4 > h->bar0.len || (a & 3))
		return 0xffffffffu;
	h->reads++;
	return *(volatile uint32_t *)(h->bar0.p + a);
}

static void
h_wr32(void *u, uint32_t a, uint32_t v)
{
	struct hw *h = u;

	if (!h->go) {
		fprintf(stderr, "BUG: write 0x%06x = 0x%08x without --go\n", a, v);
		abort();
	}
	if ((uint64_t)a + 4 > h->bar0.len || (a & 3))
		return;
	h->writes++;
	*(volatile uint32_t *)(h->bar0.p + a) = v;
	__sync_synchronize();
}

static void
h_delay(void *u, uint32_t us)
{
	struct timespec ts = { us / 1000000u, (long)(us % 1000000u) * 1000L };

	(void)u;
	if (us < 50) { // spin: nanosleep granularity is too coarse
		struct timespec a, b;

		clock_gettime(CLOCK_MONOTONIC, &a);
		do
			clock_gettime(CLOCK_MONOTONIC, &b);
		while ((b.tv_sec - a.tv_sec) * 1000000000L + (b.tv_nsec - a.tv_nsec) <
		       (long)us * 1000L);
		return;
	}
	nanosleep(&ts, NULL);
}

static void
b1_wr(void *u, uint64_t off, uint32_t v)
{
	struct hw *h = u;

	if (!h->go)
		abort();
	if (off + 4 > h->bar1.len)
		return;
	*(volatile uint32_t *)(h->bar1.p + off) = v;
	__sync_synchronize();
}

static uint32_t
b1_rd(void *u, uint64_t off)
{
	struct hw *h = u;

	if (off + 4 > h->bar1.len)
		return 0xffffffffu;
	return *(volatile uint32_t *)(h->bar1.p + off);
}

static int
read_file(const char *path, char *buf, size_t cap)
{
	int fd = open(path, O_RDONLY);
	ssize_t n;

	if (fd < 0)
		return -errno;
	n = read(fd, buf, cap - 1);
	close(fd);
	if (n < 0)
		return -errno;
	buf[n] = 0;
	return (int)n;
}

static uint8_t *
slurp(const char *path, size_t *len)
{
	struct stat st;
	uint8_t *b;
	int fd = open(path, O_RDONLY);

	if (fd < 0)
		return NULL;
	if (fstat(fd, &st) || st.st_size <= 0 || !(b = malloc((size_t)st.st_size))) {
		close(fd);
		return NULL;
	}
	if (read(fd, b, (size_t)st.st_size) != st.st_size) {
		free(b);
		close(fd);
		return NULL;
	}
	close(fd);
	*len = (size_t)st.st_size;
	return b;
}

static int
map_bar(const char *dev, int idx, bool rw, struct bar *b)
{
	char path[512];
	struct stat st;
	int fd;
	void *p;

	snprintf(path, sizeof(path), "%s/resource%d", dev, idx);
	fd = open(path, rw ? O_RDWR : O_RDONLY);
	if (fd < 0)
		return -errno;
	if (fstat(fd, &st) || st.st_size <= 0) {
		close(fd);
		return -ENODEV;
	}
	p = mmap(NULL, (size_t)st.st_size, PROT_READ | (rw ? PROT_WRITE : 0),
		 MAP_SHARED, fd, 0);
	close(fd);
	if (p == MAP_FAILED)
		return -errno;
	b->p = p;
	b->len = (uint64_t)st.st_size;
	return 0;
}

static void
on_event(void *u, const struct pg106_devboot_event *e)
{
	(void)u;
	printf("boot %-14s %-5s ret=%d a=0x%llx b=0x%llx %s\n",
	       pg106_devboot_stage_name(e->stage), pg106_devboot_ev_name(e->ev),
	       e->ret, (unsigned long long)e->a, (unsigned long long)e->b,
	       e->detail ? e->detail : "");
	fflush(stdout);
}

static int
load_fw(const char *root, struct pg106_devboot_fw *fw, uint8_t **bufs)
{
	size_t i;
	int missing = 0;

	memset(fw, 0, sizeof(*fw));
	for (i = 0; i < pg106_devboot_fw_count; i++) {
		const struct pg106_fw_name *n = &pg106_devboot_fw_names[i];
		struct pg106_fw_blob *b =
			(struct pg106_fw_blob *)((uint8_t *)fw + n->offset);
		char path[1024];
		size_t len = 0;

		snprintf(path, sizeof(path), "%s/%s", root, n->path);
		bufs[i] = slurp(path, &len);
		if (!bufs[i]) {
			printf("fw: %s missing (stage %s will fail)\n", path,
			       pg106_devboot_stage_name(n->required_for));
			missing++;
			continue;
		}
		b->data = bufs[i];
		b->size = len;
	}
	return missing;
}

static int
top_rd(void *u, uint32_t a, uint32_t *v)
{
	*v = h_rd32(u, a);
	return 0;
}

static void
probe(struct hw *h)
{
	struct pg106_hw_bus bus = { h, h_rd32, h_wr32, h_delay, NULL, 0 };
	struct pg106_top_table t;
	uint32_t boot0 = h_rd32(h, 0);
	uint64_t vram = 0;
	size_t i;

	printf("probe (read-only): BOOT_0 0x%08x (%s)\n", boot0,
	       pg106_pmc_probe(boot0).is_gp106 ? "GP106" : "NOT a GP106");
	printf("posted: 0x2240c = 0x%08x (bit 1 %s)\n", h_rd32(h, 0x2240c),
	       (h_rd32(h, 0x2240c) & 2) ? "set" : "clear");
	printf("PMC_ENABLE 0x%08x  PMC_INTR_0 0x%08x\n", h_rd32(h, 0x200),
	       h_rd32(h, 0x100));
	if (pg106_fb_vram_size(&bus, &vram) == 0)
		printf("VRAM (FBP walk) %llu MiB; 0x100ce0 says %llu MiB\n",
		       (unsigned long long)(vram >> 20),
		       (unsigned long long)(pg106_fb_vidmem_size(&bus) >> 20));
	printf("BAR1 bind 0x1704 = 0x%08x, BAR2 bind 0x1714 = 0x%08x\n",
	       h_rd32(h, 0x1704), h_rd32(h, 0x1714));
	printf("SEC2 HWCFG 0x%08x, debug 0x%08x, CPUCTL 0x%08x, BINDINST 0x%08x\n",
	       h_rd32(h, 0x87108), h_rd32(h, 0x87408), h_rd32(h, 0x87100),
	       h_rd32(h, 0x87054));
	printf("FIFO: 0x2004 = 0x%x PBDMAs, 0x2008 = %u channels, 0x2254 = 0x%08x\n",
	       h_rd32(h, 0x2004) & 0xff, h_rd32(h, 0x2008), h_rd32(h, 0x2254));
	if (pg106_top_scan(top_rd, h, &t) == 0)
		for (i = 0; i < t.ndev; i++)
			printf("PTOP: type 0x%02x inst %d addr 0x%06x runlist %d "
			       "reset %d intr %d\n",
			       t.dev[i].type, t.dev[i].inst, t.dev[i].addr,
			       t.dev[i].runlist, t.dev[i].reset, t.dev[i].intr);
	printf("probe: %llu reads, %llu writes\n", (unsigned long long)h->reads,
	       (unsigned long long)h->writes);
}

static void
usage(void)
{
	fprintf(stderr,
		"usage: pg106_hwtest --probe [--dev BDF] [--sysfs DIR]\n"
		"       pg106_hwtest --go --fw DIR [--vbios FILE] [--stop-after N]\n"
		"                    [--timeout-ms N] [--dev BDF] [--sysfs DIR]\n");
}

int
main(int argc, char **argv)
{
	const char *bdf = "0000:01:00.0", *sysfs = "/sys/bus/pci/devices";
	const char *fwdir = NULL, *vbios_path = NULL;
	bool do_probe = false, go = false, any = false;
	unsigned stop_after = PG106_DB_READY, timeout_ms = 1000;
	char dev[512], path[600], buf[64];
	struct stat st;
	int i, ret;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--probe"))
			do_probe = true;
		else if (!strcmp(argv[i], "--go"))
			go = true;
		else if (!strcmp(argv[i], "--any-gp106"))
			any = true;
		else if (!strcmp(argv[i], "--dev") && i + 1 < argc)
			bdf = argv[++i];
		else if (!strcmp(argv[i], "--sysfs") && i + 1 < argc)
			sysfs = argv[++i];
		else if (!strcmp(argv[i], "--fw") && i + 1 < argc)
			fwdir = argv[++i];
		else if (!strcmp(argv[i], "--vbios") && i + 1 < argc)
			vbios_path = argv[++i];
		else if (!strcmp(argv[i], "--stop-after") && i + 1 < argc)
			stop_after = (unsigned)strtoul(argv[++i], NULL, 0);
		else if (!strcmp(argv[i], "--timeout-ms") && i + 1 < argc)
			timeout_ms = (unsigned)strtoul(argv[++i], NULL, 0);
		else {
			usage();
			return 64;
		}
	}
	if (do_probe == go || (go && !fwdir)) {
		usage();
		return 64;
	}
	snprintf(dev, sizeof(dev), "%s/%s", sysfs, bdf);
	// Identity from sysfs (no config-space access needed).
	snprintf(path, sizeof(path), "%s/vendor", dev);
	if (read_file(path, buf, sizeof(buf)) < 0 || strtoul(buf, NULL, 0) != 0x10de) {
		fprintf(stderr, "refusing: %s is not an NVIDIA function\n", dev);
		return 2;
	}
	snprintf(path, sizeof(path), "%s/device", dev);
	if (read_file(path, buf, sizeof(buf)) < 0) {
		fprintf(stderr, "refusing: no device id at %s\n", path);
		return 2;
	}
	{
		unsigned long id = strtoul(buf, NULL, 0);
		bool ok = id == 0x1c20 ||
			  (any && (id == 0x1c21 || id == 0x1c22 || id == 0x1c23 ||
				   id == 0x1c60 || id == 0x1c61 || id == 0x1c62));

		if (!ok) {
			fprintf(stderr, "refusing: device 0x%04lx is not the GTX 1060 "
				"Mobile (10de:1c20)\n", id);
			return 2;
		}
	}
	snprintf(path, sizeof(path), "%s/driver", dev);
	if (lstat(path, &st) == 0) {
		char tgt[512];
		ssize_t n = readlink(path, tgt, sizeof(tgt) - 1);

		tgt[n > 0 ? n : 0] = 0;
		fprintf(stderr, "refusing: a driver is bound (%s); unbind it first\n",
			tgt);
		return 2;
	}
	snprintf(path, sizeof(path), "%s/enable", dev);
	if (read_file(path, buf, sizeof(buf)) > 0 && buf[0] == '0') {
		if (!go) {
			fprintf(stderr, "refusing: the function's decode is off "
				"(%s = 0); --probe does not turn it on\n", path);
			return 2;
		}
		{
			int fd = open(path, O_WRONLY);

			if (fd < 0 || write(fd, "1", 1) != 1) {
				fprintf(stderr, "cannot enable %s\n", path);
				return 2;
			}
			close(fd);
			printf("enabled memory decode (%s = 1)\n", path);
		}
	}
	g_hw.go = go;
	ret = map_bar(dev, 0, go, &g_hw.bar0);
	if (ret) {
		fprintf(stderr, "resource0: %s\n", strerror(-ret));
		return 3;
	}
	{
		uint32_t boot0 = h_rd32(&g_hw, 0);

		if (!pg106_pmc_probe(boot0).is_gp106) {
			fprintf(stderr, "refusing: BOOT_0 0x%08x is not a GP106 "
				"(BAR0 not decoding, or D3cold)\n", boot0);
			return 2;
		}
	}
	if (do_probe) {
		probe(&g_hw);
		return 0;
	}
	ret = map_bar(dev, 1, true, &g_hw.bar1);
	if (ret) {
		fprintf(stderr, "resource1 (BAR1): %s\n", strerror(-ret));
		return 3;
	}
	if (map_bar(dev, 3, true, &g_hw.bar3))
		g_hw.bar3.len = 0; // BAR2 VMM skipped
	{
		struct pg106_hw_bus bus = { &g_hw, h_rd32, h_wr32, h_delay, NULL, 10 };
		struct pg106_devboot_fw fw;
		uint8_t *bufs[32] = { 0 };
		uint8_t *vbios = NULL;
		size_t vbios_len = 0;
		struct pg106_gpu_cfg cfg;
		struct pg106_gpu *g = calloc(1, sizeof(*g));
		struct pg106_gpu_selftest res;
		int missing = load_fw(fwdir, &fw, bufs);

		if (vbios_path && !(vbios = slurp(vbios_path, &vbios_len))) {
			fprintf(stderr, "cannot read %s\n", vbios_path);
			return 3;
		}
		printf("firmware: %zu files, %d missing; BAR1 %llu MiB, BAR3 %llu MiB\n",
		       pg106_devboot_fw_count, missing,
		       (unsigned long long)(g_hw.bar1.len >> 20),
		       (unsigned long long)(g_hw.bar3.len >> 20));
		memset(&cfg, 0, sizeof(cfg));
		cfg.bus = &bus;
		cfg.vbios = vbios;
		cfg.vbios_size = (uint32_t)vbios_len;
		cfg.fw = &fw;
		cfg.hostmem = NULL; // HS image in VRAM: no host DMA
		cfg.bar1_size = g_hw.bar1.len;
		cfg.bar2_size = g_hw.bar3.len;
		cfg.bar1_io = (struct pg106_bar1_io){ &g_hw, b1_wr, b1_rd };
		cfg.stop_after = (uint8_t)stop_after;
		cfg.event = on_event;
		cfg.sec2_max_polls = 1000;
		ret = pg106_gpu_boot(g, &cfg);
		printf("gpu_boot: ret %d, stage %s, VRAM %llu MiB, fifo %s\n", ret,
		       pg106_devboot_stage_name(g->stage),
		       (unsigned long long)(g->vram_size >> 20),
		       g->fifo_up ? "up" : "down");
		if (g->fifo_up) {
			int r2 = pg106_gpu_selftest(g, timeout_ms * 1000u, &res);

			printf("selftest: ret %d%s%s; runlist %d chid %u; host sem 0x%08x "
			       "(%llu us); CE sem 0x%08x (%llu us); copy %u bytes, %u "
			       "mismatched words; GP_GET %u; PFIFO_INTR 0x%08x; non-stall "
			       "events %llu\n",
			       r2, res.failed_at ? " at: " : "",
			       res.failed_at ? res.failed_at : "", res.runlist, res.chid,
			       res.sem_value, (unsigned long long)res.sem_wait_us,
			       res.ce_sem_value, (unsigned long long)res.ce_wait_us,
			       res.ce_bytes, res.ce_mismatch, res.gp_get, res.pfifo_intr,
			       (unsigned long long)res.nonstall_events);
			if (!ret)
				ret = r2;
		}
		pg106_gpu_fini(g);
		printf("done: %llu MMIO reads, %llu MMIO writes\n",
		       (unsigned long long)g_hw.reads, (unsigned long long)g_hw.writes);
		free(g);
		free(vbios);
		for (i = 0; i < 32; i++)
			free(bufs[i]);
	}
	return ret ? 1 : 0;
}
