cmake_minimum_required(VERSION 3.16)
project(ubersdr_ndb LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# AVX2 and FMA (as ubersdr-skimmer): the IQ stream's decoder (pcm_v4.hpp), the
# filter bank and the FIRs vectorise with them. The default, x86-64-v3, runs
# on any x86 since Haswell or Zen; "native" tunes for the build machine alone,
# and an empty value builds for the compiler's baseline. Not applied on ARM.
set(NDB_X86_ARCH "x86-64-v3" CACHE STRING "-march for x86-64 builds (x86-64-v3, native, or empty)")
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64" AND NDB_X86_ARCH)
    add_compile_options(-march=${NDB_X86_ARCH})
endif()
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    add_compile_options(-Wno-psabi)
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(FFTW3F REQUIRED IMPORTED_TARGET fftw3f)

# -----------------------------------------------------------------------
# ggmorse — vendored from UberSDR's cw-decoder (third_party/ggmorse).
# -----------------------------------------------------------------------
set(GGMORSE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/ggmorse)
add_library(ggmorse STATIC
    ${GGMORSE_DIR}/src/ggmorse.cpp
    ${GGMORSE_DIR}/src/resampler.cpp
)
target_include_directories(ggmorse PUBLIC ${GGMORSE_DIR}/include ${GGMORSE_DIR}/src)
target_compile_options(ggmorse PRIVATE -w)

# -----------------------------------------------------------------------
# NDB DSP core — no network dependencies, so tools and tests can link it.
# -----------------------------------------------------------------------
add_library(ndbcore STATIC
    src/fft.cpp
    src/channelizer.cpp
    src/carrier_detector.cpp
    src/ndb_channel.cpp
    src/ndb_decoder.cpp
    src/navaids.cpp
    src/keying_decoder.cpp
    src/fold_decoder.cpp
)
target_include_directories(ndbcore PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(ndbcore PUBLIC ggmorse PkgConfig::FFTW3F)
target_compile_options(ndbcore PRIVATE -Wall -Wextra)

# -----------------------------------------------------------------------
# IXWebSocket: system install, else a source tree (same arrangement as
# ubersdr_navtex / ubersdr_dsc). The Dockerfile clones it and passes
# -DIXWS_ROOT; locally it defaults to the copy in the ka9q_ubersdr checkout.
# -----------------------------------------------------------------------
pkg_check_modules(CURL REQUIRED libcurl)
find_package(Threads REQUIRED)

find_package(ixwebsocket QUIET)
if(ixwebsocket_FOUND)
    set(IXWS_LIBS ixwebsocket::ixwebsocket)
else()
    set(IXWS_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../ka9q_ubersdr/clients/CW_Skimmer/UberSDRIntf/IXWebSocket"
        CACHE PATH "Path to IXWebSocket source tree")
    if(NOT EXISTS "${IXWS_ROOT}/ixwebsocket/IXWebSocket.h")
        message(FATAL_ERROR "IXWebSocket not found. Install it or pass -DIXWS_ROOT=/path/to/IXWebSocket")
    endif()
    file(GLOB IXWS_SOURCES "${IXWS_ROOT}/ixwebsocket/*.cpp")
    add_library(ixwebsocket_bundled STATIC ${IXWS_SOURCES})
    target_include_directories(ixwebsocket_bundled PUBLIC "${IXWS_ROOT}")
    find_package(OpenSSL REQUIRED)
    find_package(ZLIB REQUIRED)
    target_link_libraries(ixwebsocket_bundled PUBLIC OpenSSL::SSL OpenSSL::Crypto ZLIB::ZLIB Threads::Threads)
    # TLS is compiled out of IXWebSocket unless these are defined; upstream's
    # CMake sets them, but globbing the sources bypasses that (see the long
    # note in ubersdr_navtex/src/CMakeLists.txt).
    target_compile_definitions(ixwebsocket_bundled PUBLIC IXWEBSOCKET_USE_TLS IXWEBSOCKET_USE_OPEN_SSL IXWEBSOCKET_USE_ZLIB)
    target_compile_options(ixwebsocket_bundled PRIVATE -w)
    set(IXWS_LIBS ixwebsocket_bundled)
endif()

add_executable(ubersdr_ndb src/main.cpp src/mqtt.cpp)
target_include_directories(ubersdr_ndb PRIVATE ${CURL_INCLUDE_DIRS})
target_link_libraries(ubersdr_ndb PRIVATE ndbcore ${IXWS_LIBS} ${CURL_LIBRARIES} Threads::Threads m)
target_compile_options(ubersdr_ndb PRIVATE -Wall -Wextra)

install(TARGETS ubersdr_ndb)

# Debug tool: one channel's ggmorse input audio as a WAV (not built by default).
add_executable(chan_audio EXCLUDE_FROM_ALL tools/chan_audio.cpp)
target_link_libraries(chan_audio PRIVATE ndbcore)

# Debug tool: chosen beacons' copies from a raw IQ file, scored (not built by default).
add_executable(chan_eval EXCLUDE_FROM_ALL tools/chan_eval.cpp)
target_link_libraries(chan_eval PRIVATE ndbcore)

# Test: a recycled carrier waits out its cooldown even for a free slot.
# ctest --test-dir build (not built by default: cmake --build build --target slot_rotation_test)
enable_testing()
add_executable(slot_rotation_test EXCLUDE_FROM_ALL tools/slot_rotation_test.cpp)
target_link_libraries(slot_rotation_test PRIVATE ndbcore)
add_test(NAME slot_rotation COMMAND slot_rotation_test)
