#include "onion.h"

#include <cstring>

namespace onion {

  static inline uint64_t rotl(uint64_t x, int n) { return (x << n) | (x >> (64 - n)); }

  static void keccak_f(uint64_t s[25]) {
    static const uint64_t rc[24] = {
      0x0000000000000001ull, 0x0000000000008082ull, 0x800000000000808aull, 0x8000000080008000ull, 0x000000000000808bull,
      0x0000000080000001ull, 0x8000000080008081ull, 0x8000000000008009ull, 0x000000000000008aull, 0x0000000000000088ull,
      0x0000000080008009ull, 0x000000008000000aull, 0x000000008000808bull, 0x800000000000008bull, 0x8000000000008089ull,
      0x8000000000008003ull, 0x8000000000008002ull, 0x8000000000000080ull, 0x000000000000800aull, 0x800000008000000aull,
      0x8000000080008081ull, 0x8000000000008080ull, 0x0000000080000001ull, 0x8000000080008008ull
    };
    static const int rot[24] = { 1,  3,  6,  10, 15, 21, 28, 36, 45, 55, 2,  14,
                                 27, 41, 56, 8,  25, 43, 62, 18, 39, 61, 20, 44 };
    static const int pil[24] = {
      10, 7, 11, 17, 18, 3, 5, 16, 8, 21, 24, 4, 15, 23, 19, 13, 12, 2, 20, 14, 22, 9, 6, 1
    };
    for (int round = 0; round < 24; round++) {
      uint64_t bc[5];
      for (int i = 0; i < 5; i++)
        bc[i] = s[i] ^ s[i + 5] ^ s[i + 10] ^ s[i + 15] ^ s[i + 20];
      for (int i = 0; i < 5; i++) {
        uint64_t t = bc[(i + 4) % 5] ^ rotl(bc[(i + 1) % 5], 1);
        for (int j = 0; j < 25; j += 5)
          s[j + i] ^= t;
      }
      uint64_t t = s[1];
      for (int i = 0; i < 24; i++) {
        int j      = pil[i];
        uint64_t u = s[j];
        s[j]       = rotl(t, rot[i]);
        t          = u;
      }
      for (int j = 0; j < 25; j += 5) {
        for (int i = 0; i < 5; i++)
          bc[i] = s[j + i];
        for (int i = 0; i < 5; i++)
          s[j + i] ^= (~bc[(i + 1) % 5]) & bc[(i + 2) % 5];
      }
      s[0] ^= rc[round];
    }
  }

  void sha3_256(uint8_t out[32], const uint8_t *in, size_t len) {
    const size_t rate = 136;
    uint64_t s[25]    = { 0 };
    uint8_t block[136];
    while (len >= rate) {
      for (size_t i = 0; i < rate / 8; i++) {
        uint64_t w;
        memcpy(&w, in + 8 * i, 8);
        s[i] ^= w;
      }
      keccak_f(s);
      in += rate;
      len -= rate;
    }
    memset(block, 0, sizeof block);
    memcpy(block, in, len);
    block[len] = 0x06;
    block[rate - 1] |= 0x80;
    for (size_t i = 0; i < rate / 8; i++) {
      uint64_t w;
      memcpy(&w, block + 8 * i, 8);
      s[i] ^= w;
    }
    keccak_f(s);
    memcpy(out, s, 32);
  }

  std::string address(const uint8_t pk[32]) {
    static const char prefix[] = ".onion checksum";
    uint8_t buf[15 + 32 + 1], digest[32], raw[35];
    memcpy(buf, prefix, 15);
    memcpy(buf + 15, pk, 32);
    buf[47] = 0x03;
    sha3_256(digest, buf, sizeof buf);
    memcpy(raw, pk, 32);
    raw[32] = digest[0];
    raw[33] = digest[1];
    raw[34] = 0x03;

    return base32(raw, sizeof raw); // 35 bytes = 280 bits = exactly 56 characters
  }

  std::string base32(const uint8_t *data, size_t len) {
    static const char alphabet[] = "abcdefghijklmnopqrstuvwxyz234567";
    std::string out((len * 8 + 4) / 5, '?');
    for (size_t i = 0; i < out.size(); i++) {
      size_t bit = 5 * i;
      uint32_t w = ((uint32_t)data[bit / 8] << 8) | (bit / 8 + 1 < len ? data[bit / 8 + 1] : 0);
      out[i]     = alphabet[(w >> (11 - bit % 8)) & 31];
    }
    return out;
  }

