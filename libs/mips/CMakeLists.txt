add_library(psprecore_mips STATIC
    src/decode.cpp
    src/format.cpp
)
add_library(psprecore::mips ALIAS psprecore_mips)

target_include_directories(psprecore_mips PUBLIC include)
target_link_libraries(psprecore_mips PUBLIC nx::foundation)
psprecore_target_defaults(psprecore_mips)
