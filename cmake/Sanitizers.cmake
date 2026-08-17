function(highcache_enable_sanitizers target)
    if(NOT HIGHCACHE_ENABLE_SANITIZERS)
        return()
    endif()

    if(NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
        message(WARNING "HIGHCACHE_ENABLE_SANITIZERS is intended for Debug builds")
    endif()

    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
        target_compile_options(${target} INTERFACE
            -fsanitize=address,undefined
            -fno-omit-frame-pointer
        )
        target_link_options(${target} INTERFACE
            -fsanitize=address,undefined
            -fno-omit-frame-pointer
        )
    else()
        message(WARNING "ASan/UBSan bootstrap flags are only configured for GCC/Clang-like compilers")
    endif()
endfunction()
