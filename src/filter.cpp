#include "filter.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#define CIP_TARGET_CPU
#include "kernel/defs.h"
#include "onion.h"

// Address bit n (0 = first, most significant bit of the first character) is bit 7 - n%8
// of public key byte n/8. The kernel reads the first 8 key bytes as a little-endian u64,
// which puts that bit at position 8*(n/8) + 7 - n%8.
static inline int le_position(int n) { return 8 * (n / 8) + 7 - n % 8; }

static void prefix_mask(const std::string &p, uint64_t &mask, uint64_t &value)
{
    mask = value = 0;
    int nbits = std::min<int>(5 * (int)p.size(), 64);
    for (int n = 0; n < nbits; n++) {
        int v = onion::base32_value(p[n / 5]);
        uint64_t bit = (v >> (4 - n % 5)) & 1;
        mask |= (uint64_t)1 << le_position(n);
        value |= bit << le_position(n);
    }
}

Filter build_filter(const std::vector<std::string> &prefixes)
{
    Filter f;
    for (const std::string &p : prefixes) {
        uint64_t mask, value;
        prefix_mask(p, mask, value);
        f.records.push_back((uint32_t)mask);
        f.records.push_back((uint32_t)value);
        f.records.push_back((uint32_t)(mask >> 32));
        f.records.push_back((uint32_t)(value >> 32));
    }
    if (prefixes.size() <= CIP_MAX_DIRECT) {
        f.mode = CIP_MODE_DIRECT;
        for (size_t i = 0; i < prefixes.size(); i++) {
            f.dm[i] = f.records[4 * i];
            f.dv[i] = f.records[4 * i + 1];
        }
        return f;
    }
    f.mode = CIP_MODE_BITMAP;
    f.bitmap.assign(CIP_BITMAP_WORDS, 0);
    const uint32_t full = (1u << CIP_BITMAP_BITS) - 1;
    for (size_t i = 0; i < prefixes.size(); i++) {
        uint32_t mask = f.records[4 * i] & full, value = f.records[4 * i + 1] & full;
        // Enumerate every index that agrees with the prefix on the masked bits.
        uint32_t freebits = ~mask & full, sub = 0;
        do {
            uint32_t idx = value | sub;
            f.bitmap[idx >> 5] |= 1u << (idx & 31);
            sub = (sub - freebits) & freebits;
        } while (sub != 0);
    }
    return f;
}

bool Filter::matches(const uint8_t pk[32]) const
{
    uint32_t lo, hi;
    memcpy(&lo, pk, 4);
    memcpy(&hi, pk + 4, 4);
    for (size_t i = 0; i < records.size(); i += 4)
        if ((lo & records[i]) == records[i + 1] && (hi & records[i + 2]) == records[i + 3]) return true;
    return false;
}

double match_probability(std::vector<std::string> prefixes)
{
    std::sort(prefixes.begin(), prefixes.end());
    double p = 0;
    std::string last;
    bool have = false;
    for (const std::string &s : prefixes) {
        // Sorted order puts every extension right after (a chain from) its shortest stem.
        if (have && s.compare(0, last.size(), last) == 0) continue;
        p += std::pow(32.0, -(double)s.size());
        last = s;
        have = true;
    }
    return p;
}
