add_executable(ff7cc_tests
    test_main.cpp
    pkg_tests.cpp
    atel_tests.cpp
    atel_vm_tests.cpp
    atel_asm_tests.cpp
    prx_tests.cpp
    mips_tests.cpp
    audio_ring_tests.cpp
)
target_link_libraries(ff7cc_tests PRIVATE ff7cc::formats ff7cc::mips)
target_include_directories(ff7cc_tests PRIVATE ${PROJECT_SOURCE_DIR}/runtime/src)  # header-only pieces of the runtime
ff7cc_target_defaults(ff7cc_tests)

add_test(NAME ff7cc_tests COMMAND ff7cc_tests)
