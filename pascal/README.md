# pascal/ — portable GP106 core (Phase 0)

A portable C11 core library for the NVIDIA GP106 (GeForce GTX 1060, Helios 300
target): the nvkm subset that matters for Pascal, extracted so it builds and
unit-tests on a plain Linux box with only libc. This is **Phase 0** of
`HANDOFF-PASCAL.md`: host-side foundation work that later gets wired into a
separate **GPL-2.0-only XNU kext** (Phase 3). Nothing here touches hardware;
all tests are pure CPU.

## License

Everything in this tree is **GPL-2.0-only**. Code derived from nouveau/nvkm
carries an `// SPDX-License-Identifier: GPL-2.0-only` header plus a provenance
comment naming the upstream file it derives from. GPL code must never leak
into the PolyForm-NC `kexts/`/`plugin/` trees (`HANDOFF-PASCAL.md` §3).

Upstream reference sources are fetched for reading into `/tmp/pascal-ref/`
(`/tmp/opencode/` was root-owned on this box); they are **not** committed.
Citations in comments point at the upstream file + function.

## Layout

    core/include/pascal/   public headers, one per subsystem
    core/src/              implementation, libc only
    tests/test_*.c         one standalone binary per test file

Current contents:

| File | What it covers |
| --- | --- |
| `core/include/pascal/pmc.h`, `core/src/pmc.c` | PMC_BOOT_0 decode: architecture/implementation/revision, GP10x family + GP106 detection, NV_04/NV_05 legacy path, validity checks. Derives from nouveau `nvkm/engine/device/base.c` (`nvkm_device_ctor`, `nvkm_device_endianness`). |
| `core/include/pascal/mmio.h` | BAR0 engine-block map (PMC 0x000000 … PDISP 0x610000) and address→block lookup. Addresses from the envytools MMIO map, as cross-recorded in `helios300-gp106-simtest.py` §2. |
| `tests/test_pmc.c` | PMC decode vectors (synthetic GP106 boot0 `0x136000a1`, wrong-architecture rejects, legacy NV_04/05) and MMIO range round-trips against the simtest constants. |

Other Phase 0 pieces (MMU v2 page tables, QMD builder, pushbuffer encoders,
ACR blob parser) land as additional `core/src/*.c` + `tests/test_*.c` files;
the Makefile picks them up via wildcards.

## Build and test

Requires gcc or clang, make, and python3 (the last only for `make test`,
which also runs the repo-wide regression simtest).

    make          # build every tests/test_*.c against core/
    make test     # build + run all tests + python3 ../helios300-gp106-simtest.py
    make coverage # gcov coverage of core/src, best-effort
    make clean

## Key facts (sources cited in-code)

- PMC_BOOT_0 lives at BAR0+0x000000; `chipset = (boot0 & 0x1ff00000) >> 20`
  when `(boot0 & 0x1f000000) > 0`; `chipset & 0x1f0 == 0x130` is the GP10x
  family (nvkm `card_type` `GP100`); **GP106 is chipset 0x136**; revision is
  `boot0 & 0xff`. Real silicon reports boot0 = `0x136000a1`.
  Sources: nouveau `nvkm/engine/device/base.c` (`nvkm_device_ctor`,
  `nv136_chipset`), Mesa `src/nouveau/drm-shim/nouveau_noop.c` (chip_id 0x130
  family → PASCAL_B / PASCAL_COMPUTE_B), nouveau ML report of a real GTX 1060
  boot0 (`136000a1`).
- Chips whose BOOT_0 does not match the modern layout fall back to the
  NV_04/NV_05 legacy pattern `(boot0 & 0xff00fff0) == 0x20004000` — the
  old-code "0xf masking" quirk; GP10x always decodes on the modern path.

Simulation/unit-test success only — no hardware claims (`HANDOFF-PASCAL.md` §7).
