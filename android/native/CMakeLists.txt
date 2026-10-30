# Android (arm64-v8a) native build: the recompiled game code (ppc/) and the host layer.
#
# Two targets: the app's library (HC09_ANDROID_APP=ON: Vulkan renderer, AAudio/OpenSL ES output)
# and hc09_headless, a command-line program without renderer or audio output that boots the game
# on a phone over adb.
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
# host_input_posix (XInput calls). Audio output and the renderer are per target below.
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
    src/gpu/shaders.cpp
    src/gpu/ucode.cpp
    src/kernel/format.cpp
    src/kernel/heap.cpp
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

option(HC09_ANDROID_APP "Build libhc09.so for the Android app (android/app) instead of hc09_headless" OFF)

if(HC09_ANDROID_APP)
    # The app's library: the same host layer, entered through JNI (src/android/jni_main.cpp), with
    # the Vulkan renderer (shaders from the APK's assets, built offline by tools/shaders_to_spirv.py).
    add_library(hc09 SHARED ${HC09_HOST_SOURCES}
        ${HC09_ROOT}/src/android/jni_main.cpp
        ${HC09_ROOT}/src/kernel/host_audio_android.cpp
        ${HC09_ROOT}/src/gpu/hlsl_translator.cpp
        ${HC09_ROOT}/src/gpu/vulkan_context.cpp
        ${HC09_ROOT}/src/gpu/vulkan_renderer.cpp
        ${HC09_ROOT}/src/gpu/vulkan_texture_cache.cpp)
    set(HC09_TARGET hc09)
    target_compile_definitions(hc09 PRIVATE HC09_ANDROID_APP)
    target_link_libraries(hc09 PRIVATE vulkan aaudio OpenSLES)
else()
    add_executable(hc09_headless ${HC09_HOST_SOURCES} ${HC09_ROOT}/src/gpu/null_renderer.cpp
        ${HC09_ROOT}/src/kernel/host_audio_null.cpp)
    set(HC09_TARGET hc09_headless)
    # --export-dynamic names recompiled functions (sub_<address>) in stack dumps.
    target_link_options(hc09_headless PRIVATE -Wl,--export-dynamic)
endif()

target_include_directories(${HC09_TARGET} PRIVATE ${HC09_ROOT}/src ${HC09_ROOT}/thirdparty/XenonRecomp/thirdparty/xxHash)
target_compile_definitions(${HC09_TARGET} PRIVATE HC09_VERSION="${PROJECT_VERSION}")

# FFmpeg's XMA2 decoder (LGPL 2.1+), compiled from an FFmpeg 8.0 source tree: only libavutil and
# the part of libavcodec the decoder uses, in plain C (no assembly). FFmpeg's configure doesn't run:
# tools/ffmpeg_android_config.py writes its headers. The source tree comes from
# -DHC09_FFMPEG_SOURCE (android/local.properties: hc09.ffmpegSource=...), the HC09_FFMPEG_SOURCE
# environment variable, or thirdparty/ffmpeg-src. Without it, XMA decodes to silence.
set(HC09_FFMPEG_SOURCE "$ENV{HC09_FFMPEG_SOURCE}" CACHE PATH "FFmpeg 8.0 source tree for the XMA decoder")
if(NOT HC09_FFMPEG_SOURCE)
    set(HC09_FFMPEG_SOURCE ${HC09_ROOT}/thirdparty/ffmpeg-src)
