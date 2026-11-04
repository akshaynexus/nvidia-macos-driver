#!/bin/sh
# gen_mesa_ref.sh - reproduce pascal/tests/mesa_ref/ from a Mesa checkout.
#
# pascal/tests/mesa_ref/ holds the artifacts Mesa's header generators
# (src/nouveau/headers/class_parser.py + struct_parser.py) produce from the
# NVIDIA class headers, so test_mesa_diff.c can differential-test our
# pg106_* encoders against the code NVK actually ships. Everything in
# mesa_ref/ is Mesa/NVIDIA MIT-licensed; provenance is mesa_commit.txt and
# README.md.
#
# Usage:
#   ./scripts/gen_mesa_ref.sh                     # download from gitlab at $MESA_COMMIT
#   MESA_SRC=/path/to/mesa ./scripts/gen_mesa_ref.sh   # use a local checkout
#   MESA_COMMIT=<sha> ./scripts/gen_mesa_ref.sh   # override pinned commit
#
# The script is idempotent: it re-downloads/re-copies the inputs, re-runs
# the generators, and rewrites the generated files in place. The two
# hand-written support files (README.md, util/u_math.h shim) are only
# created when missing.
set -e

MESACOMMIT=${MESA_COMMIT:-759935bea4bd12c04456d954294890d3cbd630d3}
SRC=${MESA_SRC:-}
REF=$(dirname "$0")/../tests/mesa_ref

# Class chains needed to link nv_push_clc{06f,1c0}.c: Mesa's meson.build
# gives each class the previous class with the same subchannel suffix (last
# two chars) in nv_classes order as --prev-in-h, and the generated .c dump
# code tail-calls P_PARSE_<prev>_MTHD, so the whole chain must exist.
#   06f: cl906f <- cla06f <- cla16f <- clb06f <- clc06f
#   1c0: cl90c0 <- cl91c0 <- cla1c0 <- clb0c0 <- clb1c0 <- clc1c0
CHAIN_06F='cl906f cla06f cla16f clb06f clc06f'
CHAIN_1C0='cl90c0 cl91c0 cla1c0 clb0c0 clb1c0 clc1c0'
HDRS=$(for c in $CHAIN_06F $CHAIN_1C0; do echo "nvidia/classes/$c.h"; done)' nvidia/classes/clc1c0qmd.h'
SCRIPTS='class_parser.py struct_parser.py util.py'
# Hand-written mesa files the generated headers include or use.
MISC='nvtypes.h drf.h nv_push.h'

mkdir -p "$REF/nvidia/classes" "$REF/util"

fetch() { # fetch <rel-path-under-src/nouveau/headers>
    out="$REF/$1"
    mkdir -p "$(dirname "$out")"
    if [ -n "$SRC" ]; then
        cp "$SRC/src/nouveau/headers/$1" "$out"
    else
        url="https://gitlab.freedesktop.org/mesa/mesa/-/raw/$MESACOMMIT/src/nouveau/headers/$1"
        curl -fsSL -o "$out" "$url"
    fi
}

for f in $SCRIPTS $MISC $HDRS; do fetch "$f"; done
# src/util/macros.h (UNUSED/ATTRIBUTE_COLD/MIN2/BITFIELD_BIT, needed by the
# generated headers and nv_push.h), fetched from the same commit.
if [ -n "$SRC" ]; then
    cp "$SRC/src/util/macros.h" "$REF/util/macros.h"
else
    curl -fsSL -o "$REF/util/macros.h" \
        "https://gitlab.freedesktop.org/mesa/mesa/-/raw/$MESACOMMIT/src/util/macros.h"
fi

# The generated headers #include the NVIDIA class headers by bare basename
# ("#include "clc06f.h""), which resolves next to them. Top-level copies
# are byte-identical to nvidia/classes/; kept in sync here on purpose.
for cl in $CHAIN_06F $CHAIN_1C0; do
    cp "$REF/nvidia/classes/$cl.h" "$REF/$cl.h"
done
cp "$REF/nvidia/classes/clc1c0qmd.h" "$REF/clc1c0qmd.h"

printf '%s\n' "$MESACOMMIT" > "$REF/mesa_commit.txt"

# 1) Pushbuffer emit helpers, exactly as meson.build builds them:
#    class N uses the previous class with the same subchannel-suffix as
#    --prev-in-h (see CHAIN_* above) so the dump code's P_PARSE tail-call
#    chain links. clc06f/clc1c0 are what the test consumes; the rest of
#    the chain exists to satisfy that linkage.
prev_of() { # prev_of <class> <chain...> -> class before <class> in chain
    prev=; for c in $2 $3 $4 $5 $6 $7 $8; do
        [ "$c" = "$1" ] && break
        prev=$c
    done
    [ -n "$prev" ] && echo "nvidia/classes/$prev.h"
}
for chain in "$CHAIN_06F" "$CHAIN_1C0"; do
    for cl in $chain; do
        python3 "$REF/class_parser.py" \
            --in-h "$REF/nvidia/classes/$cl.h" \
            $(prev_args=$(prev_of "$cl" $chain) && [ -n "$prev_args" ] && \
              echo "--prev-in-h $REF/$prev_args") \
            --out-h "$REF/nv_push_$cl.h" --out-c "$REF/nv_push_$cl.c"
    done
