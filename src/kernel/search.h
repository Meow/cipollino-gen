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
//                               Which is faster is a property of the GPU's memory system.
//   hitctr  [1]                 atomic count of hits this launch
//   hits    [3 * hitcap]        (thread, batch, code); code 0 = centre, else 2*i + sign
//                               with sign 0 for C + Q_i and 1 for C - Q_i
//   filt    [4 * nfilt]         per prefix: mask_lo, value_lo, mask_hi, value_hi over the
//                               first 8 public key bytes read as a little-endian u64
//   bitmap  [CIP_BITMAP_WORDS]  bitmap mode: the near and the two far bit tables (defs.h)

CIP_FN fe cip_table_load(u32 at CIP_BUFS_DECL) {
  fe r;
  r.v[0] = table[at];
  r.v[1] = table[at + 1u];
  r.v[2] = table[at + 2u];
  r.v[3] = table[at + 3u];
  r.v[4] = table[at + 4u];
  r.v[5] = table[at + 5u];
  r.v[6] = table[at + 6u];
  r.v[7] = table[at + 7u];
  r.v[8] = table[at + 8u];
  r.v[9] = table[at + 9u];
  return r;
}

CIP_FN fe cip_state_load(u32 at CIP_BUFS_DECL) {
  fe r;
  r.v[0] = state[at];
  r.v[1] = state[at + 1u];
  r.v[2] = state[at + 2u];
  r.v[3] = state[at + 3u];
  r.v[4] = state[at + 4u];
  r.v[5] = state[at + 5u];
  r.v[6] = state[at + 6u];
  r.v[7] = state[at + 7u];
  r.v[8] = state[at + 8u];
  r.v[9] = state[at + 9u];
  return r;
}

CIP_FN void cip_state_store(u32 at, fe a CIP_BUFS_DECL) {
  state[at]      = a.v[0];
  state[at + 1u] = a.v[1];
  state[at + 2u] = a.v[2];
  state[at + 3u] = a.v[3];
  state[at + 4u] = a.v[4];
  state[at + 5u] = a.v[5];
  state[at + 6u] = a.v[6];
  state[at + 7u] = a.v[7];
  state[at + 8u] = a.v[8];
  state[at + 9u] = a.v[9];
}

#if CIP_SCRATCH_AOS
#define CIP_SCRATCH_AT(slot, tid, T) (((slot) * (T) + (tid)) * 8u)
#define CIP_SCRATCH_STRIDE(T)        1u
#else
#define CIP_SCRATCH_AT(slot, tid, T) ((slot) * 8u * (T) + (tid))
#define CIP_SCRATCH_STRIDE(T)        (T)
#endif

// Scratch slot `slot` of thread `tid`; `a` must be tight.
CIP_FN void cip_scratch_store(u32 slot, u32 tid, u32 T, fe a CIP_BUFS_DECL) {
  fe8 p                     = fe_pack(a);
  u32 at                    = CIP_SCRATCH_AT(slot, tid, T);
  u32 stride                = CIP_SCRATCH_STRIDE(T);
  scratch[at]               = p.w[0];
  scratch[at + stride]      = p.w[1];
  scratch[at + 2u * stride] = p.w[2];
  scratch[at + 3u * stride] = p.w[3];
  scratch[at + 4u * stride] = p.w[4];
  scratch[at + 5u * stride] = p.w[5];
  scratch[at + 6u * stride] = p.w[6];
  scratch[at + 7u * stride] = p.w[7];
}

// The eight raw words of a scratch slot (fe_unpack turns them back into a field element).
CIP_FN fe8 cip_scratch_fetch(u32 slot, u32 tid, u32 T CIP_BUFS_DECL) {
  fe8 p;
  u32 at     = CIP_SCRATCH_AT(slot, tid, T);
  u32 stride = CIP_SCRATCH_STRIDE(T);
  p.w[0]     = scratch[at];
  p.w[1]     = scratch[at + stride];
  p.w[2]     = scratch[at + 2u * stride];
  p.w[3]     = scratch[at + 3u * stride];
  p.w[4]     = scratch[at + 4u * stride];
  p.w[5]     = scratch[at + 5u * stride];
  p.w[6]     = scratch[at + 6u * stride];
  p.w[7]     = scratch[at + 7u * stride];
  return p;
}

