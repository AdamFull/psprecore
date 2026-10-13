add_executable(ff7cc_tests
    test_main.cpp
    pkg_tests.cpp
    atel_tests.cpp
    atel_vm_tests.cpp
    atel_asm_tests.cpp
    prx_tests.cpp
    mips_tests.cpp
)
target_link_libraries(ff7cc_tests PRIVATE ff7cc::formats ff7cc::mips)
ff7cc_target_defaults(ff7cc_tests)

add_test(NAME ff7cc_tests COMMAND ff7cc_tests)
