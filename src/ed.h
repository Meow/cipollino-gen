// Host-side Ed25519 helpers built on the kernel field arithmetic.
//
// Everything here handles public data only (points, table entries). Secret scalars are
// only ever touched by libsodium; see keys.cpp.
#pragma once

#include <cstdint>
#include <vector>

#define CIP_TARGET_CPU
#include "kernel/defs.h"
#include "kernel/field.h"

namespace ed {

struct Affine {
    fe x, y;  // canonical (fully reduced) limbs
};

// Must be called once before anything else (after sodium_init()).
void init();

fe from_bytes(const uint8_t s[32]);  // ignores bit 255
void to_bytes(uint8_t s[32], fe a);  // canonical encoding
fe canonical(fe a);

// Decode a compressed point, recovering x. Returns false if it is not on the curve.
bool decompress(Affine &out, const uint8_t pk[32]);
void compress(uint8_t pk[32], const Affine &p);

// k*B for a public multiplier k (via libsodium), in affine coordinates.
Affine base_mult_u64(uint64_t k);

// Kernel table for N pairs: kappa_i, (k_i, y_i, x_i) for Q_i = 8i*B, and the step point
// 8(2N+1)*B. Layout documented in kernel/search.h.
std::vector<uint32_t> build_table(uint32_t npairs);

}  // namespace ed
