// Search driver: seeds the kernel threads, runs launches, validates and stores hits.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "backend.h"

struct Options {
    std::vector<std::string> prefixes;  // normalized
    uint64_t want = 1;                  // stop after this many keys; 0 = until interrupted
    std::string outdir = ".";
    uint32_t threads = 0;               // kernel threads per device; 0 = backend default
    uint32_t pairs = 0;                 // pairs per batch; 0 = fit the backend's memory budget
    int scratch = -1;                   // CIP_SCRATCH_* to force a scratch mode; -1 = backend's choice
    double launch_seconds = 0;          // 0 = backend default
    double bench_seconds = 0;           // > 0: measure throughput for this long, write nothing
    bool filter_tables = false;         // bitmap pre-filter even for few prefixes (see filter.h)
    bool quiet = false;
};

// Runs the search on all given backends. Returns a process exit code.
int run_search(std::vector<std::unique_ptr<Backend>> &backends, const Options &opt);

// Measures every scratch mode at several thread counts on each backend and prints the
// fastest combination as command-line flags. Honours opt.threads / opt.pairs as the
// starting point. Returns a process exit code.
int run_tune(std::vector<std::unique_ptr<Backend>> &backends, const Options &opt);

// Exhaustive cross-check of a backend against libsodium on small configurations: every
// candidate key of every thread is recomputed on the CPU and the kernel must report
// exactly the matching ones, in every filter mode. Returns false and explains in `log`.
bool selftest_exhaustive(Backend &be, std::string &log);

// Async-signal-safe request to stop after the current launch.
void request_stop();
