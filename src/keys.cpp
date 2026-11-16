#include "keys.h"

#include <sodium.h>

#include <cstring>

#include "onion.h"
#include "platform.h"

namespace keys {

Key::Key()
{
    memset(sk, 0, sizeof sk);
    memset(pk, 0, sizeof pk);
}

Key::~Key() { sodium_memzero(sk, sizeof sk); }

ClientKey::ClientKey()
{
    memset(sk, 0, sizeof sk);
    memset(pk, 0, sizeof pk);
}

ClientKey::~ClientKey() { sodium_memzero(sk, sizeof sk); }

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
    uint64_t mag = negative ? 0 - (uint64_t)m : (uint64_t)m;
    uint8_t delta[32] = {0};  // 8 * |m|
    for (int i = 0; i < 8; i++) delta[i] = (uint8_t)((mag << 3) >> (8 * i));
    delta[8] = (uint8_t)(mag >> 61);
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
        if (onion::prefix_matches(p, out.address)) match = true;
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

bool client_public_key(uint8_t pk[32], const uint8_t seed[32], Offset m)
{
    uint8_t sk[64];
    expand(sk, seed);
    bool ok = shift_scalar(sk, m) && crypto_scalarmult_base(pk, sk) == 0;
    sodium_memzero(sk, sizeof sk);
    return ok;
}

bool finalize_client(ClientKey &out, const uint8_t seed[32], Offset m, const std::vector<std::string> &prefixes,
                     std::string &why)
{
    uint8_t sk[64], ed_pk[32], converted[32], other_sk[32], other_pk[32], mine[32], theirs[32];
    expand(sk, seed);
    bool clamped = shift_scalar(sk, m);
    memcpy(out.sk, sk, 32);
    bool derived = crypto_scalarmult_base(out.pk, out.sk) == 0 && crypto_scalarmult_ed25519_base_noclamp(ed_pk, sk) == 0;
    sodium_memzero(sk, sizeof sk);
    if (!clamped) {
        why = "scalar left the clamped range";
        return false;
    }
    if (!derived) {
        why = "scalar multiplication failed";
        return false;
    }
    out.text = onion::base32(out.pk, 32);
    if (crypto_sign_ed25519_pk_to_curve25519(converted, ed_pk) != 0 || sodium_memcmp(converted, out.pk, 32) != 0) {
        why = "public key is not the Montgomery form of the scalar's Ed25519 key";
        return false;
    }
    bool match = false;
    for (const std::string &p : prefixes)
        if (onion::prefix_matches(p, out.text)) match = true;
    if (!match) {
        why = "key " + out.text + " does not match any prefix";
        return false;
    }
    randombytes_buf(other_sk, sizeof other_sk);
    bool agree = crypto_scalarmult_base(other_pk, other_sk) == 0 && crypto_scalarmult(mine, out.sk, other_pk) == 0 &&
                 crypto_scalarmult(theirs, other_sk, out.pk) == 0 && sodium_memcmp(mine, theirs, 32) == 0;
    sodium_memzero(other_sk, sizeof other_sk);
    sodium_memzero(mine, sizeof mine);
    sodium_memzero(theirs, sizeof theirs);
    if (!agree) {
        why = "key exchange with the derived key does not agree";
        return false;
    }
    return true;
}

static std::string upper(std::string s)
{
    for (char &c : s)
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
    return s;
}

bool write_client_key(const std::string &dir, const ClientKey &key, const std::string &service, std::string &name,
                      std::string &err)
{
    name = upper(key.text);
    if (!plat::make_private_dir(dir, true, err)) return false;
    std::string pub = "descriptor:x25519:" + name + "\n";
    std::string sec = upper(onion::base32(key.sk, 32)) + "\n";
    if (!service.empty()) sec = service + ":descriptor:x25519:" + sec;
    bool ok = plat::write_new_file(dir + "/" + name + ".auth", pub.data(), pub.size(), err) &&
              plat::write_new_file(dir + "/" + name + (service.empty() ? ".private" : ".auth_private"), sec.data(), sec.size(), err);
    sodium_memzero(&sec[0], sec.size());
    return ok;
}

bool write_tor_dir(const std::string &dir, const Key &key, std::string &err)
{
    std::string sub = dir + "/" + key.address + ".onion";
    if (!plat::make_private_dir(dir, true, err) || !plat::make_private_dir(sub, false, err)) return false;
    // 32-byte headers: the tag padded with NULs.
    uint8_t pub[32 + 32] = "== ed25519v1-public: type0 ==";
    uint8_t sec[32 + 64] = "== ed25519v1-secret: type0 ==";
    memcpy(pub + 32, key.pk, 32);
    memcpy(sec + 32, key.sk, 64);
    std::string host = key.address + ".onion\n";
    bool ok = plat::write_new_file(sub + "/hostname", host.data(), host.size(), err) &&
              plat::write_new_file(sub + "/hs_ed25519_public_key", pub, sizeof pub, err) &&
              plat::write_new_file(sub + "/hs_ed25519_secret_key", sec, sizeof sec, err);
    sodium_memzero(sec, sizeof sec);
    return ok;
}

bool verify_tor_dir(const std::string &dir, std::string &address, std::vector<std::string> &notes, std::string &err)
{
    static const uint8_t pub_tag[32] = "== ed25519v1-public: type0 ==";
    static const uint8_t sec_tag[32] = "== ed25519v1-secret: type0 ==";
    static const uint8_t msg[] = "cipollino key validation";
    uint8_t pub[32 + 32], sec[32 + 64], pk[32], sig[64];
    char host[128];
    size_t pub_len = 0, sec_len = 0, host_len = 0;
    notes.clear();

    bool ok = plat::read_file(dir + "/hs_ed25519_secret_key", sec, sizeof sec, sec_len, err) &&
              plat::read_file(dir + "/hs_ed25519_public_key", pub, sizeof pub, pub_len, err) &&
              plat::read_file(dir + "/hostname", host, sizeof host, host_len, err);
    if (ok && (sec_len != sizeof sec || memcmp(sec, sec_tag, 32) != 0)) {
        err = "hs_ed25519_secret_key is not a 96-byte Tor secret key file";
        ok = false;
    }
    if (ok && (pub_len != sizeof pub || memcmp(pub, pub_tag, 32) != 0)) {
        err = "hs_ed25519_public_key is not a 64-byte Tor public key file";
        ok = false;
    }
    const uint8_t *sk = sec + 32;
    if (ok && (crypto_scalarmult_ed25519_base_noclamp(pk, sk) != 0 || sodium_memcmp(pk, pub + 32, 32) != 0)) {
        err = "the public key file does not hold the public key of the secret key";
        ok = false;
    }
    if (ok && crypto_core_ed25519_is_valid_point(pk) != 1) {
        err = "the public key is not a canonical point of the prime-order group";
        ok = false;
    }
    if (ok && (!sign_expanded(sig, msg, sizeof msg, sk, pk) ||
               crypto_sign_ed25519_verify_detached(sig, msg, sizeof msg, pk) != 0)) {
        err = "a signature made with the secret key does not verify";
        ok = false;
    }
    if (ok) {
        address = onion::address(pk);
        std::string name(host, host_len);
        while (!name.empty() && (name.back() == '\n' || name.back() == '\r')) name.pop_back();
        if (name != address + ".onion") {
            err = "hostname says " + name + ", the key's address is " + address + ".onion";
            ok = false;
        }
    }
    if (ok) {
        if ((sk[0] & 7) != 0 || (sk[31] & 0xc0) != 0x40) notes.push_back("the secret scalar is not in clamped form");
        if (plat::open_to_others(dir)) notes.push_back("the directory is open to other users; Tor insists on mode 0700");
        if (plat::open_to_others(dir + "/hs_ed25519_secret_key")) notes.push_back("the secret key file is open to other users");
    }
    sodium_memzero(sec, sizeof sec);
    return ok;
}

}  // namespace keys
