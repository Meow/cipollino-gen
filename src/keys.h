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

  // The public walk offset m, in units of 8*B. A kernel thread moves a few thousand to a few
  // million keys per second away from its seed, so 63 bits last longer than any search.
  typedef int64_t Offset;

  struct Key {
    uint8_t sk[64]; // expanded secret key: scalar (LE) || nonce prefix, as Tor stores it
    uint8_t pk[32];
    std::string address; // 56 characters, no ".onion"
    Key();
    ~Key(); // wipes sk
    Key(const Key &)            = delete;
    Key &operator=(const Key &) = delete;
  };

  // An x25519 key pair for client authorization. The private key is the same kind of scalar,
  // which x25519 uses as it is.
  struct ClientKey {
    uint8_t sk[32];
    uint8_t pk[32];
    std::string text; // the public key in base32: 52 characters, lower case
    ClientKey();
    ~ClientKey(); // wipes sk
    ClientKey(const ClientKey &)            = delete;
    ClientKey &operator=(const ClientKey &) = delete;
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
  bool finalize(Key &out, const uint8_t seed[32], Offset m, const std::vector<std::string> &prefixes, std::string &why);

  // The x25519 public key for seed + offset. Returns false as public_key does.
  bool client_public_key(uint8_t pk[32], const uint8_t seed[32], Offset m);

  // Derive the x25519 key pair for seed + offset and validate it, independently of anything
  // the GPU computed:
  //   - the private key is a clamped scalar, so x25519 uses exactly these 32 bytes,
  //   - the public key is recomputed from it, and is the Montgomery form of the Ed25519
  //     public key of the same scalar,
  //   - a key exchange with a throwaway key pair gives both sides the same secret,
  //   - the public key in base32 starts with one of `prefixes`.
  bool finalize_client(
    ClientKey &out, const uint8_t seed[32], Offset m, const std::vector<std::string> &prefixes, std::string &why
  );

  // Write the pair as Tor takes it, named after the public key in upper-case base32 (`name`):
  //   <dir>/<name>.auth          "descriptor:x25519:<public key>", for the service's
  //                              authorized_clients directory
  //   <dir>/<name>.auth_private  "<service>:descriptor:x25519:<private key>", for the
  //                              client's ClientOnionAuthDir, if `service` (an onion address
  //                              without ".onion") is given; otherwise
  //   <dir>/<name>.private       the private key in base32 alone
  // Files are created 0600 and never overwritten. Returns false with a message in `err`.
  bool write_client_key(
    const std::string &dir, const ClientKey &key, const std::string &service, std::string &name, std::string &err
  );

  // Write <dir>/<address>.onion/{hostname,hs_ed25519_public_key,hs_ed25519_secret_key} in the
  // layout Tor's HiddenServiceDir expects (directory 0700, secret key 0600; on Windows they
  // take the access list of the directory they are created in). Refuses to overwrite.
  // Returns false with a message in `err`.
  bool write_tor_dir(const std::string &dir, const Key &key, std::string &err);

  // Check a HiddenServiceDir, whoever wrote it: the three files have Tor's headers and
  // lengths, the public key is the secret scalar's, it is a canonical point of the
  // prime-order group, a signature made with the secret key verifies under it, and the
  // hostname is its address. On success `address` is that address (without ".onion") and
  // `notes` lists what is unusual but does not make the key wrong; otherwise `err` says
  // which check failed.
  bool verify_tor_dir(const std::string &dir, std::string &address, std::vector<std::string> &notes, std::string &err);

} // namespace keys
