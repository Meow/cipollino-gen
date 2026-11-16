# cipollino, built with CMake. This is how the Windows versions are built (with vcpkg.json
# supplying libsodium, the Vulkan headers and glslang). It works on Linux and macOS too,
# but there the Makefile is the reference: only it builds the Metal backend and the
# container images.
#
#   cmake -B build [-DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake]
#   cmake --build build
#   ctest --test-dir build -LE gpu      the tests that need no GPU
#   ctest --test-dir build -L gpu       every GPU backend in this build, on the real device
#
#   -DCIP_CUDA=ON|OFF|AUTO     CUDA backend (AUTO: if nvcc is found)
#   -DCIP_ROCM=ON              ROCm backend instead of CUDA. The C++ compiler has to be
#                              ROCm's clang++ (-DCMAKE_CXX_COMPILER=<rocm>/lib/llvm/bin/clang++)
#                              and -DCIP_ROCM_PATH or HIP_PATH name the ROCm installation
#   -DCIP_ROCM_ARCH="gfx11-generic;gfx1036"   AMD GPU architectures to generate code for
#   -DCIP_VULKAN=ON|OFF|AUTO   Vulkan backend (AUTO: if the headers and glslang are found)
#   -DCIP_CUDA_ARCH=sm_86      GPU generation to compile for instead of the one in this
#                              machine; `all` for a binary that runs on every NVIDIA GPU
#   -DCIP_CUDA_AOS=0 -DCIP_VULKAN_AOS=0   scratch layout per backend (see kernel/search.h)

cmake_minimum_required(VERSION 3.21)

set(CIP_CUDA AUTO CACHE STRING "CUDA backend: ON, OFF or AUTO")
set(CIP_VULKAN AUTO CACHE STRING "Vulkan backend: ON, OFF or AUTO")
set(CIP_CUDA_ARCH native CACHE STRING "nvcc -arch: native, all, all-major or sm_NN")
set(CIP_CUDA_AOS 1 CACHE STRING "CUDA scratch layout")
set(CIP_ROCM OFF CACHE BOOL "ROCm backend (instead of CUDA)")
set(CIP_ROCM_PATH "$ENV{HIP_PATH}" CACHE PATH "ROCm installation")
set(CIP_ROCM_ARCH "gfx9-generic;gfx9-4-generic;gfx10-1-generic;gfx10-3-generic;gfx11-generic;gfx12-generic"
    CACHE STRING "AMD GPU architectures to generate code for")
set(CIP_ROCM_AOS 1 CACHE STRING "ROCm scratch layout")
set(CIP_VULKAN_AOS 1 CACHE STRING "Vulkan scratch layout")

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" CIP_VERSION LIMIT_COUNT 1)
project(cipollino VERSION ${CIP_VERSION} LANGUAGES CXX)
add_compile_definitions(CIP_VERSION=${CIP_VERSION})

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CUDA_STANDARD 17)
set(CMAKE_CUDA_STANDARD_REQUIRED ON)

# A statically linked C runtime when the libraries were built that way (vcpkg's
# x64-windows-static): the program is then a single file with no redistributable to install.
if(WIN32 AND VCPKG_TARGET_TRIPLET MATCHES "-static$")
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()
if(WIN32)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
endif()
add_compile_options(
  "$<$<AND:$<COMPILE_LANGUAGE:CXX>,$<CXX_COMPILER_ID:MSVC>>:/W3>"
  "$<$<AND:$<COMPILE_LANGUAGE:CXX>,$<NOT:$<CXX_COMPILER_ID:MSVC>>>:-Wall;-Wextra>")

# ---- libraries ------------------------------------------------------------------------

find_package(Threads REQUIRED)

find_package(unofficial-sodium CONFIG QUIET)  # vcpkg
if(TARGET unofficial-sodium::sodium)
  set(cip_sodium unofficial-sodium::sodium)
