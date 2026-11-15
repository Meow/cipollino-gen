// CUDA backend (NVIDIA GPUs, nvcc) and ROCm backend (AMD GPUs, hipcc): one instance per
// device. HIP mirrors CUDA's API name for name, so the two differ only in the header
// below and in a few numbers; -DCIP_ROCM selects the ROCm build.
//
// For testing on machines without either, this file also builds as plain C++ against
// tests/cuda_sim.h, which emulates the handful of runtime calls used here
// (tests/sim_backend_cuda.cpp).
#if defined(CIP_CUDA_SIM)
// tests/cuda_sim.h has been included
#elif defined(CIP_ROCM)
#include "hip_as_cuda.h"
#define CIP_LAUNCH(kernel, grid, block, ...) kernel<<<(grid), (block)>>>(__VA_ARGS__)
#else
#include <cuda_runtime.h>
#define CIP_LAUNCH(kernel, grid, block, ...) kernel<<<(grid), (block)>>>(__VA_ARGS__)
#endif

// The kernel entry points have external linkage, so the two builds need different names
// to live in one binary.
#if defined(CIP_ROCM)
#define CIP_THREADS_PER_UNIT 1024
#define CIP_API "ROCm"
#define CIP_KERNEL cip_kernel_rocm
#define CIP_CREATE create_rocm_backends
#else
#define CIP_THREADS_PER_UNIT 2048
#define CIP_API "CUDA"
#define CIP_KERNEL cip_kernel_cuda
#define CIP_CREATE create_cuda_backends
#endif

#include <algorithm>
#include <cstring>
#include <stdexcept>

#include "backend.h"

#define CIP_TARGET_CUDA
#include "kernel/defs.h"
#include "kernel/field.h"
#include "kernel/search.h"

template <int SMODE>
__global__ void CIP_KERNEL(u32 *state, const u32 *table, u32 *scratch, u32 *hitctr, u32 *hits,
                           const u32 *filt, const u32 *bitmap, cip_params prm)
{
    u32 tid = blockIdx.x * blockDim.x + threadIdx.x;
    if (tid >= prm.nthreads) return;
    cip_search_thread<SMODE>(tid, prm CIP_BUFS);
}

namespace {

void check(cudaError_t e, const char *what)
{
    if (e != cudaSuccess) throw std::runtime_error(std::string(CIP_API ": ") + what + ": " + cudaGetErrorString(e));
}

class CudaBackend : public Backend {
public:
    explicit CudaBackend(int dev) : dev_(dev)
    {
        check(cudaSetDevice(dev_), "cudaSetDevice");
        cudaDeviceProp prop;
        check(cudaGetDeviceProperties(&prop, dev_), "cudaGetDeviceProperties");
        name_ = prop.name;
        sm_count_ = prop.multiProcessorCount;
        hitctr_ = alloc(4);
        hits_ = alloc((size_t)kHitCap * CIP_HIT_WORDS * 4);
        bitmap_ = alloc((size_t)CIP_BITMAP_WORDS * 4);
        check(cudaMemset(bitmap_, 0, (size_t)CIP_BITMAP_WORDS * 4), "cudaMemset");
    }

    ~CudaBackend() override
    {
        if (cudaSetDevice(dev_) != cudaSuccess) return;
        for (u32 *p : {state_, table_, scratch_, hitctr_, hits_, filt_, bitmap_})
            if (p) (void)cudaFree(p);
    }

    std::string describe() const override { return CIP_API " #" + std::to_string(dev_) + ": " + name_; }

    // Threads per multiprocessor. Measured on an RTX 4090 (128 SMs) in the lean scratch
    // mode: 512 per SM gives 14.1 G keys/s, 1024 gives 16.0, 2048 gives 16.6, 4096 gives 16.8.
    // The only AMD device tried, a two-unit iGPU, was fastest at 1024.
    uint32_t suggested_threads() const override { return (uint32_t)std::max(sm_count_, 1) * kThreadsPerUnit; }

