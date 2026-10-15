add_executable(psptex main.cpp)
target_link_libraries(psptex PRIVATE psprecore::formats psprecore_stb)
psprecore_target_defaults(psptex)
