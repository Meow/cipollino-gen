# cipollino build.
#
#   make            every backend this machine can build: CUDA (nvcc on PATH), ROCm (hipcc),
#                   Metal (macOS), Vulkan (Vulkan headers + glslangValidator); CPU always
#   make test       unit tests + exhaustive self-test of every backend that can run here
#   make CUDA=0 ROCM=0 METAL=0 VULKAN=0     leave a backend out
#   make CUDA_ARCH=sm_86   target a specific GPU generation instead of the one in this
#                   machine (needed if your nvcc is too old to know -arch=native)
#   make CUDA_AOS=0 ROCM_AOS=0 VULKAN_AOS=0   scratch layout per backend (see kernel/search.h):
#                   1 = a thread's slot is contiguous (the default), 0 = one word per thread
#                   side by side. Compare with --bench.
#   make ROCM_ARCH="gfx1100 gfx1030"   AMD GPU architectures to generate code for

CXX      ?= c++
CXXFLAGS ?= -O2
NVCC     ?= nvcc
CUDA_AOS ?= 1
CUDA_ARCH ?= native
ROCM_PATH ?= /opt/rocm
HIPCC    ?= $(firstword $(shell command -v hipcc 2>/dev/null) $(wildcard $(ROCM_PATH)/bin/hipcc))
ROCM_ARCH ?=
ROCM_AOS ?= 1
VULKAN_AOS ?= 1
GLSLANG  ?= glslangValidator
BUILD    := build

UNAME := $(shell uname -s)

# Homebrew keeps each package in its own prefix; elsewhere pkg-config or the default paths do.
BREW := $(shell brew --prefix libsodium vulkan-headers vulkan-loader 2>/dev/null)
SODIUM_PREFIX := $(word 1,$(BREW))
SODIUM_CFLAGS := $(shell pkg-config --cflags libsodium 2>/dev/null || { [ -n "$(SODIUM_PREFIX)" ] && echo -I$(SODIUM_PREFIX)/include; })
SODIUM_LIBS   := $(shell pkg-config --libs libsodium 2>/dev/null || { [ -n "$(SODIUM_PREFIX)" ] && echo -L$(SODIUM_PREFIX)/lib -lsodium; } || echo -lsodium)
VULKAN_INC    := $(firstword $(wildcard $(word 2,$(BREW))/include /usr/include /usr/local/include))
VULKAN_CFLAGS := $(shell pkg-config --cflags vulkan 2>/dev/null || { [ -f "$(word 2,$(BREW))/include/vulkan/vulkan.h" ] && echo -I$(word 2,$(BREW))/include; })
VULKAN_LIBS   := $(shell pkg-config --libs vulkan 2>/dev/null || { [ -d "$(word 3,$(BREW))/lib" ] && echo -L$(word 3,$(BREW))/lib -lvulkan; } || echo -lvulkan)
VULKAN_HEADER := $(shell echo '\#include <vulkan/vulkan.h>' | $(CXX) $(VULKAN_CFLAGS) -E -x c++ - >/dev/null 2>&1 && echo yes)

CUDA   ?= $(if $(shell command -v $(NVCC) 2>/dev/null),1,0)
ROCM   ?= $(if $(HIPCC),1,0)
METAL  ?= $(if $(filter Darwin,$(UNAME)),1,0)
VULKAN ?= $(if $(and $(VULKAN_HEADER),$(shell command -v $(GLSLANG) 2>/dev/null)),1,0)

# The version lives in the VERSION file; main.cpp turns the macro into a string.
VERSION  := $(shell cat VERSION)
DEFS     := -DCIP_VERSION=$(VERSION)
ALLFLAGS = -std=c++17 -Wall -Wextra $(CXXFLAGS) -Isrc -I$(BUILD) $(SODIUM_CFLAGS) $(DEFS)
LDLIBS   := $(SODIUM_LIBS) -lpthread
LINK     := $(CXX)

