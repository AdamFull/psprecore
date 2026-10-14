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
    src/gpu/button_icons.cpp
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

# The front end (ff7cc/platform/frontend.hpp): window, audio, renderers, pause menu. A game's main() runs it;
# games build their executable with ff7cc_add_game (cmake/FF7CCGame.cmake).
if(FF7CC_BUILD_GAME)
    add_library(ff7cc_frontend STATIC
        src/frontend.cpp
        src/platform/display_vulkan.cpp
        src/platform/audio_miniaudio.cpp
        src/platform/gpu_vulkan.cpp
        src/platform/sampling_profiler.cpp
        src/platform/overlay.cpp
        src/platform/settings.cpp
        src/platform/touch_controls.cpp
        src/platform/button_glyphs.cpp
    )
    add_library(ff7cc::frontend ALIAS ff7cc_frontend)
    target_include_directories(ff7cc_frontend PRIVATE src)
    ff7cc_add_shader(ff7cc_frontend ${CMAKE_CURRENT_SOURCE_DIR}/shaders/ge.slang vertex_main vertex ge_vert)
    ff7cc_add_shader(ff7cc_frontend ${CMAKE_CURRENT_SOURCE_DIR}/shaders/ge.slang fragment_main fragment ge_frag)
    ff7cc_add_shader(ff7cc_frontend ${CMAKE_CURRENT_SOURCE_DIR}/shaders/copy.slang vertex_main vertex copy_vert)
    ff7cc_add_shader(ff7cc_frontend ${CMAKE_CURRENT_SOURCE_DIR}/shaders/copy.slang fragment_main fragment copy_frag)
    target_link_libraries(ff7cc_frontend PUBLIC ff7cc::runtime ff7cc::formats SDL3::SDL3-static ff7cc_vulkan ff7cc_imgui
                                               ff7cc_miniaudio)
    if(ANDROID)
        target_link_libraries(ff7cc_frontend PUBLIC android log)
    endif()
    ff7cc_target_defaults(ff7cc_frontend)
    # Global operator new/delete (mimalloc) belong in the executable itself, not in a library the linker may skip.
    set(FF7CC_ALLOCATOR_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/src/platform/allocator.cpp" CACHE INTERNAL "")
endif()
