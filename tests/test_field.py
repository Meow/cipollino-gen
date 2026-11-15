#!/usr/bin/env python3
"""Check the kernel field arithmetic against Python big integers.

Covers random inputs and the worst-case limb values of every operand class the search
loop feeds to fe_mul, and asserts the documented output bounds (so a later change that
breaks the "tight" contract or overflows a u64 column is caught here).
"""
import ctypes
import random
import sys

P = 2**255 - 19
OFF = [0, 26, 51, 77, 102, 128, 153, 179, 204, 230]
BITS = [26, 25, 26, 25, 26, 25, 26, 25, 26, 25]
# Upper bounds (exclusive) per limb for each operand class, see field.h.
TIGHT = [1 << 26, 1 << 25] * 4 + [1 << 26, 1 << 26]
LOOSE = [3 << 26, 3 << 25] * 4 + [3 << 26, 5 << 25]

lib = ctypes.CDLL(sys.argv[1])
U10 = ctypes.c_uint32 * 10
U8 = ctypes.c_uint32 * 8
lib.t_low32.restype = ctypes.c_uint32
lib.t_probe.restype = ctypes.c_uint32
lib.t_high32.restype = ctypes.c_uint32


def val(limbs):
    return sum(int(l) << o for l, o in zip(limbs, OFF))


def call(fn, *args):
    r = U10()
    fn(r, *[U10(*a) for a in args])
    return list(r)


def rand_limbs(bound, rng):
    mode = rng.randrange(6)
    if mode == 0:
        return [b - 1 for b in bound]
    if mode == 1:
        return [0] * 10
    if mode == 2:
        return [rng.choice([0, b - 1]) for b in bound]
    if mode == 3:
        return [b - 1 - rng.randrange(64) for b in bound]
    return [rng.randrange(b) for b in bound]


def canonical_limbs(x):
    out = []
    for o, b in zip(OFF, BITS):
        out.append((x >> o) & ((1 << b) - 1))
    return out


def assert_tight(l, what):
    for i, (x, b) in enumerate(zip(l, TIGHT)):
        assert x < b, f"{what}: limb {i} = {x} not tight"
    assert l[9] < (1 << 25) + (1 << 13), f"{what}: top limb {l[9]}"


def main():
    rng = random.Random(0xC1B0)
    n = n_unsure = 0
    for it in range(60000):
        fb = rng.choice([TIGHT, LOOSE])
        gb = rng.choice([TIGHT, LOOSE])
        f, g = rand_limbs(fb, rng), rand_limbs(gb, rng)
        h = call(lib.t_mul, f, g)
        assert_tight(h, "mul")
        assert val(h) % P == val(f) * val(g) % P, ("mul", f, g)

        # the 32-bit probe must agree with the full product whenever it claims certainty
        unsure = ctypes.c_uint32()
        lo = lib.t_probe(ctypes.byref(unsure), U10(*f), U10(*g))
        v = val(f) * val(g)
        if unsure.value:
            n_unsure += 1
        elif not (P <= (v % 2**255) + 19 * (v >> 255)):
            assert lo == (v % P) & 0xFFFFFFFF, ("probe", f, g)

        t = rand_limbs(TIGHT, rng)
        s = call(lib.t_sq, t)
        assert_tight(s, "sq")
        assert val(s) % P == val(t) ** 2 % P, ("sq", t)

        c = call(lib.t_carry, f)
        assert_tight(c, "carry")
        assert val(c) % P == val(f) % P

        a, b = rand_limbs(TIGHT, rng), rand_limbs(TIGHT, rng)
        for name, fn, want in (
            ("add", lib.t_add, val(a) + val(b)),
            ("sub", lib.t_sub, val(a) - val(b)),
        ):
            r = call(fn, a, b)
            assert all(x < m for x, m in zip(r, LOOSE)), (name, r)
            assert val(r) % P == want % P, name
        r = call(lib.t_one_minus, a)
        assert all(x < m for x, m in zip(r, LOOSE)) and val(r) % P == (1 - val(a)) % P
        r = call(lib.t_one_plus, a)
        assert val(r) % P == (1 + val(a)) % P

        # pack/unpack round-trips any tight value exactly
        p8 = U8()
        lib.t_pack(p8, U10(*h))
        assert sum(int(w) << (32 * i) for i, w in enumerate(p8)) == val(h)
        back = U10()
        lib.t_unpack(back, p8)
        assert list(back) == h

        # canonical low 64 bits of a tight value
        v = val(h)
        if not (P <= v < 2**255):
            want = v % P & (2**64 - 1)
            got = lib.t_low32(U10(*h)) | (lib.t_high32(U10(*h)) << 32)
            assert got == want, ("low64", h)
        n += 1

    # low64 on values straddling 2^255 (the one case needing the +19 correction)
    for v in (2**255, 2**255 + 5, 2**255 + 2**242, 2**255 - 20, 2**64 - 19, 2**255 + 2**64 - 19):
        l = canonical_limbs(v % 2**255)
        l[9] |= (v >> 255) << 25
        got = lib.t_low32(U10(*l)) | (lib.t_high32(U10(*l)) << 32)
        assert got == (v % P) & (2**64 - 1), hex(v)

    for it in range(300):
        a = canonical_limbs(rng.randrange(1, P))
        r = call(lib.t_invert, a)
        assert val(r) * val(a) % P == 1
    # Uniformly random operands: the probe may only be wrong where it says it is unsure,
    # and that has to stay rare (expected: 1 in 4096) or the kernel's fallback gets hot.
    forced = 0
    for it in range(200000):
        f, g = [rng.randrange(b) for b in TIGHT], [rng.randrange(b) for b in TIGHT]
        unsure = ctypes.c_uint32()
        lo = lib.t_probe(ctypes.byref(unsure), U10(*f), U10(*g))
        if not unsure.value:
            assert lo == (val(f) * val(g) % P) & 0xFFFFFFFF, ("probe", f, g)
        else:
            forced += 1
    assert forced < 150, f"probe unsure {forced} times in 200000"
    print(f"field: ok ({n} rounds; probe unsure on {n_unsure} edge-case and {forced}/200000 random products)")

if __name__ == "__main__":
    main()
