add_executable(ccprx main.cpp)
target_link_libraries(ccprx PRIVATE ff7cc::formats ff7cc::mips)
ff7cc_target_defaults(ccprx)
