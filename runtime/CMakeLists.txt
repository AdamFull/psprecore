# Runtime for recompiled code: guest memory, HLE kernel and system libraries.
add_library(ff7cc_runtime STATIC
    src/recomp_runtime.c
    src/memory.cpp
    src/kernel.cpp
    src/hle.cpp
    src/hle_imports.cpp
    src/hle_threadman.cpp
    src/hle_sysmem.cpp
    src/hle_io.cpp
    src/hle_display.cpp
    src/hle_audio.cpp
    src/crash_handler.cpp
    src/vfpu.cpp
    src/ge.cpp
    src/image_io.cpp
    src/gpu/texture.cpp
    src/gpu/soft_renderer.cpp
)
if(WIN32)
    target_link_libraries(ff7cc_runtime PRIVATE dbghelp)
endif()
add_library(ff7cc::runtime ALIAS ff7cc_runtime)
target_include_directories(ff7cc_runtime PUBLIC include PRIVATE src)
target_link_libraries(ff7cc_runtime PUBLIC ff7cc::mips PRIVATE ff7cc_stb)
ff7cc_target_defaults(ff7cc_runtime)

# The game executable needs code generated from the user's own BOOT.BIN (ccrecomp -> generated/).
set(FF7CC_GENERATED_DIR "${PROJECT_SOURCE_DIR}/generated" CACHE PATH "ccrecomp output directory")
if(FF7CC_BUILD_GAME AND EXISTS "${FF7CC_GENERATED_DIR}/funcs.h")
    file(GLOB _ff7cc_generated_sources CONFIGURE_DEPENDS "${FF7CC_GENERATED_DIR}/*.c")
    # Semantics the translation relies on: wrapping integer arithmetic, no type-based alias analysis.
    if(MSVC)
        set(_ff7cc_generated_flags /w /clang:-fwrapv /clang:-fno-strict-aliasing)
    else()
        set(_ff7cc_generated_flags -w -fwrapv -fno-strict-aliasing -fexceptions)
    endif()
    set_source_files_properties(${_ff7cc_generated_sources} PROPERTIES COMPILE_OPTIONS "${_ff7cc_generated_flags}")

    add_executable(ff7cc src/main.cpp src/platform/display_vulkan.cpp ${_ff7cc_generated_sources})
    target_include_directories(ff7cc PRIVATE "${FF7CC_GENERATED_DIR}")
    target_link_libraries(ff7cc PRIVATE ff7cc::runtime ff7cc::formats SDL3::SDL3-static ff7cc_vulkan)
    ff7cc_target_defaults(ff7cc)
else()
    message(STATUS "FF7CC: no generated code in ${FF7CC_GENERATED_DIR} (run ccrecomp to create it)")
endif()