else()
  find_path(SODIUM_INCLUDE_DIR sodium.h REQUIRED)
  find_library(SODIUM_LIBRARY NAMES sodium libsodium REQUIRED)
  add_library(cip_sodium INTERFACE)
  target_include_directories(cip_sodium INTERFACE "${SODIUM_INCLUDE_DIR}")
  target_link_libraries(cip_sodium INTERFACE "${SODIUM_LIBRARY}")
  set(cip_sodium cip_sodium)
endif()

# ---- backends -------------------------------------------------------------------------

set(cip_backend_sources src/backend_cpu.cpp)
set(cip_backend_definitions "")
set(cip_backend_libraries "")
set(cip_gpu_backends "")
set(cip_generated "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(cip_kernel src/kernel/defs.h src/kernel/field.h src/kernel/field13.h src/kernel/search.h)

set(cip_backend_includes "")

# ROCm: backend_cuda.cu compiled as HIP by ROCm's clang++, which is then the compiler of
# everything else too. One program has CUDA or ROCm, as the two are the same source file.
# On Windows the program needs the HIP runtime's DLLs (amdhip64, rocm_kpack, amd_comgr,
# in ROCm's bin directory) next to it or on PATH: the kernel registers itself with the
# runtime before main() runs.
if(CIP_ROCM)
  file(TO_CMAKE_PATH "${CIP_ROCM_PATH}" CIP_ROCM_PATH)
  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang" OR NOT EXISTS "${CIP_ROCM_PATH}/include/hip/hip_runtime.h")
    message(FATAL_ERROR "CIP_ROCM needs ROCm's clang++ as CMAKE_CXX_COMPILER and its installation in CIP_ROCM_PATH (or HIP_PATH)")
  endif()
  set(CIP_CUDA OFF)
  find_library(CIP_HIP_LIBRARY NAMES amdhip64 amdhip64_7 amdhip64_6 PATHS "${CIP_ROCM_PATH}/lib" NO_DEFAULT_PATH REQUIRED)
  set(cip_hip_options -xhip "--rocm-path=${CIP_ROCM_PATH}" -Wno-pass-failed)
  foreach(arch ${CIP_ROCM_ARCH})
    list(APPEND cip_hip_options "--offload-arch=${arch}")
  endforeach()
  set_source_files_properties(src/backend_cuda.cu PROPERTIES LANGUAGE CXX
    COMPILE_DEFINITIONS "CIP_ROCM;CIP_SCRATCH_AOS=${CIP_ROCM_AOS}"
    COMPILE_OPTIONS "${cip_hip_options}")
  list(APPEND cip_backend_sources src/backend_cuda.cu)
  list(APPEND cip_backend_definitions CIP_HAVE_ROCM)
  list(APPEND cip_backend_includes "${CIP_ROCM_PATH}/include")
  list(APPEND cip_backend_libraries "${CIP_HIP_LIBRARY}")
  list(APPEND cip_gpu_backends rocm)
endif()

if(CIP_CUDA STREQUAL "AUTO" OR CIP_CUDA)
  # The architecture flag is ours (CIP_CUDA_ARCH), as in the Makefile.
  set(CMAKE_CUDA_ARCHITECTURES OFF)
  include(CheckLanguage)
  check_language(CUDA)
  if(CMAKE_CUDA_COMPILER)
    enable_language(CUDA)
    list(APPEND cip_backend_sources src/backend_cuda.cu)
    list(APPEND cip_backend_definitions CIP_HAVE_CUDA)
    list(APPEND cip_gpu_backends cuda)
    set_source_files_properties(src/backend_cuda.cu PROPERTIES
      COMPILE_DEFINITIONS "CIP_SCRATCH_AOS=${CIP_CUDA_AOS}"
      COMPILE_OPTIONS "-arch=${CIP_CUDA_ARCH}")
  elseif(NOT CIP_CUDA STREQUAL "AUTO")
    message(FATAL_ERROR "CIP_CUDA=${CIP_CUDA}, but no CUDA compiler (nvcc) was found")
  endif()
endif()

if(CIP_VULKAN STREQUAL "AUTO" OR CIP_VULKAN)
  # Windows needs only the headers: the loader is opened at run time (backend_vulkan.cpp).
  set(cip_vulkan "")
  if(WIN32)
    find_package(VulkanHeaders CONFIG QUIET)  # vcpkg, Vulkan SDK
  endif()
  if(TARGET Vulkan::Headers)
    set(cip_vulkan Vulkan::Headers)
  else()
    find_package(Vulkan QUIET)
    if(Vulkan_FOUND AND WIN32)
      set(cip_vulkan Vulkan::Headers)
    elseif(Vulkan_FOUND)
      set(cip_vulkan Vulkan::Vulkan)
    endif()
  endif()
  find_program(CIP_GLSLANG NAMES glslangValidator glslang
    HINTS "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/tools/glslang" ENV VULKAN_SDK
    PATH_SUFFIXES bin Bin)
  if(cip_vulkan AND CIP_GLSLANG)
    # The kernel compiled to SPIR-V and embedded as C arrays: one module per variant of the
    # loop (CIP_SCRATCH_*) for each field arithmetic (w: wide, n: narrow).
    set(cip_spirv "")
    foreach(arith 0 1)
      foreach(mode 0 1 2 3)
        if(arith)
          set(spv "${cip_generated}/kernel_vk_n${mode}.spv")
        else()
          set(spv "${cip_generated}/kernel_vk_w${mode}.spv")
        endif()
        add_custom_command(OUTPUT "${spv}"
          COMMAND "${CMAKE_COMMAND}" -E make_directory "${cip_generated}"
          COMMAND "${CIP_GLSLANG}" -V --target-env vulkan1.1 -DCIP_ARITH=${arith} -DCIP_SMODE=${mode}
                  -DCIP_SCRATCH_AOS=${CIP_VULKAN_AOS} "-I${CMAKE_CURRENT_SOURCE_DIR}/src/kernel"
                  -o "${spv}" "${CMAKE_CURRENT_SOURCE_DIR}/src/kernel/entry_vulkan.comp"
          DEPENDS src/kernel/entry_vulkan.comp ${cip_kernel}
          VERBATIM)
        list(APPEND cip_spirv "${spv}")
      endforeach()
    endforeach()
    add_custom_command(OUTPUT "${cip_generated}/kernel_vk.inc"
      COMMAND "${CMAKE_COMMAND}" "-DDIR=${cip_generated}" "-DOUT=${cip_generated}/kernel_vk.inc"
              -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_spirv.cmake"
      DEPENDS ${cip_spirv} cmake/embed_spirv.cmake
      VERBATIM)
    list(APPEND cip_backend_sources src/backend_vulkan.cpp "${cip_generated}/kernel_vk.inc")
    list(APPEND cip_backend_definitions CIP_HAVE_VULKAN)
    list(APPEND cip_backend_libraries ${cip_vulkan} ${CMAKE_DL_LIBS})
    list(APPEND cip_gpu_backends vulkan)
  elseif(NOT CIP_VULKAN STREQUAL "AUTO")
    message(FATAL_ERROR "CIP_VULKAN=${CIP_VULKAN}, but the Vulkan headers or glslangValidator were not found")
  endif()
endif()

# ---- the program ----------------------------------------------------------------------

add_library(cipcore STATIC
  src/engine.cpp src/keys.cpp src/onion.cpp src/ed.cpp src/filter.cpp src/platform.cpp)
target_include_directories(cipcore PUBLIC src)
target_link_libraries(cipcore PUBLIC ${cip_sodium} Threads::Threads)

add_executable(cipollino src/main.cpp ${cip_backend_sources})
target_compile_definitions(cipollino PRIVATE ${cip_backend_definitions})
target_include_directories(cipollino PRIVATE "${cip_generated}" ${cip_backend_includes})
target_link_libraries(cipollino PRIVATE cipcore ${cip_backend_libraries})
# No CUDA runtime DLL to ship: only the driver's own library is needed, and only when a
# CUDA device is actually used.
set_target_properties(cipollino PROPERTIES CUDA_RUNTIME_LIBRARY Static)

list(JOIN cip_gpu_backends ", " cip_summary)
message(STATUS "cipollino backends: cpu ${cip_summary}")

# ---- tests ----------------------------------------------------------------------------

enable_testing()
find_package(Python3 COMPONENTS Interpreter)

# The kernel's field arithmetic behind a C ABI, checked against Python integers: once
# for each implementation of the multiplication.
add_library(fieldshim SHARED tests/field_shim.cpp)
target_include_directories(fieldshim PRIVATE src)
add_library(fieldshim13 SHARED tests/field_shim.cpp)
target_include_directories(fieldshim13 PRIVATE src)
target_compile_definitions(fieldshim13 PRIVATE CIP_ARITH=1)

# The CUDA backend as ordinary C++ on a stand-in for the CUDA runtime, so its host logic
# and the kernel as CUDA sees it are exercised without a GPU or nvcc.
add_executable(cipollino-cudasim src/main.cpp src/backend_cpu.cpp tests/sim_backend_cuda.cpp)
target_compile_definitions(cipollino-cudasim PRIVATE CIP_HAVE_CUDA)
set_source_files_properties(tests/sim_backend_cuda.cpp PROPERTIES
  COMPILE_DEFINITIONS "CIP_SCRATCH_AOS=${CIP_CUDA_AOS}")
target_link_libraries(cipollino-cudasim PRIVATE cipcore)

# The CPU backend running the kernel with the narrow field arithmetic, which otherwise only
# Vulkan devices see.
add_executable(cipollino-narrow src/main.cpp src/backend_cpu.cpp)
target_compile_definitions(cipollino-narrow PRIVATE CIP_ARITH=1)
target_link_libraries(cipollino-narrow PRIVATE cipcore)

add_test(NAME selftest-cpu COMMAND cipollino --backend cpu --selftest)
add_test(NAME selftest-cudasim COMMAND cipollino-cudasim --backend cuda --selftest)
add_test(NAME selftest-narrow COMMAND cipollino-narrow --backend cpu --selftest)
foreach(backend ${cip_gpu_backends})
  add_test(NAME selftest-${backend} COMMAND cipollino --backend ${backend} --selftest)
  set_tests_properties(selftest-${backend} PROPERTIES LABELS gpu)
endforeach()
if(Python3_FOUND)
  set(cip_tests "${CMAKE_CURRENT_SOURCE_DIR}/tests")
  add_test(NAME field COMMAND Python3::Interpreter "${cip_tests}/test_field.py" $<TARGET_FILE:fieldshim>)
  add_test(NAME field13 COMMAND Python3::Interpreter "${cip_tests}/test_field.py" $<TARGET_FILE:fieldshim13>)
  add_test(NAME field13-generated
    COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_field13.py" --check
            "${CMAKE_CURRENT_SOURCE_DIR}/src/kernel/field13.h")
  add_test(NAME cli-cpu COMMAND Python3::Interpreter "${cip_tests}/test_cli.py" $<TARGET_FILE:cipollino> cpu)
  add_test(NAME cli-cudasim
    COMMAND Python3::Interpreter "${cip_tests}/test_cli.py" $<TARGET_FILE:cipollino-cudasim> cuda)
  foreach(backend ${cip_gpu_backends})
    add_test(NAME cli-${backend}
      COMMAND Python3::Interpreter "${cip_tests}/test_cli.py" $<TARGET_FILE:cipollino> ${backend})
    set_tests_properties(cli-${backend} PROPERTIES LABELS gpu)
  endforeach()
endif()
