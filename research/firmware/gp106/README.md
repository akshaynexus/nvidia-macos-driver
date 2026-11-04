# gp106 firmware blobs

NVIDIA GP106 firmware files needed by the ACR secure-boot work
(Phase 0 blob parser, `pascal/core/src/acr.c`; Phase 2 simulation), vendored
into this repo like the GSP firmware already shipped here — these blobs are
freely redistributable under NVIDIA's redistribution license (see
`licenses/` and the linux-firmware `LICENCE.nvidia` terms: redistribution in
source and binary form is permitted as part of a Linux distribution or
driver package).

Origin: the `linux-firmware.git` project
(https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git),
fetched 2026-11-05 from the `nvidia/gp106` and `nvidia/gp102` directories of
git revision `703a4d95f86606b14b8433e7197663874ef89eed` (master at fetch
time). Plain-file URLs of the form:

    https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git/plain/nvidia/<dir>/<file>

## What is committed

### gr/ — GP106 graphics microcode signatures (real GP106 files)

These are exactly what nouveau requests for a GP106 via
`nvidia/gp106/gr/fecs_sig` / `nvidia/gp106/gr/gpccs_sig`
(`drivers/gpu/drm/nouveau/nvkm/subdev/acr/lsfw.c`,
`nvkm_acr_lsfw_load_sig_image_desc_v1`, fw path `gr/fecs_sig`).

| file           | size | md5                              |
|----------------|------|----------------------------------|
| gr/fecs_sig.bin | 192 | 440018c2d7c587d51c1c98fe500261d1 |
| gr/gpccs_sig.bin| 192 | d95b72d944b8863fe2f086b01cb270e6 |

192 bytes = `sizeof(struct lsf_signature_v1)`
(`drivers/gpu/drm/nouveau/include/nvfw/acr.h`); `fecs_sig.bin` carries
`falcon_id = 2` (FECS), `gpccs_sig.bin` carries `falcon_id = 3` (GPCCS),
both `version = 2`.

### acr/ — ACR HS (high-secure) falcon ucode — GP102 blobs, by necessity

**Substitution note:** nouveau wires `acr = gp102_acr_new` for all GP10x,
GP106 included (`nvkm/engine/device/base.c`), and
`nvkm/subdev/acr/gp102.c` declares `MODULE_FIRMWARE("nvidia/gp106/acr/...")`
— **but linux-firmware has never shipped a `nvidia/gp106/acr/` directory**
(as of the revision above, `nvidia/gp106/` contains only `gr/`). The
GP10x-generation ACR HS ucode that actually exists upstream is under
`nvidia/gp102/acr/`, so those files are committed here and used by the
parser tests. They implement the same r352 ACR wire format parsed by
`pascal/core/src/acr.c`; if real `gp106/acr` blobs surface (e.g. via
nouveau's signed-firmware packages), they can be dropped in here and the
tests will validate them unchanged.

| file                | size  | md5                              |
|---------------------|-------|----------------------------------|
| acr/bl.bin          | 1280  | 11c0dfbe40e431588d4c69a93c9d6b2a |
| acr/ucode_load.bin  | 17152 | b5e7fc1c08467e4082cb74f5f33ebabf |
| acr/ucode_unload.bin| 3328  | a93179166888a5d4ed84c534a0c1ba2d |
| acr/unload_bl.bin   | 1280  | b38a20ad7f88c9ab9d1ab7844f1cbbc3 |

Sources for these four:

    .../plain/nvidia/gp102/acr/bl.bin
    .../plain/nvidia/gp102/acr/ucode_load.bin
    .../plain/nvidia/gp102/acr/ucode_unload.bin
    .../plain/nvidia/gp102/acr/unload_bl.bin

## Consumers

- `pascal/tests/test_acr.c` — parses all of the above and asserts the
  structural invariants (container headers, HS load headers, signature
  patch sites, `lsf_signature_v1` fields).
- Phase 2 (ACR secure boot simulation) will feed the same blobs through the
  simulated falcon IMEM/DMEM load sequence.
