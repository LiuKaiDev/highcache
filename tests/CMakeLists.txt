include(FetchContent)

find_package(GTest CONFIG QUIET)

if(NOT GTest_FOUND)
    message(STATUS "GoogleTest not found locally; fetching a pinned release")
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)

    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG v1.15.2
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(googletest)
endif()

add_executable(highcache_tests
    bootstrap_test.cpp
)

target_link_libraries(highcache_tests PRIVATE
    highcache_project_options
    GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(highcache_tests)