// With CIP_FETCH_AHEAD the backward pass fetches a scratch slot one pair before it uses
// it: the fetch misses the caches, and a thread with a whole pair of arithmetic to do in
// the meantime does not wait for it. CIP_AHEAD declares the look-ahead buffer primed with
// the first slot, CIP_TAKE yields slot `cur` and requests slot `next`.
#if CIP_FETCH_AHEAD
#define CIP_AHEAD(buf, first) fe8 buf = cip_scratch_fetch(first, tid, T CIP_BUFS)
#define CIP_TAKE(dst, buf, cur, next) \
  dst = buf;                          \
  buf = cip_scratch_fetch(next, tid, T CIP_BUFS)
#else
#define CIP_AHEAD(buf, first)
#define CIP_TAKE(dst, buf, cur, next) dst = cip_scratch_fetch(cur, tid, T CIP_BUFS)
#endif

// Rare path: a candidate passed the pre-filter on its first 32 bits. Confirm against the full
// 64-bit filter records and append a hit. The host re-derives and re-checks everything.
CIP_COLD_FN void cip_confirm(fe y, u32 lo, u32 tid, u32 batch, u32 code, cip_params prm CIP_BUFS_DECL) {
  u32 hi = fe_high32(y);
  for (u32 j = 0u; j < prm.nfilt; j++) {
    if ((lo & filt[4u * j]) == filt[4u * j + 1u] && (hi & filt[4u * j + 2u]) == filt[4u * j + 3u]) {
      u32 slot = CIP_HIT_NEXT();
      if (slot < prm.hitcap) {
        hits[3u * slot]      = tid;
        hits[3u * slot + 1u] = batch;
        hits[3u * slot + 2u] = code;
      }
      break;
    }
  }
}

// Top bit set unless direct-mode entry j accepts `lo`.
#define CIP_DIRECT_MISS(j) cip_nonzero((lo & prm.dm[j]) ^ prm.dv[j])

CIP_FN u32 cip_nonzero(u32 x) { return x | (0u - x); }

// Pre-filter on the first 32 bits of a key (as a little-endian word): can it match any prefix?
CIP_FN bool cip_prefilter(u32 lo, cip_params prm CIP_BUFS_DECL) {
  if (prm.mode == CIP_MODE_DIRECT) {
    return ((lo & prm.dm[0]) == prm.dv[0]) || ((lo & prm.dm[1]) == prm.dv[1]) || ((lo & prm.dm[2]) == prm.dv[2]) ||
           ((lo & prm.dm[3]) == prm.dv[3]);
  }
  if (prm.mode == CIP_MODE_DIRECT_MANY) {
    // Twelve entries, folded without a branch per entry.
    u32 miss = CIP_DIRECT_MISS(0) & CIP_DIRECT_MISS(1) & CIP_DIRECT_MISS(2) & CIP_DIRECT_MISS(3) & CIP_DIRECT_MISS(4) &
               CIP_DIRECT_MISS(5) & CIP_DIRECT_MISS(6) & CIP_DIRECT_MISS(7) & CIP_DIRECT_MISS(8) & CIP_DIRECT_MISS(9) &
               CIP_DIRECT_MISS(10) & CIP_DIRECT_MISS(11);
    return (miss >> 31) == 0u;
  }
  u32 idx = lo & ((1u << CIP_BITMAP_NEAR_BITS) - 1u);
  return ((bitmap[idx >> 5] >> (idx & 31u)) & 1u) != 0u;
}

// Second stage of the bitmap pre-filter, for the few keys that pass the first: the far
// tables. The other modes have no second stage.
CIP_FN bool cip_prefilter_far(u32 lo, cip_params prm CIP_BUFS_DECL) {
  if (prm.mode != CIP_MODE_BITMAP)
    return true;
  u32 a = lo & ((1u << CIP_BITMAP_FAR_BITS) - 1u);
  u32 b = lo >> (32u - CIP_BITMAP_FAR_BITS);
  return ((bitmap[CIP_BITMAP_FAR_A + (a >> 5)] >> (a & 31u)) & (bitmap[CIP_BITMAP_FAR_B + (b >> 5)] >> (b & 31u)) &
          1u) != 0u;
}

// Test a candidate whose y coordinate is known in full.
CIP_COLD_FN void cip_check(fe y, u32 tid, u32 batch, u32 code, cip_params prm CIP_BUFS_DECL) {
  u32 lo = fe_low32(y);
  if (cip_prefilter(lo, prm CIP_BUFS) && cip_prefilter_far(lo, prm CIP_BUFS))
    cip_confirm(y, lo, tid, batch, code, prm CIP_BUFS);
}

