# A strict warning set for code in this repository. Third party targets
# (GoogleTest) do not get these, only targets passed to this function.
function(sigflow_set_warnings target)
    if(MSVC)
        set(warnings /W4 /permissive-)
        if(SIGFLOW_WARNINGS_AS_ERRORS)
            list(APPEND warnings /WX)
        endif()
    else()
        set(warnings
            -Wall
            -Wextra
            -Wpedantic
            -Wshadow
            -Wconversion
            -Wsign-conversion
            -Wold-style-cast
            -Wnon-virtual-dtor
            -Woverloaded-virtual
            -Wcast-align
            -Wformat=2
            -Wimplicit-fallthrough
            -Wdouble-promotion)
        if(SIGFLOW_WARNINGS_AS_ERRORS)
            list(APPEND warnings -Werror)
        endif()
    endif()
    target_compile_options(${target} PRIVATE ${warnings})
endfunction()
