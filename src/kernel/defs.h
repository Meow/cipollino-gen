// cipollino kernel: target abstraction.
//
// The kernel sources (defs.h, field.h, search.h) are written in the common subset of
// CUDA/HIP C++, the Metal Shading Language, Vulkan GLSL and ordinary host C++, so the
// exact same arithmetic runs on every backend and can be unit-tested on the CPU.
//
// Rules for that subset:
//   - field elements are passed and returned by value; no references, no pointers to
//     thread-local data (MSL wants an address space on every pointer, GLSL has none);
//   - conversions are written CIP_U32(x) / CIP_U64(x), never as C casts (GLSL only has
//     constructor syntax);
//   - device buffers are reached only through the names state, table, scratch, hits,
//     filt, bitmap and CIP_HIT_NEXT(). On the C-like targets those are pointers handed
//     down the call chain by CIP_BUFS_DECL / CIP_BUFS; in GLSL they are globals;
//   - booleans combine with || and &&, integer literals carry a `u`, no recursion, no
//     typedefs, no identifiers that GLSL reserves (step, half, filter, input, ...).
//
// Functions are CIP_FN (always inlined: everything executed per key pair) or CIP_COLD_FN
// (code that runs once per batch or on rare paths). The distinction only matters to nvcc,
// whose device compiler needs tens of minutes if the whole kernel is one inlined function;
// the other compilers are free to inline cold functions too.
//
// The including file selects the target by defining exactly one of
// CIP_TARGET_METAL / CIP_TARGET_CUDA (also used for HIP) / CIP_TARGET_GLSL / CIP_TARGET_CPU.

#if defined(CIP_TARGET_METAL)

#include <metal_stdlib>
using namespace metal;
typedef uint u32;
typedef ulong u64;
#define CIP_FN static inline __attribute__((always_inline))
#define CIP_COLD_FN static inline
#define CIP_GPTR(T) device T *
#define CIP_CPTR(T) const device T *
#define CIP_HITCTR device atomic_uint *
#define CIP_ATOMIC_INC(p) atomic_fetch_add_explicit((p), 1u, memory_order_relaxed)

#elif defined(CIP_TARGET_CUDA)

#include <stdint.h>
typedef uint32_t u32;
typedef uint64_t u64;
#define CIP_FN __device__ __forceinline__
// Spelled as an attribute: CUDA's __noinline__ macro has a history of colliding with
// libstdc++, which writes __attribute__((__noinline__)).
#define CIP_COLD_FN static __device__ __attribute__((noinline))
#define CIP_GPTR(T) T *
#define CIP_CPTR(T) const T *
#define CIP_HITCTR u32 *
#define CIP_ATOMIC_INC(p) atomicAdd((p), 1u)

#if defined(__NVCC__)
// A 32x32->64 product, spelled in PTX. Written as (u64)a * (u64)b, nvcc hoists the
// zero-extension of a loop-invariant operand out of the loop; inside the loop the product
// is then a 64x64 multiply as far as ptxas can tell, and every one of them drags along an
// add of the (zero) high word: 92 dead instructions per pair in the forward pass alone,
// and 10% of the throughput of an RTX 4090.
static __device__ __forceinline__ u64 cip_mul_wide(u32 a, u32 b)
{
    u64 r;
    asm("mul.wide.u32 %0, %1, %2;" : "=l"(r) : "r"(a), "r"(b));
    return r;
}
#define CIP_MUL(a, b) cip_mul_wide((a), (b))
#endif

#elif defined(CIP_TARGET_CPU)

#include <stdint.h>
typedef uint32_t u32;
typedef uint64_t u64;
#define CIP_FN static inline
#define CIP_COLD_FN static inline
#define CIP_GPTR(T) T *
#define CIP_CPTR(T) const T *
#define CIP_HITCTR u32 *
#define CIP_ATOMIC_INC(p) __atomic_fetch_add((p), 1u, __ATOMIC_RELAXED)

#elif defined(CIP_TARGET_GLSL)

// The including shader declares the buffers as globals and CIP_SMODE as a macro.
#define u32 uint
#define u64 uint64_t
#define CIP_FN
#define CIP_COLD_FN
#define CIP_U32(x) uint(x)
#define CIP_U64(x) uint64_t(x)
#define CIP_BUFS_DECL
#define CIP_BUFS
#define CIP_HIT_NEXT() atomicAdd(hitctr, 1u)
#define CIP_SMODE_TEMPLATE
#define SMODE CIP_SMODE

#else
#error define a CIP_TARGET_ macro before including the kernel
#endif

