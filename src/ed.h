// Host-side Ed25519 and Curve25519 helpers built on the kernel field arithmetic.
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
    fe x, y; // canonical (fully reduced) limbs
  };

  // Must be called once before anything else (after sodium_init()).
  void init();

  fe from_bytes(const uint8_t s[32]); // ignores bit 255
  void to_bytes(uint8_t s[32], fe a); // canonical encoding
  fe canonical(fe a);

  // Decode a compressed point, recovering x. Returns false if it is not on the curve.
  bool decompress(Affine &out, const uint8_t pk[32]);
  void compress(uint8_t pk[32], const Affine &p);

  // k*B for a public multiplier k (via libsodium), in affine coordinates.
  Affine base_mult_u64(uint64_t k);

  // The same point on Curve25519 in Montgomery form, v^2 = u^3 + 486662 u^2 + u, where u is
  // its x25519 public key: u = (1 + y)/(1 - y), v = sqrt(-486664) u/x.
  struct Montgomery {
    fe u, v; // canonical
  };

  Montgomery to_montgomery(const Affine &p);

  // Kernel table for N pairs: kappa_i, (k_i, y_i, x_i) for Q_i = 8i*B, and the step point
  // 8(2N+1)*B. Layout documented in kernel/search.h.
  std::vector<uint32_t> build_table(uint32_t npairs);

  // The table of the x25519 search: (u_i, v_i) of the same points, the step's last.
  std::vector<uint32_t> build_table_x25519(uint32_t npairs);

} // namespace ed
