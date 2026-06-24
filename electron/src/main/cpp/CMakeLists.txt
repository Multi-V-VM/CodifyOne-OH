cmake_minimum_required(VERSION 3.5.0)
project(ohcode_wasmer)

set(NATIVERENDER_ROOT_PATH ${CMAKE_CURRENT_SOURCE_DIR})

include_directories(${NATIVERENDER_ROOT_PATH})

add_library(entry SHARED
    wasmer_host.cpp
    window_pool_host.cpp
    v8_pool_hook.cpp
)

target_compile_features(entry PRIVATE cxx_std_17)

target_link_libraries(entry PUBLIC
    libace_napi.z.so
    libhilog_ndk.z.so
    dl
)
