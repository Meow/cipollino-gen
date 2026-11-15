// Tor v3 onion address encoding (rend-spec-v3 section 6).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace onion {

void sha3_256(uint8_t out[32], const uint8_t *in, size_t len);

// The 56-character address (without ".onion") for an Ed25519 public key:
// base32(pubkey || checksum || version), checksum = SHA3-256(".onion checksum" || pubkey || version)[:2].
std::string address(const uint8_t pk[32]);

// 0..31 for a base32 character (a-z, 2-7; upper case accepted), -1 otherwise.
int base32_value(char c);

// Lower-cases a prefix and strips an optional ".onion"; returns false (with a message in
// `err`) if it contains characters that can never appear in an address or is too long.
bool normalize_prefix(std::string &prefix, std::string &err);

}  // namespace onion
