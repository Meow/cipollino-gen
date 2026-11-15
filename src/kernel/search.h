// cipollino kernel: the search loop run by every GPU thread.
//
// Each thread owns a "centre" point C = (x1, y1) in affine coordinates on Ed25519
// (-x^2 + y^2 = 1 + d x^2 y^2). The GPU only ever sees these public points, never a
// secret scalar. One batch tests the 2N+1 keys C + j*G8 for j in [-N, N], where
// G8 = 8*B, then moves the centre by (2N+1)*G8.
//
// An onion address is the y coordinate, so only y is computed. With Q_i = i*G8 = (x_i, y_i)
// from a shared table and
//     A = y1*y_i,  B = x1*x_i,  t = d*x1*y1*x_i*y_i = c*k_i   (c = x1*y1, k_i = d*x_i*y_i)
// the two candidates of a pair are
//     y(C + Q_i) = (A + B) / (1 - t)         y(C - Q_i) = (A - B) / (1 + t)
// Both denominators divide 1 - t^2 = 1 - z*kappa_i (z = c^2, kappa_i = k_i^2), so one
// shared inverse per pair is enough:
//     inv = 1/(1 - t^2),  w = t*inv,  y+ = (A + B)(inv + w),  y- = (A - B)(inv - w)
// and Montgomery's trick turns the N inversions of a batch into one field inversion plus
// 3 multiplications per pair. That is 10 multiplications per pair, but the last two, the
// products giving y+ and y-, are only needed to 32 bits for the prefix test and cost about
// a third of a multiplication each (fe_mul_probe): under 9 per pair, 4.4 per key.
//
// Buffers:
//   state   [T * 20]            per thread: x1 (10 limbs), y1 (10 limbs), tight
//   table   [40*N + 30]         kappa_i for i=1..N (10 each), then (k_i, y_i, x_i) for
//                               i=1..N (30 each), then (k, y, x) of the step (2N+1)*G8
//   scratch                     8-word slots, 2N (FULL), N (LEAN) or N/2 (HALF, N even) per
//                               thread. Word w of slot s of thread t lives at
//                                 (8*s + w)*T + t     CIP_SCRATCH_AOS=0: one access by a
//                                                     warp covers one contiguous 128 bytes
//                                 (s*T + t)*8 + w     CIP_SCRATCH_AOS=1: a thread's slot is
//                                                     contiguous
//                               Which is faster is a property of the GPU's memory system:
//                               Apple GPUs pay almost 4x more for the first layout.
//   hitctr  [1]                 atomic count of hits this launch
//   hits    [3 * hitcap]        (thread, batch, code); code 0 = centre, else 2*i + sign
//                               with sign 0 for C + Q_i and 1 for C - Q_i
//   filt    [4 * nfilt]         per prefix: mask_lo, value_lo, mask_hi, value_hi over the
//                               first 8 public key bytes read as a little-endian u64
//   bitmap  [2^19]              bitmap mode: one bit per value of the first 3 key bytes

CIP_FN fe cip_load10(CIP_CPTR(u32) p)
{
    fe r;
    r.v[0] = p[0]; r.v[1] = p[1]; r.v[2] = p[2]; r.v[3] = p[3]; r.v[4] = p[4];
    r.v[5] = p[5]; r.v[6] = p[6]; r.v[7] = p[7]; r.v[8] = p[8]; r.v[9] = p[9];
    return r;
}

CIP_FN void cip_store10(CIP_GPTR(u32) p, fe a)
{
    p[0] = a.v[0]; p[1] = a.v[1]; p[2] = a.v[2]; p[3] = a.v[3]; p[4] = a.v[4];
    p[5] = a.v[5]; p[6] = a.v[6]; p[7] = a.v[7]; p[8] = a.v[8]; p[9] = a.v[9];
}