// Slow path of cip_check_product: the probe's low word `lo` passed the pre-filter, or is
// not certain (`unsure`). A certain one is first put through the second stage; what is
// left has the whole product formed.
CIP_COLD_FN void
  cip_check_exact(fe f, fe g, u32 lo, u32 unsure, u32 tid, u32 batch, u32 code, cip_params prm CIP_BUFS_DECL) {
  if (unsure == 0u && !cip_prefilter_far(lo, prm CIP_BUFS))
    return;
  cip_check(fe_mul(f, g), tid, batch, code, prm CIP_BUFS);
}

// Test the candidate y = f*g. Almost always only its low 32 bits are computed; the full
// product is formed in the rare cases where those pass the pre-filter or are not certain.
CIP_FN void cip_check_product(fe f, fe g, u32 tid, u32 batch, u32 code, cip_params prm CIP_BUFS_DECL) {
  fe_probe pr = fe_mul_probe(f, g);
  if (cip_prefilter(pr.lo, prm CIP_BUFS) || pr.unsure != 0u)
    cip_check_exact(f, g, pr.lo, pr.unsure, tid, batch, code, prm CIP_BUFS);
}

// Finish pair i (table entry at q) given t = c*k_i and inv = 1/(1 - t^2): 3 multiplications
// and two probes.
CIP_FN void cip_pair(u32 i, fe t, fe inv, fe x1, fe y1, u32 q, u32 tid, u32 batch, cip_params prm CIP_BUFS_DECL) {
  fe w     = fe_mul_t(t, inv);
  fe A     = fe_mul_t(cip_table_load(q + 10u CIP_BUFS), y1);
  fe B     = fe_mul_t(cip_table_load(q + 20u CIP_BUFS), x1);
  u32 code = 2u * (i + 1u);
  cip_check_product(fe_add(A, B), fe_add(inv, w), tid, batch, code, prm CIP_BUFS);
  cip_check_product(fe_sub(A, B), fe_sub(inv, w), tid, batch, code + 1u, prm CIP_BUFS);
}

// ---- the x25519 search ---------------------------------------------------------------
//
// An x25519 public key (what Tor calls a client authorization key) is the u coordinate of
// a point of the same curve in Montgomery form, v^2 = u^3 + A u^2 + u with A = 486662.
// There the centre is C = (u1, v1), Q_i = (u_i, v_i), and the candidates of a pair
//     u(C + Q_i) = ((v_i - v1) / (u_i - u1))^2 - A - u1 - u_i
//     u(C - Q_i) = ((v_i + v1) / (u_i - u1))^2 - A - u1 - u_i
// share the inverse of u_i - u1, a denominator that costs nothing to form. With
// Montgomery's trick a pair takes 5 multiplications and 2 squarings: 3.5 a key.
//
// Buffers as above, except
//   state   [T * 20]     per thread: u1, v1, tight
//   table   [20*N + 20]  (u_i, v_i) for i=1..N, then (u, v) of the step (2N+1)*G8
//   scratch              N slots per thread: the running product before each pair

CIP_FN fe cip_zero() {
  fe r   = fe_one();
  r.v[0] = 0u;
  return r;
}

// Test the candidate u = lambda2 - u_i - (u1 + A), given m = -(u1 + A), tight.
CIP_FN void cip_check_u(fe lambda2, fe ui, fe m, u32 tid, u32 batch, u32 code, cip_params prm CIP_BUFS_DECL) {
  fe u = fe_carry(fe_add(fe_sub(lambda2, ui), m));
  if (cip_prefilter(fe_low32(u), prm CIP_BUFS))
    cip_check(u, tid, batch, code, prm CIP_BUFS);
}

