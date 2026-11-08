//! LLVM 23's instcombine writes a scaled pointer step `getelementptr T, ptr %p, i64 %i` as
//! `getelementptr [sizeof(T) x i8], ptr %p, i64 %i` (its canonical form since the GEP source-type
//! canonicalization). The single index steps over whole `[N x i8]` objects, so it is the same address as a
//! step over any type of N bytes. The emitter models the source type as the pointee and, for a buffer
//! parameter whose pointee is a scalar, builds an access chain that descends into the byte
//! ("owned PtrAccessChain violates its access-chain contract: index 0 descends into ... TypeInt 8").
//!
//! Seen on the plugin's own Apple-compiled nvconv_reduce kernel read through the release's LLVM 23.1.4 with
//! NVMTL_CANONICAL_POINTERS (plugin/tests/air_xlate_check.py): `gep [4 x i8], ptr addrspace(2) %6, i64 %x`.
//!
//! The rewrite gives such a GEP the integer type of the same size: `[1|2|4|8 x i8]` -> `i8|i16|i32|i64`. Only
//! instructions with exactly one index are rewritten; a GEP that also indexes into the array
//! (`[8 x i8], ptr %a, i64 0, i64 %k`) addresses bytes and is left alone, and so are constant expressions and
//! vector GEPs. The address is unchanged, since the alloc size of `iN` is N/8 bytes for these widths.
//!
//! The wrapper (translator/wrapper) applies this only as a retry of a module that failed as written, so no
//! module that translated before changes.

/// The module with every single-index `[N x i8]` GEP instruction (N = 1, 2, 4, 8) given the same-size integer
/// source type; None when there is none.
pub(crate) fn scalarize_byte_stride_geps(ll: &str) -> Option<String> {
    if !ll.contains(" x i8], ptr") {
        return None;
    }
    let mut out = String::with_capacity(ll.len());
    let mut changed = 0usize;
    for line in ll.split_inclusive('\n') {
        match rewrite_line(line) {
            Some(new) => {
                out.push_str(&new);
                changed += 1;
            }
            None => out.push_str(line),
        }
    }
    (changed > 0).then_some(out)
}

fn rewrite_line(line: &str) -> Option<String> {
    let body = line.trim_end_matches(['\n', '\r']);
    let eq = body.find(" = getelementptr ")?;
    if !body[..eq].trim_start().starts_with('%') {
        return None;
    }
    let mut rest = &body[eq + " = getelementptr ".len()..];
    loop {
        let mut advanced = false;
        for flag in ["inbounds ", "nuw ", "nusw "] {
            if let Some(r) = rest.strip_prefix(flag) {
                rest = r;
                advanced = true;
            }
        }
        if !advanced {
            break;
        }
    }
    let ty_start = body.len() - rest.len();
    let inner = rest.strip_prefix('[')?;
    let close = inner.find(" x i8], ")?;
    let n: u32 = inner[..close].parse().ok()?;
    let scalar = match n {
        1 => "i8",
        2 => "i16",
        4 => "i32",
        8 => "i64",
        _ => return None,
    };
    let operands = &inner[close + " x i8], ".len()..];
    let parts = split_top_level(operands);
    // the pointer operand and one index; trailing metadata attachments (", !dbg !7") are allowed
    let values: Vec<&str> = parts.iter().map(|p| p.trim()).filter(|p| !p.starts_with('!')).collect();
    if values.len() != 2 {
        return None;
    }
    let (ptr, index) = (values[0], values[1]);
    if !(ptr.starts_with("ptr ") || ptr.starts_with("ptr addrspace(")) {
        return None; // a vector of pointers
    }
    if !index.starts_with('i') {
        return None; // a vector index
    }
    let type_end = ty_start + 1 + close + " x i8]".len();
    let mut new = String::with_capacity(line.len());
    new.push_str(&body[..ty_start]);
    new.push_str(scalar);
    new.push_str(&body[type_end..]);
    new.push_str(&line[body.len()..]);
    Some(new)
}

fn split_top_level(s: &str) -> Vec<&str> {
    let mut parts = Vec::new();
    let (mut depth, mut start) = (0i32, 0usize);
    for (i, c) in s.char_indices() {
        match c {
            '(' | '[' | '{' | '<' => depth += 1,
            ')' | ']' | '}' | '>' => depth -= 1,
            ',' if depth == 0 => {
                parts.push(&s[start..i]);
                start = i + 1;
            }
            _ => {}
        }
    }
    parts.push(&s[start..]);
    parts
}

#[cfg(test)]
mod tests {
    use super::scalarize_byte_stride_geps as rewrite;

    #[test]
    fn single_index_byte_strides_take_the_same_size_integer() {
        let ll = "  %39 = getelementptr inbounds nuw [4 x i8], ptr addrspace(2) %6, i64 %38\n\
                  \x20 %83 = getelementptr inbounds nuw [4 x i8], ptr addrspace(1) %5, i64 %82, !dbg !4\n\
                  \x20 %9 = getelementptr [8 x i8], ptr %p, i32 %i\n\
                  \x20 %10 = getelementptr nusw [2 x i8], ptr addrspace(3) @g, i64 %k\n\
                  \x20 %11 = getelementptr [1 x i8], ptr %p, i64 3\n";
        let got = rewrite(ll).expect("rewritten");
        assert_eq!(
            got,
            "  %39 = getelementptr inbounds nuw i32, ptr addrspace(2) %6, i64 %38\n\
             \x20 %83 = getelementptr inbounds nuw i32, ptr addrspace(1) %5, i64 %82, !dbg !4\n\
             \x20 %9 = getelementptr i64, ptr %p, i32 %i\n\
             \x20 %10 = getelementptr nusw i16, ptr addrspace(3) @g, i64 %k\n\
             \x20 %11 = getelementptr i8, ptr %p, i64 3\n"
        );
    }

    #[test]
    fn byte_arrays_indexed_into_and_other_shapes_stay() {
        for ll in [
            // indexes into the array: byte addressing, not a stride
            "  %113 = getelementptr inbounds [8 x i8], ptr %12, i64 0, i64 %112\n",
            // 12 bytes has no integer of that size
            "  %1 = getelementptr inbounds [12 x i8], ptr %p, i64 %i\n",
            // a vector GEP
            "  %2 = getelementptr [4 x i8], <2 x ptr> %v, <2 x i64> %i\n",
            // a constant expression operand is not an instruction
            "  %3 = load i32, ptr addrspace(3) getelementptr inbounds nuw ([4 x i8], ptr addrspace(3) @g, i64 2), align 4\n",
            // plain scalar GEPs
            "  %4 = getelementptr inbounds nuw i8, ptr addrspace(2) %7, i64 128\n",
        ] {
            assert_eq!(rewrite(ll), None, "{ll}");
        }
    }
}
