cmake_minimum_required(VERSION 3.25)

# The recompiler and runtime for PSP games: MIPS (Allegrex) decoding and recompilation to C, the HLE runtime
# (kernel, system libraries, GE with Vulkan and software renderers, audio, video) and its front end. Built on its
# own (tools and tests) or by a game repository (add_subdirectory + ff7cc_add_game).
project(psprecore
    VERSION 0.1.0
    DESCRIPTION "Static recompilation of PSP games to native code"
    LANGUAGES C CXX)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(FF7CC_CORE_DIR "${CMAKE_CURRENT_SOURCE_DIR}" CACHE INTERNAL "psprecore source directory")

# Host tools (recompiler, PRX inspector) are not built for Android.
if(ANDROID)
    set(_ff7cc_host_default OFF)
else()
    set(_ff7cc_host_default ON)
endif()
option(FF7CC_BUILD_TOOLS "Build host-side tools" ${_ff7cc_host_default})
option(FF7CC_BUILD_TESTS "Build unit tests (no game data required)" ${_ff7cc_host_default})
option(FF7CC_BUILD_GAME "Build the front end for games (needs SDL3/ImGui/miniaudio, fetched at configure time)" ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(FF7CCCompilerOptions)
include(FF7CCStb)
include(FF7CCDav1d)
if(FF7CC_BUILD_GAME)
    include(FF7CCDependencies)
    include(FF7CCGame)
endif()

# nx-foundation (github.com/AdamFull/nx-foundation): libs/foundation, unless the including project provides it.
if(NOT TARGET nx::foundation)
    add_subdirectory(libs/foundation)
endif()
add_subdirectory(libs/formats)
add_subdirectory(libs/mips)
add_subdirectory(libs/analysis)
add_subdirectory(libs/recomp)
add_subdirectory(runtime)

if(FF7CC_BUILD_TOOLS)
    add_subdirectory(tools/ccprx)
    add_subdirectory(tools/ccrecomp)
endif()

if(FF7CC_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
