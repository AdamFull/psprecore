add_library(ff7cc_mips STATIC
    src/decode.cpp
    src/format.cpp
)
add_library(ff7cc::mips ALIAS ff7cc_mips)

target_include_directories(ff7cc_mips PUBLIC include)
target_link_libraries(ff7cc_mips PUBLIC nx::foundation)
ff7cc_target_defaults(ff7cc_mips)
