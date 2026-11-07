// SPDX-License-Identifier: GPL-2.0-only
// pascal/core/src/vmm.c — VMMs with page tables in VRAM, TLB invalidate,
// instance-block join, BAR1/BAR2. See vmm.h for the nouveau sources.

#include <pascal/vmm.h>

#include <errno.h>
#include <string.h>

static int
tables_write(void *user, uint64_t addr, const void *data, uint32_t len)
{
	struct pg106_vmm *vmm = user;

	if (addr < vmm->table_base ||
	    addr + len > vmm->table_base + vmm->table_size)
		return -EFAULT;
	return pg106_vram_write(vmm->vram, addr, data, len);
}

int
pg106_vmm_init(struct pg106_vmm *vmm, struct pg106_vram *vram,
	       uint64_t limit, uint64_t table_bytes, bool hub_only)
{
	struct pg106_mmu_cfg cfg;
	int ret;

	if (!vmm || !vram || !limit || limit > PG106_MMU_VA_LIMIT)
		return -EINVAL;
	memset(vmm, 0, sizeof(*vmm));
	if (!table_bytes)
		table_bytes = PG106_VMM_TABLES_DEFAULT;
	table_bytes = (table_bytes + 0xfff) & ~0xfffull;
	ret = pg106_vram_alloc(vram, table_bytes, 0x1000, PG106_VRAM_TAIL,
			       &vmm->table_base);
	if (ret)
		return ret;
	vmm->vram = vram;
	vmm->table_size = table_bytes;
	vmm->limit = limit;
	vmm->hub_only = hub_only;
	memset(&cfg, 0, sizeof(cfg));
	cfg.table_target = PG106_TARGET_VRAM;
	cfg.table_base = vmm->table_base;
	cfg.table_limit = vmm->table_base + table_bytes;
	ret = pg106_mmu_init(&vmm->mmu, &cfg);
	if (ret) {
		pg106_vram_free(vram, vmm->table_base, table_bytes);
		memset(vmm, 0, sizeof(*vmm));
		return ret;
	}
	// The root page directory exists from the start (nvkm_vmm_ctor
	// allocates vmm->pd); write it now so a join never points at
	// uninitialised VRAM.
	ret = pg106_vmm_sync(vmm);
	if (ret) {
		pg106_vmm_fini(vmm);
		return ret;
	}
	return 0;
}

void
pg106_vmm_fini(struct pg106_vmm *vmm)
{
	if (!vmm || !vmm->vram)
		return;
	pg106_mmu_destroy(vmm->mmu);
	pg106_vram_free(vmm->vram, vmm->table_base, vmm->table_size);
	memset(vmm, 0, sizeof(*vmm));
}

int
pg106_vmm_map(struct pg106_vmm *vmm, const struct pg106_map *m)
{
	if (!vmm || !vmm->mmu || !m || m->va + m->size > vmm->limit ||
	    m->va + m->size < m->va)
		return -EINVAL;
	return pg106_mmu_map(vmm->mmu, m);
}

int
pg106_vmm_unmap(struct pg106_vmm *vmm, uint64_t va, uint64_t size,
		unsigned page_shift)
{
	if (!vmm || !vmm->mmu)
		return -EINVAL;
	return pg106_mmu_unmap(vmm->mmu, va, size, page_shift);
}

int
pg106_vmm_sync(struct pg106_vmm *vmm)
{
	int ret;

	if (!vmm || !vmm->mmu)
		return -EINVAL;
	ret = pg106_mmu_sync(vmm->mmu, tables_write, vmm, &vmm->last_sync);
	vmm->syncs++;
	return ret;
}

uint64_t
pg106_vmm_pdb_join(const struct pg106_vmm *vmm)
{
	// gp100_vmm_join base bits, gf100_vmm_join_ target VRAM (0) | pd addr.
	return PG106_PDB_VER2 | PG106_PDB_BIG_64K |
	       pg106_mmu_root_addr(vmm->mmu);
}

uint32_t
pg106_vmm_pdb_invalidate(const struct pg106_vmm *vmm)
{
	// gf100_vmm_invalidate: addr |= target (VRAM 0); addr |= (pd >> 12) << 4.
	return (uint32_t)((pg106_mmu_root_addr(vmm->mmu) >> 12) << 4);
}

static int
poll_set(const struct pg106_hw_bus *b, uint32_t addr, uint32_t mask,
	 uint32_t budget_us)
{
	return pg106_hw_poll(b, addr, mask, 0, true, budget_us, NULL, NULL);
}