#ifndef CIP_BUFS_DECL
#define CIP_U32(x) ((u32)(x))
#define CIP_U64(x) ((u64)(x))
// Not every function touches every buffer.
#if defined(CIP_TARGET_METAL)
#define CIP_UNUSED
#else
#define CIP_UNUSED [[maybe_unused]]
#endif
#define CIP_BUFS_DECL                                                                          \
    , CIP_UNUSED CIP_GPTR(u32) state, CIP_UNUSED CIP_CPTR(u32) table,                          \
        CIP_UNUSED CIP_GPTR(u32) scratch, CIP_UNUSED CIP_HITCTR hitctr,                        \
        CIP_UNUSED CIP_GPTR(u32) hits, CIP_UNUSED CIP_CPTR(u32) filt,                          \
        CIP_UNUSED CIP_CPTR(u32) bitmap
#define CIP_BUFS , state, table, scratch, hitctr, hits, filt, bitmap
#define CIP_HIT_NEXT() CIP_ATOMIC_INC(hitctr)
#define CIP_SMODE_TEMPLATE template <int SMODE>
#endif

// ---- layout constants shared by the host and the kernel -----------------------------

// Scratch modes: how much of the forward pass is kept for the backward pass. Less scratch
// means less memory traffic (what limits fast GPUs) for a little more arithmetic. The mode
// is a template argument of the search loop, so each variant is compiled on its own.
// Costs per pair (M = multiplication, S = squaring, on top of two 32-bit probes):
//   FULL: running product and z*kappa_i per pair   16 words/pair, 8M
//   LEAN: running product per pair                  8 words/pair, 8M + 1S
//   HALF: running product per two pairs             4 words/pair, 8.5M + 1S
#define CIP_SCRATCH_HALF 0u
#define CIP_SCRATCH_LEAN 1u
#define CIP_SCRATCH_FULL 2u

// Scratch layout, chosen by the including backend (see search.h).
#ifndef CIP_SCRATCH_AOS
#define CIP_SCRATCH_AOS 0
#endif

// Whether the backward pass fetches scratch a pair ahead (see search.h). It pays where it
// was measured to: 9% on an RTX 4090 under CUDA. The same card under Vulkan and an M5 Pro
// under Metal lose about 1% to it.
#ifndef CIP_FETCH_AHEAD
#if defined(CIP_TARGET_CUDA)
#define CIP_FETCH_AHEAD 1
#else
#define CIP_FETCH_AHEAD 0
#endif
#endif

#define CIP_STATE_WORDS 20u  // per thread: x and y of its centre, 10 limbs each
#define CIP_HIT_WORDS 3u

// The pre-filter on the first 32 bits of a key, by number of prefixes:
//   DIRECT       up to CIP_FEW_DIRECT: mask-and-compare against launch parameters
//   DIRECT_MANY  up to CIP_MAX_DIRECT: the same with all twelve slots, about 2% of a
//                pair's instructions and still no memory access
//   BITMAP       any number: bit tables, see below
#define CIP_FEW_DIRECT 4u
#define CIP_MAX_DIRECT 12u
#define CIP_MODE_DIRECT 0u
#define CIP_MODE_BITMAP 1u
#define CIP_MODE_DIRECT_MANY 2u

// Bitmap mode keeps three bit tables in one buffer. Every key is looked up in the near
// table, on key bits 0-15: at 8 KiB it stays in the cache next to the cores, where a
// table of megabytes costs a trip to memory per key. The keys that pass it (one in
// 65536 per prefix) are looked up in the two far tables, on bits 0-23 and on bits 8-31.
#define CIP_BITMAP_NEAR_BITS 16u
#define CIP_BITMAP_FAR_BITS 24u
#define CIP_BITMAP_FAR_A (1u << (CIP_BITMAP_NEAR_BITS - 5u))  // word offsets in the buffer
#define CIP_BITMAP_FAR_B (CIP_BITMAP_FAR_A + (1u << (CIP_BITMAP_FAR_BITS - 5u)))
#define CIP_BITMAP_WORDS (CIP_BITMAP_FAR_B + (1u << (CIP_BITMAP_FAR_BITS - 5u)))

// Launch parameters. Plain u32s only so the layout is identical everywhere.
struct cip_params {
    u32 nthreads;  // kernel threads T
    u32 npairs;    // pairs per batch N: each batch tests 2N+1 keys per thread
    u32 nbatches;  // batches per launch
    u32 nfilt;     // number of filter records in the filter buffer
    u32 mode;      // CIP_MODE_*
    u32 hitcap;    // capacity of the hit buffer, in records
    u32 pad0;
    u32 pad1;
    u32 dm[12];    // direct modes: low-32 masks (unused entries: mask 0 / value 1, never match)
    u32 dv[12];    // direct modes: low-32 values
};
