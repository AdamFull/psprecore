# nx-foundation (github.com/AdamFull/nx-foundation, a git submodule in core/foundation so its own include paths,
# "core/foundation/...", work from here). Its build expects the targets of the engine it comes from; this file
# provides them:
#   nx::mimalloc         mimalloc, fetched here
#   Tracy::TracyClient   empty: the profiler stays off (NX_PROFILE_ENABLED 0, the macros compile to nothing)
#   nx::compile_options  empty: its definitions and warning options are set on the target below
#   nx::minizip          not used: the ZIP device is replaced by zip_device_stub.cpp

include(FetchContent)
FetchContent_Declare(mimalloc
    GIT_REPOSITORY https://github.com/microsoft/mimalloc.git
    GIT_TAG v2.4.1
    GIT_SHALLOW TRUE
    EXCLUDE_FROM_ALL)
set(MI_OVERRIDE      OFF CACHE BOOL "" FORCE)
set(MI_BUILD_SHARED  OFF CACHE BOOL "" FORCE)
set(MI_BUILD_STATIC  ON  CACHE BOOL "" FORCE)
set(MI_BUILD_OBJECT  OFF CACHE BOOL "" FORCE)
set(MI_BUILD_TESTS   OFF CACHE BOOL "" FORCE)
set(MI_NO_OPT_ARCH   ON  CACHE BOOL "" FORCE)
set(MI_DEBUG_FULL    OFF CACHE BOOL "" FORCE)
set(MI_SHOW_ERRORS   OFF CACHE BOOL "" FORCE)
# Games are shared libraries on Android (libmain.so, loaded by the Java activity with dlopen), where mimalloc's
# default initial-exec thread-local storage fails to load ("TLS symbol ... using IE access model").
if(ANDROID)
    set(MI_LOCAL_DYNAMIC_TLS ON CACHE BOOL "" FORCE)
endif()
FetchContent_MakeAvailable(mimalloc)
# mimalloc.h is not our code: keep its warnings out of our builds.
get_target_property(_psprecore_mi_includes mimalloc-static INTERFACE_INCLUDE_DIRECTORIES)
set_target_properties(mimalloc-static PROPERTIES INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${_psprecore_mi_includes}")
add_library(nx::mimalloc ALIAS mimalloc-static)

add_library(psprecore_nx_tracy INTERFACE)
add_library(Tracy::TracyClient ALIAS psprecore_nx_tracy)
add_library(psprecore_nx_compile_options INTERFACE)
add_library(nx::compile_options ALIAS psprecore_nx_compile_options)
add_library(psprecore_nx_minizip INTERFACE)
add_library(nx::minizip ALIAS psprecore_nx_minizip)

set(NX_ENGINE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")  # its include root, <root>/engine, is replaced below
add_subdirectory(core/foundation)

set_target_properties(nx_foundation PROPERTIES INTERFACE_INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}"
                                               INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}")
get_target_property(_psprecore_nx_sources nx_foundation SOURCES)
list(FILTER _psprecore_nx_sources EXCLUDE REGEX "vfs/zip_device\\.cpp$")
set_target_properties(nx_foundation PROPERTIES SOURCES "${_psprecore_nx_sources}")
target_sources(nx_foundation PRIVATE zip_device_stub.cpp)

# What the engine's compile options gave it (consumers need the definitions too); its code is kept as upstream.
target_compile_features(nx_foundation PUBLIC cxx_std_20)
target_compile_definitions(nx_foundation PUBLIC
    $<$<CONFIG:Debug>:NX_BUILD_DEBUG=1>
    $<$<NOT:$<CONFIG:Debug>>:NX_BUILD_RELEASE=1>
    $<$<PLATFORM_ID:Windows>:NOMINMAX;WIN32_LEAN_AND_MEAN>
    $<$<BOOL:${MSVC}>:_CRT_SECURE_NO_WARNINGS;_CRT_NONSTDC_NO_WARNINGS>)
if(MSVC)
    target_compile_options(nx_foundation PRIVATE /W4 /utf-8 /EHsc -Wno-unused-parameter
                                                 -Wno-missing-designated-field-initializers)
else()
    target_compile_options(nx_foundation PRIVATE -Wall -Wextra -Wno-unused-parameter
                                                 -Wno-missing-designated-field-initializers)
endif()