    uint64_t scratch_budget() const override
    {
        size_t free_bytes = 0, total_bytes = 0;
        if (cudaSetDevice(dev_) != cudaSuccess || cudaMemGetInfo(&free_bytes, &total_bytes) != cudaSuccess) return 1u << 30;
        return std::min<uint64_t>(free_bytes / 2, (uint64_t)6 << 30);
    }

    // Batches past 512 pairs gain nothing measurable (the inversion is already under 3%).
    uint32_t suggested_pairs() const override { return 1024; }

    // Fast cards can outrun their memory bandwidth, slow ones cannot: measure.
    std::vector<uint32_t> scratch_candidates() const override
    {
        return {CIP_SCRATCH_LEAN, CIP_SCRATCH_HALF, CIP_SCRATCH_FULL};
    }

    void configure(uint32_t nthreads, uint32_t npairs, uint32_t smode, const std::vector<uint32_t> &table) override
    {
        use();
        if (smode > CIP_SCRATCH_FULL) throw std::runtime_error("bad scratch mode");
        for (u32 **p : {&state_, &table_, &scratch_}) {
            if (*p) check(cudaFree(*p), "cudaFree");
            *p = nullptr;
        }
        prm_ = cip_params();
        prm_.nthreads = nthreads;
        prm_.npairs = npairs;
        prm_.hitcap = kHitCap;
        smode_ = smode;
        state_ = alloc((size_t)nthreads * CIP_STATE_WORDS * 4);
        table_ = alloc(table.size() * 4);
        scratch_ = alloc(scratch_bytes(nthreads, npairs, smode));
        check(cudaMemcpy(table_, table.data(), table.size() * 4, cudaMemcpyHostToDevice), "upload table");
    }

    void set_filter(const Filter &f) override
    {
        use();
        prm_.mode = f.mode;
        prm_.nfilt = f.count();
        memcpy(prm_.dm, f.dm, sizeof prm_.dm);
        memcpy(prm_.dv, f.dv, sizeof prm_.dv);
        size_t bytes = f.records.size() * 4;
        if (bytes > filt_bytes_) {
            if (filt_) check(cudaFree(filt_), "cudaFree");
            filt_ = nullptr;
            filt_ = alloc(bytes);
            filt_bytes_ = bytes;
        }
        if (bytes) check(cudaMemcpy(filt_, f.records.data(), bytes, cudaMemcpyHostToDevice), "upload filter");
        if (!f.bitmap.empty())
            check(cudaMemcpy(bitmap_, f.bitmap.data(), f.bitmap.size() * 4, cudaMemcpyHostToDevice), "upload bitmap");
    }

    void write_state(uint32_t tid, uint32_t count, const uint32_t *words) override
    {
        use();
        check(cudaMemcpy(state_ + (size_t)tid * CIP_STATE_WORDS, words, (size_t)count * CIP_STATE_WORDS * 4,
                         cudaMemcpyHostToDevice),
              "upload state");
    }

    void read_state(uint32_t tid, uint32_t count, uint32_t *words) override
    {
        use();
        check(cudaMemcpy(words, state_ + (size_t)tid * CIP_STATE_WORDS, (size_t)count * CIP_STATE_WORDS * 4,
                         cudaMemcpyDeviceToHost),
              "download state");
    }

