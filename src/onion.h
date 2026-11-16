// Tor v3 onion address encoding (rend-spec-v3 section 6).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace onion {

  void sha3_256(uint8_t out[32], const uint8_t *in, size_t len);

  // The 56-character address (without ".onion") for an Ed25519 public key:
  // base32(pubkey || checksum || version), checksum = SHA3-256(".onion checksum" || pubkey || version)[:2].
  std::string address(const uint8_t pk[32]);

  // 0..31 for a base32 character (a-z, 2-7; upper case accepted), -1 otherwise.
  int base32_value(char c);

  // `len` bytes in base32: lower case, no padding. 32 bytes give 52 characters.
  std::string base32(const uint8_t *data, size_t len);

  // Lower-cases an onion address and strips an optional ".onion"; returns false unless it
  // is 56 characters with the version and checksum of a v3 address.
  bool normalize_address(std::string &address);

  // A prefix is a sequence of address characters and wildcards: ?d is a digit (2-7), ?l a
  // letter, ?a either.
  //
  // Lower-cases a prefix and strips an optional ".onion"; returns false (with a message in
  // `err`) if it contains characters that can never appear in an address, a malformed
  // wildcard, or is too long. With `client` the prefix is of an x25519 key in base32, which
  // has 52 characters.
  bool normalize_prefix(std::string &prefix, std::string &err, bool client = false);

  // What a normalized prefix allows at each of its positions: bit v is set if the character
  // with base32 value v may stand there.
  std::vector<uint32_t> prefix_sets(const std::string &prefix);

  // The first `n` positions of a normalized prefix (all of it if it has fewer).
  std::string prefix_head(const std::string &prefix, size_t n);

  // Whether an address starts with a normalized prefix.
  bool prefix_matches(const std::string &prefix, const std::string &address);

} // namespace onion
