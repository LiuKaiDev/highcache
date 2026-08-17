function(highcache_enable_warnings target)
    if(MSVC)
        target_compile_options(${target} INTERFACE /W4 /permissive-)
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
        target_compile_options(${target} INTERFACE
            -Wall
            -Wextra
            -Wpedantic
        )
    endif()
endfunction()
