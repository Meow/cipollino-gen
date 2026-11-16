# cipollino with the CUDA and Vulkan backends: NVIDIA GPUs through either, AMD and Intel
# GPUs through Vulkan (Mesa). For AMD GPUs through ROCm see Dockerfile.rocm.
#
#   docker build -t cipollino .                      (or: make docker)
#   ./cipollino-docker -n 2 someprefix               keys land in ./keys
#
# The host needs its GPU driver (and nvidia-container-toolkit for NVIDIA), nothing else.
# Targets: `runtime` (default; the binary and the libraries it loads) and `build`
# (toolchain and sources, for `make test-gpu` and for poking at the kernel).

ARG CUDA_VERSION=12.6.3
ARG UBUNTU_VERSION=24.04

FROM nvidia/cuda:${CUDA_VERSION}-base-ubuntu${UBUNTU_VERSION} AS build
# Only nvcc and the CUDA runtime headers are needed, not the multi-gigabyte devel image.
RUN pkg="$(echo "$CUDA_VERSION" | cut -d. -f1,2 | tr . -)" \
 && apt-get update \
 && apt-get install -y --no-install-recommends \
      "cuda-nvcc-$pkg" "cuda-cudart-dev-$pkg" g++ make pkg-config libsodium-dev python3 \
      libvulkan-dev glslang-tools xxd libvulkan1 mesa-vulkan-drivers libxext6 libegl1 \
 && rm -rf /var/lib/apt/lists/*
ENV PATH=/usr/local/cuda/bin:$PATH
# Have nvidia-container-toolkit mount the driver's Vulkan parts too, not only CUDA's.
ENV NVIDIA_DRIVER_CAPABILITIES=all

# No GPU is visible during `docker build`, so -arch=native cannot work here. `all` embeds
# machine code for every GPU architecture this nvcc knows. (`all-major` would be a
# quarter of the size, but an RTX 4090 then runs code generated for the 30 series and is
# 6% slower.)
ARG CUDA_ARCH=all
WORKDIR /src
COPY Makefile VERSION ./
COPY src src
COPY tests tests
COPY tools tools
RUN make -j"$(nproc)" CUDA_ARCH="$CUDA_ARCH" \
 && make test-host CUDA_ARCH="$CUDA_ARCH"

FROM nvidia/cuda:${CUDA_VERSION}-base-ubuntu${UBUNTU_VERSION} AS runtime
# Vulkan: the loader, Mesa's drivers for AMD and Intel, and libXext and libEGL, which
# NVIDIA's Vulkan driver (mounted in by nvidia-container-toolkit) needs even headless.
RUN apt-get update \
 && apt-get install -y --no-install-recommends libsodium23 libvulkan1 mesa-vulkan-drivers libxext6 libegl1 \
 && rm -rf /var/lib/apt/lists/*
ENV NVIDIA_DRIVER_CAPABILITIES=all
COPY --from=build /src/cipollino /usr/local/bin/cipollino
COPY docker/entrypoint.sh /usr/local/bin/cipollino-entrypoint
WORKDIR /keys
ENTRYPOINT ["cipollino-entrypoint"]
CMD ["--help"]
