// C ABI over the kernel field arithmetic so tests/test_field.py can drive it with
// Python big integers as the oracle. Built once per field arithmetic (-DCIP_ARITH=n).
#define CIP_TARGET_CPU
#include "kernel/defs.h"
#include "kernel/field.h"

static fe in(const u32 *p)
{
    fe r;
    for (int i = 0; i < 10; i++) r.v[i] = p[i];
    return r;
}

static void out(u32 *p, fe a)
{
    for (int i = 0; i < 10; i++) p[i] = a.v[i];
}

#if defined(_WIN32)
#define T_API __declspec(dllexport)
#else
#define T_API
#endif

extern "C" {
T_API u32 t_arith() { return CIP_ARITH; }
T_API void t_mul(u32 *r, const u32 *a, const u32 *b) { out(r, fe_mul(in(a), in(b))); }
T_API void t_mul_t(u32 *r, const u32 *a, const u32 *b) { out(r, fe_mul_t(in(a), in(b))); }
T_API void t_sq(u32 *r, const u32 *a) { out(r, fe_sq(in(a))); }
T_API void t_carry(u32 *r, const u32 *a) { out(r, fe_carry(in(a))); }
T_API void t_add(u32 *r, const u32 *a, const u32 *b) { out(r, fe_add(in(a), in(b))); }
T_API void t_sub(u32 *r, const u32 *a, const u32 *b) { out(r, fe_sub(in(a), in(b))); }
T_API void t_one_minus(u32 *r, const u32 *a) { out(r, fe_one_minus(in(a))); }
T_API void t_one_plus(u32 *r, const u32 *a) { out(r, fe_one_plus(in(a))); }
T_API void t_invert(u32 *r, const u32 *a) { out(r, fe_invert(in(a))); }
T_API void t_invert_gcd(u32 *r, const u32 *a) { out(r, fe_invert_gcd(in(a))); }
T_API void t_pack(u32 *r8, const u32 *a)
{
    fe8 p = fe_pack(in(a));
    for (int i = 0; i < 8; i++) r8[i] = p.w[i];
}
T_API void t_unpack(u32 *r, const u32 *a8)
{
    fe8 p;
    for (int i = 0; i < 8; i++) p.w[i] = a8[i];
    out(r, fe_unpack(p));
}
T_API u32 t_probe(u32 *unsure, const u32 *a, const u32 *b)
{
    fe_probe p = fe_mul_probe(in(a), in(b));
    *unsure = p.unsure;
    return p.lo;
}
T_API u32 t_low32(const u32 *a) { return fe_low32(in(a)); }
T_API u32 t_high32(const u32 *a) { return fe_high32(in(a)); }
}
