# research/reference — pointers and test data only

Per project policy (third_party/README.md): upstream CODE is never copied
into this tree; it lives in git submodules. This directory now holds only:

- `edid/` — REAL EDID dumps used as offline test fixtures (data, not code;
  CC-BY-4.0 from the linuxhw/EDID project, provenance per file). The corpus
  differential test (`pascal/tests/test_edid_corpus.c`) loads these.

Canonical locations for the header files this directory used to cache:

| File (removed) | Canonical location |
| --- | --- |
| clc06f.h | submodule `third_party/open-gpu-kernel-modules`, `src/common/sdk/nvidia/inc/class/clc06f.h` |
| clc1c0.h | submodule `third_party/open-gpu-kernel-modules`, `src/common/sdk/nvidia/inc/class/clc1c0.h` |
| mesa_clc1c0qmd.h | submodule `third_party/mesa` (pinned), `src/nouveau/headers/nvidia/classes/clc1c0qmd.h` |
| hwref_gp100_dev_mmu.h | submodule `third_party/mesa` (pinned), `src/nouveau/headers/nvidia/hwref/pascal/gp100/dev_mmu.h` |

Mesa codegen fixtures for `pascal/tests/test_mesa_diff.c` are likewise no
longer committed: `pascal/scripts/gen_mesa_ref.sh` regenerates them into
`pascal/tests/mesa_ref/` (gitignored) from the submodule — the `make test`
target runs it automatically when the fixtures are missing.