    uint32_t launch(uint32_t nbatches, std::vector<Hit> &hits) override
    {
        use();
        prm_.nbatches = nbatches;
        check(cudaMemset(hitctr_, 0, 4), "cudaMemset");
        unsigned grid = (prm_.nthreads + kBlock - 1) / kBlock;
        if (smode_ == CIP_SCRATCH_FULL)
            CIP_LAUNCH(CIP_KERNEL<CIP_SCRATCH_FULL>, grid, kBlock, state_, table_, scratch_, hitctr_, hits_, filt_, bitmap_, prm_);
        else if (smode_ == CIP_SCRATCH_LEAN)
            CIP_LAUNCH(CIP_KERNEL<CIP_SCRATCH_LEAN>, grid, kBlock, state_, table_, scratch_, hitctr_, hits_, filt_, bitmap_, prm_);
        else
            CIP_LAUNCH(CIP_KERNEL<CIP_SCRATCH_HALF>, grid, kBlock, state_, table_, scratch_, hitctr_, hits_, filt_, bitmap_, prm_);
        check(cudaGetLastError(), "kernel launch");
        check(cudaDeviceSynchronize(), "kernel execution");

        uint32_t n = 0;
        check(cudaMemcpy(&n, hitctr_, 4, cudaMemcpyDeviceToHost), "download hit count");
        uint32_t keep = std::min(n, kHitCap);
        std::vector<uint32_t> raw((size_t)keep * CIP_HIT_WORDS);
        if (keep) check(cudaMemcpy(raw.data(), hits_, raw.size() * 4, cudaMemcpyDeviceToHost), "download hits");
        hits.clear();
        for (uint32_t i = 0; i < keep; i++) hits.push_back(Hit{raw[3 * i], raw[3 * i + 1], raw[3 * i + 2]});
        return n;
    }

private:
    // The kernels need 160-200 registers per thread, so a multiprocessor only holds a few
    // hundred threads at once; small blocks let it fill up (128 vs 256: +2% on a 4090).
    static constexpr unsigned kBlock = 128;
    static constexpr unsigned kThreadsPerUnit = CIP_THREADS_PER_UNIT;

    // The CUDA runtime tracks the current device per host thread, and the engine calls in
    // from more than one.
    void use() { check(cudaSetDevice(dev_), "cudaSetDevice"); }

    u32 *alloc(size_t bytes)
    {
        void *p = nullptr;
        cudaError_t e = cudaMalloc(&p, std::max<size_t>(bytes, 4));
        if (e != cudaSuccess)
            throw std::runtime_error(CIP_API ": allocating " + std::to_string(bytes >> 20) + " MiB on device " +
                                     std::to_string(dev_) + " failed: " + cudaGetErrorString(e) +
                                     " (lower --threads or --pairs, or use --scratch half)");
        return (u32 *)p;
    }

    int dev_;
    int sm_count_ = 0;
    std::string name_;
    u32 *state_ = nullptr, *table_ = nullptr, *scratch_ = nullptr, *hitctr_ = nullptr, *hits_ = nullptr;
    u32 *filt_ = nullptr, *bitmap_ = nullptr;
    size_t filt_bytes_ = 0;
    uint32_t smode_ = CIP_SCRATCH_LEAN;
    cip_params prm_ = cip_params();
};

}  // namespace

std::vector<std::unique_ptr<Backend>> CIP_CREATE(const std::vector<int> &devices, std::string &err)
{
    std::vector<std::unique_ptr<Backend>> v;
    int count = 0;
    cudaError_t e = cudaGetDeviceCount(&count);
    if (e != cudaSuccess || count <= 0) {
        err = e != cudaSuccess ? std::string(CIP_API ": ") + cudaGetErrorString(e) : "no " CIP_API " device found";
        return v;
    }
    std::vector<int> use = devices;
    if (use.empty())
        for (int i = 0; i < count; i++) use.push_back(i);
    try {
        for (int d : use) {
            if (d < 0 || d >= count) throw std::runtime_error("no " CIP_API " device " + std::to_string(d));
            // Sleep instead of spinning a CPU core while a launch runs. Best effort: it
            // only takes effect if the device has not been initialised yet.
            if (cudaSetDevice(d) == cudaSuccess) (void)cudaSetDeviceFlags(cudaDeviceScheduleBlockingSync);
            v.emplace_back(new CudaBackend(d));
        }
    } catch (const std::exception &ex) {
        err = ex.what();
        v.clear();
    }
    return v;
}
