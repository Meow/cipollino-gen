# cipollino build.
#
#   make            CUDA backend if nvcc is on PATH, Metal backend on macOS, CPU fallback always
#   make test       unit tests + exhaustive self-test of every backend that can run here
#   make CUDA=0     force a build without CUDA        make METAL=0   ... without Metal
#   make CUDA_ARCH=sm_86   target a specific GPU generation instead of the one in this
#                   machine (needed if your nvcc is too old to know -arch=native)
#   make CUDA_AOS=1 CUDA build with the array-of-structs scratch layout (see kernel/search.h);
#                   compare both with `cipollino --bench` if you want the last few percent

CXX      ?= c++
CXXFLAGS ?= -O2
NVCC     ?= nvcc
CUDA_AOS ?= 0
CUDA_ARCH ?= native
BUILD    := build

UNAME := $(shell uname -s)
CUDA  ?= $(if $(shell command -v $(NVCC) 2>/dev/null),1,0)
METAL ?= $(if $(filter Darwin,$(UNAME)),1,0)

# libsodium: pkg-config if present, otherwise Homebrew's prefix, otherwise the default paths.
SODIUM_PREFIX := $(shell brew --prefix libsodium 2>/dev/null)
SODIUM_CFLAGS := $(shell pkg-config --cflags libsodium 2>/dev/null || { [ -n "$(SODIUM_PREFIX)" ] && echo -I$(SODIUM_PREFIX)/include; })
SODIUM_LIBS   := $(shell pkg-config --libs libsodium 2>/dev/null || { [ -n "$(SODIUM_PREFIX)" ] && echo -L$(SODIUM_PREFIX)/lib -lsodium; } || echo -lsodium)

DEFS     :=
ALLFLAGS = -std=c++17 -Wall -Wextra $(CXXFLAGS) -Isrc -I$(BUILD) $(SODIUM_CFLAGS) $(DEFS)
LDLIBS   := $(SODIUM_LIBS) -lpthread
LINK     := $(CXX)

KERNEL := src/kernel/defs.h src/kernel/field.h src/kernel/search.h
HDRS   := $(wildcard src/*.h) $(KERNEL) $(BUILD)/config
OBJS   := $(addprefix $(BUILD)/,main.o engine.o keys.o onion.o ed.o filter.o backend_cpu.o)

ifeq ($(METAL),1)
DEFS   += -DCIP_HAVE_METAL
OBJS   += $(BUILD)/backend_metal.o
LDLIBS += -framework Metal -framework Foundation
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

$(BUILD)/backend_cuda.o: src/backend_cuda.cu $(HDRS) | $(BUILD)
	$(NVCC) $(NVCCFLAGS) -std=c++17 -Isrc $(DEFS) -DCIP_SCRATCH_AOS=$(CUDA_AOS) -c -o $@ $<

$(BUILD):
	mkdir -p $(BUILD)

# Rebuild everything when the configuration (flags, backends) changes.
$(BUILD)/config: FORCE | $(BUILD)
	@echo '$(CXX) $(ALLFLAGS) cuda=$(CUDA)/$(CUDA_AOS) metal=$(METAL)' | cmp -s - $@ || echo '$(CXX) $(ALLFLAGS) cuda=$(CUDA)/$(CUDA_AOS) metal=$(METAL)' > $@

FORCE:

# ---- tests ----------------------------------------------------------------------------

FIELD_SHIM := $(BUILD)/libfieldshim.so

$(FIELD_SHIM): tests/field_shim.cpp $(KERNEL) | $(BUILD)
	$(CXX) -std=c++17 -O2 -Wall -Wextra -fPIC -shared -Isrc -o $@ $<

# The CUDA backend compiled as ordinary C++ against a small stand-in for the CUDA runtime,
# so its host logic and the kernel as CUDA sees it are exercised without a GPU or nvcc.
SIM_OBJS := $(addprefix $(BUILD)/,main.o engine.o keys.o onion.o ed.o filter.o) $(BUILD)/sim_backend_cpu.o $(BUILD)/sim_backend_cuda.o

$(BUILD)/sim_backend_cpu.o: src/backend_cpu.cpp $(HDRS) | $(BUILD)
	$(CXX) $(ALLFLAGS) -DCIP_HAVE_CUDA -UCIP_HAVE_METAL -c -o $@ $<

$(BUILD)/sim_backend_cuda.o: src/backend_cuda.cu tests/cuda_sim.h $(HDRS) | $(BUILD)
	$(CXX) $(ALLFLAGS) -DCIP_CUDA_SIM -include tests/cuda_sim.h -x c++ -c -o $@ $<

$(BUILD)/cipollino-cudasim: $(SIM_OBJS)
	$(CXX) -o $@ $(SIM_OBJS) $(SODIUM_LIBS) -lpthread

test: cipollino $(FIELD_SHIM) $(BUILD)/cipollino-cudasim
	python3 tests/test_field.py $(FIELD_SHIM)
	./cipollino --backend cpu --selftest
	$(BUILD)/cipollino-cudasim --backend cuda --selftest
	python3 tests/test_cli.py ./cipollino cpu
	python3 tests/test_cli.py $(BUILD)/cipollino-cudasim cuda
ifeq ($(METAL),1)
	./cipollino --backend metal --selftest
	python3 tests/test_cli.py ./cipollino metal
endif
ifeq ($(CUDA),1)
	./cipollino --backend cuda --selftest
	python3 tests/test_cli.py ./cipollino cuda
endif

clean:
	rm -rf $(BUILD) cipollino

.PHONY: all test clean FORCE
