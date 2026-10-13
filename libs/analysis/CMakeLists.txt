add_library(ff7cc_analysis STATIC
    src/program.cpp
)
add_library(ff7cc::analysis ALIAS ff7cc_analysis)

target_include_directories(ff7cc_analysis PUBLIC include)
target_link_libraries(ff7cc_analysis PUBLIC ff7cc::formats ff7cc::mips)
ff7cc_target_defaults(ff7cc_analysis)
