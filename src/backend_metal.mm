// Metal backend (Apple GPUs). The kernel source is embedded in the binary and compiled by
// the OS for the device at start-up.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <algorithm>
#include <cstring>
#include <stdexcept>

#include "backend.h"

#define CIP_TARGET_CPU
#include "kernel/defs.h"

#include "kernel_metal.inc"  // kernel_metal_metal[], kernel_metal_metal_len

namespace {

class MetalBackend : public Backend {
public:
    explicit MetalBackend(id<MTLDevice> dev) : dev_(dev)
    {
        @autoreleasepool {
            queue_ = [dev_ newCommandQueue];
            pipeline(CIP_SCRATCH_LEAN);  // fail early if the kernel does not compile
            hitctr_ = buffer(4, true);
            hits_ = buffer((size_t)kHitCap * CIP_HIT_WORDS * 4, true);
            filt_ = buffer(16, true);
            bitmap_ = buffer((size_t)CIP_BITMAP_WORDS * 4, true);
        }
    }

    std::string describe() const override { return std::string("Metal: ") + [[dev_ name] UTF8String]; }

    uint32_t suggested_threads() const override { return 1u << 16; }

    uint64_t scratch_budget() const override
    {
        // Unified memory: stay well inside what the OS recommends for this GPU.
        uint64_t rec = [dev_ recommendedMaxWorkingSetSize];
        return std::min<uint64_t>({rec / 4, (uint64_t)[dev_ maxBufferLength], (uint64_t)4 << 30});
    }

    // One batch of 65536 x 2049 keys is about 80 ms on an M5 Pro.
    uint32_t suggested_pairs() const override { return 1024; }

    // Apple GPUs are arithmetic-bound here: full and lean measure the same, so take the
    // one that needs half the memory and skip the calibration.
    std::vector<uint32_t> scratch_candidates() const override { return {CIP_SCRATCH_LEAN}; }

    // Short launches keep the desktop responsive: the window server shares this GPU.
    double suggested_launch_seconds() const override { return 0.1; }

    void configure(uint32_t nthreads, uint32_t npairs, uint32_t smode, const std::vector<uint32_t> &table) override
    {
        @autoreleasepool {
            prm_ = cip_params();
            prm_.nthreads = nthreads;
            prm_.npairs = npairs;
            prm_.hitcap = kHitCap;
            pso_ = pipeline(smode);
            scratch_ = nil;  // release the old one before allocating its replacement
            if (scratch_bytes(nthreads, npairs, smode) > [dev_ maxBufferLength])
                throw std::runtime_error("scratch buffer exceeds this device's maximum buffer size; lower --threads or --pairs");
            state_ = buffer((size_t)nthreads * CIP_STATE_WORDS * 4, true);
            table_ = buffer(table.size() * 4, true);
            memcpy([table_ contents], table.data(), table.size() * 4);
            scratch_ = buffer(scratch_bytes(nthreads, npairs, smode), false);
        }
    }

    void set_filter(const Filter &f) override
    {
        @autoreleasepool {
            prm_.mode = f.mode;
            prm_.nfilt = f.count();
            memcpy(prm_.dm, f.dm, sizeof prm_.dm);
            memcpy(prm_.dv, f.dv, sizeof prm_.dv);
            if (f.records.size() * 4 > [filt_ length]) filt_ = buffer(f.records.size() * 4, true);
            memcpy([filt_ contents], f.records.data(), f.records.size() * 4);
            if (!f.bitmap.empty()) memcpy([bitmap_ contents], f.bitmap.data(), f.bitmap.size() * 4);
        }
    }

    void write_state(uint32_t tid, uint32_t count, const uint32_t *words) override
    {
        memcpy((uint32_t *)[state_ contents] + (size_t)tid * CIP_STATE_WORDS, words, (size_t)count * CIP_STATE_WORDS * 4);
    }

    void read_state(uint32_t tid, uint32_t count, uint32_t *words) override
    {
        memcpy(words, (uint32_t *)[state_ contents] + (size_t)tid * CIP_STATE_WORDS, (size_t)count * CIP_STATE_WORDS * 4);
    }

