// Compute backend interface: one instance drives one device.
#pragma once

#include <cstdint>
#include <memory>
#include <stdexcept>
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
    // Thread counts worth trying where the device's width is not known, ascending; with
    // more than one, the engine measures them at start-up.
    virtual std::vector<uint32_t> thread_candidates() const { return {suggested_threads()}; }
    virtual uint64_t scratch_budget() const = 0;
    virtual uint32_t suggested_pairs() const = 0;
    // Scratch modes (CIP_SCRATCH_*) worth trying on this device; with more than one, the
    // engine measures each at start-up and keeps the fastest.
    virtual std::vector<uint32_t> scratch_candidates() const = 0;
    // Preferred wall time of one launch, in seconds.
    virtual double suggested_launch_seconds() const { return 0.25; }
    // Field arithmetics (CIP_ARITH_*) worth trying on this device, the likeliest winner
    // first; with more than one, the engine measures each at start-up and keeps the fastest.
    virtual std::vector<uint32_t> arith_candidates() const { return {0}; }
    // Use the kernels built for this field arithmetic from the next configure() on. Throws
    // if the backend has none; all have CIP_ARITH_WIDE (0).
    virtual void set_arith(uint32_t arith)
    {
        if (arith != 0) throw std::runtime_error(describe() + ": this backend has no kernel for that arithmetic");
    }

    // Allocate device buffers for `nthreads` kernel threads and `npairs` pairs per batch
    // in scratch mode `smode` (CIP_SCRATCH_*), and upload the precomputed table
    // (ed::build_table(npairs)).
    virtual void configure(uint32_t nthreads, uint32_t npairs, uint32_t smode, const std::vector<uint32_t> &table) = 0;
    virtual void set_filter(const Filter &filter) = 0;

    // Per-thread centre points, CIP_STATE_WORDS words each.
    virtual void write_state(uint32_t tid, uint32_t count, const uint32_t *words) = 0;
    virtual void read_state(uint32_t tid, uint32_t count, uint32_t *words) = 0;

    // Run `nbatches` batches on the threads first .. first + count - 1 (as many of them as
    // exist) and collect the hits; the other threads do not move. Returns the number of
    // hits the kernel saw; those beyond kHitCap are counted but not recorded.
    virtual uint32_t launch(uint32_t nbatches, std::vector<Hit> &hits, uint32_t first, uint32_t count) = 0;
    // The same on every thread.
    uint32_t launch(uint32_t nbatches, std::vector<Hit> &hits) { return launch(nbatches, hits, 0, UINT32_MAX); }
};

// Each returns an empty list if the backend is not compiled in or finds no device.
// `devices` selects device indices as that backend numbers them; empty means all.
std::vector<std::unique_ptr<Backend>> create_cuda_backends(const std::vector<int> &devices, std::string &err);
std::vector<std::unique_ptr<Backend>> create_rocm_backends(const std::vector<int> &devices, std::string &err);
std::vector<std::unique_ptr<Backend>> create_vulkan_backends(const std::vector<int> &devices, std::string &err);
std::vector<std::unique_ptr<Backend>> create_metal_backends(std::string &err);
std::vector<std::unique_ptr<Backend>> create_cpu_backends(unsigned workers);

// Scratch bytes the kernel needs for a configuration.
uint64_t scratch_bytes(uint32_t nthreads, uint32_t npairs, uint32_t smode);
