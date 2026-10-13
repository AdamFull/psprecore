add_library(ff7cc_recomp STATIC
    src/generator.cpp
)
add_library(ff7cc::recomp ALIAS ff7cc_recomp)

target_include_directories(ff7cc_recomp PUBLIC include)
target_link_libraries(ff7cc_recomp PUBLIC ff7cc::analysis)
ff7cc_target_defaults(ff7cc_recomp)
