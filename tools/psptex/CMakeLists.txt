add_executable(psptex main.cpp)

# Block compression for baked textures (host tool only): bc7enc (BC7, desktop GPUs; MIT / public domain) and Arm's
# astc-encoder (ASTC, mobile GPUs; Apache-2.0). Only bc7enc's codec translation unit is built (its CMake file adds a
# demo and changes global flags); astcenc as the portable scalar library, so results do not depend on the host ISA.
include(FetchContent)
FetchContent_Declare(bc7enc
    GIT_REPOSITORY https://github.com/richgel999/bc7enc.git
    GIT_TAG f66c2e489b07138f2673a2fb3d27c1aa1d565c48
    SOURCE_SUBDIR do-not-configure)
set(ASTCENC_CLI OFF CACHE BOOL "" FORCE)
set(ASTCENC_SHAREDLIB OFF CACHE BOOL "" FORCE)
set(ASTCENC_DECOMPRESSOR OFF CACHE BOOL "" FORCE)
set(ASTCENC_DIAGNOSTICS OFF CACHE BOOL "" FORCE)
set(ASTCENC_UNITTEST OFF CACHE BOOL "" FORCE)
set(ASTCENC_WERROR OFF CACHE BOOL "" FORCE)
set(ASTCENC_ISA_NATIVE OFF CACHE BOOL "" FORCE)
set(ASTCENC_ISA_AVX2 OFF CACHE BOOL "" FORCE)
set(ASTCENC_ISA_SSE41 OFF CACHE BOOL "" FORCE)
set(ASTCENC_ISA_SSE2 OFF CACHE BOOL "" FORCE)
set(ASTCENC_ISA_NEON OFF CACHE BOOL "" FORCE)
set(ASTCENC_ISA_NONE ON CACHE BOOL "" FORCE)
FetchContent_Declare(astcenc
    GIT_REPOSITORY https://github.com/ARM-software/astc-encoder.git
    GIT_TAG 4316299fb0bf26c6ada2a3b2e7a1b747ac22d98d
    EXCLUDE_FROM_ALL)
FetchContent_MakeAvailable(bc7enc astcenc)

add_library(psprecore_bc7enc STATIC ${bc7enc_SOURCE_DIR}/bc7enc.c)
target_include_directories(psprecore_bc7enc SYSTEM PUBLIC ${bc7enc_SOURCE_DIR})
target_compile_options(psprecore_bc7enc PRIVATE -w)
if(UNIX)
    target_link_libraries(psprecore_bc7enc PRIVATE m)
endif()

target_link_libraries(psptex PRIVATE psprecore::formats psprecore_stb psprecore_bc7enc astcenc-none-static)
psprecore_target_defaults(psptex)
