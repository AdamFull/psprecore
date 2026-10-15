cmake_minimum_required(VERSION 3.25)

# The recompiler and runtime for PSP games: MIPS (Allegrex) decoding and recompilation to C, the HLE runtime
# (kernel, system libraries, GE with Vulkan and software renderers, audio, video) and its front end. Built on its
# own (tools and tests) or by a game repository (add_subdirectory + psprecore_add_game).
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

set(PSPRECORE_DIR "${CMAKE_CURRENT_SOURCE_DIR}" CACHE INTERNAL "psprecore source directory")

# Host tools (recompiler, PRX inspector) are not built for Android.
if(ANDROID)
    set(_psprecore_host_default OFF)
else()
    set(_psprecore_host_default ON)
endif()
option(PSPRECORE_BUILD_TOOLS "Build host-side tools" ${_psprecore_host_default})
option(PSPRECORE_BUILD_TESTS "Build unit tests (no game data required)" ${_psprecore_host_default})
option(PSPRECORE_BUILD_GAME "Build the front end for games (needs SDL3/ImGui/miniaudio, fetched at configure time)" ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(PsprecoreCompilerOptions)
include(PsprecoreStb)
include(PsprecoreDav1d)
if(PSPRECORE_BUILD_GAME)
    include(PsprecoreDependencies)
    include(PsprecoreGame)
endif()

# nx-foundation (github.com/AdamFull/nx-foundation, submodule in libs/nx-foundation), unless the including project
# provides it. Its own third-party code is in submodules too (git submodule update --init --recursive); Tracy is not
# needed (the profiler stays off).
if(NOT TARGET nx::foundation)
    set(NX_FOUNDATION_TRACY OFF CACHE BOOL "" FORCE)
    set(NX_FOUNDATION_TESTS OFF CACHE BOOL "" FORCE)
    # Games are shared libraries on Android (libmain.so, loaded by the Java activity with dlopen), where mimalloc's
    # default initial-exec thread-local storage fails to load ("TLS symbol ... using IE access model").
    if(ANDROID)
        set(MI_LOCAL_DYNAMIC_TLS ON CACHE BOOL "" FORCE)
    endif()
    add_subdirectory(libs/nx-foundation EXCLUDE_FROM_ALL)
endif()
add_subdirectory(libs/formats)
add_subdirectory(libs/mips)
add_subdirectory(libs/analysis)
add_subdirectory(libs/recomp)
add_subdirectory(runtime)

if(PSPRECORE_BUILD_TOOLS)
    add_subdirectory(tools/psprx)
    add_subdirectory(tools/psprecomp)
    add_subdirectory(tools/psptex)
endif()

if(PSPRECORE_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
