#include "keys.h"

#include <sodium.h>

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "onion.h"

namespace keys {

Key::Key()
{
    memset(sk, 0, sizeof sk);
    memset(pk, 0, sizeof pk);
}

Key::~Key() { sodium_memzero(sk, sizeof sk); }

void random_seed(uint8_t seed[32]) { randombytes_buf(seed, 32); }

// Standard Ed25519 key expansion (RFC 8032 5.1.5).
static void expand(uint8_t sk[64], const uint8_t seed[32])
{
    crypto_hash_sha512(sk, seed, 32);
    sk[0] &= 248;
    sk[31] &= 63;
    sk[31] |= 64;
}

static bool is_clamped(const uint8_t a[32]) { return (a[0] & 7) == 0 && (a[31] & 0xc0) == 0x40; }

// a += 8*m as a plain 256-bit integer (no reduction mod the group order, so the clamped
// shape is preserved). Returns false if the result is no longer a clamped scalar.
static bool shift_scalar(uint8_t a[32], Offset m)
{
    bool negative = m < 0;
    unsigned __int128 mag = (negative ? (unsigned __int128)(-m) : (unsigned __int128)m) << 3;
    uint8_t delta[32] = {0};
    for (int i = 0; i < 16; i++) delta[i] = (uint8_t)(mag >> (8 * i));
    if (negative)
        sodium_sub(a, delta, 32);
    else
        sodium_add(a, delta, 32);
    return is_clamped(a);
}

void start_point(uint8_t pk[32], const uint8_t seed[32])
{
    uint8_t sk[64];
    expand(sk, seed);
    crypto_scalarmult_ed25519_base_noclamp(pk, sk);
    sodium_memzero(sk, sizeof sk);
}

bool public_key(uint8_t pk[32], const uint8_t seed[32], Offset m)
{
    uint8_t sk[64];
    expand(sk, seed);
    bool ok = shift_scalar(sk, m) && crypto_scalarmult_ed25519_base_noclamp(pk, sk) == 0;
    sodium_memzero(sk, sizeof sk);
    return ok;
}

// Ed25519 signature from an expanded secret key (scalar || nonce prefix), which is how Tor
// signs with an onion service identity key. RFC 8032 5.1.6 with the hash of the seed
// replaced by the stored expansion.
static bool sign_expanded(uint8_t sig[64], const uint8_t *msg, size_t len, const uint8_t sk[64],
                          const uint8_t pk[32])
{
    uint8_t h[64], r[32], k[32], a[64], a_red[32], ka[32];
    crypto_hash_sha512_state st;

    crypto_hash_sha512_init(&st);
    crypto_hash_sha512_update(&st, sk + 32, 32);
    crypto_hash_sha512_update(&st, msg, len);
    crypto_hash_sha512_final(&st, h);
    crypto_core_ed25519_scalar_reduce(r, h);
    bool ok = crypto_scalarmult_ed25519_base_noclamp(sig, r) == 0;  // R

    crypto_hash_sha512_init(&st);
    crypto_hash_sha512_update(&st, sig, 32);
    crypto_hash_sha512_update(&st, pk, 32);
    crypto_hash_sha512_update(&st, msg, len);
    crypto_hash_sha512_final(&st, h);
    crypto_core_ed25519_scalar_reduce(k, h);

    memset(a, 0, sizeof a);
    memcpy(a, sk, 32);
    crypto_core_ed25519_scalar_reduce(a_red, a);
    crypto_core_ed25519_scalar_mul(ka, k, a_red);
    crypto_core_ed25519_scalar_add(sig + 32, r, ka);  // S = r + k*a

    sodium_memzero(h, sizeof h);
    sodium_memzero(r, sizeof r);
    sodium_memzero(a, sizeof a);
    sodium_memzero(a_red, sizeof a_red);
    sodium_memzero(ka, sizeof ka);
    return ok;
}

bool finalize(Key &out, const uint8_t seed[32], Offset m, const std::vector<std::string> &prefixes,
              std::string &why)
{
    expand(out.sk, seed);
    if (!shift_scalar(out.sk, m)) {
        why = "scalar left the clamped range";
        return false;
    }
    if (crypto_scalarmult_ed25519_base_noclamp(out.pk, out.sk) != 0) {
        why = "scalar multiplication failed";
        return false;
    }
    if (crypto_core_ed25519_is_valid_point(out.pk) != 1) {
        why = "public key is not a canonical point of the prime-order group";
        return false;
    }
    out.address = onion::address(out.pk);
    bool match = false;
    for (const std::string &p : prefixes)
        if (out.address.compare(0, p.size(), p) == 0) match = true;
    if (!match) {
        why = "address " + out.address + " does not match any prefix";
        return false;
    }
    static const uint8_t msg[] = "cipollino key validation";
    uint8_t sig[64];
    if (!sign_expanded(sig, msg, sizeof msg, out.sk, out.pk) ||
        crypto_sign_ed25519_verify_detached(sig, msg, sizeof msg, out.pk) != 0) {
        why = "signature made with the derived key does not verify";
        return false;
    }
    return true;
}

static bool write_file(const std::string &path, const void *data, size_t len, mode_t mode, std::string &err)
{
    int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, mode);
    if (fd < 0) {
        err = path + ": " + strerror(errno);
        return false;
    }
    const uint8_t *p = (const uint8_t *)data;
    while (len > 0) {
        ssize_t n = write(fd, p, len);
        if (n < 0) {
            if (errno == EINTR) continue;
            err = path + ": " + strerror(errno);
            close(fd);
            return false;
        }
        p += n;
        len -= (size_t)n;
    }
    if (fsync(fd) != 0 || close(fd) != 0) {
        err = path + ": " + strerror(errno);
        return false;
    }
    return true;
}

bool write_tor_dir(const std::string &dir, const Key &key, std::string &err)
{
    if (mkdir(dir.c_str(), 0700) != 0 && errno != EEXIST) {
        err = dir + ": " + strerror(errno);
        return false;
    }
    std::string sub = dir + "/" + key.address + ".onion";
    if (mkdir(sub.c_str(), 0700) != 0) {
        err = sub + ": " + strerror(errno);
        return false;
    }
    // 32-byte headers: the tag padded with NULs.
    uint8_t pub[32 + 32] = "== ed25519v1-public: type0 ==";
    uint8_t sec[32 + 64] = "== ed25519v1-secret: type0 ==";
    memcpy(pub + 32, key.pk, 32);
    memcpy(sec + 32, key.sk, 64);
    std::string host = key.address + ".onion\n";
    bool ok = write_file(sub + "/hostname", host.data(), host.size(), 0600, err) &&
              write_file(sub + "/hs_ed25519_public_key", pub, sizeof pub, 0600, err) &&
              write_file(sub + "/hs_ed25519_secret_key", sec, sizeof sec, 0600, err);
    sodium_memzero(sec, sizeof sec);
    return ok;
}

}  // namespace keys
