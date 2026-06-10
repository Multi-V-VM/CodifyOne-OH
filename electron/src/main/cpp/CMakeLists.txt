cmake_minimum_required(VERSION 3.5.0)
project(ohcode_qemu)

set(NATIVERENDER_ROOT_PATH ${CMAKE_CURRENT_SOURCE_DIR})

include_directories(${NATIVERENDER_ROOT_PATH})

add_library(entry SHARED
    qemu_runner.cpp
    host_command_bridge.cpp
)

target_compile_features(entry PRIVATE cxx_std_17)

target_link_libraries(entry PUBLIC
    libace_napi.z.so
    libhilog_ndk.z.so
    dl
)