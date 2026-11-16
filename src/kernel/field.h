// cipollino kernel: arithmetic in GF(2^255 - 19).
//
// Radix 2^25.5: ten unsigned limbs, alternately 26 and 25 bits wide, at bit offsets
// 0,26,51,77,102,128,153,179,204,230. Every 32x32->64 partial product accumulates into a
// u64 column without carry handling, which is what GPUs (no carry flag in MSL, IMAD.WIDE
// on NVIDIA) are good at.
//
// Limb classes used below ("even" = limbs 0,2,4,6,8; "odd" = 1,3,5,7; "top" = limb 9):
//
//   tight : even < 2^26, odd < 2^25, top < 2^26.   Output of fe_mul/fe_sq/fe_carry.
//           The value is < 2^256 but not necessarily < p.
//   loose : even < 3*2^26, odd < 3*2^25, top < 5*2^25.   Output of one fe_add/fe_sub of
//           tight values.
//
// fe_mul accepts loose inputs: the largest column is then below 2^62.3, and every u32
// premultiple (19*g, 2*f) fits in 32 bits. fe_sq requires tight input.

struct fe {
  u32 v[10];
};

#define CIP_M26 0x3ffffffu
#define CIP_M25 0x1ffffffu
#ifndef CIP_MUL // a 32x32->64 product; defs.h may supply a target-specific spelling
#define CIP_MUL(a, b) (CIP_U64(a) * CIP_U64(b))
#endif

CIP_FN fe fe_one() {
  fe r;
  r.v[0] = 1u;
  r.v[1] = 0u;
  r.v[2] = 0u;
  r.v[3] = 0u;
  r.v[4] = 0u;
  r.v[5] = 0u;
  r.v[6] = 0u;
  r.v[7] = 0u;
  r.v[8] = 0u;
  r.v[9] = 0u;
  return r;
}

// Carry ten u64 columns (each < 2^63) down to a tight field element.
//
// q underestimates the true carry out of bit 255 by at most 1 (the carries it ignores
// add less than 2^13 to column 9), so folding 19*q into column 0 up front lets a single
// pass finish with limbs 0..8 exactly masked and limb 9 < 2^25 + 2^13: no wrap-around
// second pass, and the result packs into 256 bits with plain shifts.
CIP_FN fe fe_reduce(u64 h0, u64 h1, u64 h2, u64 h3, u64 h4, u64 h5, u64 h6, u64 h7, u64 h8, u64 h9) {
  fe r;
  u64 q = (h9 + (h8 >> 26)) >> 25;
  h0 += q * CIP_U64(19u);
  h1 += h0 >> 26;
  r.v[0] = CIP_U32(h0) & CIP_M26;
  h2 += h1 >> 25;
  r.v[1] = CIP_U32(h1) & CIP_M25;
  h3 += h2 >> 26;
  r.v[2] = CIP_U32(h2) & CIP_M26;
  h4 += h3 >> 25;
  r.v[3] = CIP_U32(h3) & CIP_M25;
  h5 += h4 >> 26;
  r.v[4] = CIP_U32(h4) & CIP_M26;
  h6 += h5 >> 25;
  r.v[5] = CIP_U32(h5) & CIP_M25;
  h7 += h6 >> 26;
  r.v[6] = CIP_U32(h6) & CIP_M26;
  h8 += h7 >> 25;
  r.v[7] = CIP_U32(h7) & CIP_M25;
  h9 += h8 >> 26;
  r.v[8] = CIP_U32(h8) & CIP_M26;
  r.v[9] = CIP_U32(h9 - (q << 25));
  return r;
}

CIP_FN fe fe_carry(fe a) {
  return fe_reduce(
    CIP_U64(a.v[0]),
    CIP_U64(a.v[1]),
    CIP_U64(a.v[2]),
    CIP_U64(a.v[3]),
    CIP_U64(a.v[4]),
    CIP_U64(a.v[5]),
    CIP_U64(a.v[6]),
    CIP_U64(a.v[7]),
    CIP_U64(a.v[8]),
    CIP_U64(a.v[9])
  );
}

