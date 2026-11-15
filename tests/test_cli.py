#!/usr/bin/env python3
"""End-to-end test: run the real binary and re-verify everything it wrote with an
independent pure-Python Ed25519 (RFC 8032 reference style) and hashlib's SHA3.

usage: test_cli.py path/to/cipollino backend
"""
import base64
import hashlib
import os
import random
import stat
import subprocess
import sys
import tempfile

P = 2**255 - 19
L = 2**252 + 27742317777372353535851937790883648493
D = -121665 * pow(121666, P - 2, P) % P
SQRT_M1 = pow(2, (P - 1) // 4, P)


def recover_x(y, sign):
    x2 = (y * y - 1) * pow(D * y * y + 1, P - 2, P) % P
    x = pow(x2, (P + 3) // 8, P)
    if (x * x - x2) % P:
        x = x * SQRT_M1 % P
    assert (x * x - x2) % P == 0
    return P - x if (x & 1) != sign else x


GY = 4 * pow(5, P - 2, P) % P
G = (recover_x(GY, 0), GY, 1, recover_x(GY, 0) * GY % P)


def add(p, q):
    a = (p[1] - p[0]) * (q[1] - q[0]) % P
    b = (p[1] + p[0]) * (q[1] + q[0]) % P
    c = 2 * p[3] * q[3] * D % P
    d = 2 * p[2] * q[2] % P
    e, f, g, h = b - a, d - c, d + c, b + a
    return (e * f % P, g * h % P, f * g % P, e * h % P)


def mul(s, p):
    q = (0, 1, 1, 0)
    while s:
        if s & 1:
            q = add(q, p)
        p = add(p, p)
        s >>= 1
    return q


def compress(p):
    zi = pow(p[2], P - 2, P)
    x, y = p[0] * zi % P, p[1] * zi % P
    return (y | ((x & 1) << 255)).to_bytes(32, "little")


def decompress(b):
    y = int.from_bytes(b, "little")
    sign, y = y >> 255, y & (2**255 - 1)
    assert y < P
    x = recover_x(y, sign)
    return (x, y, 1, x * y % P)


def h512(*parts):
    return int.from_bytes(hashlib.sha512(b"".join(parts)).digest(), "little")


def sign_expanded(sk, pk, msg):
    a, prefix = int.from_bytes(sk[:32], "little"), sk[32:]
    r = h512(prefix, msg) % L
    R = compress(mul(r, G))
    s = (r + h512(R, pk, msg) * a) % L
    return R + s.to_bytes(32, "little")


def verify(pk, msg, sig):
    R, s = sig[:32], int.from_bytes(sig[32:], "little")
    assert s < L
    k = h512(R, pk, msg) % L
    lhs = compress(mul(s, G))
    rhs = compress(add(decompress(R), mul(k, decompress(pk))))
    return lhs == rhs


def onion_address(pk):
    checksum = hashlib.sha3_256(b".onion checksum" + pk + b"\x03").digest()[:2]
    return base64.b32encode(pk + checksum + b"\x03").decode().lower()


def check_key_dir(path, prefixes):
    name = os.path.basename(path)
    assert name.endswith(".onion") and len(name) == 62, name
    addr = name[:-6]
    posix = os.name == "posix"  # Windows has access lists instead of modes
    assert not posix or stat.S_IMODE(os.stat(path).st_mode) == 0o700, "directory mode"
    assert sorted(os.listdir(path)) == ["hostname", "hs_ed25519_public_key", "hs_ed25519_secret_key"]
    assert not posix or stat.S_IMODE(os.stat(os.path.join(path, "hs_ed25519_secret_key")).st_mode) == 0o600, "secret key mode"
    assert open(os.path.join(path, "hostname"), "rb").read() == (name + "\n").encode()
    pub = open(os.path.join(path, "hs_ed25519_public_key"), "rb").read()
    sec = open(os.path.join(path, "hs_ed25519_secret_key"), "rb").read()
    assert pub[:32] == b"== ed25519v1-public: type0 ==\0\0\0" and len(pub) == 64
    assert sec[:32] == b"== ed25519v1-secret: type0 ==\0\0\0" and len(sec) == 96
    pk, sk = pub[32:], sec[32:]

    assert onion_address(pk) == addr, "address does not belong to the public key"
    assert any(addr.startswith(p) for p in prefixes), f"{addr} matches none of {prefixes}"
    a = int.from_bytes(sk[:32], "little")
    assert a % 8 == 0 and 2**254 <= a < 2**255, "scalar is not clamped"
    assert compress(mul(a, G)) == pk, "public key does not belong to the secret scalar"
    assert compress(mul(L, decompress(pk))) == compress((0, 1, 1, 0)), "public key not in the prime-order group"
    msg = b"descriptor signing key certificate, more or less"
    assert verify(pk, msg, sign_expanded(sk, pk, msg)), "signature does not verify"
    return addr, sk


def run(binary, backend, args, expect=0):
    cmd = [binary, "--backend", backend, "-q"] + args
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=600)
    assert r.returncode == expect, f"{' '.join(cmd)} -> {r.returncode}\n{r.stderr}"
    return r


def main():
    binary, backend = sys.argv[1], sys.argv[2]
    rng = random.Random(7)
    alphabet = "abcdefghijklmnopqrstuvwxyz234567"

    with tempfile.TemporaryDirectory() as tmp:
        # a few prefixes (direct filter mode), stop after exactly N
        out = os.path.join(tmp, "few")
        prefixes = ["ab", "q7", "zzz"]
        r = run(binary, backend, ["-n", "5", "-d", out] + prefixes)
        listed = r.stdout.split()
        dirs = sorted(os.listdir(out))
        assert len(dirs) == 5 and sorted(listed) == dirs, (listed, dirs)
        seen = [check_key_dir(os.path.join(out, d), prefixes) for d in dirs]
        assert len({sk for _, sk in seen}) == 5

        # no two keys may share a seed: nonce prefixes (and scalars) must all differ
        assert len({sk[32:] for _, sk in seen}) == 5, "two keys share a nonce prefix"

        # many prefixes from a file (bitmap filter mode), upper case and .onion accepted
        out = os.path.join(tmp, "many")
        many = sorted({"".join(rng.choice(alphabet) for _ in range(3)) for _ in range(60)})
        listfile = os.path.join(tmp, "prefixes.txt")
        with open(listfile, "w") as f:
            f.write("# comment\n\n" + "\n".join(p.upper() + ".onion" if i % 2 else p for i, p in enumerate(many)) + "\n")
        r = run(binary, backend, ["-n", "4", "-d", out, "-f", listfile])
        dirs = sorted(os.listdir(out))
        assert len(dirs) == 4
        for d in dirs:
            check_key_dir(os.path.join(out, d), many)

        # a longer single prefix
        out = os.path.join(tmp, "one")
        r = run(binary, backend, ["-d", out, "onio"])
        (d,) = os.listdir(out)
        check_key_dir(os.path.join(out, d), ["onio"])

        # bad input is rejected before any work happens
        for bad in (["abc1"], ["has space"], ["a" * 57], []):
            run(binary, backend, ["-d", os.path.join(tmp, "bad")] + bad, expect=2)
        assert not os.path.exists(os.path.join(tmp, "bad"))

    print(f"cli ({backend}): ok")


if __name__ == "__main__":
    main()
