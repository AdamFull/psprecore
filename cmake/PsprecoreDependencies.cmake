# Third-party runtime dependencies (see PORT_PLAN.md §0). Pinned versions, fetched once into the build tree.
#   SDL3 (zlib), Dear ImGui (MIT), miniaudio (MIT-0) via FetchContent;
#   Vulkan headers, volk and VMA from the installed Vulkan SDK.

include(FetchContent)
include(PsprecoreShaders)

FetchContent_Declare(SDL3
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    GIT_TAG release-3.2.10
    GIT_SHALLOW TRUE
    EXCLUDE_FROM_ALL)
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)

FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG v1.91.9
    GIT_SHALLOW TRUE)

FetchContent_Declare(miniaudio
    GIT_REPOSITORY https://github.com/mackron/miniaudio.git
    GIT_TAG 0.11.22
    GIT_SHALLOW TRUE
    SOURCE_SUBDIR do-not-configure)  # header only; skip its CMake project

FetchContent_MakeAvailable(SDL3 imgui miniaudio)

# ---- Vulkan (headers + volk loader + VMA allocator from the Vulkan SDK) ----
if(ANDROID)
    # The NDK ships Vulkan headers but not volk or VMA; both are portable, so take them (and matching headers)
    # from the host Vulkan SDK. The loader is libvulkan.so on the device, opened by volk at run time.
    if(NOT DEFINED ENV{VULKAN_SDK})
        message(FATAL_ERROR "Android builds need the VULKAN_SDK environment variable (volk and VMA sources)")
    endif()
    file(TO_CMAKE_PATH "$ENV{VULKAN_SDK}" _psprecore_vulkan_sdk)
    set(_psprecore_vulkan_include "${_psprecore_vulkan_sdk}/Include")
    if(NOT EXISTS "${_psprecore_vulkan_include}/Volk/volk.c")
        set(_psprecore_vulkan_include "${_psprecore_vulkan_sdk}/include")
    endif()
else()
    find_package(Vulkan REQUIRED)
    set(_psprecore_vulkan_include "${Vulkan_INCLUDE_DIRS}")
endif()
add_library(psprecore_vulkan STATIC "${_psprecore_vulkan_include}/Volk/volk.c")
target_include_directories(psprecore_vulkan SYSTEM PUBLIC "${_psprecore_vulkan_include}" "${_psprecore_vulkan_include}/Volk"
                                                  "${_psprecore_vulkan_include}/vma")
target_compile_definitions(psprecore_vulkan PUBLIC VK_NO_PROTOTYPES)
if(WIN32)
    target_compile_definitions(psprecore_vulkan PUBLIC VK_USE_PLATFORM_WIN32_KHR)
elseif(ANDROID)
    target_compile_definitions(psprecore_vulkan PUBLIC VK_USE_PLATFORM_ANDROID_KHR)
    target_link_libraries(psprecore_vulkan PUBLIC ${CMAKE_DL_LIBS})
endif()

# ---- Dear ImGui with the SDL3 + Vulkan backends ----
add_library(psprecore_imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp)
target_include_directories(psprecore_imgui SYSTEM PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
target_compile_definitions(psprecore_imgui PUBLIC IMGUI_IMPL_VULKAN_USE_VOLK)
target_link_libraries(psprecore_imgui PUBLIC SDL3::SDL3-static psprecore_vulkan)

# ---- miniaudio (single header) ----
add_library(psprecore_miniaudio INTERFACE)
target_include_directories(psprecore_miniaudio SYSTEM INTERFACE ${miniaudio_SOURCE_DIR})
