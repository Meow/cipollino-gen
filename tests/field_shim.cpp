// C ABI over the kernel field arithmetic so tests/test_field.py can drive it with
// Python big integers as the oracle.
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

extern "C" {
void t_mul(u32 *r, const u32 *a, const u32 *b) { out(r, fe_mul(in(a), in(b))); }
void t_sq(u32 *r, const u32 *a) { out(r, fe_sq(in(a))); }
void t_carry(u32 *r, const u32 *a) { out(r, fe_carry(in(a))); }
void t_add(u32 *r, const u32 *a, const u32 *b) { out(r, fe_add(in(a), in(b))); }
void t_sub(u32 *r, const u32 *a, const u32 *b) { out(r, fe_sub(in(a), in(b))); }
void t_one_minus(u32 *r, const u32 *a) { out(r, fe_one_minus(in(a))); }
void t_one_plus(u32 *r, const u32 *a) { out(r, fe_one_plus(in(a))); }
void t_invert(u32 *r, const u32 *a) { out(r, fe_invert(in(a))); }
void t_pack(u32 *r8, const u32 *a)
{
    fe8 p = fe_pack(in(a));
    for (int i = 0; i < 8; i++) r8[i] = p.w[i];
}
void t_unpack(u32 *r, const u32 *a8)
{
    fe8 p;
    for (int i = 0; i < 8; i++) p.w[i] = a8[i];
    out(r, fe_unpack(p));
}
u32 t_probe(u32 *unsure, const u32 *a, const u32 *b)
{
    fe_probe p = fe_mul_probe(in(a), in(b));
    *unsure = p.unsure;
    return p.lo;
}
u32 t_low32(const u32 *a) { return fe_low32(in(a)); }
u32 t_high32(const u32 *a) { return fe_high32(in(a)); }
}
