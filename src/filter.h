// Turns address prefixes into the kernel's filter tables.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct Filter {
    uint32_t mode = 0;               // CIP_MODE_*
    uint32_t dm[12];                 // direct modes: pre-filter on the low 32 bits, as in
    uint32_t dv[12];                 // cip_params (an unused entry never matches)
    std::vector<uint32_t> records;   // 4 words per prefix: mask_lo, value_lo, mask_hi, value_hi
    std::vector<uint32_t> bitmap;    // bitmap mode only: CIP_BITMAP_WORDS words
    Filter();
    uint32_t count() const { return (uint32_t)(records.size() / 4); }

    // The kernel's decision for a public key, evaluated on the host.
    bool matches(const uint8_t pk[32]) const;
};

// Prefixes must already be normalized (onion::normalize_prefix). The GPU compares the
// first 64 bits (12.8 characters) of the key; longer prefixes are finished on the host.
// Up to twelve prefixes get a direct mode unless `tables` asks for the bitmap mode, which
// any longer list gets.
Filter build_filter(const std::vector<std::string> &prefixes, bool tables = false);

// Removes duplicates and prefixes that are extensions of another one in the list, and
// returns the probability that a uniformly random address matches any of them.
double match_probability(std::vector<std::string> prefixes);
