# Changelog

Versions follow [semantic versioning](https://semver.org). Before 1.0, options and output
may change between minor versions.

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
