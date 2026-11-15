// Compute backend interface: one instance drives one device.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "filter.h"

struct Hit {
    uint32_t tid;    // kernel thread
    uint32_t batch;  // batch index within the launch
    uint32_t code;   // 0: the centre; otherwise 2*i + s: centre + i*G8 (s=0) or centre - i*G8 (s=1)
};

class Backend {
public:
    static constexpr uint32_t kHitCap = 4096;

    virtual ~Backend() {}
    virtual std::string describe() const = 0;

    // Defaults for this device: kernel thread count, how many bytes of scratch memory
    // the search may reasonably take, and the scratch mode to use when not calibrating.
    virtual uint32_t suggested_threads() const = 0;
    virtual uint64_t scratch_budget() const = 0;
    virtual uint32_t suggested_pairs() const = 0;
    // Scratch modes (CIP_SCRATCH_*) worth trying on this device; with more than one, the
    // engine measures each at start-up and keeps the fastest.
    virtual std::vector<uint32_t> scratch_candidates() const = 0;
    // Preferred wall time of one launch, in seconds.
    virtual double suggested_launch_seconds() const { return 0.25; }

    // Allocate device buffers for `nthreads` kernel threads and `npairs` pairs per batch
    // in scratch mode `smode` (CIP_SCRATCH_*), and upload the precomputed table
    // (ed::build_table(npairs)).
    virtual void configure(uint32_t nthreads, uint32_t npairs, uint32_t smode, const std::vector<uint32_t> &table) = 0;
    virtual void set_filter(const Filter &filter) = 0;

    // Per-thread centre points, CIP_STATE_WORDS words each.
    virtual void write_state(uint32_t tid, uint32_t count, const uint32_t *words) = 0;
    virtual void read_state(uint32_t tid, uint32_t count, uint32_t *words) = 0;

    // Run `nbatches` batches on every thread and collect the hits. Returns the number of
    // hits the kernel saw; if that exceeds kHitCap the excess was dropped.
    virtual uint32_t launch(uint32_t nbatches, std::vector<Hit> &hits) = 0;
};

// Each returns an empty list if the backend is not compiled in or finds no device.
// `devices` selects CUDA device indices; empty means all.
std::vector<std::unique_ptr<Backend>> create_cuda_backends(const std::vector<int> &devices, std::string &err);
std::vector<std::unique_ptr<Backend>> create_metal_backends(std::string &err);
std::vector<std::unique_ptr<Backend>> create_cpu_backends(unsigned workers);

// Scratch bytes the kernel needs for a configuration.
uint64_t scratch_bytes(uint32_t nthreads, uint32_t npairs, uint32_t smode);