CIP_FN fe fe_add(fe a, fe b) {
  fe r;
  r.v[0] = a.v[0] + b.v[0];
  r.v[1] = a.v[1] + b.v[1];
  r.v[2] = a.v[2] + b.v[2];
  r.v[3] = a.v[3] + b.v[3];
  r.v[4] = a.v[4] + b.v[4];
  r.v[5] = a.v[5] + b.v[5];
  r.v[6] = a.v[6] + b.v[6];
  r.v[7] = a.v[7] + b.v[7];
  r.v[8] = a.v[8] + b.v[8];
  r.v[9] = a.v[9] + b.v[9];
  return r;
}

// 3p spread over the limbs so that every limb dominates the same limb of a tight value:
// (2^27-57, 2^26-2, 2^27-2, ..., 2^27-2, 3*2^25-2).
#define CIP_B0 134217671u
#define CIP_BO 67108862u
#define CIP_BE 134217726u
#define CIP_B9 100663294u

// a - b for tight b (and tight a, if the result feeds fe_mul).
CIP_FN fe fe_sub(fe a, fe b) {
  fe r;
  r.v[0] = a.v[0] + (CIP_B0 - b.v[0]);
  r.v[1] = a.v[1] + (CIP_BO - b.v[1]);
  r.v[2] = a.v[2] + (CIP_BE - b.v[2]);
  r.v[3] = a.v[3] + (CIP_BO - b.v[3]);
  r.v[4] = a.v[4] + (CIP_BE - b.v[4]);
  r.v[5] = a.v[5] + (CIP_BO - b.v[5]);
  r.v[6] = a.v[6] + (CIP_BE - b.v[6]);
  r.v[7] = a.v[7] + (CIP_BO - b.v[7]);
  r.v[8] = a.v[8] + (CIP_BE - b.v[8]);
  r.v[9] = a.v[9] + (CIP_B9 - b.v[9]);
  return r;
}

// 1 - b and 1 + b for tight b.
CIP_FN fe fe_one_minus(fe b) {
  fe r;
  r.v[0] = (CIP_B0 + 1u) - b.v[0];
  r.v[1] = CIP_BO - b.v[1];
  r.v[2] = CIP_BE - b.v[2];
  r.v[3] = CIP_BO - b.v[3];
  r.v[4] = CIP_BE - b.v[4];
  r.v[5] = CIP_BO - b.v[5];
  r.v[6] = CIP_BE - b.v[6];
  r.v[7] = CIP_BO - b.v[7];
  r.v[8] = CIP_BE - b.v[8];
  r.v[9] = CIP_B9 - b.v[9];
  return r;
}

CIP_FN fe fe_one_plus(fe b) {
  b.v[0] += 1u;
  return b;
}

// The result of fe_mul_probe.
struct fe_probe {
  u32 lo;     // low 32 bits of the canonical product, valid if unsure == 0
  u32 unsure; // nonzero: lo may be 19 too small
};

// Multiplication comes in two implementations with one contract. The one below builds
// 64-bit columns from 32x32 products; field13.h gets by on 24-bit multipliers, for GPUs
// where the wide ones are slow. fe_mul_t is fe_mul for the operands of the search loop's
// own products, which field13.h can take as they are: f tight, and g tight or one of
// fe_one_minus, fe_one_plus of a tight value. fe_mul_tl is that with any loose g.
#if CIP_ARITH == CIP_ARITH_NARROW
#include "field13.h"
#define fe_mul_tl(f, g) fe_mul_t((f), fe_weak(g))
#else

#define fe_mul_t(f, g)  fe_mul((f), (g))
#define fe_mul_tl(f, g) fe_mul((f), (g))

