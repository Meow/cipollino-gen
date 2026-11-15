# cipollino for NVIDIA GPUs.
#
#   docker build -t cipollino .                      (or: make docker)
#   ./cipollino-docker -n 2 someprefix               keys land in ./keys
#
# The host needs the NVIDIA driver and nvidia-container-toolkit, nothing else.
# Targets: `runtime` (default; the binary and its two shared libraries) and `build`
# (toolchain and sources, for `make test-gpu` and for poking at the kernel).

ARG CUDA_VERSION=12.6.3
ARG UBUNTU_VERSION=24.04

FROM nvidia/cuda:${CUDA_VERSION}-base-ubuntu${UBUNTU_VERSION} AS build
# Only nvcc and the CUDA runtime headers are needed, not the multi-gigabyte devel image.
RUN pkg="$(echo "$CUDA_VERSION" | cut -d. -f1,2 | tr . -)" \
 && apt-get update \
 && apt-get install -y --no-install-recommends \
      "cuda-nvcc-$pkg" "cuda-cudart-dev-$pkg" g++ make pkg-config libsodium-dev python3 \
 && rm -rf /var/lib/apt/lists/*
ENV PATH=/usr/local/cuda/bin:$PATH

# No GPU is visible during `docker build`, so -arch=native cannot work here. all-major
# embeds machine code for every GPU generation this nvcc knows, plus PTX for newer ones.
ARG CUDA_ARCH=all-major
WORKDIR /src
COPY Makefile ./
COPY src src
COPY tests tests
RUN make -j"$(nproc)" CUDA_ARCH="$CUDA_ARCH" \
 && make test-host CUDA_ARCH="$CUDA_ARCH"

FROM nvidia/cuda:${CUDA_VERSION}-base-ubuntu${UBUNTU_VERSION} AS runtime
RUN apt-get update \
 && apt-get install -y --no-install-recommends libsodium23 \
 && rm -rf /var/lib/apt/lists/*
COPY --from=build /src/cipollino /usr/local/bin/cipollino
COPY docker/entrypoint.sh /usr/local/bin/cipollino-entrypoint
WORKDIR /keys
ENTRYPOINT ["cipollino-entrypoint"]
CMD ["--help"]
