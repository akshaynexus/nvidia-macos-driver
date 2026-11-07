# gp106 firmware blobs

NVIDIA GP106 firmware files used by the ACR secure-boot model
(`pascal/core/src/acr.c` parsers, `pascal/core/src/acr_boot.c` boot plan and
SEC2 queue code) and their tests. They are redistributable under NVIDIA's
firmware licence, shipped with them as `licenses/NVIDIA-firmware-LICENCE.txt`
at the repo root (byte-identical to linux-firmware's `LICENSES/LICENCE.nvidia`;
WHENCE: "Licence: Redistributable. See LICENCE.nvidia for details"). The
files are unmodified.

## Origin

All files come from linux-firmware
(https://gitlab.com/kernel-firmware/linux-firmware.git). The `acr/` and
`gr/*_sig.bin` files were fetched 2026-11-05 at revision
`703a4d95f86606b14b8433e7197663874ef89eed`; the `sec2/` files and
`gr/*_{bl,inst,data}.bin` were added 2026-11-07 from revision
`6d6bffb1d85a1fe077f26853edce482f6c112f2b`. The `acr/` and `gr/*_sig.bin`
files are byte-identical at both revisions.

## What linux-firmware ships for GP106

The git tree has a real `nvidia/gp106/` directory with only four files
(`gr/{fecs,gpccs}_{data,sig}.bin`). Everything else nouveau requests under
`nvidia/gp106/` is a **symlink declared in WHENCE** and created by
linux-firmware's install script (`copy-firmware.sh`), so on an installed
system `nvidia/gp106/acr/`, `nvidia/gp106/sec2/` and the rest of
`nvidia/gp106/gr/` exist and resolve to the GP102/GM200 files below.
The earlier version of this README said linux-firmware "has never shipped a
`nvidia/gp106/acr/` directory" and called the GP102 ACR files a
substitution; that was wrong. They are what nouveau loads on GP106.

WHENCE links (revision `6d6bffb1`, lines 6029-6051):

| nouveau path (`nvidia/gp106/...`) | target |
|---|---|
| `acr/{bl,ucode_load,unload_bl,ucode_unload}.bin` | `../../gp102/acr/*` |
| `gr/fecs_bl.bin`, `gr/gpccs_bl.bin` | `../../gm200/gr/*` |
| `gr/fecs_inst.bin`, `gr/gpccs_inst.bin` | `../../gp102/gr/*` |
| `gr/sw_{bundle_init,ctx,method_init,nonctx}.bin` | `../../gp102/gr/*` |
| `sec2/{desc,image,sig}.bin`, `sec2/{desc,image,sig}-1.bin` | `../../gp102/sec2/*` |
| `nvdec/scrubber.bin` | `../../gp102/nvdec/scrubber.bin` |

`gr/fecs_data.bin` and `gr/gpccs_data.bin` are GP106-specific files (they
differ from the GP102 ones); `gr/fecs_sig.bin` / `gr/gpccs_sig.bin` too.

## Which SEC2 version nouveau uses

`gp102_sec2_fwif` (`nvkm/engine/sec2/gp102.c:305-311`) lists version 1
(`sec2/desc-1.bin`, `image-1.bin`, `sig-1.bin`, bootloader block
`flcn_bl_dmem_desc_v2`) before version 0 (`desc.bin`, ...,
`loader_config_v1`), and `nvkm_firmware_load`
(`include/nvkm/core/firmware.h:46-71`) takes the first entry that loads.
With linux-firmware installed, version 1 is the one used, so only the `-1`
files are vendored. The version-0 files exist upstream as well.

## What is committed

| file | size | md5 | linux-firmware source |
|---|---|---|---|
| acr/bl.bin | 1280 | 11c0dfbe40e431588d4c69a93c9d6b2a | nvidia/gp102/acr/bl.bin |
| acr/ucode_load.bin | 17152 | b5e7fc1c08467e4082cb74f5f33ebabf | nvidia/gp102/acr/ucode_load.bin |
| acr/ucode_unload.bin | 3328 | a93179166888a5d4ed84c534a0c1ba2d | nvidia/gp102/acr/ucode_unload.bin |
| acr/unload_bl.bin | 1280 | b38a20ad7f88c9ab9d1ab7844f1cbbc3 | nvidia/gp102/acr/unload_bl.bin |
| gr/fecs_bl.bin | 576 | 2218d5ef13ae6f4ff1b69169325b6e79 | nvidia/gm200/gr/fecs_bl.bin |
| gr/fecs_inst.bin | 20927 | 4e1d689701cae51261dd12cb2a8edee8 | nvidia/gp102/gr/fecs_inst.bin |
| gr/fecs_data.bin | 2256 | 6d917b56fb651e7ba7475e7a29675966 | nvidia/gp106/gr/fecs_data.bin |
| gr/fecs_sig.bin | 192 | 440018c2d7c587d51c1c98fe500261d1 | nvidia/gp106/gr/fecs_sig.bin |
| gr/gpccs_bl.bin | 576 | 66b91964bd2d7875b971790a505eea0a | nvidia/gm200/gr/gpccs_bl.bin |
| gr/gpccs_inst.bin | 13307 | 6d537b66cbd4106b2403a87274b4f164 | nvidia/gp102/gr/gpccs_inst.bin |
| gr/gpccs_data.bin | 1832 | d4cd5ec2ee9c1ddb8c92b0042a30819b | nvidia/gp106/gr/gpccs_data.bin |
| gr/gpccs_sig.bin | 192 | d95b72d944b8863fe2f086b01cb270e6 | nvidia/gp106/gr/gpccs_sig.bin |
| sec2/desc-1.bin | 656 | 62e5aed56532cfec3ddaf59d8784bca2 | nvidia/gp102/sec2/desc-1.bin |
| sec2/image-1.bin | 109568 | c42dfbca7dee819f014524c7db8de6b3 | nvidia/gp102/sec2/image-1.bin |
| sec2/sig-1.bin | 192 | 5a2a965c7a565ec252c765707a5cd92f | nvidia/gp102/sec2/sig-1.bin |
| gr/sw_bundle_init.bin | 7680 | e6b76152d93a6c1b8c332da8ff58ac10 | nvidia/gp102/gr/sw_bundle_init.bin (gp106 path is a WHENCE link) |
| gr/sw_ctx.bin | 6216 | 9ad5e54fa9db3569c367bff236c1d37e | nvidia/gp102/gr/sw_ctx.bin (gp106 path is a WHENCE link) |
| gr/sw_method_init.bin | 12288 | 04850386fd457e43b4177463cb64bd8c | nvidia/gp102/gr/sw_method_init.bin (gp106 path is a WHENCE link) |
| gr/sw_nonctx.bin | 2496 | cf136410777f59b0ae8fb1b12f1e8f56 | nvidia/gp102/gr/sw_nonctx.bin (gp106 path is a WHENCE link) |

The four `gr/sw_*` bundles (added 2026-11-07, linux-firmware 6d6bffb) are the
GR init lists `gm200_gr_load` reads (`nvkm/engine/gr/gm200.c`); the kext
requests `sw_nonctx` today (`pascal/core/src/devboot.c`).

Files are stored under the GP106 names nouveau requests
(`nvkm_firmware_get` builds `nvidia/<chip>/<name>[-<ver>].bin`,
`nvkm/core/firmware.c:72-102`).

Facts read out of these files by the tests:

- `fecs_sig.bin` / `gpccs_sig.bin` / `sec2/sig-1.bin` are 192-byte
  `lsf_signature_v1` with `falcon_id` 2 (FECS), 3 (GPCCS), 7 (SEC2).
- `sec2/desc-1.bin` is an `nvfw_ls_desc_v1`: bootloader 0x192 bytes at IMEM
  0xfd00, app at image offset 0x200 (0x1aa00 bytes), entry 0x500, resident
  code 0x500/0x21b0, resident data 0x5300/0x5474. The bootloader sitting at
  0xfd00 fits a 64 KB IMEM, matching the 64 KB measured on GP106 SEC2
  (`research/prior-art/pascal-egpu/FINDINGS.md:160`).
- `fecs_bl.bin` / `gpccs_bl.bin` use the `0x3b1d14f0` container; their
  `nvfw_bl_desc.start_tag` is 0x5c / 0x34 (bootloader IMEM 0x5c00 / 0x3400).

## Consumers

- `pascal/tests/test_acr.c` — parser invariants, truncation/mutation fuzz.
- `pascal/tests/test_acr_boot.c` — WPR build from the real LS images, HS
  boot plan, SEC2 init message and `BOOTSTRAP_FALCON` against a mock
  (simulation only).
- `pascal/tests/test_bringup.c` — the ACR stage of the end-to-end model.
