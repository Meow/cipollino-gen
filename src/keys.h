// Secret-key handling. This is the only code that touches secret material, and all of the
// cryptography in it is libsodium's.
//
// A key is born the way every Ed25519 key is: 32 bytes from the OS CSPRNG, expanded with
// SHA-512 into a clamped scalar `a` and a nonce prefix `RH`. The search then walks
// a + 8m for a public offset m, so the final secret scalar is still a uniformly random
// clamped scalar, conditioned only on its public key having the wanted prefix. Each seed
// yields at most one key (see engine.cpp), so no two reported keys are related.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace keys {

typedef __int128 Offset;  // the public walk offset m, in units of 8*B

struct Key {
    uint8_t sk[64];  // expanded secret key: scalar (LE) || nonce prefix, as Tor stores it
    uint8_t pk[32];
    std::string address;  // 56 characters, no ".onion"
    Key();
    ~Key();  // wipes sk
    Key(const Key &) = delete;
    Key &operator=(const Key &) = delete;
};

// Fresh seed from the OS CSPRNG.
void random_seed(uint8_t seed[32]);

// Public key of the seed's (unshifted) scalar: the point a GPU thread starts from.
void start_point(uint8_t pk[32], const uint8_t seed[32]);

// Public key for seed + offset, computed from the secret scalar by libsodium alone.
// Returns false if seed + offset left the clamped range (probability ~2^-190).
bool public_key(uint8_t pk[32], const uint8_t seed[32], Offset m);

// Derive the final key for seed + offset and validate it end to end, independently of
// anything the GPU computed:
//   - the scalar is a correctly clamped Ed25519 scalar,
//   - the public key is recomputed from the scalar, is canonical and in the prime-order group,
//   - a signature made with the expanded key verifies under that public key,
//   - the address derived from that public key starts with one of `prefixes`.
// On failure `why` says which check failed.
bool finalize(Key &out, const uint8_t seed[32], Offset m, const std::vector<std::string> &prefixes,
              std::string &why);

// Write <dir>/<address>.onion/{hostname,hs_ed25519_public_key,hs_ed25519_secret_key} in the
// layout Tor's HiddenServiceDir expects (directory 0700, secret key 0600). Refuses to
// overwrite. Returns false with a message in `err`.
bool write_tor_dir(const std::string &dir, const Key &key, std::string &err);

}  // namespace keys
