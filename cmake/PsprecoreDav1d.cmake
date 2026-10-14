# dav1d (BSD-2-Clause) AV1 decoder for cooked movies. Upstream builds with Meson; we compile its portable C
# sources directly (8-bit only, no assembly). That is plenty for 480x272 movies; the assembly can be enabled
# later through Meson if HD replacement movies need it.

include(FetchContent)

FetchContent_Declare(dav1d
    GIT_REPOSITORY https://code.videolan.org/videolan/dav1d.git
    GIT_TAG 1.5.1
    GIT_SHALLOW TRUE
    SOURCE_SUBDIR do-not-configure)
FetchContent_MakeAvailable(dav1d)

set(_dav1d ${dav1d_SOURCE_DIR})
set(_dav1d_gen ${CMAKE_CURRENT_BINARY_DIR}/dav1d_config)

if(WIN32)
    set(_dav1d_posix 0)
    set(_dav1d_win 1)
else()
    set(_dav1d_posix 1)
    set(_dav1d_win 0)
endif()
file(CONFIGURE OUTPUT ${_dav1d_gen}/config.h CONTENT [[
#pragma once
#define CONFIG_8BPC 1
#define CONFIG_16BPC 0
#define CONFIG_LOG 1
#define CONFIG_MACOS_KPERF 0
#define HAVE_ASM 0
#define TRIM_DSP_FUNCTIONS 0
#define ARCH_AARCH64 0
#define ARCH_ARM 0
#define ARCH_X86 0
#define ARCH_X86_64 0
#define ARCH_X86_32 0
#define ARCH_PPC64LE 0
#define ARCH_RISCV 0
#define ARCH_RV32 0
#define ARCH_RV64 0
#define ARCH_LOONGARCH 0
#define ARCH_LOONGARCH32 0
#define ARCH_LOONGARCH64 0
#define ENDIANNESS_BIG 0
#define HAVE_C11_GENERIC 1
#define HAVE_CLOCK_GETTIME @_dav1d_posix@
#define HAVE_POSIX_MEMALIGN @_dav1d_posix@
#define HAVE_MEMALIGN 0
#define HAVE_ALIGNED_ALLOC 0
#define HAVE_DLSYM 0
#define HAVE_SYS_TYPES_H 1
#define HAVE_UNISTD_H @_dav1d_posix@
#define HAVE_IO_H @_dav1d_win@
#define HAVE_PTHREAD_NP_H 0
#define HAVE_GETAUXVAL 0
#define HAVE_ELF_AUX_INFO 0
#define HAVE_PTHREAD_GETAFFINITY_NP 0
#define HAVE_PTHREAD_SETAFFINITY_NP 0
#define HAVE_PTHREAD_SETNAME_NP 0
#define HAVE_PTHREAD_SET_NAME_NP 0
#if defined(_WIN32)
#define _WIN32_WINNT 0x0601
#define UNICODE 1
#define _UNICODE 1
#define _CRT_DECLARE_NONSTDC_NAMES 1
#endif
]] @ONLY)
file(WRITE ${_dav1d_gen}/vcs_version.h "#define DAV1D_VERSION \"1.5.1\"\n")

set(_dav1d_sources
    cdf.c cpu.c ctx.c data.c decode.c dequant_tables.c getbits.c intra_edge.c itx_1d.c lf_mask.c lib.c log.c
    mem.c msac.c obu.c pal.c picture.c qm.c ref.c refmvs.c scan.c tables.c thread_task.c warpmv.c wedge.c)
set(_dav1d_tmpl_sources
    cdef_apply_tmpl.c cdef_tmpl.c fg_apply_tmpl.c filmgrain_tmpl.c ipred_prepare_tmpl.c ipred_tmpl.c itx_tmpl.c
    lf_apply_tmpl.c loopfilter_tmpl.c looprestoration_tmpl.c lr_apply_tmpl.c mc_tmpl.c recon_tmpl.c)
list(TRANSFORM _dav1d_sources PREPEND ${_dav1d}/src/)
list(TRANSFORM _dav1d_tmpl_sources PREPEND ${_dav1d}/src/)
if(WIN32)
    list(APPEND _dav1d_sources ${_dav1d}/src/win32/thread.c)
endif()

add_library(psprecore_dav1d STATIC ${_dav1d_sources} ${_dav1d_tmpl_sources})
set_source_files_properties(${_dav1d_tmpl_sources} PROPERTIES COMPILE_DEFINITIONS BITDEPTH=8)
target_include_directories(psprecore_dav1d PRIVATE ${_dav1d_gen} ${_dav1d} ${_dav1d}/include/dav1d)
target_include_directories(psprecore_dav1d SYSTEM PUBLIC ${_dav1d}/include)
set_target_properties(psprecore_dav1d PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
if(MSVC)
    target_compile_options(psprecore_dav1d PRIVATE /w)
else()
    target_compile_options(psprecore_dav1d PRIVATE -w)
    target_compile_definitions(psprecore_dav1d PRIVATE _GNU_SOURCE _FILE_OFFSET_BITS=64)
    find_package(Threads REQUIRED)
    target_link_libraries(psprecore_dav1d PUBLIC Threads::Threads)
endif()
