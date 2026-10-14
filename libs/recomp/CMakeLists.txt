add_library(psprecore_recomp STATIC
    src/generator.cpp
)
add_library(psprecore::recomp ALIAS psprecore_recomp)

target_include_directories(psprecore_recomp PUBLIC include)
target_link_libraries(psprecore_recomp PUBLIC psprecore::analysis)
psprecore_target_defaults(psprecore_recomp)
