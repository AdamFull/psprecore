add_executable(psprecomp main.cpp)
target_link_libraries(psprecomp PRIVATE psprecore::recomp)
psprecore_target_defaults(psprecomp)
