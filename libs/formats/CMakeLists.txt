add_library(ff7cc_formats STATIC
    src/io/file.cpp
    src/formats/pkg.cpp
    src/formats/atel.cpp
    src/formats/atel_vm.cpp
    src/formats/atel_disasm.cpp
    src/formats/atel_asm.cpp
    src/formats/elf.cpp
    src/formats/prx.cpp
    src/formats/atrac.cpp
    src/formats/mbd.cpp
    src/formats/psmf.cpp
)
add_library(ff7cc::formats ALIAS ff7cc_formats)

target_include_directories(ff7cc_formats PUBLIC include)
target_link_libraries(ff7cc_formats PUBLIC nx::foundation)
ff7cc_target_defaults(ff7cc_formats)
