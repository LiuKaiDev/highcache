cmake_minimum_required(VERSION 3.20)

project(
    highcache
    VERSION 1.0.0
    DESCRIPTION "High-performance multithreaded in-memory KV cache server"
    LANGUAGES CXX
)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "HighCache requires Linux")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(HIGHCACHE_ENABLE_SANITIZERS "Enable ASan and UBSan in supported debug builds" OFF)
option(HIGHCACHE_ENABLE_TSAN "Enable ThreadSanitizer in supported debug builds" OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(CompilerWarnings)
include(Sanitizers)
find_package(Threads REQUIRED)

add_library(highcache_project_options INTERFACE)
target_compile_features(highcache_project_options INTERFACE cxx_std_20)
highcache_enable_warnings(highcache_project_options)
highcache_enable_sanitizers(highcache_project_options)

add_library(highcache_core
    src/cache/cache.cpp
    src/cache/cache_engine.cpp
    src/cache/cache_shard.cpp
    src/common/error.cpp
    src/config/config.cpp
    src/logging/logger.cpp
    src/memory/slab_allocator.cpp
    src/memory/value_allocator.cpp
    src/net/buffer.cpp
    src/net/cache_server.cpp
    src/net/connection.cpp
    src/protocol/protocol_codec.cpp
    src/timer/timing_wheel.cpp
)
target_include_directories(highcache_core PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
)
target_link_libraries(highcache_core PUBLIC
    highcache_project_options
    Threads::Threads
)

add_executable(highcache_server
    src/server/main.cpp
)
target_link_libraries(highcache_server PRIVATE highcache_core)

add_executable(highcache_client
    src/client/main.cpp
)
target_link_libraries(highcache_client PRIVATE highcache_core)

add_library(highcache_benchmark_lib
    benchmark/benchmark.cpp
)
target_include_directories(highcache_benchmark_lib PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>"
)
target_link_libraries(highcache_benchmark_lib PUBLIC highcache_core)

add_executable(highcache_benchmark
    benchmark/main.cpp
)
target_link_libraries(highcache_benchmark PRIVATE highcache_benchmark_lib)

include(CTest)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