#if CIP_SCRATCH_AOS
#define CIP_SCRATCH_AT(slot, tid, T) (((slot) * (T) + (tid)) * 8u)
#define CIP_SCRATCH_STEP(T) 1u
#else
#define CIP_SCRATCH_AT(slot, tid, T) ((slot) * 8u * (T) + (tid))
#define CIP_SCRATCH_STEP(T) (T)
#endif

// Scratch slot `slot` of thread `tid`; `a` must be tight.
CIP_FN void cip_scratch_store(CIP_GPTR(u32) s, u32 slot, u32 tid, u32 T, fe a)
{
    fe8 p = fe_pack(a);
    u32 at = CIP_SCRATCH_AT(slot, tid, T);
    u32 step = CIP_SCRATCH_STEP(T);
    s[at] = p.w[0];
    s[at + step] = p.w[1];
    s[at + 2u * step] = p.w[2];
    s[at + 3u * step] = p.w[3];
    s[at + 4u * step] = p.w[4];
    s[at + 5u * step] = p.w[5];
    s[at + 6u * step] = p.w[6];
    s[at + 7u * step] = p.w[7];
}

CIP_FN fe cip_scratch_load(CIP_CPTR(u32) s, u32 slot, u32 tid, u32 T)
{
    fe8 p;
    u32 at = CIP_SCRATCH_AT(slot, tid, T);
    u32 step = CIP_SCRATCH_STEP(T);
    p.w[0] = s[at];
    p.w[1] = s[at + step];
    p.w[2] = s[at + 2u * step];
    p.w[3] = s[at + 3u * step];
    p.w[4] = s[at + 4u * step];
    p.w[5] = s[at + 5u * step];
    p.w[6] = s[at + 6u * step];
    p.w[7] = s[at + 7u * step];
    return fe_unpack(p);
}

// Rare path: a candidate passed the 32-bit / bitmap pre-filter. Confirm against the full
// 64-bit filter records and append a hit. The host re-derives and re-checks everything.
CIP_COLD_FN void cip_confirm(fe y, u32 lo, u32 tid, u32 batch, u32 code,
                             CIP_HITCTR hitctr, CIP_GPTR(u32) hits, CIP_CPTR(u32) filt, cip_params prm)
{
    u32 hi = fe_high32(y);
    for (u32 j = 0u; j < prm.nfilt; j++) {
        if ((lo & filt[4u * j]) == filt[4u * j + 1u] && (hi & filt[4u * j + 2u]) == filt[4u * j + 3u]) {
            u32 slot = CIP_ATOMIC_INC(hitctr);
            if (slot < prm.hitcap) {
                hits[3u * slot] = tid;
                hits[3u * slot + 1u] = batch;
                hits[3u * slot + 2u] = code;
            }
            break;
        }
    }
}

// Pre-filter on the first 32 bits of a key (as a little-endian word): can it match any prefix?
CIP_FN bool cip_prefilter(u32 lo, CIP_CPTR(u32) bitmap, cip_params prm)
{
    if (prm.mode == CIP_MODE_DIRECT) {
        return ((lo & prm.dm[0]) == prm.dv[0]) | ((lo & prm.dm[1]) == prm.dv[1])
             | ((lo & prm.dm[2]) == prm.dv[2]) | ((lo & prm.dm[3]) == prm.dv[3]);
    }
    u32 idx = lo & ((1u << CIP_BITMAP_BITS) - 1u);
    return ((bitmap[idx >> 5] >> (idx & 31u)) & 1u) != 0u;
}

// Test a candidate whose y coordinate is known in full.
CIP_COLD_FN void cip_check(fe y, u32 tid, u32 batch, u32 code,
                           CIP_HITCTR hitctr, CIP_GPTR(u32) hits, CIP_CPTR(u32) filt,
                           CIP_CPTR(u32) bitmap, cip_params prm)
{
    u32 lo = fe_low32(y);
    if (cip_prefilter(lo, bitmap, prm)) cip_confirm(y, lo, tid, batch, code, hitctr, hits, filt, prm);
}

