# Android (arm64-v8a) native build: the recompiled game code (ppc/) and the host layer.
#
# For now the host layer runs without a renderer, audio output or FFmpeg (XMA decodes to
# silence), as a command-line program (hc09_headless) that boots the game on a phone over adb.
#
# Configure with the NDK toolchain, e.g.:
#   cmake -S android/native -B build/android -G Ninja -DCMAKE_BUILD_TYPE=Release
#         -DCMAKE_TOOLCHAIN_FILE=<ndk>/build/cmake/android.toolchain.cmake -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=26
cmake_minimum_required(VERSION 3.20)
project(HC09Android VERSION 0.8.2 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

get_filename_component(HC09_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../.. ABSOLUTE)
set(SIMDE_DIR ${HC09_ROOT}/thirdparty/XenonRecomp/thirdparty/simde)

file(GLOB HC09_PPC_SOURCES CONFIGURE_DEPENDS ${HC09_ROOT}/ppc/*.cpp)
if(NOT HC09_PPC_SOURCES)
    message(FATAL_ERROR "ppc/ is empty: run XenonRecomp on config/HC09.toml first")
endif()

# One build for both phones: the LG V30 is ARMv8.0 (no LSE atomics, no dotprod), so don't raise -march.
set(HC09_ARM_FLAGS -march=armv8-a -fno-strict-aliasing -gline-tables-only)

add_library(HC09RecompLib STATIC ${HC09_PPC_SOURCES})
target_include_directories(HC09RecompLib PUBLIC ${HC09_ROOT}/ppc ${SIMDE_DIR})
target_compile_options(HC09RecompLib PRIVATE
    ${HC09_ARM_FLAGS}
    -Wno-unused-label
    -Wno-unused-variable
    -Wno-unused-but-set-variable
    -Wno-unused-function)

# Host sources shared with the Windows build (src/), plus the POSIX and stand-in versions of the
# Windows-only parts: memory_posix (guest memory), main_posix (entry point), stack_dump_posix,
# host_input_posix (XInput calls), host_audio_null and gpu/null_renderer.
set(HC09_HOST_SOURCES
    src/main_posix.cpp
    src/memory_posix.cpp
    src/thread.cpp
    src/game_fixes.cpp
    src/common/edition.cpp
    src/common/host_file.cpp
    src/common/host_time.cpp
    src/debug/hooks.cpp
    src/debug/stack_dump_posix.cpp
    src/gpu/command_processor.cpp
    src/gpu/null_renderer.cpp
    src/gpu/shaders.cpp
    src/gpu/ucode.cpp
    src/kernel/format.cpp
    src/kernel/heap.cpp
    src/kernel/host_audio_null.cpp
    src/kernel/host_input_posix.cpp
    src/kernel/import_stubs.cpp
    src/kernel/objects.cpp
    src/kernel/variables.cpp
    src/kernel/vfs.cpp
    src/kernel/xam_content.cpp
    src/kernel/xam_input.cpp
    src/kernel/xam_misc.cpp
    src/kernel/xam_net.cpp
    src/kernel/xam_notify.cpp
    src/kernel/xam_user.cpp
    src/kernel/xboxkrnl_audio.cpp
    src/kernel/xboxkrnl_io.cpp
    src/kernel/xboxkrnl_memory.cpp
    src/kernel/xboxkrnl_misc.cpp
    src/kernel/xboxkrnl_rtl.cpp
    src/kernel/xboxkrnl_strings.cpp
    src/kernel/xboxkrnl_threading.cpp
    src/kernel/xboxkrnl_video.cpp
    src/kernel/xboxkrnl_xma.cpp
    src/kernel/xex.cpp)
list(TRANSFORM HC09_HOST_SOURCES PREPEND ${HC09_ROOT}/)

add_executable(hc09_headless ${HC09_HOST_SOURCES})
target_include_directories(hc09_headless PRIVATE ${HC09_ROOT}/src ${HC09_ROOT}/thirdparty/XenonRecomp/thirdparty/xxHash)
target_compile_definitions(hc09_headless PRIVATE HC09_VERSION="${PROJECT_VERSION}" HC09_NO_FFMPEG)
target_compile_options(hc09_headless PRIVATE ${HC09_ARM_FLAGS})
# --export-dynamic names recompiled functions (sub_<address>) in stack dumps; 16 KB pages for newer phones.
target_link_options(hc09_headless PRIVATE -Wl,--export-dynamic -Wl,-z,max-page-size=16384)
target_link_libraries(hc09_headless PRIVATE HC09RecompLib android log)