int
pg106_vmm_flush(struct pg106_vmm *vmm)
{
	const struct pg106_hw_bus *b;
	uint32_t type = PG106_MMU_INV_PAGE_ALL;
	uint64_t pdb;
	int ret;

	if (!vmm || !vmm->mmu)
		return -EINVAL;
	b = vmm->vram->bus;
	if (vmm->hub_only)
		type |= PG106_MMU_INV_HUB_ONLY; // gp100_vmm_flush (engref BAR)
	// vmmgf100.c:197-200: free flush slots.
	ret = poll_set(b, PG106_MMU_CTRL, 0x00ff0000u, 2000000);
	if (ret)
		return ret;
	// vmmgf100.c:202-214 -> gp100_vmm_invalidate_pdb (vmmgp100.c:564-570).
	pdb = pg106_vmm_pdb_invalidate(vmm);
	b->wr32(b->user, PG106_MMU_INVALIDATE_PDB_LO, (uint32_t)pdb);
	b->wr32(b->user, PG106_MMU_INVALIDATE_PDB_HI, (uint32_t)(pdb >> 32));
	// vmmgf100.c:216.
	b->wr32(b->user, PG106_MMU_INVALIDATE, 0x80000000u | type);
	// vmmgf100.c:218-222: flush queued.
	ret = poll_set(b, PG106_MMU_CTRL, 0x00008000u, 2000000);
	vmm->flushes++;
	return ret;
}

int
pg106_vmm_commit(struct pg106_vmm *vmm)
{
	int ret = pg106_vmm_sync(vmm);

	if (ret || !vmm->last_sync.writes)
		return ret;
	return pg106_vmm_flush(vmm);
}

int
pg106_vmm_join(struct pg106_vmm *vmm, uint64_t inst_pa)
{
	int ret;

	if (!vmm || !vmm->mmu || inst_pa & 0xfff)
		return -EINVAL;
	ret = pg106_vram_wr64(vmm->vram, inst_pa + PG106_INST_PDB,
			      pg106_vmm_pdb_join(vmm));
	if (ret)
		return ret;
	return pg106_vram_wr64(vmm->vram, inst_pa + PG106_INST_LIMIT,
			       vmm->limit - 1);
}

int
pg106_vmm_part(struct pg106_vmm *vmm, uint64_t inst_pa)
{
	if (!vmm || !vmm->vram)
		return -EINVAL;
	return pg106_vram_wr64(vmm->vram, inst_pa + PG106_INST_PDB, 0);
}

// ---- BARs ------------------------------------------------------------------------

int
pg106_bar_flush(const struct pg106_hw_bus *b)
{
	// g84_bar_flush (bar/g84.c:34-38).
	b->wr32(b->user, PG106_BAR_FLUSH, 0x00000001u);
	return pg106_hw_poll(b, PG106_BAR_FLUSH, 0x00000002u, 0, false,
			     2000000, NULL, NULL);
}

static int
bar_wait(const struct pg106_bar *bar)
{
	const struct pg106_hw_bus *b = bar->vmm.vram->bus;
	// gm107_bar_bar1_wait / _bar2_wait (bar/gm107.c:26-44).
	uint32_t mask = bar->id == PG106_BAR1 ? 0x3u : 0xcu;

	return pg106_hw_poll(b, PG106_BAR_STATUS, mask, 0, false, 2000000,
			     NULL, NULL);
}

int
pg106_bar_bind(struct pg106_bar *bar)
{
	const struct pg106_hw_bus *b;
	uint32_t reg;
	int ret;

	if (!bar || !bar->vmm.vram)
		return -EINVAL;
	b = bar->vmm.vram->bus;
	reg = bar->id == PG106_BAR1 ? PG106_BAR1_BLOCK : PG106_BAR2_BLOCK;
	// gf100_bar_bar1_init :57 / gf100_bar_bar2_init :80 (no NvBar2Halve).
	b->wr32(b->user, reg,
		PG106_BAR_BIND_VALID | (uint32_t)(bar->inst >> 12));
	// nvkm_bar_bar2_init / nvkm_bar_init call func->barN.wait after init.
	ret = bar_wait(bar);
	if (!ret)
		bar->bound = true;
	return ret;
}

void
pg106_bar_unbind(struct pg106_bar *bar)
{
	const struct pg106_hw_bus *b;
	uint32_t reg, v;

	if (!bar || !bar->vmm.vram || !bar->bound)
		return;
	b = bar->vmm.vram->bus;
	reg = bar->id == PG106_BAR1 ? PG106_BAR1_BLOCK : PG106_BAR2_BLOCK;
	v = b->rd32(b->user, reg);
	b->wr32(b->user, reg, v & ~PG106_BAR_BIND_VALID);
	bar->bound = false;
}

