//! `llvm.copysign` as integer sign-bit operations.
//!
//! The AIR the Metal compiler writes has no copysign, but LLVM's instcombine forms it from select/fneg/fabs
//! patterns. The plugin runs instcombine on AIR under NVMTL_CANONICAL_POINTERS (app profiles), and with the
//! release's LLVM 23.1.4 iOS 26 QuartzCore shaders came out with `llvm.copysign.f32` / `.f16` / `.v3f16`, which the
//! emitter has no lowering for ("unresolved external shader function: llvm.copysign.v3f16";
//! plugin/tests/air_xlate_check.py). Vulkan's GLSL.std.450 has no copysign either, so each call becomes
//!
//!   %r.m = bitcast T %mag to IT ; %r.s = bitcast T %sign to IT      (a half through fpext to float first)
//!   %r.a = and IT %r.m, ~SIGN   ; %r.b = and IT %r.s, SIGN
//!   %r.o = or IT %r.a, %r.b     ; %r = bitcast IT %r.o to T
//!
//! (a vector lane by lane, through extractelement / insertelement), which is copysign exactly (IEEE 754 copySign is a bit operation, NaNs included). Only call instructions whose
//! result is named and whose two operands are plain values are rewritten; anything else stays and fails as before.

/// The module with every `llvm.copysign.*` call lowered to sign-bit operations; None when there is none.
pub(crate) fn lower_copysign(ll: &str) -> Option<String> {
    if !ll.contains("@llvm.copysign.") {
        return None;
    }
    let mut out = String::with_capacity(ll.len() + 512);
    let mut changed = 0usize;
    for line in ll.split_inclusive('\n') {
        match rewrite_call(line, changed) {
            Some(new) => {
                out.push_str(&new);
                changed += 1;
            }
            None => out.push_str(line),
        }
    }
    (changed > 0).then_some(out)
}

/// (bits, IEEE type name) of a scalar float type.
fn scalar(ty: &str) -> Option<(u32, &'static str)> {
    match ty {
        "half" => Some((16, "half")),
        "float" => Some((32, "float")),
        "double" => Some((64, "double")),
        _ => None,
    }
}

