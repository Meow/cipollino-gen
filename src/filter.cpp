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
    const std::vector<uint32_t> sets = onion::prefix_sets(p);
    for (int n = 0; n < 64 && n / 5 < (int)sets.size(); n++) {
        // A bit of a position counts if every character allowed there has it the same.
        uint32_t ones = 0;
        for (int v = 0; v < 32; v++)
            if ((v >> (4 - n % 5)) & 1) ones |= 1u << v;
        const uint32_t set = sets[n / 5];
        if ((set & ones) != 0 && (set & ~ones) != 0) continue;
        mask |= (uint64_t)1 << le_position(n);
        value |= (uint64_t)((set & ones) != 0) << le_position(n);
    }
}

Filter::Filter()
{
    static_assert(sizeof dm == sizeof(cip_params().dm) && sizeof dm / sizeof dm[0] == CIP_MAX_DIRECT, "direct slots");
    for (uint32_t &m : dm) m = 0;
    for (uint32_t &v : dv) v = 1;
}

// Sets, in the bit table of 2^bits bits at word `at`, every index that agrees with the
// prefix (mask, value: its first 32 key bits) on key bits shift .. shift+bits-1.
static void mark(std::vector<uint32_t> &bitmap, size_t at, uint32_t mask, uint32_t value, int shift, int bits)
{
    const uint32_t full = (1u << bits) - 1;
    mask = (mask >> shift) & full;
    value = (value >> shift) & full;
    uint32_t freebits = ~mask & full, sub = 0;
    do {
        uint32_t idx = value | sub;
        bitmap[at + (idx >> 5)] |= 1u << (idx & 31);
        sub = (sub - freebits) & freebits;
    } while (sub != 0);
}

Filter build_filter(const std::vector<std::string> &prefixes, bool tables)
{
    Filter f;
    for (const std::string &p : prefixes) {
        uint64_t mask, value;
        prefix_mask(p, mask, value);
        f.records.push_back((uint32_t)mask);
        f.records.push_back((uint32_t)value);
        f.records.push_back((uint32_t)(mask >> 32));
        f.records.push_back((uint32_t)(value >> 32));
        int bits = 0;
        for (uint64_t m = mask; m; m &= m - 1) bits++;
        f.density = std::min(1.0, f.density + std::ldexp(1.0, -bits));
    }
    if (prefixes.size() <= CIP_MAX_DIRECT && !tables) {
        f.mode = prefixes.size() <= CIP_FEW_DIRECT ? CIP_MODE_DIRECT : CIP_MODE_DIRECT_MANY;
        for (size_t i = 0; i < prefixes.size(); i++) {
            f.dm[i] = f.records[4 * i];
            f.dv[i] = f.records[4 * i + 1];
        }
        return f;
    }
    f.mode = CIP_MODE_BITMAP;
    f.bitmap.assign(CIP_BITMAP_WORDS, 0);
    for (size_t i = 0; i < prefixes.size(); i++) {
        uint32_t mask = f.records[4 * i], value = f.records[4 * i + 1];
        mark(f.bitmap, 0, mask, value, 0, CIP_BITMAP_NEAR_BITS);
        mark(f.bitmap, CIP_BITMAP_FAR_A, mask, value, 0, CIP_BITMAP_FAR_BITS);
        mark(f.bitmap, CIP_BITMAP_FAR_B, mask, value, 32 - CIP_BITMAP_FAR_BITS, CIP_BITMAP_FAR_BITS);
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
    prefixes.erase(std::unique(prefixes.begin(), prefixes.end()), prefixes.end());
    bool wild = false;
    for (const std::string &s : prefixes) wild = wild || s.find('?') != std::string::npos;
    double p = 0;
    if (!wild) {
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
    // With wildcards one prefix covers another if it is no longer and allows, at each of
    // its positions, everything the other does. Lists with wildcards are short.
    std::vector<std::vector<uint32_t>> sets;
    for (const std::string &s : prefixes) sets.push_back(onion::prefix_sets(s));
    for (size_t i = 0; i < sets.size(); i++) {
        bool covered = false;
        for (size_t j = 0; j < sets.size() && !covered; j++) {
            if (j == i || sets[j].size() > sets[i].size() || (sets[j] == sets[i] && j > i)) continue;
            covered = true;
            for (size_t k = 0; k < sets[j].size(); k++) covered = covered && (sets[i][k] & ~sets[j][k]) == 0;
        }
        if (covered) continue;
        double q = 1;
        for (uint32_t set : sets[i]) {
            int n = 0;
            for (uint32_t m = set; m; m &= m - 1) n++;
            q *= n / 32.0;
        }
        p += q;
    }
    return std::min(p, 1.0);
}
