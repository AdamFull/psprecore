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
    src/hle_mpeg.cpp
    src/hle_atrac.cpp
    src/hle_savedata.cpp
    src/crash_handler.cpp
    src/vfpu.cpp
    src/ge.cpp
    src/image_io.cpp
    src/gpu/texture.cpp
    src/gpu/soft_renderer.cpp
    src/gpu/vertex_pipeline.cpp
    src/audio/sas.cpp
    src/audio/reverb.cpp
    src/audio/vorbis.cpp
    src/media/av1_video.cpp
)
if(WIN32)
    target_link_libraries(ff7cc_runtime PRIVATE dbghelp)
endif()
add_library(ff7cc::runtime ALIAS ff7cc_runtime)
target_include_directories(ff7cc_runtime PUBLIC include PRIVATE src)
target_link_libraries(ff7cc_runtime PUBLIC ff7cc::mips ff7cc::formats nx::foundation PRIVATE ff7cc_stb ff7cc_dav1d)
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

    # Hand-written replacements for recompiled functions (game/, GAME_FUNC). ccrecomp must have been run with
    # --overrides game so the generated originals are renamed to f_XXXXXXXX_orig.
    file(GLOB_RECURSE _ff7cc_game_overrides CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/game/*.c" "${PROJECT_SOURCE_DIR}/game/*.cpp")
    file(READ "${FF7CC_GENERATED_DIR}/funcs.h" _ff7cc_funcs_h)
    foreach(_src IN LISTS _ff7cc_game_overrides)
        file(STRINGS "${_src}" _lines REGEX "GAME_FUNC[(] *[0-9A-Fa-f]+")
        foreach(_line IN LISTS _lines)
            string(REGEX MATCH "GAME_FUNC[(] *([0-9A-Fa-f]+)" _ "${_line}")
            if(NOT _ff7cc_funcs_h MATCHES "f_${CMAKE_MATCH_1}_orig")
                # Stale generated code: build without game/ (so ccrecomp itself can still be built) until it is
                # regenerated.
                message(WARNING "generated/ predates the replacement of f_${CMAKE_MATCH_1} in ${_src}; game/ is left "
                                "out of this build. Regenerate: ccrecomp <BOOT.BIN> \"${FF7CC_GENERATED_DIR}\" --overrides game")
                set(_ff7cc_game_overrides "")
                break()
            endif()
        endforeach()
    endforeach()

    set(_ff7cc_game_sources src/main.cpp src/platform/display_vulkan.cpp src/platform/audio_miniaudio.cpp
        src/platform/gpu_vulkan.cpp src/platform/allocator.cpp src/platform/overlay.cpp src/platform/settings.cpp src/platform/touch_controls.cpp ${_ff7cc_generated_sources} ${_ff7cc_game_overrides})
    if(ANDROID)
        # SDL's Java activity loads libmain.so and calls SDL_main (see android/).
        add_library(ff7cc SHARED ${_ff7cc_game_sources})
        set_target_properties(ff7cc PROPERTIES OUTPUT_NAME main)
        target_link_libraries(ff7cc PRIVATE android log)
    else()
        add_executable(ff7cc ${_ff7cc_game_sources})
    endif()
    target_include_directories(ff7cc PRIVATE "${FF7CC_GENERATED_DIR}" src)
    ff7cc_add_shader(ff7cc ${CMAKE_CURRENT_SOURCE_DIR}/shaders/ge.slang vertex_main vertex ge_vert)
    ff7cc_add_shader(ff7cc ${CMAKE_CURRENT_SOURCE_DIR}/shaders/ge.slang fragment_main fragment ge_frag)
    target_link_libraries(ff7cc PRIVATE ff7cc::runtime ff7cc::formats SDL3::SDL3-static ff7cc_vulkan ff7cc_imgui ff7cc_miniaudio)
    ff7cc_target_defaults(ff7cc)
else()
    message(STATUS "FF7CC: no generated code in ${FF7CC_GENERATED_DIR} (run ccrecomp to create it)")
endif()
