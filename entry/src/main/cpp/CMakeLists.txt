cmake_minimum_required(VERSION 3.5.0)
project(BlenderHost)
set(CMAKE_CXX_STANDARD 17)

add_library(entry SHARED napi_init.cpp)

target_include_directories(entry PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(entry PUBLIC
    libace_napi.z.so
    libace_ndk.z.so
    libhilog_ndk.z.so
    libnative_window.so
    libnative_vsync.so
    libvulkan.so
    libuv.so
    dl
)

target_link_options(entry PRIVATE "-Wl,-z,defs")