#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Regenerate nvk/pascal/nvk-pascal.patch.

The patch applies on top of Mesa 17ca6174 + nvk/nvk-macos.patch. It adds
src/nouveau/vulkan/nvkmd/pg106/ (copied from nvk/pascal/src/nvkmd/pg106/,
which is the canonical source) and makes four small edits to files
nvk-macos.patch (or Mesa) already owns:

  nvkmd/nvkmd.c         backend selection: NVRM (Turing+) then PG106 (Pascal)
  meson.build           build the pg106 backend, the bridge and its IOKit transport
  nvk_device_memory.c   the plugin's surface-dirty export is NVRM-only
  nvk_physical_device.c shaderFloat16 from SM 5.0 (NAK lowers f16 ALU to f32 below SM 7.0)

Usage: mkpatch.py <pristine-macos-tree> <scratch-dir> [--check]
  pristine-macos-tree  Mesa 17ca6174 with nvk-macos.patch applied (made by
                       prepare-mesa.sh --macos-only)
  --check              fail if nvk-pascal.patch differs from the result
"""
import filecmp
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
PASCAL = os.path.dirname(HERE)
SRC = os.path.join(PASCAL, "src", "nvkmd", "pg106")
PATCH = os.path.join(PASCAL, "nvk-pascal.patch")

BACKEND_FILES = [
    "nvkmd_pg106.h",
    "nvkmd_pg106_ctx.c",
    "nvkmd_pg106_dev.c",
    "nvkmd_pg106_mem.c",
    "nvkmd_pg106_pdev.c",
    "nvkmd_pg106_sync.c",
    "nvkmd_pg106_va.c",
]


def edit(path, old, new):
    with open(path) as f:
        text = f.read()
    if text.count(old) != 1:
        sys.exit(f"mkpatch: anchor not found exactly once in {path}:\n{old}")
    with open(path, "w") as f:
        f.write(text.replace(old, new))


def apply_edits(tree):
    vk = os.path.join(tree, "src", "nouveau", "vulkan")

    # 1. Backend selection.
    nvkmd_c = os.path.join(vk, "nvkmd", "nvkmd.c")
    edit(nvkmd_c,
         '#include "nvrm/nvkmd_nvrm.h"\n',
         '#include "pg106/nvkmd_pg106.h"\n'
         '#include "util/os_misc.h"\n'
         '\n'
         '#include <string.h>\n'
         '\n'
         '/* nvkmd_nvrm.h pulls in the open-gpu-kernel-modules headers; this file\n'
         ' * only needs the enumerator. */\n'
         'VkResult nvkmd_nvrm_enum_pdev(struct vk_object_base *log_obj,\n'
         '                              enum nvk_debug debug_flags,\n'
         '                              nvkmd_enum_pdev_visitor visitor,\n'
         '                              void *arg);\n')
    edit(nvkmd_c,
         '   return nvkmd_nvrm_enum_pdev(log_obj, debug_flags, visitor, arg);\n',
         '   /* Turing and newer run on NVRM (GSP); Pascal GP106 runs on the PG106\n'
         '    * kext through the pg106 bridge. A machine has at most one of the\n'
         '    * two kexts for a given GPU, so asking both is safe. NVK_KMD=nvrm or\n'
         '    * NVK_KMD=pg106 restricts the search to one backend. */\n'
         '   const char *only = os_get_option("NVK_KMD");\n'
         '   VkResult result = VK_SUCCESS;\n'
         '\n'
         '   if (only == NULL || strcmp(only, "nvrm") == 0)\n'
         '      result = nvkmd_nvrm_enum_pdev(log_obj, debug_flags, visitor, arg);\n'
         '\n'
         '   if (result == VK_SUCCESS && (only == NULL || strcmp(only, "pg106") == 0))\n'
         '      result = nvkmd_pg106_enum_pdev(log_obj, debug_flags, visitor, arg);\n'
         '\n'
         '   return result;\n')

    # 2. Build.
    meson = os.path.join(vk, "meson.build")
    edit(meson,
         'nvk_files += nvrm_files\n',
         'nvk_files += nvrm_files\n'
         '\n'
         '# Pascal GP106: nvkmd backend over the pg106 bridge (pascal/bridge in the\n'
         '# nvidia-macos-driver repo, MIT). nvkmd/pg106/bridge and nvkmd/pg106/core\n'
         '# are links to that repo\'s pascal/bridge and pascal/core, made by\n'
         '# nvk/pascal/scripts/prepare-mesa.sh; only MIT files are compiled from them.\n'
         'pg106_files = files(\n'
         "  'nvkmd/pg106/nvkmd_pg106.h',\n"
         "  'nvkmd/pg106/nvkmd_pg106_ctx.c',\n"
         "  'nvkmd/pg106/nvkmd_pg106_dev.c',\n"
         "  'nvkmd/pg106/nvkmd_pg106_mem.c',\n"
         "  'nvkmd/pg106/nvkmd_pg106_pdev.c',\n"
         "  'nvkmd/pg106/nvkmd_pg106_sync.c',\n"
         "  'nvkmd/pg106/nvkmd_pg106_va.c',\n"
         "  'nvkmd/pg106/bridge/src/bridge.c',\n"
         "  'nvkmd/pg106/core/src/uapi.c',\n"
         "  # IOKit user-client transport on macOS; stubs returning -ENOSYS elsewhere\n"
         "  'nvkmd/pg106/bridge/src/transport_iokit.c',\n"
         ')\n'
         'nvk_files += pg106_files\n')
    edit(meson,
         "  include_directories('nvkmd/nvrm/open-gpu-kernel-modules/src/nvidia/inc/kernel'),\n",
         "  include_directories('nvkmd/nvrm/open-gpu-kernel-modules/src/nvidia/inc/kernel'),\n"
         "  include_directories('nvkmd/pg106'),\n"
         "  include_directories('nvkmd/pg106/bridge/include'),\n"
         "  include_directories('nvkmd/pg106/core/include'),\n")

    # 3. The plugin's dirty-surface export talks NVRM escapes; on any other
    #    backend it reports "not supported" so the plugin keeps its copy path.
    devmem = os.path.join(vk, "nvk_device_memory.c")
    edit(devmem,
         '__attribute__((visibility("default"))) uint32_t\nnvmtl_nvk_surface_dirty(',
         'extern const struct nvkmd_dev_ops nvkmd_nvrm_dev_ops;\n'
         '\n'
         '__attribute__((visibility("default"))) uint32_t\nnvmtl_nvk_surface_dirty(')
    edit(devmem,
         '   if (dev == NULL || dev->nvkmd == NULL)\n'
         '      return 0x80000002u;\n'
         '   return nvkmd_nvrm_surface_dirty(dev->nvkmd, surfaceID, plane);\n',
         '   if (dev == NULL || dev->nvkmd == NULL)\n'
         '      return 0x80000002u;\n'
         '   /* Only NVRM has the surface-share escapes (NVAccel page-off). The\n'
         '    * pg106 backend renders for a display-less GPU, where the plugin\n'
         '    * copies frames out to the IOSurface (OFFLOAD-DESIGN.md section 2). */\n'
         '   if (dev->nvkmd->ops != &nvkmd_nvrm_dev_ops)\n'
         '      return 0x80000002u;\n'
         '   return nvkmd_nvrm_surface_dirty(dev->nvkmd, surfaceID, plane);\n')


    # 4. Metal's half arithmetic: SPIR-V Float16 needs shaderFloat16, which NVK
    #    gates on SM 7.0. Below that NAK already computes 16-bit float ALU at
    #    32 bits (nak_nir.c lower_bit_size_cb), so Pascal can offer it. Without
    #    it the plugin refuses every module that declares Float16 (164 of the
    #    244 iOS 26 QuartzCore/RenderBox entry points, plugin/tests).
    pdev_c = os.path.join(vk, "nvk_physical_device.c")
    edit(pdev_c,
         '      .shaderFloat16 = info->sm >= 70,\n',
         '      /* Below SM 7.0 NAK computes 16-bit float ALU at 32 bits\n'
         '       * (nak_nir.c lower_bit_size_cb), so Maxwell/Pascal can offer\n'
         '       * the capability Metal\'s half arithmetic translates to. */\n'
         '      .shaderFloat16 = info->sm >= 50,\n')


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    pristine = os.path.abspath(sys.argv[1])
    scratch = os.path.abspath(sys.argv[2])
    check = "--check" in sys.argv[3:]

    a = os.path.join(scratch, "a")
    b = os.path.join(scratch, "b")
    shutil.rmtree(scratch, ignore_errors=True)
    os.makedirs(scratch)
    rel = ["src/nouveau/vulkan/nvkmd/nvkmd.c",
           "src/nouveau/vulkan/meson.build",
           "src/nouveau/vulkan/nvk_device_memory.c",
           "src/nouveau/vulkan/nvk_physical_device.c"]
    for side in (a, b):
        for r in rel:
            os.makedirs(os.path.dirname(os.path.join(side, r)), exist_ok=True)
            shutil.copy2(os.path.join(pristine, r), os.path.join(side, r))
    apply_edits(b)
    dst = os.path.join(b, "src", "nouveau", "vulkan", "nvkmd", "pg106")
    os.makedirs(dst)
    for name in BACKEND_FILES:
        shutil.copy2(os.path.join(SRC, name), os.path.join(dst, name))

    out = subprocess.run(["diff", "-ruN", "a", "b"], cwd=scratch,
                         capture_output=True, text=True)
    if out.returncode not in (0, 1):
        sys.exit(out.stderr)
    # Drop the timestamps so regeneration is byte-stable.
    lines = []
    for line in out.stdout.splitlines(keepends=True):
        if line.startswith(("--- ", "+++ ")):
            line = line.split("\t", 1)[0].rstrip("\n") + "\n"
        lines.append(line)
    text = "".join(lines)

    if check:
        with open(PATCH) as f:
            if f.read() != text:
                sys.exit("mkpatch: nvk-pascal.patch is stale; rerun without --check")
        print("mkpatch: nvk-pascal.patch is up to date")
        return
    with open(PATCH, "w") as f:
        f.write(text)
    print(f"mkpatch: wrote {PATCH} ({len(lines)} lines)")


if __name__ == "__main__":
    main()
