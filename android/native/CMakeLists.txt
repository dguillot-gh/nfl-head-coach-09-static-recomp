# Android (arm64-v8a) native build. For now only the recompiled game code (ppc/), to check it compiles
# for ARM; the host layer joins once its Android versions exist.
#
# Configure with the NDK toolchain, e.g.:
#   cmake -S android/native -B build/android -G Ninja -DCMAKE_BUILD_TYPE=Release
#         -DCMAKE_TOOLCHAIN_FILE=<ndk>/build/cmake/android.toolchain.cmake -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=26
cmake_minimum_required(VERSION 3.20)
project(HC09Android LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

get_filename_component(HC09_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../.. ABSOLUTE)
set(SIMDE_DIR ${HC09_ROOT}/thirdparty/XenonRecomp/thirdparty/simde)

file(GLOB HC09_PPC_SOURCES CONFIGURE_DEPENDS ${HC09_ROOT}/ppc/*.cpp)
if(NOT HC09_PPC_SOURCES)
    message(FATAL_ERROR "ppc/ is empty: run XenonRecomp on config/HC09.toml first")
endif()

add_library(HC09RecompLib STATIC ${HC09_PPC_SOURCES})
target_include_directories(HC09RecompLib PUBLIC ${HC09_ROOT}/ppc ${SIMDE_DIR})
# One build for both phones: the LG V30 is ARMv8.0 (no LSE atomics, no dotprod), so don't raise -march.
target_compile_options(HC09RecompLib PRIVATE
    -march=armv8-a
    -fno-strict-aliasing
    -gline-tables-only
    -Wno-unused-label
    -Wno-unused-variable
    -Wno-unused-but-set-variable
    -Wno-unused-function)
