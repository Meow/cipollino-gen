// Metal compute entry point; concatenated after defs.h, field.h and search.h at build time.
// CIP_SMODE (the scratch mode) is supplied as a preprocessor macro when the host compiles
// the kernel for a configuration.

kernel void cipollino_search(device u32 *state [[buffer(0)]],
                             const device u32 *table [[buffer(1)]],
                             device u32 *scratch [[buffer(2)]],
                             device atomic_uint *hitctr [[buffer(3)]],
                             device u32 *hits [[buffer(4)]],
                             const device u32 *filt [[buffer(5)]],
                             const device u32 *bitmap [[buffer(6)]],
                             constant cip_params &prm [[buffer(7)]],
                             uint tid [[thread_position_in_grid]])
{
    if (tid >= prm.nthreads) return;
    cip_search_thread<CIP_SMODE>(tid, state, table, scratch, hitctr, hits, filt, bitmap, prm);
}
