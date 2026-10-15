# File formats of PSP software (ELF/PRX, ATRAC3, PSMF), the cooked media container, KTX2 textures, and file and
# text helpers.
add_library(psprecore_formats STATIC
    src/io/file.cpp
    src/formats/elf.cpp
    src/formats/prx.cpp
    src/formats/atrac.cpp
    src/formats/psmf.cpp
    src/formats/ktx2.cpp
)
add_library(psprecore::formats ALIAS psprecore_formats)

target_include_directories(psprecore_formats PUBLIC include)
target_link_libraries(psprecore_formats PUBLIC nx::foundation)
psprecore_target_defaults(psprecore_formats)
