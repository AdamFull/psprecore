# Common compile options for first-party targets.
#
# Recompiled code (generated/) is not built with these: it gets its own flags
# (-fwrapv -fno-strict-aliasing, no warnings) in runtime/CMakeLists.txt.

function(psprecore_target_defaults target)
    if(MSVC)  # MSVC or clang-cl
        # Vulkan's VkFoo{VK_STRUCTURE_TYPE_FOO} idiom leaves the remaining fields zeroed on purpose.
        target_compile_options(${target} PRIVATE /W4 /utf-8 /permissive- -Wno-missing-field-initializers)
        target_compile_definitions(${target} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wno-missing-field-initializers)
    endif()
    if(NOT WIN32)
        target_compile_definitions(${target} PRIVATE _FILE_OFFSET_BITS=64)
    endif()
    # Windows executables run with the UTF-8 code page (argv, file names), see utf8.manifest.
    get_target_property(_psprecore_type ${target} TYPE)
    if(WIN32 AND _psprecore_type STREQUAL "EXECUTABLE")
        target_sources(${target} PRIVATE "${PSPRECORE_DIR}/cmake/utf8.manifest")
    endif()
endfunction()
