#!/bin/sh
# SPDX-License-Identifier: MIT
# Regenerate smoke_comp_spv.h from smoke.comp (needs glslangValidator).
set -eu
cd "$(dirname "$0")"
glslangValidator -V --target-env vulkan1.1 -o smoke.spv smoke.comp
python3 - <<'PY'
import struct
d = open('smoke.spv', 'rb').read()
w = struct.unpack('<%dI' % (len(d) // 4), d)
out = ['/* SPDX-License-Identifier: MIT */', '/* Generated from smoke.comp by:',
       '   glslangValidator -V --target-env vulkan1.1 -o smoke.spv smoke.comp',
       '   (glslang 16, see README.md). Regenerate with gen_spv.sh. */',
       'static const uint32_t smoke_comp_spv[] = {']
for i in range(0, len(w), 6):
    out.append('   ' + ', '.join('0x%08x' % x for x in w[i:i + 6]) + ',')
out.append('};')
open('smoke_comp_spv.h', 'w').write('\n'.join(out) + '\n')
PY
rm -f smoke.spv
