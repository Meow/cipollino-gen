#!/usr/bin/env python3
"""Write src/kernel/field13.h, the field multiplication on 24-bit multipliers.

    python3 tools/gen_field13.py > src/kernel/field13.h
    python3 tools/gen_field13.py --check src/kernel/field13.h     (is the file up to date?)

The generator also proves, by interval arithmetic over the largest value each operand
class can take, that no column or carry overflows 32 bits and that every multiplier
operand fits 24 bits; it refuses to write code for which that fails.
"""
import math
import sys

# A radix-2^25.5 limb is two half-limbs: twenty of them, at bit offsets ceil(12.75 i),
# 13 bits wide except every fourth, which has 12.
OFF = [-(-51 * i // 4) for i in range(41)]
WIDTH = [OFF[i + 1] - OFF[i] for i in range(40)]
M13 = (1 << 13) - 1

# Largest limb values (ten radix-2^25.5 limbs) per operand class, see field.h.
TIGHT = [(1 << 26) - 1, (1 << 25) - 1] * 4 + [(1 << 26) - 1, (1 << 26) - 1]
LOOSE = [(3 << 26) - 1, (3 << 25) - 1] * 4 + [(3 << 26) - 1, (5 << 25) - 1]
# fe_weak of a loose value: a masked limb plus the carry of its neighbour.
WEAK = [(1 << 26) - 1 + 19 * (LOOSE[9] >> 25)] + [
    ((1 << 25) - 1 if j % 2 else (1 << 26) - 1) + (LOOSE[j - 1] >> (26 if j % 2 else 25)) for j in range(1, 10)
]
# fe_one_minus and fe_one_plus of a tight value (CIP_B0 + 1, CIP_BO, CIP_BE, CIP_B9).
SEMI = [134217671 + 1, 67108862] + [134217726, 67108862] * 3 + [134217726, 100663294]


def excess(i, j):
    """Half-limbs i and j multiply to 2^excess times the unit of column i + j."""
    return OFF[i] + OFF[j] - OFF[i + j]


def halves(limbs):
    out = []
    for x in limbs:
        out += [min(x, M13), x >> 13]
    return out


def wrap(first, terms, indent):
    lines, cur = [], first
    for n, t in enumerate(terms):
        piece = t if n == 0 else " + " + t
        if len(cur) + len(piece) > 100:
            lines.append(cur)
            cur = " " * indent + "+ " + t
        else:
            cur += piece
    lines.append(cur)
    return "\n".join(lines)


def split(src, name):
    return ["    u32 " + ", ".join(f"{name}{2 * k} = {src}.v[{k}] & CIP_M13, {name}{2 * k + 1} = {src}.v[{k}] >> 13"
                                   for k in (j, j + 1)) + ";" for j in range(0, 10, 2)]


def doubled(name, residues):
    items = [f"{name}{i}d = 2u * {name}{i}" for i in range(20) if i % 4 in residues]
    return "\n".join("    u32 " + ", ".join(items[n:n + 5]) + ";" for n in range(0, len(items), 5))


def term(i, j, a, b):
    """The product of half-limbs i of `a` and j of `b` in units of column i + j."""
    if not excess(i, j):
        return f"{a}{i} * {b}{j}"
    if i % 4 in (2, 3):
        return f"{a}{i}d * {b}{j}"
    assert j % 4 == 3
    return f"{a}{i} * {b}{j}d"


def column(k, amax, bmax, a="a", b="b"):
    terms, total = [], 0
    for i in range(max(0, k - 19), min(19, k) + 1):
        j = k - i
        e = excess(i, j)
        assert e in (0, 1) and (amax[i] << e) < 1 << 24 and (bmax[j] << e) < 1 << 24
        terms.append(term(i, j, a, b))
        total += (amax[i] * bmax[j]) << e
    return terms, total


def gen_reduce(cmax):
    c = list(cmax) + [0]
    L = ["// Carry 39 columns (bounds: see the generator) down to a tight field element.",
         "CIP_FN fe fe_reduce13(" + wrap("", [", ".join(f"u32 c{k}" for k in range(n, min(n + 10, 39))) + ("," if n < 30 else "")
                                              for n in range(0, 39, 10)], 0).replace(" + ", " ").replace("\n+ ", "\n                      ") + ")",
         "{",
         "    // Columns 20..38 stand for 19 times columns 0..18 (2^255 = 19): carry along them,",
         "    // then fold. c39 is what is carried out of column 38."]
    steps = []
    for k in range(20, 39):
        carry = c[k] >> WIDTH[k]
        if k == 38:
            steps.append(f"u32 c39 = c38 >> {WIDTH[k]};")
            c[39] = carry
        else:
            steps.append(f"c{k + 1} += c{k} >> {WIDTH[k]};")
            c[k + 1] += carry
            assert c[k + 1] < 1 << 32
        c[k] = (1 << WIDTH[k]) - 1
    L += ["    " + " ".join(steps[n:n + 5]) for n in range(0, len(steps), 5)]
    steps = []
    for k in range(20):
        assert c[20 + k] < 1 << 24
        steps.append(f"c{k} += 19u * " + ("c39;" if k == 19 else f"(c{20 + k} & CIP_M{WIDTH[20 + k]});"))
        c[k] += 19 * c[20 + k]
        assert c[k] < 1 << 32
    L += ["    " + " ".join(steps[n:n + 4]) for n in range(0, len(steps), 4)]
    q = (c[19] + (c[18] >> 13)) >> 12
    assert q < 1 << 24 and c[0] + 19 * q < 1 << 32
    c[0] += 19 * q
    L += ["    // As in fe_reduce: q is the carry out of bit 255, at most one too small, so one pass",
          "    // leaves limbs 0..8 exactly masked. Limb 9 keeps what q missed and stays below",
          "    // 2^25 + 2^20.",
          "    u32 q = (c19 + (c18 >> 13)) >> 12;",
          "    c0 += 19u * q;"]
    steps = []
    for k in range(18):
        steps.append(f"c{k + 1} += c{k} >> {WIDTH[k]};")
        c[k + 1] += c[k] >> WIDTH[k]
        assert c[k + 1] < 1 << 32
    L += ["    " + " ".join(steps[n:n + 6]) for n in range(0, len(steps), 6)]
    # Limb 9 is the remainder of q's division, the 13 bits of c18 that q ignored and the
    # carry into c18.
    assert (1 << 25) + (1 << 13) + (c[17] >> 13) < (1 << 25) + (1 << 20)
    L += ["    c19 -= q << 12;", "    fe r;"]
    for j in range(0, 10, 2):
        L.append("    " + " ".join(
            "r.v[9] = c18 + (c19 << 13);" if k == 9 else
            f"r.v[{k}] = ((c{2 * k} & CIP_M13) + (c{2 * k + 1} << 13)) & CIP_M{25 if k % 2 else 26};" for k in (j, j + 1)))
    L += ["    return r;", "}"]
    return L


def gen_mul(fmax, gmax):
    a, b = halves(fmax), halves(gmax)
    L = ["CIP_FN fe fe_mul_t(fe f, fe g)", "{"] + split("f", "a") + split("g", "b")
    L += [doubled("a", (2, 3)), doubled("b", (3,))]
    cmax = []
    for k in range(39):
        terms, total = column(k, a, b)
        assert total < 1 << 32
        cmax.append(total)
        L.append(wrap(f"    u32 c{k} = ", terms, 12) + ";")
    L.append(wrap("    return fe_reduce13(", [", ".join(f"c{k}" for k in range(n, min(n + 13, 39))) + ("," if n < 26 else ");")
                                             for n in range(0, 39, 13)], 0).replace(" + ", " ").replace("\n+ ", "\n                       "))
    L.append("}")
    return L, cmax


def gen_sq(fmax):
    a = halves(fmax)
    L = ["CIP_FN fe fe_sq(fe f)", "{"] + split("f", "a") + [doubled("a", (2, 3))]
    cmax = []
    for k in range(39):
        terms, total = [], 0
        for i in range(max(0, k - 19), (k + 1) // 2):
            j = k - i
            e = excess(i, j)
            # one of the two is doubled; the other order of term() covers i = 1 (mod 4)
            terms.append(term(i, j, "a", "a") if not e or i % 4 in (2, 3) else term(j, i, "a", "a"))
            total += (a[i] * a[j]) << (e + 1)
        expr = wrap("2u * (", terms, 21) + ")" if terms else ""
        if k % 2 == 0:
            i = k // 2
            expr = term(i, i, "a", "a") + (" + " + expr if expr else "")
            total += (a[i] * a[i]) << excess(i, i)
        assert total < 1 << 32
        cmax.append(total)
        L.append(f"    u32 c{k} = {expr};")
    L.append(wrap("    return fe_reduce13(", [", ".join(f"c{k}" for k in range(n, min(n + 13, 39))) + ("," if n < 26 else ");")
                                             for n in range(0, 39, 13)], 0).replace(" + ", " ").replace("\n+ ", "\n                       "))
    L.append("}")
    return L, cmax


PROBE_MARGIN = 256


def gen_probe():
    a = b = halves(WEAK)
    L = ["CIP_FN fe_probe fe_mul_probe(fe f, fe g)", "{", "    f = fe_weak(f);", "    g = fe_weak(g);"]
    L += split("f", "a") + split("g", "b") + [doubled("a", (2, 3)), doubled("b", (3,))]
    L.append("    // Folded columns 0..2 modulo 2^32 (l: the terms below 2^255, m: those above).")
    for k in range(3):
        L.append(wrap(f"    u32 l{k} = ", column(k, a, b)[0], 12) + ";")
        L.append(wrap(f"    u32 m{k} = ", column(20 + k, a, b)[0], 12) + ";")
    L.append("    // Folded columns 17..19, exactly.")
    cm = {}
    for k in (17, 18, 19, 37, 38):
        terms, cm[k] = column(k, a, b)
        assert cm[k] < 1 << 32
        L.append(wrap(f"    u32 c{k} = ", terms, 12) + ";")
    assert 19 * cm[37] < 1 << 32
    t18 = cm[18] + 19 * cm[38] + (cm[17] >> 13) + ((19 * cm[37]) >> 13)
    n19 = cm[19] + (t18 >> 13)
    assert t18 < 1 << 32 and n19 < 1 << 32 and (n19 >> 12) < 1 << 24
    # What t18 leaves out, in its own units: the folded columns below 17, and one for
    # taking the two floors separately.
    below = sum((column(k, a, b)[1] + 19 * column(20 + k, a, b)[1]) / 2.0 ** (OFF[17] - OFF[k]) for k in range(17))
    slack = below / 2.0 ** 13 + 2
    assert slack < PROBE_MARGIN
    L += ["    u32 t18 = c18 + CIP_X19(c38) + (c17 >> 13) + (CIP_X19(c37) >> 13);",
          "    u32 n19 = c19 + (t18 >> 13);",
          "    fe_probe r;",
          "    r.lo = (l0 + CIP_X19(m0)) + ((l1 + CIP_X19(m1)) << 13) + ((l2 + CIP_X19(m2)) << 26) + 19u * (n19 >> 12);",
          f"    r.unsure = ((n19 & CIP_M12) == CIP_M12 && (t18 & CIP_M13) >= CIP_M13 - {PROBE_MARGIN}u) ? 1u : 0u;",
          "    return r;", "}"]
    return L, slack


HEADER = """\
// cipollino kernel: field multiplication on 24-bit multipliers (CIP_ARITH_NARROW).
//
// GENERATED by tools/gen_field13.py, which also proves the bounds claimed below. Change
// the generator, not this file.
//
// A 32x32 multiply is a slow instruction on AMD GPUs: 4.5 times the cost of an addition
// on the one measured (RDNA 2), for either half of the product. ROCm's compiler forms a
// 64-bit product and adds it to a column in one of those (v_mad_u64_u32); Mesa's, behind
// Vulkan, needs two and a carry chain per product, and the search runs at half the speed.
// A multiply of operands below 2^24 costs the same as an addition, though, including the
// addition it can be fused with. So here every limb is cut into two half-limbs of 13 (or
// 12) bits, twenty per element at bit offsets ceil(12.75 i). A product of two fits 26
// bits, 27 where the offsets of the two half-limbs overshoot the column's by one bit and
// one factor is doubled, and a column of twenty fits a u32 with nothing to carry: 400
// multiply-adds and about 200 instructions of splitting and carrying, against 200 slow
// multiplies and 200 additions.
//
// Not for LLVM's AMD backend (hipcc, and any driver built on it): its dot-product combine
// takes a sum of products of values masked to 13 bits for a sum of products of bytes and
// emits v_dot4_u32_u8. The engine's start-up self-test notices and falls back to the wide
// arithmetic, and the ROCm backend is built with the wide one, which hipcc compiles to the
// faster kernel anyway; see the README.
//
// Operand classes are those of field.h, plus
//   weak : what fe_weak returns for a loose value, at most 76 above the tight bounds.

#define CIP_M13 0x1fffu
#define CIP_M12 0xfffu
// 19 x modulo 2^32 without a multiply, for x that may not fit 24 bits.
#define CIP_X19(x) (((x) << 4) + ((x) << 1) + (x))

// One carry step on every limb at once: loose in, weak out.
CIP_FN fe fe_weak(fe a)
{
    fe r;
    r.v[0] = (a.v[0] & CIP_M26) + 19u * (a.v[9] >> 25);
    r.v[1] = (a.v[1] & CIP_M25) + (a.v[0] >> 26); r.v[2] = (a.v[2] & CIP_M26) + (a.v[1] >> 25);
    r.v[3] = (a.v[3] & CIP_M25) + (a.v[2] >> 26); r.v[4] = (a.v[4] & CIP_M26) + (a.v[3] >> 25);
    r.v[5] = (a.v[5] & CIP_M25) + (a.v[4] >> 26); r.v[6] = (a.v[6] & CIP_M26) + (a.v[5] >> 25);
    r.v[7] = (a.v[7] & CIP_M25) + (a.v[6] >> 26); r.v[8] = (a.v[8] & CIP_M26) + (a.v[7] >> 25);
    r.v[9] = (a.v[9] & CIP_M25) + (a.v[8] >> 26);
    return r;
}
"""


def main():
    fmax = [max(t, w) for t, w in zip(TIGHT, WEAK)]
    assert all(w <= s for w, s in zip(fmax, SEMI))
    mul, cmul = gen_mul(fmax, SEMI)
    sq, csq = gen_sq(TIGHT)
    probe, slack = gen_probe()
    reduce = gen_reduce([max(m, s) for m, s in zip(cmul, csq)])
    out = [HEADER]
    out += reduce + [""]
    out += ["// h = f * g for f tight or weak, and g tight, weak or one of fe_one_minus, fe_one_plus of a",
            "// tight value. Column k collects the products of half-limbs i and k - i."] + mul + [""]
    out += ["// h = f * g for any loose inputs.",
            "CIP_FN fe fe_mul(fe f, fe g) { return fe_mul_t(fe_weak(f), fe_weak(g)); }", ""]
    out += ["// h = f * f, f tight: each cross product once, doubled."] + sq + [""]
    out += ["// The low 32 bits of the canonical value of f*g, as the fe_mul_probe of field.h and for",
            "// any loose inputs. With F the folded product (column k plus 19 times column 20 + k) the",
            "// canonical value is F mod 2^255 plus 19 times the carry out of bit 255. The low bits of F",
            "// need columns 0..2 only modulo 2^32; the carry is taken from columns 17..19, and what the",
            f"// columns below those could add to it is less than {math.ceil(slack)} units of column 18. Where that",
            "// could change the carry the result is `unsure`: about one product in 130000."] + probe
    text = "\n".join(out) + "\n"
    if len(sys.argv) == 3 and sys.argv[1] == "--check":
        with open(sys.argv[2]) as f:  # text mode: a checkout with other line endings is fine
            if f.read() != text:
                sys.exit(f"{sys.argv[2]} is not what tools/gen_field13.py generates")
        return
    sys.stdout.write(text)
    sys.stderr.write(f"largest column: 2^{math.log2(max(cmul)):.2f} (mul), 2^{math.log2(max(csq)):.2f} (sq); "
                     f"probe slack {slack:.1f} of {PROBE_MARGIN}\n")


if __name__ == "__main__":
    main()