// h = f * g. Term f_i*g_j lands in column (i+j) mod 10, times 19 if it wrapped, times 2
// if i and j are both odd (two 25-bit limbs overshoot the column offset by one bit).
CIP_FN fe fe_mul(fe f, fe g) {
  u32 f0 = f.v[0], f1 = f.v[1], f2 = f.v[2], f3 = f.v[3], f4 = f.v[4];
  u32 f5 = f.v[5], f6 = f.v[6], f7 = f.v[7], f8 = f.v[8], f9 = f.v[9];
  u32 g0 = g.v[0], g1 = g.v[1], g2 = g.v[2], g3 = g.v[3], g4 = g.v[4];
  u32 g5 = g.v[5], g6 = g.v[6], g7 = g.v[7], g8 = g.v[8], g9 = g.v[9];
  u32 g1_19 = 19u * g1, g2_19 = 19u * g2, g3_19 = 19u * g3, g4_19 = 19u * g4, g5_19 = 19u * g5;
  u32 g6_19 = 19u * g6, g7_19 = 19u * g7, g8_19 = 19u * g8, g9_19 = 19u * g9;
  u32 f1_2 = 2u * f1, f3_2 = 2u * f3, f5_2 = 2u * f5, f7_2 = 2u * f7, f9_2 = 2u * f9;

  u64 h0 = CIP_MUL(f0, g0) + CIP_MUL(f1_2, g9_19) + CIP_MUL(f2, g8_19) + CIP_MUL(f3_2, g7_19) + CIP_MUL(f4, g6_19) +
           CIP_MUL(f5_2, g5_19) + CIP_MUL(f6, g4_19) + CIP_MUL(f7_2, g3_19) + CIP_MUL(f8, g2_19) + CIP_MUL(f9_2, g1_19);
  u64 h1 = CIP_MUL(f0, g1) + CIP_MUL(f1, g0) + CIP_MUL(f2, g9_19) + CIP_MUL(f3, g8_19) + CIP_MUL(f4, g7_19) +
           CIP_MUL(f5, g6_19) + CIP_MUL(f6, g5_19) + CIP_MUL(f7, g4_19) + CIP_MUL(f8, g3_19) + CIP_MUL(f9, g2_19);
  u64 h2 = CIP_MUL(f0, g2) + CIP_MUL(f1_2, g1) + CIP_MUL(f2, g0) + CIP_MUL(f3_2, g9_19) + CIP_MUL(f4, g8_19) +
           CIP_MUL(f5_2, g7_19) + CIP_MUL(f6, g6_19) + CIP_MUL(f7_2, g5_19) + CIP_MUL(f8, g4_19) + CIP_MUL(f9_2, g3_19);
  u64 h3 = CIP_MUL(f0, g3) + CIP_MUL(f1, g2) + CIP_MUL(f2, g1) + CIP_MUL(f3, g0) + CIP_MUL(f4, g9_19) +
           CIP_MUL(f5, g8_19) + CIP_MUL(f6, g7_19) + CIP_MUL(f7, g6_19) + CIP_MUL(f8, g5_19) + CIP_MUL(f9, g4_19);
  u64 h4 = CIP_MUL(f0, g4) + CIP_MUL(f1_2, g3) + CIP_MUL(f2, g2) + CIP_MUL(f3_2, g1) + CIP_MUL(f4, g0) +
           CIP_MUL(f5_2, g9_19) + CIP_MUL(f6, g8_19) + CIP_MUL(f7_2, g7_19) + CIP_MUL(f8, g6_19) + CIP_MUL(f9_2, g5_19);
  u64 h5 = CIP_MUL(f0, g5) + CIP_MUL(f1, g4) + CIP_MUL(f2, g3) + CIP_MUL(f3, g2) + CIP_MUL(f4, g1) + CIP_MUL(f5, g0) +
           CIP_MUL(f6, g9_19) + CIP_MUL(f7, g8_19) + CIP_MUL(f8, g7_19) + CIP_MUL(f9, g6_19);
  u64 h6 = CIP_MUL(f0, g6) + CIP_MUL(f1_2, g5) + CIP_MUL(f2, g4) + CIP_MUL(f3_2, g3) + CIP_MUL(f4, g2) +
           CIP_MUL(f5_2, g1) + CIP_MUL(f6, g0) + CIP_MUL(f7_2, g9_19) + CIP_MUL(f8, g8_19) + CIP_MUL(f9_2, g7_19);
  u64 h7 = CIP_MUL(f0, g7) + CIP_MUL(f1, g6) + CIP_MUL(f2, g5) + CIP_MUL(f3, g4) + CIP_MUL(f4, g3) + CIP_MUL(f5, g2) +
           CIP_MUL(f6, g1) + CIP_MUL(f7, g0) + CIP_MUL(f8, g9_19) + CIP_MUL(f9, g8_19);
  u64 h8 = CIP_MUL(f0, g8) + CIP_MUL(f1_2, g7) + CIP_MUL(f2, g6) + CIP_MUL(f3_2, g5) + CIP_MUL(f4, g4) +
           CIP_MUL(f5_2, g3) + CIP_MUL(f6, g2) + CIP_MUL(f7_2, g1) + CIP_MUL(f8, g0) + CIP_MUL(f9_2, g9_19);
  u64 h9 = CIP_MUL(f0, g9) + CIP_MUL(f1, g8) + CIP_MUL(f2, g7) + CIP_MUL(f3, g6) + CIP_MUL(f4, g5) + CIP_MUL(f5, g4) +
           CIP_MUL(f6, g3) + CIP_MUL(f7, g2) + CIP_MUL(f8, g1) + CIP_MUL(f9, g0);
  return fe_reduce(h0, h1, h2, h3, h4, h5, h6, h7, h8, h9);
}

