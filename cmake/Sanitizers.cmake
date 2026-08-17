function(highcache_enable_sanitizers target)
    if(HIGHCACHE_ENABLE_SANITIZERS AND HIGHCACHE_ENABLE_TSAN)
        message(FATAL_ERROR "ASan/UBSan and TSan cannot be enabled together")
    endif()

    if(NOT HIGHCACHE_ENABLE_SANITIZERS AND NOT HIGHCACHE_ENABLE_TSAN)
        return()
    endif()

    if(NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
        message(WARNING "Sanitizer instrumentation is intended for Debug builds")
    endif()

    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
        if(HIGHCACHE_ENABLE_TSAN)
            set(HIGHCACHE_SANITIZER_FLAGS -fsanitize=thread)
        else()
            set(HIGHCACHE_SANITIZER_FLAGS -fsanitize=address,undefined)
        endif()

        target_compile_options(${target} INTERFACE
            ${HIGHCACHE_SANITIZER_FLAGS}
            -fno-omit-frame-pointer
        )
        target_link_options(${target} INTERFACE
            ${HIGHCACHE_SANITIZER_FLAGS}
            -fno-omit-frame-pointer
        )
    else()
        message(WARNING "Sanitizer flags are only configured for GCC/Clang-like compilers")
    endif()
endfunction()
