// CPU backend: runs the very same kernel source on host threads. It exists to test the
// kernel logic without a GPU and as a fallback; it is not a tuned CPU implementation.
#include <algorithm>
#include <atomic>
#include <cstring>
#include <thread>

#include "backend.h"

#define CIP_TARGET_CPU
#define CIP_SCRATCH_AOS 1
#include "kernel/defs.h"
#include "kernel/field.h"
#include "kernel/search.h"

uint64_t scratch_bytes(uint32_t nthreads, uint32_t npairs, uint32_t smode)
{
    uint64_t slots = smode == CIP_SCRATCH_FULL ? 2 * (uint64_t)npairs
                   : smode == CIP_SCRATCH_HALF ? (uint64_t)npairs / 2 : (uint64_t)npairs;
    return slots * 8 * 4 * nthreads;
}

#ifndef CIP_HAVE_CUDA
std::vector<std::unique_ptr<Backend>> create_cuda_backends(const std::vector<int> &, std::string &err)
{
    err = "this build has no CUDA support (build on a machine with nvcc)";
    return {};
}
#endif

#ifndef CIP_HAVE_ROCM
std::vector<std::unique_ptr<Backend>> create_rocm_backends(const std::vector<int> &, std::string &err)
{
    err = "this build has no ROCm support (build on a machine with hipcc)";
    return {};
}
#endif

#ifndef CIP_HAVE_VULKAN
std::vector<std::unique_ptr<Backend>> create_vulkan_backends(const std::vector<int> &, std::string &err)
{
    err = "this build has no Vulkan support (needs the Vulkan headers and glslangValidator)";
    return {};
}
#endif

#ifndef CIP_HAVE_METAL
std::vector<std::unique_ptr<Backend>> create_metal_backends(std::string &err)
{
    err = "this build has no Metal support";
    return {};
}
#endif

namespace {

class CpuBackend : public Backend {
public:
    explicit CpuBackend(unsigned workers) : workers_(std::max(1u, workers)) {}

    std::string describe() const override { return "CPU (" + std::to_string(workers_) + " host threads)"; }
    uint32_t suggested_threads() const override { return workers_ * 4; }
    uint64_t scratch_budget() const override { return scratch_bytes(workers_ * 4, 256, CIP_SCRATCH_LEAN); }
    uint32_t suggested_pairs() const override { return 256; }
    std::vector<uint32_t> scratch_candidates() const override { return {CIP_SCRATCH_LEAN}; }

    // One kernel, with the field arithmetic this file was compiled for (CIP_ARITH).
    std::vector<uint32_t> arith_candidates() const override { return {CIP_ARITH}; }
    void set_arith(uint32_t arith) override
    {
        if (arith != CIP_ARITH) throw std::runtime_error(describe() + ": not built with that field arithmetic");
    }

    void configure(uint32_t nthreads, uint32_t npairs, uint32_t smode, const std::vector<uint32_t> &table) override
    {
        prm_ = cip_params();
        prm_.nthreads = nthreads;
        prm_.npairs = npairs;
        smode_ = smode;
        prm_.hitcap = kHitCap;
        table_ = table;
        state_.assign((size_t)nthreads * CIP_STATE_WORDS, 0);
        scratch_.assign(scratch_bytes(nthreads, npairs, smode) / 4, 0);
        hits_.assign((size_t)kHitCap * CIP_HIT_WORDS, 0);
        bitmap_.assign(1, 0);
        filt_.assign(4, 0);
    }

    void set_filter(const Filter &f) override
    {
        prm_.mode = f.mode;
        prm_.nfilt = f.count();
        memcpy(prm_.dm, f.dm, sizeof prm_.dm);
        memcpy(prm_.dv, f.dv, sizeof prm_.dv);
        filt_ = f.records;
        bitmap_ = f.bitmap.empty() ? std::vector<uint32_t>(1, 0) : f.bitmap;
    }

    void write_state(uint32_t tid, uint32_t count, const uint32_t *words) override
    {
        memcpy(&state_[(size_t)tid * CIP_STATE_WORDS], words, (size_t)count * CIP_STATE_WORDS * 4);
    }

    void read_state(uint32_t tid, uint32_t count, uint32_t *words) override
    {
        memcpy(words, &state_[(size_t)tid * CIP_STATE_WORDS], (size_t)count * CIP_STATE_WORDS * 4);
    }

    uint32_t launch(uint32_t nbatches, std::vector<Hit> &hits, uint32_t first, uint32_t count) override
    {
        prm_.nbatches = nbatches;
        prm_.first = first;
        prm_.count = count;
        uint32_t ctr = 0;
        const uint32_t end = (uint32_t)std::min<uint64_t>((uint64_t)first + count, prm_.nthreads);
        std::atomic<uint32_t> next(first);
        auto body = [&]() {
            for (;;) {
                uint32_t tid = next.fetch_add(1);
                if (tid >= end) break;
                run_thread(tid, &ctr);
            }
        };
        std::vector<std::thread> pool;
        for (unsigned i = 1; i < workers_; i++) pool.emplace_back(body);
        body();
        for (std::thread &t : pool) t.join();

        hits.clear();
        for (uint32_t i = 0; i < std::min(ctr, kHitCap); i++)
            hits.push_back(Hit{hits_[3 * i], hits_[3 * i + 1], hits_[3 * i + 2]});
        return ctr;
    }

private:
    void run_thread(uint32_t tid, uint32_t *hitctr)
    {
        uint32_t *state = state_.data(), *scratch = scratch_.data(), *hits = hits_.data();
        const uint32_t *table = table_.data(), *filt = filt_.data(), *bitmap = bitmap_.data();
        if (smode_ == CIP_SCRATCH_X25519)
            cip_search_thread<CIP_SCRATCH_X25519>(tid, prm_ CIP_BUFS);
        else if (smode_ == CIP_SCRATCH_FULL)
            cip_search_thread<CIP_SCRATCH_FULL>(tid, prm_ CIP_BUFS);
        else if (smode_ == CIP_SCRATCH_LEAN)
            cip_search_thread<CIP_SCRATCH_LEAN>(tid, prm_ CIP_BUFS);
        else
            cip_search_thread<CIP_SCRATCH_HALF>(tid, prm_ CIP_BUFS);
    }

    std::vector<uint32_t> table_, state_, scratch_, hits_, filt_, bitmap_;
    cip_params prm_;
    unsigned workers_;
    uint32_t smode_ = CIP_SCRATCH_LEAN;
};

}  // namespace

std::vector<std::unique_ptr<Backend>> create_cpu_backends(unsigned workers)
{
    std::vector<std::unique_ptr<Backend>> v;
    v.emplace_back(new CpuBackend(workers));
    return v;
}