// Slow path of cip_check_product: form the whole product.
CIP_COLD_FN void cip_check_exact(fe f, fe g, u32 tid, u32 batch, u32 code,
                                 CIP_HITCTR hitctr, CIP_GPTR(u32) hits, CIP_CPTR(u32) filt,
                                 CIP_CPTR(u32) bitmap, cip_params prm)
{
    cip_check(fe_mul(f, g), tid, batch, code, hitctr, hits, filt, bitmap, prm);
}

// Test the candidate y = f*g. Almost always only its low 32 bits are computed; the full
// product is formed in the rare cases where those pass the pre-filter or are not certain.
CIP_FN void cip_check_product(fe f, fe g, u32 tid, u32 batch, u32 code,
                              CIP_HITCTR hitctr, CIP_GPTR(u32) hits, CIP_CPTR(u32) filt,
                              CIP_CPTR(u32) bitmap, cip_params prm)
{
    fe_probe pr = fe_mul_probe(f, g);
    if (cip_prefilter(pr.lo, bitmap, prm) | (pr.unsure != 0u))
        cip_check_exact(f, g, tid, batch, code, hitctr, hits, filt, bitmap, prm);
}

// Finish pair i (table entry q) given t = c*k_i and inv = 1/(1 - t^2): 3 multiplications
// and two probes.
CIP_FN void cip_pair(u32 i, fe t, fe inv, fe x1, fe y1, CIP_CPTR(u32) q, u32 tid, u32 batch,
                     CIP_HITCTR hitctr, CIP_GPTR(u32) hits, CIP_CPTR(u32) filt,
                     CIP_CPTR(u32) bitmap, cip_params prm)
{
    fe w = fe_mul(t, inv);
    fe A = fe_mul(cip_load10(q + 10u), y1);
    fe B = fe_mul(cip_load10(q + 20u), x1);
    u32 code = 2u * (i + 1u);
    cip_check_product(fe_add(A, B), fe_add(inv, w), tid, batch, code, hitctr, hits, filt, bitmap, prm);
    cip_check_product(fe_sub(A, B), fe_sub(inv, w), tid, batch, code + 1u, hitctr, hits, filt, bitmap, prm);
}

