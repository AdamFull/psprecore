add_executable(ccrecomp main.cpp)
target_link_libraries(ccrecomp PRIVATE ff7cc::recomp)
ff7cc_target_defaults(ccrecomp)