/// (Some((lanes, element type)) for a vector, None for a scalar; element width in bits).
fn shape(ty: &str) -> Option<(Option<(u32, &'static str)>, u32)> {
    if let Some(inner) = ty.strip_prefix('<').and_then(|t| t.strip_suffix('>')) {
        let (n, elem) = inner.split_once(" x ")?;
        let n: u32 = n.trim().parse().ok()?;
        let (bits, name) = scalar(elem.trim())?;
        return (1..=16).contains(&n).then_some((Some((n, name)), bits));
    }
    let (bits, _) = scalar(ty)?;
    Some((None, bits))
}

fn mask_value(bits: u32, sign: bool) -> String {
    let v: i128 = if sign { -(1i128 << (bits - 1)) } else { (1i128 << (bits - 1)) - 1 };
    v.to_string()
}

fn rewrite_call(line: &str, serial: usize) -> Option<String> {
    let body = line.trim_end_matches(['\n', '\r']);
    let indent_len = body.len() - body.trim_start().len();
    let indent = &body[..indent_len];
    let stmt = &body[indent_len..];
    let (result, rhs) = stmt.split_once(" = ")?;
    if !result.starts_with('%') || result.contains(' ') {
        return None;
    }
    let at = rhs.find("@llvm.copysign.")?;
    let head = rhs[..at].trim_end();
    // `[tail ]call [fast-math flags] <ty>` before the callee
    let mut words: Vec<&str> = head.split_whitespace().collect();
    let pos = words.iter().position(|w| *w == "call")?;
    if words[..pos].iter().any(|w| !matches!(*w, "tail" | "musttail" | "notail")) {
        return None;
    }
    words.drain(..=pos);
    // the result type is the rest after the fast-math flags (a vector type has spaces in it)
    let flags = ["fast", "nnan", "ninf", "nsz", "arcp", "contract", "afn", "reassoc"];
    let ty_start = words.iter().position(|w| !flags.contains(w))?;
    let ty = words[ty_start..].join(" ");
    let (lanes, bits) = shape(&ty)?;
    let args_start = rhs[at..].find('(')? + at;
    let args_end = rhs.rfind(')')?;
    let args = &rhs[args_start + 1..args_end];
    let rest = rhs[args_end + 1..].trim();
    // only attribute groups may follow (`#3`); metadata attachments are dropped with the call
    if !rest.is_empty() && !rest.split_whitespace().all(|w| w.starts_with('#') || w.starts_with('!') || w.ends_with(',')) {
        return None;
    }
    let ops = split_top_level(args);
    if ops.len() != 2 {
        return None;
    }
    let value = |op: &str| -> Option<String> {
        let op = op.trim();
        let v = op.strip_prefix(ty.as_str())?.trim();
        // a plain operand: `%x`, a constant, `splat (...)`; no attributes in between
        (!v.is_empty() && !v.starts_with("noundef") && !v.starts_with("nofpclass")).then(|| v.to_string())
    };
    let (mag, sgn) = (value(ops[0])?, value(ops[1])?);
    let tag = format!("%copysign.{serial}");
    let mut s = String::new();
    // one scalar: bitcast both to the integer of that width, keep the magnitude bits of one and the sign of the other
    // one scalar: bitcast both to the integer of that width, keep the magnitude bits of one and the sign of the other.
    // A half goes through float (fpext is exact, and so is the fptrunc of a value that came from a half): the emitter
    // refuses half <-> i16 bitcasts ("owned OpBitcast source and result shapes are inconsistent").
    let scalar_ops = |s: &mut String, t: &str, ety: &str, m: &str, g: &str, out: &str| {
        let (m, g, fty, wide) = if ety == "half" {
            s.push_str(&format!("{indent}{t}.wm = fpext half {m} to float\n"));
            s.push_str(&format!("{indent}{t}.wg = fpext half {g} to float\n"));
            (format!("{t}.wm"), format!("{t}.wg"), "float", true)
        } else {
            (m.to_string(), g.to_string(), ety, false)
        };
        let ibits = if wide { 32 } else { bits };
        let ity = format!("i{ibits}");
        let (not_sign, sign) = (mask_value(ibits, false), mask_value(ibits, true));
        s.push_str(&format!("{indent}{t}.m = bitcast {fty} {m} to {ity}\n"));
        s.push_str(&format!("{indent}{t}.s = bitcast {fty} {g} to {ity}\n"));
        s.push_str(&format!("{indent}{t}.a = and {ity} {t}.m, {not_sign}\n"));
        s.push_str(&format!("{indent}{t}.b = and {ity} {t}.s, {sign}\n"));
        s.push_str(&format!("{indent}{t}.o = or {ity} {t}.a, {t}.b\n"));
        if wide {
            s.push_str(&format!("{indent}{t}.f = bitcast {ity} {t}.o to float\n"));
            s.push_str(&format!("{indent}{out} = fptrunc float {t}.f to half"));
        } else {
            s.push_str(&format!("{indent}{out} = bitcast {ity} {t}.o to {ety}"));
        }
    };
    match lanes {
        None => scalar_ops(&mut s, &tag, &ty, &mag, &sgn, result),
        // a vector lane by lane: the emitter keeps <3 x half> and <3 x i16> in different shapes, so a whole-vector
        // bitcast between them is refused ("owned OpBitcast source and result shapes are inconsistent")
        Some((n, ety)) => {
            let mut prev = "undef".to_string();
            for k in 0..n {
                let t = format!("{tag}.{k}");
                s.push_str(&format!("{indent}{t}.x = extractelement {ty} {mag}, i32 {k}\n"));
                s.push_str(&format!("{indent}{t}.y = extractelement {ty} {sgn}, i32 {k}\n"));
                scalar_ops(&mut s, &t, ety, &format!("{t}.x"), &format!("{t}.y"), &format!("{t}.r"));
                s.push('\n');
                let out = if k + 1 == n { result.to_string() } else { format!("{t}.v") };
                s.push_str(&format!("{indent}{out} = insertelement {ty} {prev}, {ety} {t}.r, i32 {k}"));
                if k + 1 != n {
                    s.push('\n');
                }
                prev = out;
            }
        }
    }
    s.push_str(&line[body.len()..]);
    Some(s)
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
    use super::lower_copysign;

    #[test]
    fn scalar_copysign_becomes_sign_bit_operations() {
        let ll = "  %161 = call float @llvm.copysign.f32(float %159, float %160)\n";
        assert_eq!(
            lower_copysign(ll).expect("lowered"),
            "  %copysign.0.m = bitcast float %159 to i32\n\
             \x20 %copysign.0.s = bitcast float %160 to i32\n\
             \x20 %copysign.0.a = and i32 %copysign.0.m, 2147483647\n\
             \x20 %copysign.0.b = and i32 %copysign.0.s, -2147483648\n\
             \x20 %copysign.0.o = or i32 %copysign.0.a, %copysign.0.b\n\
             \x20 %161 = bitcast i32 %copysign.0.o to float\n"
        );
    }

    #[test]
    fn vector_copysign_goes_lane_by_lane() {
        let ll = "  %234 = tail call fast <3 x half> @llvm.copysign.v3f16(<3 x half> %233, <3 x half> %212) #4\n";
        let got = lower_copysign(ll).expect("lowered");
        assert!(got.starts_with(
            "  %copysign.0.0.x = extractelement <3 x half> %233, i32 0\n\
             \x20 %copysign.0.0.y = extractelement <3 x half> %212, i32 0\n\
             \x20 %copysign.0.0.wm = fpext half %copysign.0.0.x to float\n\
             \x20 %copysign.0.0.wg = fpext half %copysign.0.0.y to float\n\
             \x20 %copysign.0.0.m = bitcast float %copysign.0.0.wm to i32\n\
             \x20 %copysign.0.0.s = bitcast float %copysign.0.0.wg to i32\n\
             \x20 %copysign.0.0.a = and i32 %copysign.0.0.m, 2147483647\n\
             \x20 %copysign.0.0.b = and i32 %copysign.0.0.s, -2147483648\n\
             \x20 %copysign.0.0.o = or i32 %copysign.0.0.a, %copysign.0.0.b\n\
             \x20 %copysign.0.0.f = bitcast i32 %copysign.0.0.o to float\n\
             \x20 %copysign.0.0.r = fptrunc float %copysign.0.0.f to half\n\
             \x20 %copysign.0.0.v = insertelement <3 x half> undef, half %copysign.0.0.r, i32 0\n"
        ), "{got}");
        assert!(got.contains("%copysign.0.1.v = insertelement <3 x half> %copysign.0.0.v, half %copysign.0.1.r, i32 1\n"), "{got}");
        assert!(got.ends_with("  %234 = insertelement <3 x half> %copysign.0.1.v, half %copysign.0.2.r, i32 2\n"), "{got}");
        assert_eq!(got.lines().count(), 3 * 12);
    }

    #[test]
    fn declarations_and_other_calls_stay() {
        let ll = "declare float @llvm.copysign.f32(float, float) #4\n\
                  \x20 %1 = call float @llvm.fabs.f32(float %0)\n";
        assert_eq!(lower_copysign(ll), None);
    }
}
