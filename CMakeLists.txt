cmake_minimum_required(VERSION 3.20)

project(
    highcache
    VERSION 0.1.0
    DESCRIPTION "High-performance multithreaded in-memory KV cache server"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(HIGHCACHE_BUILD_TESTS "Build HighCache tests" ON)
option(HIGHCACHE_ENABLE_SANITIZERS "Enable ASan and UBSan in supported debug builds" OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(CompilerWarnings)
include(Sanitizers)

add_library(highcache_project_options INTERFACE)
target_compile_features(highcache_project_options INTERFACE cxx_std_20)
highcache_enable_warnings(highcache_project_options)
highcache_enable_sanitizers(highcache_project_options)

add_executable(highcache_server
    src/server/main.cpp
)
target_link_libraries(highcache_server PRIVATE highcache_project_options)

if(HIGHCACHE_BUILD_TESTS)
    include(CTest)
    enable_testing()
    add_subdirectory(tests)
endif()
