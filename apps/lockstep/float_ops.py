#!/usr/bin/env python3
"""Lists the floating-point instructions of a .wasm module, function by
function, and fails (exit 1) on any outside an allowed set.

  float_ops.py module.wasm                 the lockstep set (below)
  float_ops.py module.wasm --allow f64.add,f64.mul,...

The lockstep set is the arithmetic every CPU rounds the same way under Wasm:
f64 add, sub, mul and div, neg and abs (which change the sign bit alone), the
six f64 comparisons, f64 constants, loads and stores, the conversions between i32 and f64 and the reinterpretations
between i64 and f64. Wasm has no fused multiply-add outside SIMD, and the
module must hold no SIMD (0xfd) and no saturating conversions (0xfc 0..7),
which are not MVP. With a name section (a module linked without
--strip-all) the functions are listed by name.
"""
import argparse
import sys

NAMES = {
    0x2A: "f32.load", 0x2B: "f64.load", 0x38: "f32.store", 0x39: "f64.store", 0x43: "f32.const", 0x44: "f64.const",
    0x5B: "f32.eq", 0x5C: "f32.ne", 0x5D: "f32.lt", 0x5E: "f32.gt", 0x5F: "f32.le", 0x60: "f32.ge",
    0x61: "f64.eq", 0x62: "f64.ne", 0x63: "f64.lt", 0x64: "f64.gt", 0x65: "f64.le", 0x66: "f64.ge",
    0x8B: "f32.abs", 0x8C: "f32.neg", 0x8D: "f32.ceil", 0x8E: "f32.floor", 0x8F: "f32.trunc", 0x90: "f32.nearest",
    0x91: "f32.sqrt", 0x92: "f32.add", 0x93: "f32.sub", 0x94: "f32.mul", 0x95: "f32.div", 0x96: "f32.min",
    0x97: "f32.max", 0x98: "f32.copysign",
    0x99: "f64.abs", 0x9A: "f64.neg", 0x9B: "f64.ceil", 0x9C: "f64.floor", 0x9D: "f64.trunc", 0x9E: "f64.nearest",
    0x9F: "f64.sqrt", 0xA0: "f64.add", 0xA1: "f64.sub", 0xA2: "f64.mul", 0xA3: "f64.div", 0xA4: "f64.min",
    0xA5: "f64.max", 0xA6: "f64.copysign",
    0xA8: "i32.trunc_f32_s", 0xA9: "i32.trunc_f32_u", 0xAA: "i32.trunc_f64_s", 0xAB: "i32.trunc_f64_u",
    0xAE: "i64.trunc_f32_s", 0xAF: "i64.trunc_f32_u", 0xB0: "i64.trunc_f64_s", 0xB1: "i64.trunc_f64_u",
    0xB2: "f32.convert_i32_s", 0xB3: "f32.convert_i32_u", 0xB4: "f32.convert_i64_s", 0xB5: "f32.convert_i64_u",
    0xB6: "f32.demote_f64", 0xB7: "f64.convert_i32_s", 0xB8: "f64.convert_i32_u", 0xB9: "f64.convert_i64_s",
    0xBA: "f64.convert_i64_u", 0xBB: "f64.promote_f32", 0xBC: "i32.reinterpret_f32", 0xBD: "i64.reinterpret_f64",
    0xBE: "f32.reinterpret_i32", 0xBF: "f64.reinterpret_i64",
}
LOCKSTEP = ("f64.load,f64.store,f64.const,f64.eq,f64.ne,f64.lt,f64.gt,f64.le,f64.ge,f64.neg,f64.abs,f64.add,f64.sub,f64.mul,"
            "f64.div,i32.trunc_f64_s,f64.convert_i32_s,i64.reinterpret_f64,f64.reinterpret_i64")


def uleb(b, i):
    r = s = 0
    while True:
        c = b[i]
        i += 1
        r |= (c & 0x7F) << s
        s += 7
        if not c & 0x80:
            return r, i


def skip_leb(b, i):
    while b[i] & 0x80:
        i += 1
    return i + 1


def sections(b):
    if b[:4] != b"\0asm":
        raise ValueError("not a wasm module")
    i = 8
    while i < len(b):
        sid = b[i]
        n, i = uleb(b, i + 1)
        yield sid, b[i:i + n]
        i += n


