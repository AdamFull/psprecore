# psprecore_add_game(<target>
#     GENERATED_DIR <dir>     psprecomp output for the game's own BOOT.BIN (funcs.h and the *.c files)
#     [OVERRIDES_DIR <dir>]   hand-written replacements for recompiled functions (GAME_FUNC, psprecore/game/game.hpp)
#     SOURCES <files...>)     the game's main() (runs psprecore::platform::run_game) and its other code
#
# Builds the game: an executable, or on Android libmain.so, which SDL's Java activity loads and calls SDL_main in.
# Does nothing (with a message) while GENERATED_DIR holds no generated code yet.
function(psprecore_add_game target)
    cmake_parse_arguments(PARSE_ARGV 1 arg "" "GENERATED_DIR;OVERRIDES_DIR" "SOURCES")
    if(NOT EXISTS "${arg_GENERATED_DIR}/funcs.h")
        message(STATUS "${target}: no generated code in ${arg_GENERATED_DIR} (run psprecomp to create it)")
        return()
    endif()
    file(GLOB generated CONFIGURE_DEPENDS "${arg_GENERATED_DIR}/*.c")
    # Semantics the translation relies on: wrapping integer arithmetic, no type-based alias analysis.
    if(MSVC)
        set(generated_flags /w /clang:-fwrapv /clang:-fno-strict-aliasing)
    else()
        set(generated_flags -w -fwrapv -fno-strict-aliasing -fexceptions)
    endif()
    set_source_files_properties(${generated} PROPERTIES COMPILE_OPTIONS "${generated_flags}")

    # psprecomp must have been run with --overrides <dir> so the generated originals are renamed to f_XXXXXXXX_orig.
    set(overrides "")
    if(arg_OVERRIDES_DIR)
        file(GLOB_RECURSE overrides CONFIGURE_DEPENDS "${arg_OVERRIDES_DIR}/*.c" "${arg_OVERRIDES_DIR}/*.cpp")
        file(READ "${arg_GENERATED_DIR}/funcs.h" funcs_h)
        foreach(src IN LISTS overrides)
            file(STRINGS "${src}" lines REGEX "GAME_FUNC[(] *[0-9A-Fa-f]+")
            foreach(line IN LISTS lines)
                string(REGEX MATCH "GAME_FUNC[(] *([0-9A-Fa-f]+)" _ "${line}")
                if(NOT funcs_h MATCHES "f_${CMAKE_MATCH_1}_orig")
                    # Stale generated code: build without the overrides (so psprecomp itself can still be built)
                    # until it is regenerated.
                    message(WARNING "${arg_GENERATED_DIR} predates the replacement of f_${CMAKE_MATCH_1} in ${src}; "
                                    "the overrides are left out of this build. Regenerate: psprecomp <BOOT.BIN> "
                                    "\"${arg_GENERATED_DIR}\" --overrides \"${arg_OVERRIDES_DIR}\"")
                    set(overrides "")
                    break()
                endif()
            endforeach()
        endforeach()
    endif()

    set(sources ${arg_SOURCES} ${PSPRECORE_ALLOCATOR_SOURCE} ${generated} ${overrides})
    if(ANDROID)
        add_library(${target} SHARED ${sources})
        set_target_properties(${target} PROPERTIES OUTPUT_NAME main)
    else()
        add_executable(${target} ${sources})
    endif()
    target_include_directories(${target} PRIVATE "${arg_GENERATED_DIR}")
    target_link_libraries(${target} PRIVATE psprecore::frontend)
    psprecore_target_defaults(${target})
endfunction()
