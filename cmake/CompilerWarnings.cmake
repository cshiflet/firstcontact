function(fc_set_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE
            /W4 /permissive- /utf-8 /Zc:__cplusplus
            /wd4244 /wd4267)
    else()
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -Wpedantic
            -Wshadow -Wnon-virtual-dtor -Woverloaded-virtual
            -Wold-style-cast -Wcast-align -Wunused
            -Wno-unused-parameter)
    endif()
endfunction()
