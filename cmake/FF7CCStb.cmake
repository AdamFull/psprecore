# stb single-header libraries (public domain / MIT): image read/write for screenshots and the asset pipeline.
# Needed by the runtime and the tools, so fetched regardless of FF7CC_BUILD_GAME. stb has no release tags; pin a commit.

include(FetchContent)

FetchContent_Declare(stb
    GIT_REPOSITORY https://github.com/nothings/stb.git
    GIT_TAG 2c980bb59875b0d32144a71867fbdebb2f77cd20
    SOURCE_SUBDIR do-not-configure)  # header only
FetchContent_MakeAvailable(stb)

add_library(ff7cc_stb INTERFACE)
target_include_directories(ff7cc_stb SYSTEM INTERFACE ${stb_SOURCE_DIR})