// h = f * f, f tight. 55 products instead of 100.
CIP_FN fe fe_sq(fe f) {
  u32 f0 = f.v[0], f1 = f.v[1], f2 = f.v[2], f3 = f.v[3], f4 = f.v[4];
  u32 f5 = f.v[5], f6 = f.v[6], f7 = f.v[7], f8 = f.v[8], f9 = f.v[9];
  u32 f0_2 = 2u * f0, f1_2 = 2u * f1, f2_2 = 2u * f2, f3_2 = 2u * f3, f4_2 = 2u * f4;
  u32 f5_2 = 2u * f5, f6_2 = 2u * f6, f7_2 = 2u * f7;
  u32 f5_38 = 38u * f5, f6_19 = 19u * f6, f7_38 = 38u * f7, f8_19 = 19u * f8, f9_38 = 38u * f9;

  u64 h0 = CIP_MUL(f0, f0) + CIP_MUL(f1_2, f9_38) + CIP_MUL(f2_2, f8_19) + CIP_MUL(f3_2, f7_38) + CIP_MUL(f4_2, f6_19) +
           CIP_MUL(f5, f5_38);
  u64 h1 = CIP_MUL(f0_2, f1) + CIP_MUL(f2, f9_38) + CIP_MUL(f3_2, f8_19) + CIP_MUL(f4, f7_38) + CIP_MUL(f5_2, f6_19);
  u64 h2 = CIP_MUL(f0_2, f2) + CIP_MUL(f1_2, f1) + CIP_MUL(f3_2, f9_38) + CIP_MUL(f4_2, f8_19) + CIP_MUL(f5_2, f7_38) +
           CIP_MUL(f6, f6_19);
  u64 h3 = CIP_MUL(f0_2, f3) + CIP_MUL(f1_2, f2) + CIP_MUL(f4, f9_38) + CIP_MUL(f5_2, f8_19) + CIP_MUL(f6, f7_38);
  u64 h4 = CIP_MUL(f0_2, f4) + CIP_MUL(f1_2, f3_2) + CIP_MUL(f2, f2) + CIP_MUL(f5_2, f9_38) + CIP_MUL(f6_2, f8_19) +
           CIP_MUL(f7, f7_38);
  u64 h5 = CIP_MUL(f0_2, f5) + CIP_MUL(f1_2, f4) + CIP_MUL(f2_2, f3) + CIP_MUL(f6, f9_38) + CIP_MUL(f7_2, f8_19);
  u64 h6 = CIP_MUL(f0_2, f6) + CIP_MUL(f1_2, f5_2) + CIP_MUL(f2_2, f4) + CIP_MUL(f3_2, f3) + CIP_MUL(f7_2, f9_38) +
           CIP_MUL(f8, f8_19);
  u64 h7 = CIP_MUL(f0_2, f7) + CIP_MUL(f1_2, f6) + CIP_MUL(f2_2, f5) + CIP_MUL(f3_2, f4) + CIP_MUL(f8, f9_38);
  u64 h8 = CIP_MUL(f0_2, f8) + CIP_MUL(f1_2, f7_2) + CIP_MUL(f2_2, f6) + CIP_MUL(f3_2, f5_2) + CIP_MUL(f4, f4) +
           CIP_MUL(f9, f9_38);
  u64 h9 = CIP_MUL(f0_2, f9) + CIP_MUL(f1_2, f8) + CIP_MUL(f2_2, f7) + CIP_MUL(f3_2, f6) + CIP_MUL(f4_2, f5);
  return fe_reduce(h0, h1, h2, h3, h4, h5, h6, h7, h8, h9);
}

