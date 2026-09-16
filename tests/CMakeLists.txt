find_package(Catch2 3 CONFIG REQUIRED)
include(Catch)

# Every test binary is registered with ctest through catch_discover_tests so a
# failing assertion names the exact test case rather than just the executable.
function(marketscan_add_test name)
    add_executable(${name} ${ARGN})
    target_link_libraries(${name} PRIVATE
        marketscan::core
        marketscan::flags
        Catch2::Catch2WithMain)
    # Tests that read the committed samples get an absolute path, rather than
    # depending on the working directory ctest happens to launch them in.
    target_compile_definitions(${name} PRIVATE
        MARKETSCAN_TEST_DATA_DIR="${PROJECT_SOURCE_DIR}/data/samples")
    set_target_properties(${name} PROPERTIES FOLDER "marketscan/tests")
    catch_discover_tests(${name}
        WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
        PROPERTIES LABELS "unit")
endfunction()

marketscan_add_test(test_smoke       test_smoke.cpp)
marketscan_add_test(test_money       test_money.cpp)
marketscan_add_test(test_time        test_time.cpp)
marketscan_add_test(test_ring_buffer test_ring_buffer.cpp)
marketscan_add_test(test_engine      test_engine.cpp)
marketscan_add_test(test_feeds       test_feeds.cpp)
marketscan_add_test(test_determinism test_determinism.cpp)
marketscan_add_test(test_fill_model  test_fill_model.cpp)
marketscan_add_test(test_ledger      test_ledger.cpp)
