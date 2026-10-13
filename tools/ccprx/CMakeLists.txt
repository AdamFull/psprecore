add_executable(ccprx main.cpp)
target_link_libraries(ccprx PRIVATE ff7cc::analysis)
ff7cc_target_defaults(ccprx)