// The low 32 bits of the canonical value of f*g, for a fraction of the cost of fe_mul.
//
// Write the unreduced product as V = sum h_k 2^offset_k. Its canonical value is
// R + 19*Q with R = V mod 2^255 and Q = floor(V / 2^255) (plus one more 19 with
// probability 2^-200, ignored as in fe_low32). The low 32 bits of R depend only on the
// low bits of columns 0 and 1, which plain wrapping 32-bit multiplies give exactly, and
// Q = floor((h9 + h8/2^26 + h7/2^51 + ...) / 2^25) is determined by columns 8 and 9
// alone unless the dropped terms (less than 2^12 + 2 in total) can carry across a
// multiple of 2^25. That case is reported as `unsure` (about one product in 4000) and
// the caller falls back to fe_mul. So a candidate costs 20 wide and 20 narrow
// multiplications and no carry chain, instead of 100 wide ones and a full reduction.
CIP_FN fe_probe fe_mul_probe(fe f, fe g) {
  u32 f0 = f.v[0], f1 = f.v[1], f2 = f.v[2], f3 = f.v[3], f4 = f.v[4];
  u32 f5 = f.v[5], f6 = f.v[6], f7 = f.v[7], f8 = f.v[8], f9 = f.v[9];
  u32 g0 = g.v[0], g1 = g.v[1], g2 = g.v[2], g3 = g.v[3], g4 = g.v[4];
  u32 g5 = g.v[5], g6 = g.v[6], g7 = g.v[7], g8 = g.v[8], g9 = g.v[9];
  u32 g1_19 = 19u * g1, g2_19 = 19u * g2, g3_19 = 19u * g3, g4_19 = 19u * g4, g5_19 = 19u * g5;
  u32 g6_19 = 19u * g6, g7_19 = 19u * g7, g8_19 = 19u * g8, g9_19 = 19u * g9;
  u32 f1_2 = 2u * f1, f3_2 = 2u * f3, f5_2 = 2u * f5, f7_2 = 2u * f7, f9_2 = 2u * f9;

  u32 l0 = f0 * g0 + f1_2 * g9_19 + f2 * g8_19 + f3_2 * g7_19 + f4 * g6_19 + f5_2 * g5_19 + f6 * g4_19 + f7_2 * g3_19 +
           f8 * g2_19 + f9_2 * g1_19;
  u32 l1 = f0 * g1 + f1 * g0 + f2 * g9_19 + f3 * g8_19 + f4 * g7_19 + f5 * g6_19 + f6 * g5_19 + f7 * g4_19 +
           f8 * g3_19 + f9 * g2_19;
  u64 h8 = CIP_MUL(f0, g8) + CIP_MUL(f1_2, g7) + CIP_MUL(f2, g6) + CIP_MUL(f3_2, g5) + CIP_MUL(f4, g4) +
           CIP_MUL(f5_2, g3) + CIP_MUL(f6, g2) + CIP_MUL(f7_2, g1) + CIP_MUL(f8, g0) + CIP_MUL(f9_2, g9_19);
  u64 h9 = CIP_MUL(f0, g9) + CIP_MUL(f1, g8) + CIP_MUL(f2, g7) + CIP_MUL(f3, g6) + CIP_MUL(f4, g5) + CIP_MUL(f5, g4) +
           CIP_MUL(f6, g3) + CIP_MUL(f7, g2) + CIP_MUL(f8, g1) + CIP_MUL(f9, g0);
  u64 n  = h9 + (h8 >> 26);
  fe_probe r;
  r.lo     = l0 + (l1 << 26) + 19u * CIP_U32(n >> 25);
  r.unsure = ((CIP_U32(n) & CIP_M25) >= CIP_M25 - 8192u) ? 1u : 0u;
  return r;
}

