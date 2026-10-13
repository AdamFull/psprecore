# Common compile options for first-party targets.
#
# Generated (recompiled) code will get its own stricter semantics flags
# (-fwrapv -fno-strict-aliasing) via ff7cc_generated_code_options() once the
# recompiler lands.

function(ff7cc_target_defaults target)
    if(MSVC)  # MSVC or clang-cl
        target_compile_options(${target} PRIVATE /W4 /utf-8 /permissive-)
        target_compile_definitions(${target} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    if(NOT WIN32)
        target_compile_definitions(${target} PRIVATE _FILE_OFFSET_BITS=64)
    endif()
endfunction()
