# third_party/ — upstream sources as git submodules

Per project policy: upstream open-source code is referenced as pinned git
submodules, never copied into this tree. Every derived file in this repo cites
the submodule path + commit it was derived from.

| Submodule | Commit basis | Why it is here |
| --- | --- | --- |
| `open-gpu-kernel-modules` | master @ `ca2d03e6` (2026-11-05) | NVIDIA SDK class headers (clc06f.h, clc1c0.h, …) — primary authority for pushbuffer/GPFIFO encodings |
| `mesa` | `759935bea4bd` (the commit `pascal/tests/mesa_ref` fixtures were generated from) | Mesa/NVK: class codegen (`src/nouveau/headers/class_parser.py`), NVK submission shapes, hwref headers (`src/nouveau/headers/nvidia/hwref/pascal/gp100/`) |
| `tinygrad` | master @ `4e45845d` | tinygrad TinyGPU `ops_nv.py` — macOS PCIe transport pattern + pushbuffer shorthand our tests differentially compare against |
| `research/prior-art/pascal-egpu` | master @ `6dfeb2f8` | GP106 macOS bring-up attempt — failure decoder (0xbad* signatures), blob md5 cross-checks |
| `research/prior-art/meteor-lake-xe` | main @ `62de794b` | proof of the "Linux KMD ported to xnu + Mesa + NullMoth plugin" architecture; trap list |

## Deliberately NOT submoduled

- **linux-firmware**: multi-GB repository; only a handful of `nvidia/gp10x/*`
  files are relevant. Those are vendored minimally under
  `research/firmware/` with per-file provenance (URL + sha256), which the
  NVIDIA redistribution license permits and the handoff requires.

## Usage notes

- Cloning this repo needs `git clone --recurse-submodules`
  (or `git submodule update --init` after).
- `pascal/scripts/gen_mesa_ref.sh` regenerates the Mesa test fixtures from
  `third_party/mesa` when the submodule is initialized.
- Upstream audit references in `research/evidence/*.md` cite torvalds/linux
  master by path+line; the nouveau sources are NOT submoduled because the
  audit quotes whole-file context and the kernel tree is very large — the
  fetched copies live outside the repo (`/tmp/pascal-ref/`). If a future
  phase needs in-tree nouveau sources, add a `linux` submodule pinned to the
  audited tag.