KERNEL := src/kernel/defs.h src/kernel/field.h src/kernel/search.h
# field.h includes this one where the kernel is built with CIP_ARITH=1.
FIELD13 := src/kernel/field13.h
HDRS   := $(wildcard src/*.h) $(KERNEL) $(FIELD13) $(BUILD)/config
OBJS   := $(addprefix $(BUILD)/,main.o engine.o keys.o onion.o ed.o filter.o platform.o backend_cpu.o)

ifeq ($(METAL),1)
DEFS   += -DCIP_HAVE_METAL
OBJS   += $(BUILD)/backend_metal.o
LDLIBS += -framework Metal -framework Foundation
endif

ifeq ($(VULKAN),1)
DEFS   += -DCIP_HAVE_VULKAN
OBJS   += $(BUILD)/backend_vulkan.o
LDLIBS += $(VULKAN_LIBS)
endif

ifeq ($(ROCM),1)
DEFS     += -DCIP_HAVE_ROCM
OBJS     += $(BUILD)/backend_rocm.o
HIPFLAGS ?= -O3 $(addprefix --offload-arch=,$(ROCM_ARCH))
LDLIBS   += -L$(ROCM_PATH)/lib -lamdhip64 -Wl,-rpath,$(ROCM_PATH)/lib
endif

ifeq ($(CUDA),1)
DEFS      += -DCIP_HAVE_CUDA
OBJS      += $(BUILD)/backend_cuda.o
NVCCFLAGS ?= -O3 -arch=$(CUDA_ARCH)
LINK      := $(NVCC)
endif

all: cipollino

cipollino: $(OBJS)
	$(LINK) -o $@ $(OBJS) $(LDLIBS)

$(BUILD)/%.o: src/%.cpp $(HDRS) | $(BUILD)
	$(CXX) $(ALLFLAGS) -c -o $@ $<

# The Metal kernel is compiled by the OS at run time from source embedded in the binary.
$(BUILD)/kernel_metal.inc: $(KERNEL) src/kernel/entry_metal.h | $(BUILD)
	cat $(KERNEL) src/kernel/entry_metal.h > $(BUILD)/kernel_metal.metal
	cd $(BUILD) && xxd -i kernel_metal.metal > kernel_metal.inc

$(BUILD)/backend_metal.o: src/backend_metal.mm $(HDRS) $(BUILD)/kernel_metal.inc | $(BUILD)
	$(CXX) $(ALLFLAGS) -fobjc-arc -c -o $@ $<

# The Vulkan kernel is compiled to SPIR-V here and embedded: one module per scratch mode
# for each field arithmetic (w: wide, n: narrow).
GLSLFLAGS = -V --target-env vulkan1.1 -DCIP_SCRATCH_AOS=$(VULKAN_AOS) -Isrc/kernel
VK_MODULES := w0 w1 w2 n0 n1 n2

$(BUILD)/kernel_vk_w%.spv: src/kernel/entry_vulkan.comp $(KERNEL) $(BUILD)/config | $(BUILD)
	$(GLSLANG) $(GLSLFLAGS) -DCIP_ARITH=0 -DCIP_SMODE=$* -o $@ $<

$(BUILD)/kernel_vk_n%.spv: src/kernel/entry_vulkan.comp $(KERNEL) $(FIELD13) $(BUILD)/config | $(BUILD)
	$(GLSLANG) $(GLSLFLAGS) -DCIP_ARITH=1 -DCIP_SMODE=$* -o $@ $<

$(BUILD)/kernel_vk.inc: $(VK_MODULES:%=$(BUILD)/kernel_vk_%.spv)
	cd $(BUILD) && for m in $(VK_MODULES); do xxd -i kernel_vk_$$m.spv; done > kernel_vk.inc

$(BUILD)/backend_vulkan.o: src/backend_vulkan.cpp $(HDRS) $(BUILD)/kernel_vk.inc | $(BUILD)
	$(CXX) $(ALLFLAGS) $(VULKAN_CFLAGS) -c -o $@ $<

# CUDA and ROCm share one source file: HIP is CUDA's API under other names.
$(BUILD)/backend_cuda.o: src/backend_cuda.cu $(HDRS) | $(BUILD)
	$(NVCC) $(NVCCFLAGS) -std=c++17 -Isrc $(DEFS) -DCIP_SCRATCH_AOS=$(CUDA_AOS) -c -o $@ $<

$(BUILD)/backend_rocm.o: src/backend_cuda.cu src/hip_as_cuda.h $(HDRS) | $(BUILD)
	$(HIPCC) $(HIPFLAGS) -std=c++17 -fPIE -Wno-pass-failed -Isrc $(DEFS) -DCIP_ROCM -DCIP_SCRATCH_AOS=$(ROCM_AOS) -x hip -c -o $@ $<

$(BUILD):
	mkdir -p $(BUILD)

# Rebuild everything when the configuration (flags, backends) changes.
CONFIG = $(CXX) $(ALLFLAGS) cuda=$(CUDA)/$(CUDA_AOS)/$(NVCCFLAGS) rocm=$(ROCM)/$(ROCM_AOS)/$(HIPFLAGS) metal=$(METAL) vulkan=$(VULKAN)/$(VULKAN_AOS)
$(BUILD)/config: FORCE | $(BUILD)
	@echo '$(CONFIG)' | cmp -s - $@ || echo '$(CONFIG)' > $@

FORCE:

# ---- tests ----------------------------------------------------------------------------

FIELD_SHIM := $(BUILD)/libfieldshim.so $(BUILD)/libfieldshim13.so

$(BUILD)/libfieldshim.so: tests/field_shim.cpp $(KERNEL) | $(BUILD)
	$(CXX) -std=c++17 -O2 -Wall -Wextra -fPIC -shared -Isrc -o $@ $<

$(BUILD)/libfieldshim13.so: tests/field_shim.cpp $(KERNEL) $(FIELD13) | $(BUILD)
	$(CXX) -std=c++17 -O2 -Wall -Wextra -fPIC -shared -Isrc -DCIP_ARITH=1 -o $@ $<

# The CUDA backend compiled as ordinary C++ against a small stand-in for the CUDA runtime,
# so its host logic and the kernel as CUDA sees it are exercised without a GPU or nvcc.
SIM_OBJS := $(addprefix $(BUILD)/,main.o engine.o keys.o onion.o ed.o filter.o platform.o) $(BUILD)/sim_backend_cpu.o $(BUILD)/sim_backend_cuda.o

$(BUILD)/sim_backend_cpu.o: src/backend_cpu.cpp $(HDRS) | $(BUILD)
	$(CXX) $(ALLFLAGS) -UCIP_HAVE_METAL -UCIP_HAVE_VULKAN -UCIP_HAVE_ROCM -DCIP_HAVE_CUDA -c -o $@ $<

$(BUILD)/sim_backend_cuda.o: tests/sim_backend_cuda.cpp src/backend_cuda.cu tests/cuda_sim.h $(HDRS) | $(BUILD)
	$(CXX) $(ALLFLAGS) -DCIP_SCRATCH_AOS=$(CUDA_AOS) -c -o $@ $<

$(BUILD)/cipollino-cudasim: $(SIM_OBJS)
	$(CXX) -o $@ $(SIM_OBJS) $(SODIUM_LIBS) -lpthread

# The CPU backend running the kernel with the narrow field arithmetic, which otherwise only
# Vulkan devices see.
NARROW_OBJS := $(addprefix $(BUILD)/,main.o engine.o keys.o onion.o ed.o filter.o platform.o) $(BUILD)/narrow_backend_cpu.o

$(BUILD)/narrow_backend_cpu.o: src/backend_cpu.cpp $(HDRS) | $(BUILD)
	$(CXX) $(ALLFLAGS) -UCIP_HAVE_METAL -UCIP_HAVE_VULKAN -UCIP_HAVE_ROCM -UCIP_HAVE_CUDA -DCIP_ARITH=1 -c -o $@ $<

$(BUILD)/cipollino-narrow: $(NARROW_OBJS)
	$(CXX) -o $@ $(NARROW_OBJS) $(SODIUM_LIBS) -lpthread

test: test-host test-gpu

# Everything that needs no GPU (so it can run inside `docker build`).
test-host: cipollino $(FIELD_SHIM) $(BUILD)/cipollino-cudasim $(BUILD)/cipollino-narrow
	python3 tools/gen_field13.py --check $(FIELD13)
	python3 tests/test_field.py $(BUILD)/libfieldshim.so
	python3 tests/test_field.py $(BUILD)/libfieldshim13.so
	./cipollino --backend cpu --selftest
	$(BUILD)/cipollino-cudasim --backend cuda --selftest
	$(BUILD)/cipollino-narrow --backend cpu --selftest
	python3 tests/test_cli.py ./cipollino cpu
	python3 tests/test_cli.py $(BUILD)/cipollino-cudasim cuda

# The GPU backends this build has, on the real device.
test-gpu: cipollino
ifeq ($(METAL),1)
	./cipollino --backend metal --selftest
	python3 tests/test_cli.py ./cipollino metal
endif
ifeq ($(CUDA),1)
	./cipollino --backend cuda --selftest
	python3 tests/test_cli.py ./cipollino cuda
endif
ifeq ($(ROCM),1)
	./cipollino --backend rocm --selftest
	python3 tests/test_cli.py ./cipollino rocm
endif
ifeq ($(VULKAN),1)
	./cipollino --backend vulkan --selftest
	python3 tests/test_cli.py ./cipollino vulkan
endif

# ---- docker (NVIDIA) --------------------------------------------------------------------
# `make docker` builds the runtime image (running the host tests on the way) and the :dev
# image that keeps the toolchain; `make docker-test` runs the GPU tests in the latter.

DOCKER_IMAGE ?= cipollino

docker:
	docker build --target runtime -t $(DOCKER_IMAGE) .
	docker build --target build -t $(DOCKER_IMAGE):dev .

docker-test: docker
	docker run --rm --gpus all --device /dev/dri $(DOCKER_IMAGE):dev make test-gpu

# The same for AMD GPUs through ROCm (Dockerfile.rocm).
docker-rocm:
	docker build -f Dockerfile.rocm --target runtime -t $(DOCKER_IMAGE):rocm .
	docker build -f Dockerfile.rocm --target build -t $(DOCKER_IMAGE):rocm-dev .

docker-rocm-test: docker-rocm
	docker run --rm --device /dev/kfd --device /dev/dri $(DOCKER_IMAGE):rocm-dev make test-gpu

clean:
	rm -rf $(BUILD) cipollino

.PHONY: all test test-host test-gpu docker docker-test docker-rocm docker-rocm-test clean FORCE
