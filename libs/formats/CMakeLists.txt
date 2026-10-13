add_library(ff7cc_formats STATIC
    src/io/file.cpp
    src/formats/pkg.cpp
)
add_library(ff7cc::formats ALIAS ff7cc_formats)

target_include_directories(ff7cc_formats PUBLIC include)
ff7cc_target_defaults(ff7cc_formats)
