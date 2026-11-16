// A stand-in for the few pieces of the CUDA runtime that src/backend_cuda.cu uses, so
// that file can be compiled as ordinary C++ and its host logic (buffer management, kernel
// dispatch, hit read-back, multi-device handling) tested on a machine without CUDA.
// "Device memory" is host memory and a kernel launch runs the blocks on host threads.
//
// This checks our code, not nvcc: the real build is still the only proof that the kernel
// compiles for a GPU.
#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

#define __device__
#define __global__      static
#define __forceinline__ inline

struct cuda_sim_dim {
  unsigned x, y, z;
};

inline thread_local cuda_sim_dim blockIdx, blockDim, threadIdx;

#if defined(_MSC_VER)
#include <intrin.h>

inline uint32_t atomicAdd(uint32_t *p, uint32_t v) {
  return (uint32_t)_InterlockedExchangeAdd((volatile long *)p, (long)v);
}
#else
inline uint32_t atomicAdd(uint32_t *p, uint32_t v) { return __atomic_fetch_add(p, v, __ATOMIC_RELAXED); }
#endif

typedef int cudaError_t;
const cudaError_t cudaSuccess            = 0;
const cudaError_t cudaErrorInvalidDevice = 101;

enum cudaMemcpyKind { cudaMemcpyHostToDevice = 1, cudaMemcpyDeviceToHost = 2 };

const unsigned cudaDeviceScheduleBlockingSync = 4;

struct cudaDeviceProp {
  char name[256];
  int multiProcessorCount;
};

// Two pretend devices, so the multi-device paths of the engine run too.
const int cuda_sim_devices               = 2;
inline thread_local int cuda_sim_current = -1;

inline const char *cudaGetErrorString(cudaError_t e) { return e == cudaSuccess ? "no error" : "simulated error"; }

inline cudaError_t cudaGetDeviceCount(int *n) {
  *n = cuda_sim_devices;
  return cudaSuccess;
}

inline cudaError_t cudaSetDevice(int d) {
  if (d < 0 || d >= cuda_sim_devices)
    return cudaErrorInvalidDevice;
  cuda_sim_current = d;
  return cudaSuccess;
}

inline cudaError_t cudaSetDeviceFlags(unsigned) { return cudaSuccess; }

inline cudaError_t cudaGetDeviceProperties(cudaDeviceProp *p, int d) {
  snprintf(p->name, sizeof p->name, "simulated device %d", d);
  p->multiProcessorCount = 1;
  return cudaSuccess;
}

inline cudaError_t cudaMemGetInfo(size_t *free_bytes, size_t *total_bytes) {
  *free_bytes = *total_bytes = (size_t)64 << 20;
  return cudaSuccess;
}

// Every call must come from a thread that selected a device first, as with the real runtime
// a forgotten cudaSetDevice() would silently talk to device 0.
inline cudaError_t cuda_sim_need_device() { return cuda_sim_current < 0 ? cudaErrorInvalidDevice : cudaSuccess; }

inline cudaError_t cudaMalloc(void **p, size_t n) {
  if (cuda_sim_need_device())
    return cudaErrorInvalidDevice;
  *p = malloc(n);
  memset(*p, 0xa5, n); // device memory is not zeroed
  return cudaSuccess;
}

inline cudaError_t cudaFree(void *p) {
  free(p);
  return cudaSuccess;
}

inline cudaError_t cudaMemset(void *p, int v, size_t n) {
  memset(p, v, n);
  return cuda_sim_need_device();
}

inline cudaError_t cudaMemcpy(void *dst, const void *src, size_t n, cudaMemcpyKind) {
  memcpy(dst, src, n);
  return cuda_sim_need_device();
}

inline cudaError_t cudaGetLastError() { return cudaSuccess; }

inline cudaError_t cudaDeviceSynchronize() { return cuda_sim_need_device(); }

template<class F>
void cuda_sim_launch(unsigned grid, unsigned block, F body) {
  std::atomic<unsigned> next(0);
  auto run = [&]() {
    for (;;) {
      unsigned b = next.fetch_add(1);
      if (b >= grid)
        break;
      blockIdx = cuda_sim_dim { b, 0, 0 };
      blockDim = cuda_sim_dim { block, 1, 1 };
      for (unsigned t = 0; t < block; t++) {
        threadIdx = cuda_sim_dim { t, 0, 0 };
        body();
      }
    }
  };
  std::vector<std::thread> pool;
  for (unsigned i = 1; i < std::max(1u, std::thread::hardware_concurrency()); i++)
    pool.emplace_back(run);
  run();
  for (std::thread &t : pool)
    t.join();
}

#define CIP_LAUNCH(kernel, grid, block, ...) \
  cuda_sim_launch(grid, block, [&]() {       \
    kernel(__VA_ARGS__);                     \
  })