  bool normalize_address(std::string &address) {
    const std::string suffix = ".onion";
    if (address.size() >= suffix.size() && address.compare(address.size() - suffix.size(), suffix.size(), suffix) == 0)
      address.resize(address.size() - suffix.size());
    if (address.size() != 56)
      return false;
    uint8_t pk[32] = { 0 };
    for (size_t i = 0; i < 56; i++) {
      char &c = address[i];
      if (c >= 'A' && c <= 'Z')
        c = (char)(c - 'A' + 'a');
      int v = base32_value(c);
      if (v < 0)
        return false;
      for (int b = 0; b < 5; b++) {
        size_t bit = 5 * i + b;
        if (bit < 256 && ((v >> (4 - b)) & 1))
          pk[bit / 8] |= (uint8_t)(0x80 >> (bit % 8));
      }
    }
    return onion::address(pk) == address;
  }

  int base32_value(char c) {
    if (c >= 'a' && c <= 'z')
      return c - 'a';
    if (c >= 'A' && c <= 'Z')
      return c - 'A';
    if (c >= '2' && c <= '7')
      return c - '2' + 26;
    return -1;
  }

  // The characters a wildcard letter stands for, as a set of base32 values; 0 if it is none.
  static uint32_t wildcard_set(char c) {
    switch (c) {
    case 'd': return 0xfc000000u; // 2-7 are the values 26..31
    case 'l': return 0x03ffffffu;
    case 'a': return 0xffffffffu;
    }
    return 0;
  }

  bool normalize_prefix(std::string &prefix, std::string &err, bool client) {
    const std::string suffix = ".onion", given = prefix;
    if (
      !client && prefix.size() >= suffix.size() &&
      prefix.compare(prefix.size() - suffix.size(), suffix.size(), suffix) == 0
    )
      prefix.resize(prefix.size() - suffix.size());
    if (prefix.empty()) {
      err = "empty prefix";
      return false;
    }
    size_t length = 0;
    for (size_t i = 0; i < prefix.size(); i++, length++) {
      char &c = prefix[i];
      if (c >= 'A' && c <= 'Z')
        c = (char)(c - 'A' + 'a');
      if (c == '?') {
        char &w = prefix[++i < prefix.size() ? i : i - 1];
        if (w >= 'A' && w <= 'Z')
          w = (char)(w - 'A' + 'a');
        if (i >= prefix.size() || !wildcard_set(w)) {
          err = "prefix '" + given + "': a wildcard is ?d (digit), ?l (letter) or ?a (either)";
          return false;
        }
      } else if (base32_value(c) < 0) {
        err = "prefix '" + given + "' contains '" + std::string(1, c) +
              "': " + (client ? "keys in base32" : "onion addresses") + " only use a-z and 2-7";
        return false;
      }
    }
    if (length > (client ? 52u : 56u)) {
      err = "prefix '" + given + "' is longer than " +
            (client ? "a key (52 characters)" : "an onion address (56 characters)");
      return false;
    }
    return true;
  }

  std::vector<uint32_t> prefix_sets(const std::string &prefix) {
    std::vector<uint32_t> sets;
    for (size_t i = 0; i < prefix.size(); i++)
      sets.push_back(prefix[i] == '?' ? wildcard_set(prefix[++i]) : 1u << base32_value(prefix[i]));
    return sets;
  }

  std::string prefix_head(const std::string &prefix, size_t n) {
    size_t i = 0;
    for (; i < prefix.size() && n > 0; n--)
      i += prefix[i] == '?' ? 2 : 1;
    return prefix.substr(0, i);
  }

  bool prefix_matches(const std::string &prefix, const std::string &address) {
    const std::vector<uint32_t> sets = prefix_sets(prefix);
    if (address.size() < sets.size())
      return false;
    for (size_t i = 0; i < sets.size(); i++) {
      int v = base32_value(address[i]);
      if (v < 0 || !((sets[i] >> v) & 1))
        return false;
    }
    return true;
  }

} // namespace onion