done

# 2) QMD: struct_parser.py only emits Rust bit-range constants (NVK fills
#    QMDs from Rust); there is no C QMD packer anywhere in Mesa. We commit
#    the generated .rs verbatim and additionally derive a C view of the
#    same ranges (nv_qmd_bitrange_c1c0.h) so the C test can use them.
python3 "$REF/struct_parser.py" \
    --in-h "$REF/nvidia/classes/clc1c0qmd.h" \
    --out-rs "$REF/nvh_classes_clc1c0_qmd.rs"

python3 - "$REF/nvh_classes_clc1c0_qmd.rs" > "$REF/nv_qmd_bitrange_c1c0.h" <<'PYEOF'
import re, sys

# Mechanical translation of Mesa's generated nvh_classes_clc1c0_qmd.rs
# into C preprocessor macros. One line of Rust in, two macros out:
#   pub const F: Range<usize> = lo..hi;        -> F_LO lo / F_HI hi-1
#   pub const fn F(i) -> ... (i*S+lo)..(i*S+hi)-> F_LO(i)/F_HI(i), hi-1
#   pub const F_ENUM: u32 = v;                 -> F_ENUM v
# Field positions are unchanged; only the notation differs. The only
# hand-written part is this translator and the file banner.
out = [
    "/*",
    " * nv_qmd_bitrange_c1c0.h - C view of Mesa's generated QMD bit ranges.",
    " *",
    " * Mechanically derived from nvh_classes_clc1c0_qmd.rs (Mesa",
    " * struct_parser.py output, see mesa_commit.txt for the commit) by",
    " * scripts/gen_mesa_ref.sh. Do not edit; regenerate instead.",
    " */",
    "#ifndef NV_QMD_BITRANGE_C1C0_H",
    "#define NV_QMD_BITRANGE_C1C0_H",
    "",
]
fn_re = re.compile(
    r"pub const fn (\w+)\(i: usize\) -> Range<usize> \{\s*"
    r"\(i(?: \* (\d+))? \+ (\d+)\)\.\.\(i(?: \* \d+)? \+ (\d+)\)\s*\}")
range_re = re.compile(r"pub const (\w+): Range<usize> = (\d+)\.\.(\d+);")
max_re = re.compile(r"pub const (\w+): usize = (\d+);")
enum_re = re.compile(r"pub const (\w+): u32 = (0x[0-9a-fA-F]+);")

text = open(sys.argv[1], encoding="utf-8").read()
pos = 0
for m in fn_re.finditer(text):
    name, stride, lo, hi_excl = m.group(1), m.group(2) or "1", m.group(3), m.group(4)
    out.append(f"#define {name}_LO(i) ((i) * {stride} + {lo})")
    out.append(f"#define {name}_HI(i) ((i) * {stride} + {int(hi_excl) - 1})")
    out.append("")
for m in range_re.finditer(text):
    name, lo, hi_excl = m.group(1), m.group(2), int(m.group(3)) - 1
    out.append(f"#define {name}_LO {lo}")
    out.append(f"#define {name}_HI {hi_excl}")
    out.append("")
for m in max_re.finditer(text):
    out.append(f"#define {m.group(1)} {m.group(2)}")
for m in enum_re.finditer(text):
    out.append(f"#define {m.group(1)} {m.group(2)}")
out.append("")
out.append("#endif /* NV_QMD_BITRANGE_C1C0_H */")
print("\n".join(out))
PYEOF

# 3) Out-of-tree compile support: the generated .c files include
#    "util/u_math.h" but only reference uif() from it. Install the tiny
#    shim if absent (it is committed, so normally nothing happens).
if [ ! -e "$REF/util/u_math.h" ]; then
    cat > "$REF/util/u_math.h" <<'EOF'
/* Minimal stand-in for Mesa's src/util/u_math.h: the generated dump code
 * only uses uif(); NVC06F/NVC1C0 have no float methods. Not Mesa code. */
#ifndef NVK_REF_U_MATH_SHIM_H
#define NVK_REF_U_MATH_SHIM_H
#include <stdint.h>
#include <string.h>
static inline float uif(uint32_t ui)
{
   float f;
   memcpy(&f, &ui, sizeof(f));
   return f;
}
#endif
EOF
fi

# python's "import util" leaves a __pycache__ next to the scripts; not a
# source artifact, so remove it.
rm -rf "$REF/__pycache__"

if [ -z "$SRC" ]; then
    echo "mesa_ref regenerated from gitlab mesa @ $MESACOMMIT"
else
    echo "mesa_ref regenerated from local mesa ($SRC) -- update mesa_commit.txt if that is a different commit"
fi