    uint32_t launch(uint32_t nbatches, std::vector<Hit> &hits, uint32_t first, uint32_t count) override
    {
        @autoreleasepool {
            prm_.nbatches = nbatches;
            prm_.first = first;
            prm_.count = count;
            *(uint32_t *)[hitctr_ contents] = 0;
            id<MTLCommandBuffer> cb = [queue_ commandBuffer];
            id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
            [enc setComputePipelineState:pso_];
            [enc setBuffer:state_ offset:0 atIndex:0];
            [enc setBuffer:table_ offset:0 atIndex:1];
            [enc setBuffer:scratch_ offset:0 atIndex:2];
            [enc setBuffer:hitctr_ offset:0 atIndex:3];
            [enc setBuffer:hits_ offset:0 atIndex:4];
            [enc setBuffer:filt_ offset:0 atIndex:5];
            [enc setBuffer:bitmap_ offset:0 atIndex:6];
            [enc setBytes:&prm_ length:sizeof prm_ atIndex:7];
            NSUInteger group = std::min<NSUInteger>([pso_ maxTotalThreadsPerThreadgroup], [pso_ threadExecutionWidth]);
            [enc dispatchThreads:MTLSizeMake(prm_.nthreads, 1, 1) threadsPerThreadgroup:MTLSizeMake(group, 1, 1)];
            [enc endEncoding];
            [cb commit];
            [cb waitUntilCompleted];
            if ([cb status] != MTLCommandBufferStatusCompleted)
                throw std::runtime_error(std::string("Metal launch failed: ") + text([cb error]));

            uint32_t n = *(uint32_t *)[hitctr_ contents];
            const uint32_t *h = (const uint32_t *)[hits_ contents];
            hits.clear();
            for (uint32_t i = 0; i < std::min(n, kHitCap); i++) hits.push_back(Hit{h[3 * i], h[3 * i + 1], h[3 * i + 2]});
            return n;
        }
    }

private:
    // The compiled kernel specialised for a scratch mode (compiled on first use).
    id<MTLComputePipelineState> pipeline(uint32_t smode)
    {
        if (smode > 2) throw std::runtime_error("bad scratch mode");
        if (pipelines_[smode]) return pipelines_[smode];
        NSString *src = [[NSString alloc] initWithBytes:kernel_metal_metal
                                                 length:kernel_metal_metal_len
                                               encoding:NSUTF8StringEncoding];
        MTLCompileOptions *opts = [MTLCompileOptions new];
        opts.preprocessorMacros = @{@"CIP_TARGET_METAL" : @1, @"CIP_SMODE" : @(smode), @"CIP_SCRATCH_AOS" : @1};
        NSError *error = nil;
        id<MTLLibrary> lib = [dev_ newLibraryWithSource:src options:opts error:&error];
        if (!lib) throw std::runtime_error(std::string("Metal kernel failed to compile: ") + text(error));
        id<MTLFunction> fn = [lib newFunctionWithName:@"cipollino_search"];
        if (!fn) throw std::runtime_error("Metal kernel entry point not found");
        id<MTLComputePipelineState> pso = [dev_ newComputePipelineStateWithFunction:fn error:&error];
        if (!pso) throw std::runtime_error(std::string("Metal pipeline creation failed: ") + text(error));
        pipelines_[smode] = pso;
        return pso;
    }

    static std::string text(NSError *e) { return e ? [[e localizedDescription] UTF8String] : "unknown error"; }

    id<MTLBuffer> buffer(size_t bytes, bool shared)
    {
        id<MTLBuffer> b = [dev_ newBufferWithLength:std::max<size_t>(bytes, 4)
                                            options:shared ? MTLResourceStorageModeShared : MTLResourceStorageModePrivate];
        if (!b) throw std::runtime_error("Metal buffer allocation of " + std::to_string(bytes >> 20) + " MiB failed");
        if (shared) memset([b contents], 0, [b length]);
        return b;
    }

    id<MTLDevice> dev_;
    id<MTLCommandQueue> queue_;
    id<MTLComputePipelineState> pso_;
    id<MTLComputePipelineState> pipelines_[3];
    id<MTLBuffer> state_, table_, scratch_, hitctr_, hits_, filt_, bitmap_;
    cip_params prm_ = cip_params();
};

}  // namespace

std::vector<std::unique_ptr<Backend>> create_metal_backends(std::string &err)
{
    std::vector<std::unique_ptr<Backend>> v;
    @autoreleasepool {
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        if (!dev) {
            err = "no Metal device";
            return v;
        }
        try {
            v.emplace_back(new MetalBackend(dev));
        } catch (const std::exception &e) {
            err = e.what();
        }
    }
    return v;
}