CIP_FN void cip_search_x25519(u32 tid, cip_params prm CIP_BUFS_DECL) {
  const u32 T        = prm.nthreads;
  const u32 N        = prm.npairs;
  const u32 tab_step = 20u * N;
  fe a               = cip_zero();
  a.v[0]             = 486662u;

  fe u1 = cip_state_load(tid * CIP_STATE_WORDS CIP_BUFS);
  fe v1 = cip_state_load(tid * CIP_STATE_WORDS + 10u CIP_BUFS);

  for (u32 b = 0u; b < prm.nbatches; b++) {
    cip_check(u1, tid, b, 0u, prm CIP_BUFS);
    fe m = fe_carry(fe_sub(fe_sub(cip_zero(), u1), a));

    // Forward pass: running product of the denominators, the step's first.
    fe p = fe_carry(fe_sub(cip_table_load(tab_step CIP_BUFS), u1));
    for (u32 i = 0u; i < N; i++) {
      cip_scratch_store(i, tid, T, p CIP_BUFS);
      p = fe_mul_tl(p, fe_sub(cip_table_load(20u * i CIP_BUFS), u1));
    }

    fe inv_all = fe_invert_gcd(p);

    // Backward pass: peel one inverse per pair and finish both of its candidates.
    CIP_AHEAD(ahead, N - 1u);
    for (u32 i = N; i-- > 0u;) {
      fe8 mine;
      CIP_TAKE(mine, ahead, i, i > 0u ? i - 1u : 0u);
      fe ui    = cip_table_load(20u * i CIP_BUFS);
      fe vi    = cip_table_load(20u * i + 10u CIP_BUFS);
      fe inv   = fe_mul_t(inv_all, fe_unpack(mine));
      inv_all  = fe_mul_tl(inv_all, fe_sub(ui, u1));
      u32 code = 2u * (i + 1u);
      cip_check_u(fe_sq(fe_mul_tl(inv, fe_sub(vi, v1))), ui, m, tid, b, code, prm CIP_BUFS);
      cip_check_u(fe_sq(fe_mul_tl(inv, fe_add(vi, v1))), ui, m, tid, b, code + 1u, prm CIP_BUFS);
    }

    // inv_all is now 1/(us - u1): move the centre by the step point.
    fe us     = cip_table_load(tab_step CIP_BUFS);
    fe lambda = fe_mul_cold(inv_all, fe_sub(cip_table_load(tab_step + 10u CIP_BUFS), v1));
    fe u3     = fe_carry(fe_add(fe_sub(fe_sq_cold(lambda), us), m));
    v1        = fe_carry(fe_sub(fe_mul_cold(lambda, fe_sub(u1, u3)), v1));
    u1        = u3;
  }

  cip_state_store(tid * CIP_STATE_WORDS, u1 CIP_BUFS);
  cip_state_store(tid * CIP_STATE_WORDS + 10u, v1 CIP_BUFS);
}