#endif // CIP_ARITH

// Out-of-line copies for code that runs once per batch rather than once per pair.
CIP_COLD_FN fe fe_mul_cold(fe f, fe g) { return fe_mul(f, g); }

CIP_COLD_FN fe fe_sq_cold(fe f) { return fe_sq(f); }

// Tight field element <-> eight 32-bit words (the plain little-endian 256-bit integer).
struct fe8 {
  u32 w[8];
};

CIP_FN fe8 fe_pack(fe a) {
  fe8 r;
  r.w[0] = a.v[0] | (a.v[1] << 26);
  r.w[1] = (a.v[1] >> 6) | (a.v[2] << 19);
  r.w[2] = (a.v[2] >> 13) | (a.v[3] << 13);
  r.w[3] = (a.v[3] >> 19) | (a.v[4] << 6);
  r.w[4] = a.v[5] | (a.v[6] << 25);
  r.w[5] = (a.v[6] >> 7) | (a.v[7] << 19);
  r.w[6] = (a.v[7] >> 13) | (a.v[8] << 12);
  r.w[7] = (a.v[8] >> 20) | (a.v[9] << 6);
  return r;
}

CIP_FN fe fe_unpack(fe8 a) {
  fe r;
  r.v[0] = a.w[0] & CIP_M26;
  r.v[1] = ((a.w[0] >> 26) | (a.w[1] << 6)) & CIP_M25;
  r.v[2] = ((a.w[1] >> 19) | (a.w[2] << 13)) & CIP_M26;
  r.v[3] = ((a.w[2] >> 13) | (a.w[3] << 19)) & CIP_M25;
  r.v[4] = a.w[3] >> 6;
  r.v[5] = a.w[4] & CIP_M25;
  r.v[6] = ((a.w[4] >> 25) | (a.w[5] << 7)) & CIP_M26;
  r.v[7] = ((a.w[5] >> 19) | (a.w[6] << 13)) & CIP_M25;
  r.v[8] = ((a.w[6] >> 12) | (a.w[7] << 20)) & CIP_M26;
  r.v[9] = a.w[7] >> 6;
  return r;
}

