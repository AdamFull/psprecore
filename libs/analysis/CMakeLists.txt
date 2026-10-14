add_library(psprecore_analysis STATIC
    src/program.cpp
)
add_library(psprecore::analysis ALIAS psprecore_analysis)

target_include_directories(psprecore_analysis PUBLIC include)
target_link_libraries(psprecore_analysis PUBLIC psprecore::formats psprecore::mips)
psprecore_target_defaults(psprecore_analysis)
