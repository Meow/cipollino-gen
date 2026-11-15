// cipollino kernel: target abstraction.
//
// The kernel sources (defs.h, field.h, search.h) are written in the subset of C++ that
// CUDA C++, the Metal Shading Language and ordinary host C++ all accept, so the exact
// same arithmetic runs on every backend and can be unit-tested on the CPU.
//
// Rules for that subset: no references or pointers to thread-local data (MSL wants an
// address space on every pointer), field elements are passed and returned by value,
// no integer literal suffixes wider than `u`, no recursion, no globals.
//
// Functions are CIP_FN (always inlined: everything executed per key pair) or CIP_COLD_FN
// (code that runs once per batch or on rare paths). The distinction only matters to nvcc,
// whose device compiler needs tens of minutes if the whole kernel is one inlined function;
// the other compilers are free to inline cold functions too.
//
// The including file selects the target by defining exactly one of
// CIP_TARGET_METAL / CIP_TARGET_CUDA / CIP_TARGET_CPU.

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

#else
#error "define CIP_TARGET_METAL, CIP_TARGET_CUDA or CIP_TARGET_CPU before including the kernel"
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

#define CIP_STATE_WORDS 20u  // per thread: x and y of its centre, 10 limbs each
#define CIP_HIT_WORDS 3u
#define CIP_BITMAP_BITS 24u
#define CIP_BITMAP_WORDS (1u << (CIP_BITMAP_BITS - 5u))
#define CIP_MAX_DIRECT 4u

#define CIP_MODE_DIRECT 0u
#define CIP_MODE_BITMAP 1u

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
    u32 dm[4];     // direct mode: low-32 masks (unused entries: mask 0 / value 1, never match)
    u32 dv[4];     // direct mode: low-32 values
};
