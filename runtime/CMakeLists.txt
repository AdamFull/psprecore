# Runtime support for recompiled code (header-only helpers + placeholder services for now).
add_library(ff7cc_runtime STATIC src/recomp_runtime.c)
add_library(ff7cc::runtime ALIAS ff7cc_runtime)
target_include_directories(ff7cc_runtime PUBLIC include)

# Generated code (ccrecomp output). Only built when the user has generated it from their own BOOT.BIN.
set(FF7CC_GENERATED_DIR "${PROJECT_SOURCE_DIR}/generated" CACHE PATH "ccrecomp output directory")
if(EXISTS "${FF7CC_GENERATED_DIR}/funcs.h")
    file(GLOB _ff7cc_generated_sources CONFIGURE_DEPENDS "${FF7CC_GENERATED_DIR}/*.c")
    add_library(ff7cc_generated STATIC ${_ff7cc_generated_sources})
    target_include_directories(ff7cc_generated PUBLIC "${FF7CC_GENERATED_DIR}")
    target_link_libraries(ff7cc_generated PUBLIC ff7cc::runtime)
    # Semantics the translation relies on: wrapping integer arithmetic, no type-based alias analysis.
    if(MSVC)
        target_compile_options(ff7cc_generated PRIVATE /w /clang:-fwrapv /clang:-fno-strict-aliasing)
    else()
        target_compile_options(ff7cc_generated PRIVATE -w -fwrapv -fno-strict-aliasing)
    endif()

    add_executable(ff7cc_recomp_smoke src/smoke_main.c)
    target_link_libraries(ff7cc_recomp_smoke PRIVATE ff7cc_generated)
else()
    message(STATUS "FF7CC: no generated code in ${FF7CC_GENERATED_DIR} (run ccrecomp to create it)")
endif()
