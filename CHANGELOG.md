# Changelog

Versions follow [semantic versioning](https://semver.org). Before 1.0, options and output
may change between minor versions.

## Unreleased

- Vulkan tries the narrow arithmetic on AMD GPUs only: on an Intel UHD 730 under Windows
  it ended the program without a message.
- CUDA builds limit the kernels to 168 registers per thread. On an RTX 4090 the `half`
  scratch mode goes from 13.8 to 16.4 G keys/s, level with `lean` in half the memory.

## 0.3.0

- Client authorization keys: `--client` searches for x25519 key pairs with a prefix on
  the public key, `--service` writes them ready for `ClientOnionAuthDir`.
- Vulkan measures its thread count on discrete AMD GPUs: 5% on an RX 6700 XT.
- The inversion of a batch is a GCD (division steps), a third of the cost: 1 to 3% faster.

## 0.2.0

- Wildcards in prefixes: `?d` for a digit, `?l` for a letter, `?a` for either.
- Programs for five platforms attached to every release: Windows and Linux with CUDA or
  ROCm (each with Vulkan), and macOS with Metal.
- A ROCm program for Windows, built with `-DCIP_ROCM=ON`.
- `make PORTABLE=1` builds a program that carries its libraries.
- ROCm: 2% faster on a Radeon 610M.

## 0.1.0

First versioned release.

- Backends: CUDA, ROCm, Metal, Vulkan and a CPU fallback, all running one kernel source.
- Any number of prefixes per search; `-n` keys per run.
- One key per seed; every key re-derived and validated with libsodium before it is written.
- `--bench`, `--tune`, `--selftest` and `--verify`.
- Linux and macOS builds with make, Windows build with CMake, container images for
  NVIDIA and AMD.