def function_names(b, imported):
    names = {}
    for sid, body in sections(b):
        if sid != 0:
            continue
        n, i = uleb(body, 0)
        if body[i:i + n] != b"name":
            continue
        i += n
        while i < len(body):
            kind = body[i]
            size, i = uleb(body, i + 1)
            end = i + size
            if kind == 1:
                count, i = uleb(body, i)
                for _ in range(count):
                    idx, i = uleb(body, i)
                    ln, i = uleb(body, i)
                    names[idx - imported] = body[i:i + ln].decode(errors="replace")
                    i += ln
            i = end
    return names


def imported_functions(b):
    for sid, body in sections(b):
        if sid != 2:
            continue
        count, i = uleb(body, 0)
        funcs = 0
        for _ in range(count):
            for _ in range(2):
                ln, i = uleb(body, i)
                i += ln
            kind = body[i]
            i += 1
            if kind == 0:
                i = skip_leb(body, i)
                funcs += 1
            elif kind == 1:
                i += 1
                flags = body[i]
                i = skip_leb(body, skip_leb(body, i + 1)) if flags & 1 else skip_leb(body, i + 1)
            elif kind == 2:
                flags = body[i]
                i = skip_leb(body, skip_leb(body, i + 1)) if flags & 1 else skip_leb(body, i + 1)
            else:
                i += 2
        return funcs
    return 0


def scan(code):
    """Per function: {float op name: count}, and the non-MVP instructions found"""
    count, i = uleb(code, 0)
    out, bad = [], []
    for f in range(count):
        size, i = uleb(code, i)
        end = i + size
        groups, i = uleb(code, i)
        for _ in range(groups):
            i = skip_leb(code, i)
            i += 1
        ops = {}
        while i < end:
            op = code[i]
            i += 1
            if op in NAMES:
                ops[NAMES[op]] = ops.get(NAMES[op], 0) + 1
            if op in (0x02, 0x03, 0x04):
                i = skip_leb(code, i)
            elif op in (0x0C, 0x0D, 0x10, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26):
                i = skip_leb(code, i)
            elif op == 0x0E:
                n, i = uleb(code, i)
                for _ in range(n + 1):
                    i = skip_leb(code, i)
            elif op == 0x11:
                i = skip_leb(code, skip_leb(code, i))
            elif op == 0x1C:
                n, i = uleb(code, i)
                i += n
            elif 0x28 <= op <= 0x3E:
                i = skip_leb(code, skip_leb(code, i))
            elif op in (0x3F, 0x40):
                i += 1
            elif op in (0x41, 0x42):
                i = skip_leb(code, i)
            elif op == 0x43:
                i += 4
            elif op == 0x44:
                i += 8
            elif op == 0xFC:
                sub, i = uleb(code, i)
                if sub <= 7:
                    bad.append((f, "saturating conversion 0xfc %d" % sub))
                elif sub == 8:
                    i = skip_leb(code, i) + 1
                elif sub in (9, 13, 15, 16, 17):
                    i = skip_leb(code, i)
                elif sub == 10:
                    i += 2
                elif sub == 11:
                    i += 1
                elif sub in (12, 14):
                    i = skip_leb(code, skip_leb(code, i))
            elif op == 0xFD:
                bad.append((f, "SIMD instruction"))
                break
        i = end
        out.append(ops)
    return out, bad


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("wasm")
    ap.add_argument("--allow", default=LOCKSTEP, help="comma-separated instruction names (default: the lockstep set)")
    args = ap.parse_args()
    b = open(args.wasm, "rb").read()
    allow = set(args.allow.split(","))
    code = next((body for sid, body in sections(b) if sid == 10), b"\0")
    funcs, bad = scan(code)
    names = function_names(b, imported_functions(b))
    total = {}
    for f, ops in enumerate(funcs):
        if not ops:
            continue
        for k, v in ops.items():
            total[k] = total.get(k, 0) + v
        print("%-16s %s" % (names.get(f, "func %d" % f), ", ".join("%s %d" % kv for kv in sorted(ops.items()))))
    outside = sorted(k for k in total if k not in allow)
    for f, why in bad:
        print("%s: %s" % (names.get(f, "func %d" % f), why), file=sys.stderr)
    if outside:
        print("float instructions outside the allowed set: " + ", ".join(outside), file=sys.stderr)
    if bad or outside:
        return 1
    print("%s: %d float instructions, all in the allowed set; no SIMD, no saturating conversions" % (args.wasm, sum(total.values())))
    return 0


if __name__ == "__main__":
    sys.exit(main())
