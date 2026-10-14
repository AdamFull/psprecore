add_executable(psprecore_tests
    test_main.cpp
    prx_tests.cpp
    mips_tests.cpp
    audio_ring_tests.cpp
    foundation_tests.cpp
    reverb_tests.cpp
    ${PSPRECORE_DIR}/runtime/src/audio/reverb.cpp
)
target_link_libraries(psprecore_tests PRIVATE psprecore::formats psprecore::mips nx::foundation)
target_include_directories(psprecore_tests PRIVATE ${PSPRECORE_DIR}/runtime/src)  # header-only pieces of the runtime
psprecore_target_defaults(psprecore_tests)

add_test(NAME psprecore_tests COMMAND psprecore_tests)