int
pg106_bar_init(struct pg106_bar *bar, struct pg106_vram *vram, uint8_t id,
	       uint64_t size)
{
	int ret;

	if (!bar || !vram || (id != PG106_BAR1 && id != PG106_BAR2) ||
	    !size || size & 0xfff)
		return -EINVAL;
	memset(bar, 0, sizeof(*bar));
	bar->id = id;
	bar->size = size;
	// gf100_bar_oneinit_bar :91-92: INST, 0x1000.
	ret = pg106_vram_alloc(vram, PG106_INST_SIZE, 0x1000,
			       PG106_VRAM_TAIL | PG106_VRAM_ZERO, &bar->inst);
	if (ret)
		return ret;
	// :102: nvkm_vmm_new(device, 0, bar_len, ...).
	ret = pg106_vmm_init(&bar->vmm, vram, size, 0, true);
	if (ret)
		goto fail_inst;
	ret = pg106_range_init(&bar->va, 0, size, 0x1000);
	if (ret)
		goto fail_vmm;
	ret = pg106_vmm_join(&bar->vmm, bar->inst); // :119
	if (ret)
		goto fail_va;
	ret = pg106_bar_bind(bar);
	if (ret)
		goto fail_va;
	return 0;
fail_va:
	pg106_range_fini(&bar->va);
fail_vmm:
	pg106_vmm_fini(&bar->vmm);
fail_inst:
	pg106_vram_free(vram, bar->inst, PG106_INST_SIZE);
	memset(bar, 0, sizeof(*bar));
	return ret;
}

int
pg106_bar_restore(struct pg106_bar *bar)
{
	int ret;

	if (!bar || !bar->vmm.mmu)
		return -EINVAL;
	pg106_vram_write(bar->vmm.vram, bar->inst, NULL, PG106_INST_SIZE);
	pg106_mmu_mark_all_dirty(bar->vmm.mmu);
	ret = pg106_vmm_sync(&bar->vmm);
	if (!ret)
		ret = pg106_vmm_join(&bar->vmm, bar->inst);
	if (!ret)
		ret = pg106_bar_bind(bar);
	if (!ret)
		ret = pg106_vmm_flush(&bar->vmm);
	return ret;
}

void
pg106_bar_fini(struct pg106_bar *bar)
{
	struct pg106_vram *vram;

	if (!bar || !bar->vmm.vram)
		return;
	vram = bar->vmm.vram;
	pg106_bar_unbind(bar);
	pg106_range_fini(&bar->va);
	pg106_vmm_fini(&bar->vmm);
	pg106_vram_free(vram, bar->inst, PG106_INST_SIZE);
	memset(bar, 0, sizeof(*bar));
}

int
pg106_bar_map(struct pg106_bar *bar, uint64_t pa, uint64_t size,
	      enum pg106_target target, uint64_t *off)
{
	struct pg106_map m;
	uint64_t va;
	int ret;

	if (!bar || !off || !size || pa & 0xfff)
		return -EINVAL;
	size = (size + 0xfff) & ~0xfffull;
	ret = pg106_range_alloc(&bar->va, size, 0x1000, false, &va);
	if (ret)
		return ret;
	memset(&m, 0, sizeof(m));
	m.va = va;
	m.pa = pa;
	m.size = size;
	m.page_shift = PG106_PAGE_4K_SHIFT;
	m.target = target;
	m.kind = PG106_KIND_PITCH;
	m.vol = target == PG106_TARGET_SYS_COHERENT;
	ret = pg106_vmm_map(&bar->vmm, &m);
	if (!ret)
		ret = pg106_vmm_commit(&bar->vmm);
	if (ret) {
		pg106_vmm_unmap(&bar->vmm, va, size, PG106_PAGE_4K_SHIFT);
		pg106_vmm_sync(&bar->vmm);
		pg106_range_free(&bar->va, va, size);
		return ret;
	}
	*off = va;
	return 0;
}

int
pg106_bar_unmap(struct pg106_bar *bar, uint64_t off, uint64_t size)
{
	int ret;

	if (!bar || !size)
		return -EINVAL;
	size = (size + 0xfff) & ~0xfffull;
	if (!pg106_range_is_allocated(&bar->va, off, size))
		return -EINVAL;
	ret = pg106_vmm_unmap(&bar->vmm, off, size, PG106_PAGE_4K_SHIFT);
	if (!ret)
		ret = pg106_vmm_commit(&bar->vmm);
	pg106_range_free(&bar->va, off, size);
	return ret;
}
