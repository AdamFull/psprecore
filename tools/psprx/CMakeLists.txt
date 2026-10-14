add_executable(psprx main.cpp)
target_link_libraries(psprx PRIVATE psprecore::analysis)
psprecore_target_defaults(psprx)
