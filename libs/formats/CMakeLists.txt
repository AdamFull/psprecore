add_library(ff7cc_formats STATIC
    src/io/file.cpp
    src/formats/pkg.cpp
    src/formats/atel.cpp
    src/formats/atel_vm.cpp
    src/formats/atel_disasm.cpp
    src/formats/atel_asm.cpp
)
add_library(ff7cc::formats ALIAS ff7cc_formats)

target_include_directories(ff7cc_formats PUBLIC include)
ff7cc_target_defaults(ff7cc_formats)