endif()
if(EXISTS ${HC09_FFMPEG_SOURCE}/libavcodec/wmaprodec.c)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    set(FFMPEG_CONFIG_DIR ${CMAKE_CURRENT_BINARY_DIR}/ffmpeg-config)
    execute_process(
        COMMAND ${Python3_EXECUTABLE} ${HC09_ROOT}/tools/ffmpeg_android_config.py ${HC09_FFMPEG_SOURCE} ${FFMPEG_CONFIG_DIR}
        COMMAND_ERROR_IS_FATAL ANY)
    # libavutil's and libavcodec's base objects (OBJS in their Makefiles), the threading code
    # (HAVE_THREADS), the null bitstream filter and the decoder's own files (wmaprodec, wma,
    # wma_common, sinewin, wma_freqs).
    set(FFMPEG_AVUTIL_SOURCES adler32 aes aes_ctr ambient_viewing_environment audio_fifo avstring
        avsscanf base64 blowfish bprint buffer cast5 camellia channel_layout container_fifo cpu crc
        csp des detection_bbox dict display dovi_meta downmix_info encryption_info error eval executor
        fifo file file_open float_dsp float_scalarproduct film_grain_params fixed_dsp frame hash
        hdr_dynamic_metadata hdr_dynamic_vivid_metadata hmac hwcontext iamf imgutils integer intmath
        lfg lls log log2_tab lzo mathematics mastering_display_metadata md5 mem murmur3 opt parseutils
        pixdesc pixelutils random_seed rational refstruct reverse rc4 ripemd samplefmt side_data sha
        sha512 slicethread spherical stereo3d tdrdi threadmessage time timecode timecode_internal
        timestamp tree twofish utils xga_font_data xtea tea tx tx_float tx_double tx_int32 uuid version
        video_enc_params video_hint)
    set(FFMPEG_AVCODEC_SOURCES ac3_parser adts_parser allcodecs avcodec avdct packet bitstream
        bitstream_filters bsf codec_desc codec_par d3d11va decode dirac dv_profile encode get_buffer
        imgconvert jni lcevcdec mathtables mediacodec mpeg12framerate options parser parsers profiles
        qsv_api raw threadprogress utils version vlc vorbis_parser xiph
        pthread pthread_slice pthread_frame
        bsf/null wmaprodec wma wma_common sinewin wma_freqs)
    list(TRANSFORM FFMPEG_AVUTIL_SOURCES REPLACE "(.+)" "${HC09_FFMPEG_SOURCE}/libavutil/\\1.c")
    list(TRANSFORM FFMPEG_AVCODEC_SOURCES REPLACE "(.+)" "${HC09_FFMPEG_SOURCE}/libavcodec/\\1.c")
    add_library(hc09_ffmpeg STATIC ${FFMPEG_AVUTIL_SOURCES} ${FFMPEG_AVCODEC_SOURCES})
    # The generated headers first: libavcodec includes "libavcodec/codec_list.c" and the like.
    target_include_directories(hc09_ffmpeg BEFORE PRIVATE ${FFMPEG_CONFIG_DIR})
    target_include_directories(hc09_ffmpeg PUBLIC ${FFMPEG_CONFIG_DIR} ${HC09_FFMPEG_SOURCE})
    # Like FFmpeg's make, files in a subfolder also see their library's headers.
    set_source_files_properties(${HC09_FFMPEG_SOURCE}/libavcodec/bsf/null.c PROPERTIES
        INCLUDE_DIRECTORIES ${HC09_FFMPEG_SOURCE}/libavcodec)
    target_compile_definitions(hc09_ffmpeg PRIVATE HAVE_AV_CONFIG_H _ISOC11_SOURCE _FILE_OFFSET_BITS=64
        _LARGEFILE_SOURCE _POSIX_C_SOURCE=200112 _XOPEN_SOURCE=600)
    set_target_properties(hc09_ffmpeg PROPERTIES C_STANDARD 17 C_VISIBILITY_PRESET hidden)
    target_compile_options(hc09_ffmpeg PRIVATE ${HC09_ARM_FLAGS} -O2 -w)
    target_link_libraries(hc09_ffmpeg PUBLIC m)
    target_link_libraries(${HC09_TARGET} PRIVATE hc09_ffmpeg)
else()
    message(WARNING "No FFmpeg source at '${HC09_FFMPEG_SOURCE}': XMA audio decodes to silence")
    target_compile_definitions(${HC09_TARGET} PRIVATE HC09_NO_FFMPEG)
endif()
target_compile_options(${HC09_TARGET} PRIVATE ${HC09_ARM_FLAGS})
# 16 KB pages for newer phones.
target_link_options(${HC09_TARGET} PRIVATE -Wl,-z,max-page-size=16384)
target_link_libraries(${HC09_TARGET} PRIVATE HC09RecompLib android log)