// The work of thread `tid` for one launch. SMODE is one of CIP_SCRATCH_*: a template
// argument on the C++ targets, a macro in GLSL.
CIP_SMODE_TEMPLATE
CIP_FN void cip_search_thread(u32 tid, cip_params prm CIP_BUFS_DECL) {
  if (tid - prm.first >= prm.count)
    return; // not in this launch's window of threads
  const u32 T     = prm.nthreads;
  const u32 N     = prm.npairs;
  const u32 smode = CIP_U32(SMODE);
  if (smode == CIP_SCRATCH_X25519) {
    cip_search_x25519(tid, prm CIP_BUFS);
    return;
  }
  const u32 tab_pt   = 10u * N; // (k_i, y_i, x_i); kappa_i starts at 0
  const u32 tab_step = 40u * N; // the step point

  fe x1 = cip_state_load(tid * CIP_STATE_WORDS CIP_BUFS);
  fe y1 = cip_state_load(tid * CIP_STATE_WORDS + 10u CIP_BUFS);

  for (u32 b = 0u; b < prm.nbatches; b++) {
    cip_check(y1, tid, b, 0u, prm CIP_BUFS);

    fe c = fe_mul_cold(x1, y1);
    fe z = fe_sq_cold(c);

    // Forward pass: running product of the denominators, the step's first.
    fe p = fe_carry(fe_one_minus(fe_sq_cold(fe_mul_cold(c, cip_table_load(tab_step CIP_BUFS)))));
    for (u32 i = 0u; i < N; i++) {
      fe e = fe_mul_t(cip_table_load(10u * i CIP_BUFS), z);
      if (smode == CIP_SCRATCH_FULL) {
        cip_scratch_store(2u * i, tid, T, p CIP_BUFS);
        cip_scratch_store(2u * i + 1u, tid, T, e CIP_BUFS);
      } else if (smode == CIP_SCRATCH_LEAN) {
        cip_scratch_store(i, tid, T, p CIP_BUFS);
      } else if ((i & 1u) == 0u) {
        cip_scratch_store(i >> 1, tid, T, p CIP_BUFS);
      }
      p = fe_mul_t(p, fe_one_minus(e));
    }

    fe inv_all = fe_invert_gcd(p);

    // Backward pass: peel one inverse per pair and finish both of its candidates.
    // (Fetching the table entry ahead as well is slower on an RTX 4090.)
    if (smode == CIP_SCRATCH_FULL) {
      CIP_AHEAD(ahead_p, 2u * N - 2u);
      CIP_AHEAD(ahead_e, 2u * N - 1u);
      for (u32 i = N; i-- > 0u;) {
        u32 q = tab_pt + 30u * i;
        fe8 mine_p, mine_e;
        CIP_TAKE(mine_p, ahead_p, 2u * i, 2u * (i > 0u ? i - 1u : 0u));
        CIP_TAKE(mine_e, ahead_e, 2u * i + 1u, 2u * (i > 0u ? i - 1u : 0u) + 1u);
        fe t    = fe_mul_t(cip_table_load(q CIP_BUFS), c);
        fe d    = fe_one_minus(fe_unpack(mine_e));
        fe inv  = fe_mul_t(inv_all, fe_unpack(mine_p));
        inv_all = fe_mul_t(inv_all, d);
        cip_pair(i, t, inv, x1, y1, q, tid, b, prm CIP_BUFS);
      }
    } else if (smode == CIP_SCRATCH_LEAN) {
      CIP_AHEAD(ahead, N - 1u);
      for (u32 i = N; i-- > 0u;) {
        u32 q = tab_pt + 30u * i;
        fe8 mine;
        CIP_TAKE(mine, ahead, i, i > 0u ? i - 1u : 0u);
        fe t    = fe_mul_t(cip_table_load(q CIP_BUFS), c);
        fe inv  = fe_mul_t(inv_all, fe_unpack(mine));
        inv_all = fe_mul_t(inv_all, fe_one_minus(fe_sq(t)));
        cip_pair(i, t, inv, x1, y1, q, tid, b, prm CIP_BUFS);
      }
    } else {
      // Only the product before each even pair is stored (N is even in this mode).
      // The one before the odd pair of a block is that times the even pair's
      // denominator, so the even pair's t and d are computed one iteration early
      // and carried over. One pair per iteration keeps the loop body small, which
      // matters to nvcc: two inlined pairs take it minutes to compile.
      fe t_even = fe_one(), d_even = fe_one(), stored = fe_one();
      CIP_AHEAD(ahead, N / 2u - 1u);
      for (u32 i = N; i-- > 0u;) {
        u32 q = tab_pt + 30u * i;
        fe t, d, before;
        if ((i & 1u) != 0u) {
          fe8 mine;
          CIP_TAKE(mine, ahead, i >> 1, i > 1u ? (i >> 1) - 1u : 0u);
          stored = fe_unpack(mine);
          t_even = fe_mul_t(cip_table_load(q - 30u CIP_BUFS), c);
          d_even = fe_one_minus(fe_sq(t_even));
          before = fe_mul_t(stored, d_even);
          t      = fe_mul_t(cip_table_load(q CIP_BUFS), c);
          d      = fe_one_minus(fe_sq(t));
        } else {
          t      = t_even;
          d      = d_even;
          before = stored;
        }
        fe inv  = fe_mul_t(inv_all, before);
        inv_all = fe_mul_t(inv_all, d);
        cip_pair(i, t, inv, x1, y1, q, tid, b, prm CIP_BUFS);
      }
    }

    // inv_all is now 1/(1 - ts^2): move the centre by the step point (full x and y).
    fe ts    = fe_mul_cold(c, cip_table_load(tab_step CIP_BUFS));
    fe ys    = cip_table_load(tab_step + 10u CIP_BUFS);
    fe xs    = cip_table_load(tab_step + 20u CIP_BUFS);
    fe inv_m = fe_mul_cold(inv_all, fe_one_plus(ts));  // 1/(1 - ts)
    fe inv_p = fe_mul_cold(inv_all, fe_one_minus(ts)); // 1/(1 + ts)
    fe x3    = fe_mul_cold(fe_add(fe_mul_cold(x1, ys), fe_mul_cold(y1, xs)), inv_p);
    fe y3    = fe_mul_cold(fe_add(fe_mul_cold(y1, ys), fe_mul_cold(x1, xs)), inv_m);
    x1       = x3;
    y1       = y3;
  }

  cip_state_store(tid * CIP_STATE_WORDS, x1 CIP_BUFS);
  cip_state_store(tid * CIP_STATE_WORDS + 10u, y1 CIP_BUFS);
}
