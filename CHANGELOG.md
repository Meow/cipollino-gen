# Changelog

Versions follow [semantic versioning](https://semver.org). Before 1.0, options and output
may change between minor versions.

## 0.1.0

First versioned release.

- Backends: CUDA, ROCm, Metal, Vulkan and a CPU fallback, all running one kernel source.
- Any number of prefixes per search, with the wildcards `?d`, `?l` and `?a`; `-n` keys
  per run.
- One key per seed; every key re-derived and validated with libsodium before it is written.
- `--bench`, `--tune`, `--selftest` and `--verify`.
- Linux and macOS builds with make, Windows build with CMake, container images for
  NVIDIA and AMD.
