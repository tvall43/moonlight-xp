cmake_minimum_required(VERSION 3.10)
project(MoonlightXP C)

set(CMAKE_C_STANDARD 11)

include_directories(
    src
    sysroot/include
    sysroot/include/opus
    third_party/moonlight-common-c/src
    third_party/moonlight-common-c/enet/include
)

link_directories(
    sysroot/lib
    sysroot/lib/static
)

set(SOURCES
    src/main.c
    src/http_client.c
    src/nv_pairing.c
    src/video_renderer.c
    src/audio_renderer.c
    src/input_handler.c
    src/xp_compat.c
    src/resources.rc
)

add_executable(MoonlightXP WIN32 ${SOURCES})

target_compile_definitions(MoonlightXP PRIVATE
    _WIN32_WINNT=0x0501
    WINVER=0x0501
    USE_MBEDTLS
)

target_link_libraries(MoonlightXP PRIVATE
    moonlight-common-c
    enet
    opus
    avcodec
    avutil
    swscale
    mbedtls
    mbedx509
    mbedcrypto
    everest
    p256m
    ws2_32
    winmm
    comctl32
    gdi32
    user32
    kernel32
    advapi32
    pthread
)

set_target_properties(MoonlightXP PROPERTIES
    LINK_FLAGS "-mwindows -Wl,--subsystem,windows:5.1 -Wl,--major-os-version,5 -Wl,--minor-os-version,1 -Wl,--major-subsystem-version,5 -Wl,--minor-subsystem-version,1 -static -static-libgcc"
)
