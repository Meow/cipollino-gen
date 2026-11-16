// src/backend_cuda.cu as ordinary C++ on top of the simulated CUDA runtime (cuda_sim.h).
#define CIP_CUDA_SIM
// clang-format off: the runtime comes first
#include "cuda_sim.h"
#include "../src/backend_cuda.cu"
// clang-format on
