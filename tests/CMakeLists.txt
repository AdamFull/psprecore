add_executable(ff7cc_tests
    test_main.cpp
    prx_tests.cpp
    mips_tests.cpp
    audio_ring_tests.cpp
    foundation_tests.cpp
    reverb_tests.cpp
    ${FF7CC_CORE_DIR}/runtime/src/audio/reverb.cpp
)
target_link_libraries(ff7cc_tests PRIVATE ff7cc::formats ff7cc::mips nx::foundation)
target_include_directories(ff7cc_tests PRIVATE ${FF7CC_CORE_DIR}/runtime/src)  # header-only pieces of the runtime
ff7cc_target_defaults(ff7cc_tests)

add_test(NAME ff7cc_tests COMMAND ff7cc_tests)
