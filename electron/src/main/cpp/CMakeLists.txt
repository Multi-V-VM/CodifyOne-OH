cmake_minimum_required(VERSION 3.5.0)
project(ohcode_wasmer)

set(NATIVERENDER_ROOT_PATH ${CMAKE_CURRENT_SOURCE_DIR})

include_directories(${NATIVERENDER_ROOT_PATH})

add_library(entry SHARED
    window_pool_host.cpp
    v8_pool_hook.cpp
)

target_compile_features(entry PRIVATE cxx_std_17)

target_link_libraries(entry PUBLIC
    libace_napi.z.so
    libhilog_ndk.z.so
    dl
)

# Keep the Wasmer N-API module in its own shared object.  HarmonyOS resolves a
# native module from the library basename; registering entry, windowpool and
# v8poolhook from the same libentry.so made the entry exports order-dependent.
add_library(wasmerhost SHARED
    wasmer_host.cpp
)

target_compile_features(wasmerhost PRIVATE cxx_std_17)

target_link_libraries(wasmerhost PUBLIC
    libace_napi.z.so
    libhilog_ndk.z.so
    dl
)