// SMODE is one of CIP_SCRATCH_*.
template <int SMODE>
CIP_FN void cip_search_thread(u32 tid, CIP_GPTR(u32) state, CIP_CPTR(u32) table,
                              CIP_GPTR(u32) scratch, CIP_HITCTR hitctr, CIP_GPTR(u32) hits,
                              CIP_CPTR(u32) filt, CIP_CPTR(u32) bitmap, cip_params prm)
{
    const u32 T = prm.nthreads;
    const u32 N = prm.npairs;
    const u32 smode = (u32)SMODE;
    CIP_CPTR(u32) tab_kappa = table;
    CIP_CPTR(u32) tab_pt = table + 10u * N;
    CIP_CPTR(u32) tab_step = table + 40u * N;

    fe x1 = cip_load10(state + tid * CIP_STATE_WORDS);
    fe y1 = cip_load10(state + tid * CIP_STATE_WORDS + 10u);

    for (u32 b = 0u; b < prm.nbatches; b++) {
        cip_check(y1, tid, b, 0u, hitctr, hits, filt, bitmap, prm);

        fe c = fe_mul_cold(x1, y1);
        fe z = fe_sq_cold(c);

        // Forward pass: running product of the denominators, the step's first.
        fe p = fe_carry(fe_one_minus(fe_sq_cold(fe_mul_cold(c, cip_load10(tab_step)))));
        for (u32 i = 0u; i < N; i++) {
            fe e = fe_mul(cip_load10(tab_kappa + 10u * i), z);
            if (smode == CIP_SCRATCH_FULL) {
                cip_scratch_store(scratch, 2u * i, tid, T, p);
                cip_scratch_store(scratch, 2u * i + 1u, tid, T, e);
            } else if (smode == CIP_SCRATCH_LEAN) {
                cip_scratch_store(scratch, i, tid, T, p);
            } else if ((i & 1u) == 0u) {
                cip_scratch_store(scratch, i >> 1, tid, T, p);
            }
            p = fe_mul(p, fe_one_minus(e));
        }

        fe inv_all = fe_invert(p);

        // Backward pass: peel one inverse per pair and finish both of its candidates.
        if (smode == CIP_SCRATCH_FULL) {
            for (u32 i = N; i-- > 0u;) {
                CIP_CPTR(u32) q = tab_pt + 30u * i;
                fe t = fe_mul(cip_load10(q), c);
                fe d = fe_one_minus(cip_scratch_load(scratch, 2u * i + 1u, tid, T));
                fe inv = fe_mul(inv_all, cip_scratch_load(scratch, 2u * i, tid, T));
                inv_all = fe_mul(inv_all, d);
                cip_pair(i, t, inv, x1, y1, q, tid, b, hitctr, hits, filt, bitmap, prm);
            }
        } else if (smode == CIP_SCRATCH_LEAN) {
            for (u32 i = N; i-- > 0u;) {
                CIP_CPTR(u32) q = tab_pt + 30u * i;
                fe t = fe_mul(cip_load10(q), c);
                fe inv = fe_mul(inv_all, cip_scratch_load(scratch, i, tid, T));
                inv_all = fe_mul(inv_all, fe_one_minus(fe_sq(t)));
                cip_pair(i, t, inv, x1, y1, q, tid, b, hitctr, hits, filt, bitmap, prm);
            }
        } else {
            // Only the product before each even pair was stored (N is even in this mode).
            // The one before the odd pair of a block is that times the even pair's
            // denominator, so the even pair's t and d are computed one iteration early
            // and carried over. One pair per iteration keeps the loop body small, which
            // matters to nvcc: two inlined pairs take it minutes to compile.
            fe t_even = fe_one(), d_even = fe_one(), stored = fe_one();
            for (u32 i = N; i-- > 0u;) {
                CIP_CPTR(u32) q = tab_pt + 30u * i;
                fe t, d, before;
                if ((i & 1u) != 0u) {
                    stored = cip_scratch_load(scratch, i >> 1, tid, T);
                    t_even = fe_mul(cip_load10(q - 30u), c);
                    d_even = fe_one_minus(fe_sq(t_even));
                    before = fe_mul(stored, d_even);
                    t = fe_mul(cip_load10(q), c);
                    d = fe_one_minus(fe_sq(t));
                } else {
                    t = t_even;
                    d = d_even;
                    before = stored;
                }
                fe inv = fe_mul(inv_all, before);
                inv_all = fe_mul(inv_all, d);
                cip_pair(i, t, inv, x1, y1, q, tid, b, hitctr, hits, filt, bitmap, prm);
            }
        }

        // inv_all is now 1/(1 - ts^2): move the centre by the step point (full x and y).
        fe ts = fe_mul_cold(c, cip_load10(tab_step));
        fe ys = cip_load10(tab_step + 10u);
        fe xs = cip_load10(tab_step + 20u);
        fe inv_m = fe_mul_cold(inv_all, fe_one_plus(ts));   // 1/(1 - ts)
        fe inv_p = fe_mul_cold(inv_all, fe_one_minus(ts));  // 1/(1 + ts)
        fe x3 = fe_mul_cold(fe_add(fe_mul_cold(x1, ys), fe_mul_cold(y1, xs)), inv_p);
        fe y3 = fe_mul_cold(fe_add(fe_mul_cold(y1, ys), fe_mul_cold(x1, xs)), inv_m);
        x1 = x3;
        y1 = y3;
    }

    cip_store10(state + tid * CIP_STATE_WORDS, x1);
    cip_store10(state + tid * CIP_STATE_WORDS + 10u, y1);
}