// 1/z for tight z, by Bernstein and Yang's division steps as libsecp256k1 arranges them:
// f = p and g = z shrink towards 1 and 0 while d and e follow as z's cofactors modulo p.
// A round runs 30 steps on the low limbs alone and applies their sum to the four values
// as one 2x2 matrix. 20 rounds reach g = 0 from any 256-bit value. Values are nine signed
// limbs of 30 bits. Variable time is no concern: everything inverted here is public.
CIP_COLD_FN fe fe_invert(fe z) {
  const u32 m30 = 0x3fffffffu;
  const i32 s30 = 0x3fffffff;
  fe8 w         = fe_pack(z);
  u32 gw[9];
  gw[0] = w.w[0] & m30;
  gw[1] = ((w.w[0] >> 30) | (w.w[1] << 2)) & m30;
  gw[2] = ((w.w[1] >> 28) | (w.w[2] << 4)) & m30;
  gw[3] = ((w.w[2] >> 26) | (w.w[3] << 6)) & m30;
  gw[4] = ((w.w[3] >> 24) | (w.w[4] << 8)) & m30;
  gw[5] = ((w.w[4] >> 22) | (w.w[5] << 10)) & m30;
  gw[6] = ((w.w[5] >> 20) | (w.w[6] << 12)) & m30;
  gw[7] = ((w.w[6] >> 18) | (w.w[7] << 14)) & m30;
  gw[8] = w.w[7] >> 16;
  // A tight value is below 2^255 + 2^243: take p off one that has bit 255.
  gw[0] += 19u * (gw[8] >> 15);
  gw[8] &= 0x7fffu;
  for (u32 k = 0u; k < 8u; k++) {
    gw[k + 1u] += gw[k] >> 30;
    gw[k] &= m30;
  }

  i32 pl[9], f[9], g[9], d[9], e[9];
  for (u32 k = 0u; k < 9u; k++) {
    pl[k] = s30;
    g[k]  = CIP_I32(gw[k]);
    d[k]  = 0;
    e[k]  = 0;
  }
  pl[0] = 0x3fffffed;
  pl[8] = 0x7fff;
  for (u32 k = 0u; k < 9u; k++)
    f[k] = pl[k];
  e[0]     = 1;
  i32 zeta = -1; // -(delta + 1/2) of the paper, delta starting at 1/2

  for (u32 round = 0u; round < 20u; round++) {
    // 30 division steps on the low limbs: f, g -> (u f + v g, q f + r g) / 2^30.
    u32 u = 1u, v = 0u, q = 0u, r = 1u;
    u32 fl = CIP_U32(f[0]), gl = CIP_U32(g[0]);
    for (u32 s = 0u; s < 30u; s++) {
      u32 neg = CIP_U32(zeta >> 31); // all ones if zeta < 0
      u32 odd = 0u - (gl & 1u);
      gl += ((fl ^ neg) - neg) & odd;
      q += ((u ^ neg) - neg) & odd;
      r += ((v ^ neg) - neg) & odd;
      neg &= odd;
      zeta = (zeta ^ CIP_I32(neg)) - 1;
      fl += gl & neg;
      u += q & neg;
      v += r & neg;
      gl >>= 1;
      u <<= 1;
      v <<= 1;
    }
    const i64 tu = CIP_I64(CIP_I32(u)), tv = CIP_I64(CIP_I32(v)), tq = CIP_I64(CIP_I32(q)), tr = CIP_I64(CIP_I32(r));

    // d, e -> (u d + v e, q d + r e) / 2^30 modulo p: a multiple of p = 2^255 - 19
    // makes the low 30 bits zero. Both stay in (-2p, p).
    i32 sd = d[8] >> 31, se = e[8] >> 31;
    i32 md = (CIP_I32(u) & sd) + (CIP_I32(v) & se);
    i32 me = (CIP_I32(q) & sd) + (CIP_I32(r) & se);
    i64 cd = tu * CIP_I64(d[0]) + tv * CIP_I64(e[0]);
    i64 ce = tq * CIP_I64(d[0]) + tr * CIP_I64(e[0]);
    md -= CIP_I32((0x179435e5u * CIP_U32(cd) + CIP_U32(md)) & m30); // 0x179435e5 = 1/p mod 2^30
    me -= CIP_I32((0x179435e5u * CIP_U32(ce) + CIP_U32(me)) & m30);
    cd = (cd - 19 * CIP_I64(md)) >> 30;
    ce = (ce - 19 * CIP_I64(me)) >> 30;
    for (u32 k = 1u; k < 9u; k++) {
      cd += tu * CIP_I64(d[k]) + tv * CIP_I64(e[k]);
      ce += tq * CIP_I64(d[k]) + tr * CIP_I64(e[k]);
      if (k == 8u) { // 2^255 is bit 15 of limb 8
        cd += CIP_I64(md) << 15;
        ce += CIP_I64(me) << 15;
      }
      d[k - 1u] = CIP_I32(cd) & s30;
      e[k - 1u] = CIP_I32(ce) & s30;
      cd >>= 30;
      ce >>= 30;
    }
    d[8] = CIP_I32(cd);
    e[8] = CIP_I32(ce);

    i64 cf = (tu * CIP_I64(f[0]) + tv * CIP_I64(g[0])) >> 30;
    i64 cg = (tq * CIP_I64(f[0]) + tr * CIP_I64(g[0])) >> 30;
    for (u32 k = 1u; k < 9u; k++) {
      i64 fk = CIP_I64(f[k]), gk = CIP_I64(g[k]);
      cf += tu * fk + tv * gk;
      cg += tq * fk + tr * gk;
      f[k - 1u] = CIP_I32(cf) & s30;
      g[k - 1u] = CIP_I32(cg) & s30;
      cf >>= 30;
      cg >>= 30;
    }
    f[8] = CIP_I32(cf);
    g[8] = CIP_I32(cg);
  }

  // f is 1 or -1 and d the inverse times f, in (-2p, p): to [0, p).
  i32 flip = f[8] >> 31;
  i32 low  = d[8] >> 31;
  for (u32 k = 0u; k < 9u; k++)
    d[k] = ((d[k] + (pl[k] & low)) ^ flip) - flip;
  for (u32 k = 0u; k < 8u; k++) {
    d[k + 1u] += d[k] >> 30;
    d[k] &= s30;
  }
  low = d[8] >> 31;
  for (u32 k = 0u; k < 9u; k++)
    d[k] += pl[k] & low;
  for (u32 k = 0u; k < 8u; k++) {
    d[k + 1u] += d[k] >> 30;
    d[k] &= s30;
  }

  u32 dw[9];
  for (u32 k = 0u; k < 9u; k++)
    dw[k] = CIP_U32(d[k]);
  w.w[0] = dw[0] | (dw[1] << 30);
  w.w[1] = (dw[1] >> 2) | (dw[2] << 28);
  w.w[2] = (dw[2] >> 4) | (dw[3] << 26);
  w.w[3] = (dw[3] >> 6) | (dw[4] << 24);
  w.w[4] = (dw[4] >> 8) | (dw[5] << 22);
  w.w[5] = (dw[5] >> 10) | (dw[6] << 20);
  w.w[6] = (dw[6] >> 12) | (dw[7] << 18);
  w.w[7] = (dw[7] >> 14) | (dw[8] << 16);
  return fe_unpack(w);
}

// Low 64 bits of the canonical (fully reduced) value of a tight element, as two u32s.
// A tight value is < 2^255 + 2^243, so at most one subtraction of p is needed, and
// subtracting p adds 19 to the low bits. (Values in [p, 2^255) are not corrected; for the
// pseudo-random values this is applied to, that has probability 2^-250.)
CIP_FN u32 fe_low32(fe a) { return a.v[0] + (a.v[1] << 26) + 19u * (a.v[9] >> 25); }

CIP_FN u32 fe_high32(fe a) {
  u64 lo = CIP_U64(a.v[0]) + (CIP_U64(a.v[1]) << 26) + (CIP_U64(a.v[2]) << 51) + CIP_U64(19u * (a.v[9] >> 25));
  return CIP_U32(lo >> 32);
}
