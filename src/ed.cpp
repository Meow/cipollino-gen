#include "ed.h"

#include <sodium.h>

#include <cstring>
#include <stdexcept>

namespace ed {

static fe g_d;       // -121665/121666
static fe g_sqrtm1;  // sqrt(-1)

static fe small(uint32_t v)
{
    fe r = fe_one();
    r.v[0] = v;
    return r;
}

static fe neg(fe a)
{
    fe zero = fe_one();
    zero.v[0] = 0;
    return fe_carry(fe_sub(zero, fe_carry(a)));
}

fe from_bytes(const uint8_t s[32])
{
    static const int off[10] = {0, 26, 51, 77, 102, 128, 153, 179, 204, 230};
    static const int bits[10] = {26, 25, 26, 25, 26, 25, 26, 25, 26, 25};
    uint8_t buf[40] = {0};
    memcpy(buf, s, 32);
    buf[31] &= 0x7f;
    fe r;
    for (int i = 0; i < 10; i++) {
        uint64_t w;
        memcpy(&w, buf + off[i] / 8, 8);  // little-endian hosts only (x86-64, arm64)
        r.v[i] = (uint32_t)(w >> (off[i] % 8)) & ((1u << bits[i]) - 1);
    }
    return r;
}

void to_bytes(uint8_t s[32], fe a)
{
    fe8 p = fe_pack(fe_carry(a));
    uint64_t w[4];
    for (int i = 0; i < 4; i++) w[i] = (uint64_t)p.w[2 * i] | ((uint64_t)p.w[2 * i + 1] << 32);
    // The packed value is < 2^256 < 3p: subtract p at most twice.
    static const uint64_t pw[4] = {0xffffffffffffffedull, 0xffffffffffffffffull, 0xffffffffffffffffull,
                                   0x7fffffffffffffffull};
    for (int round = 0; round < 2; round++) {
        uint64_t t[4];
        unsigned __int128 borrow = 0;
        for (int i = 0; i < 4; i++) {
            unsigned __int128 d = (unsigned __int128)w[i] - pw[i] - (uint64_t)borrow;
            t[i] = (uint64_t)d;
            borrow = (d >> 64) & 1;
        }
        if (!borrow) memcpy(w, t, sizeof t);
    }
    memcpy(s, w, 32);
}

fe canonical(fe a)
{
    uint8_t s[32];
    to_bytes(s, a);
    return from_bytes(s);
}

static bool equal(fe a, fe b)
{
    uint8_t x[32], y[32];
    to_bytes(x, a);
    to_bytes(y, b);
    return memcmp(x, y, 32) == 0;
}

void init()
{
    g_d = neg(fe_mul(small(121665), fe_invert(small(121666))));
    // sqrt(-1) = 2^((p-1)/4) = 2^(2^253 - 5) = (2^(2^250 - 1))^8 * 2^3.
    fe two = small(2);
    fe_pow250 t = fe_pow_2_250_1(two);
    g_sqrtm1 = fe_mul(fe_sqn(t.z250, 3), small(8));
    fe m1 = neg(fe_one());
    if (!equal(fe_sq(g_sqrtm1), m1)) throw std::runtime_error("ed::init: sqrt(-1) self-check failed");
}

bool decompress(Affine &out, const uint8_t pk[32])
{
    fe y = from_bytes(pk);
    fe y2 = fe_sq(y);
    fe u = fe_carry(fe_sub(y2, fe_one()));              // y^2 - 1
    fe v = fe_carry(fe_one_plus(fe_mul(y2, g_d)));      // d y^2 + 1
    // x = u v^3 (u v^7)^((p-5)/8), (p-5)/8 = 2^252 - 3
    fe v3 = fe_mul(fe_sq(v), v);
    fe v7 = fe_mul(fe_sq(v3), v);
    fe uv7 = fe_mul(u, v7);
    fe_pow250 t = fe_pow_2_250_1(uv7);
    fe pw = fe_mul(fe_sqn(t.z250, 2), uv7);             // (uv^7)^(2^252 - 3)
    fe x = fe_mul(fe_mul(u, v3), pw);
    fe vx2 = fe_mul(v, fe_sq(x));
    if (!equal(vx2, u)) {
        if (!equal(vx2, neg(u))) return false;
        x = fe_mul(x, g_sqrtm1);
    }
    uint8_t xb[32];
    to_bytes(xb, x);
    if ((xb[0] & 1) != (pk[31] >> 7)) x = neg(x);
    out.x = canonical(x);
    out.y = canonical(y);
    // Belt and braces: the curve equation, and that re-encoding gives the input back.
    fe x2 = fe_sq(out.x), yy = fe_sq(out.y);
    fe lhs = fe_carry(fe_sub(yy, x2));
    fe rhs = fe_carry(fe_one_plus(fe_mul(g_d, fe_mul(x2, yy))));
    uint8_t back[32];
    compress(back, out);
    return equal(lhs, rhs) && memcmp(back, pk, 32) == 0;
}

void compress(uint8_t pk[32], const Affine &p)
{
    uint8_t xb[32];
    to_bytes(pk, p.y);
    to_bytes(xb, p.x);
    pk[31] |= (uint8_t)((xb[0] & 1) << 7);
}

Affine base_mult_u64(uint64_t k)
{
    uint8_t s[32] = {0}, pk[32];
    for (int i = 0; i < 8; i++) s[i] = (uint8_t)(k >> (8 * i));
    Affine p;
    if (crypto_scalarmult_ed25519_base_noclamp(pk, s) != 0 || !decompress(p, pk))
        throw std::runtime_error("ed::base_mult_u64 failed");
    return p;
}

static void put(std::vector<uint32_t> &t, size_t at, fe a)
{
    for (int i = 0; i < 10; i++) t[at + i] = a.v[i];
}

std::vector<uint32_t> build_table(uint32_t npairs)
{
    const size_t N = npairs;
    std::vector<uint32_t> t(40 * N + 30);
    for (size_t i = 0; i <= N; i++) {
        // i < N: Q_{i+1} = 8(i+1)*B. i == N: the step 8(2N+1)*B.
        Affine q = base_mult_u64(i < N ? 8 * (uint64_t)(i + 1) : 8 * (2 * (uint64_t)N + 1));
        fe k = canonical(fe_mul(g_d, fe_mul(q.x, q.y)));
        size_t at = 10 * N + 30 * i;
        put(t, at, k);
        put(t, at + 10, q.y);
        put(t, at + 20, q.x);
        if (i < N) put(t, 10 * i, canonical(fe_sq(k)));
    }
    return t;
}

}  // namespace ed
