cmake_minimum_required(VERSION 3.25)

project(FF7CCRECOMP
    VERSION 0.1.0
    DESCRIPTION "Native port of Crisis Core: Final Fantasy VII (PSP) via static recompilation"
    LANGUAGES C CXX)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Host tools (archive tools, recompiler, asset cooker) are not built for Android.
if(ANDROID)
    set(_ff7cc_host_default OFF)
else()
    set(_ff7cc_host_default ON)
endif()
option(FF7CC_BUILD_TOOLS "Build host-side tools" ${_ff7cc_host_default})
option(FF7CC_BUILD_TESTS "Build unit tests (no game data required)" ${_ff7cc_host_default})

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(FF7CCCompilerOptions)

include(FF7CCStb)

option(FF7CC_BUILD_GAME "Build the game executable (needs SDL3/ImGui/miniaudio, fetched at configure time)" ON)
if(FF7CC_BUILD_GAME)
    include(FF7CCDependencies)
endif()

add_subdirectory(libs/formats)
add_subdirectory(libs/mips)
add_subdirectory(libs/analysis)
add_subdirectory(libs/recomp)
add_subdirectory(runtime)

if(FF7CC_BUILD_TOOLS)
    add_subdirectory(tools/ccpkg)
    add_subdirectory(tools/ccatel)
    add_subdirectory(tools/ccprx)
    add_subdirectory(tools/ccrecomp)
endif()

if(FF7CC_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
