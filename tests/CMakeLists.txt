find_package(Catch2 3 CONFIG REQUIRED)
include(Catch)

# ---------------------------------------------------------------------------
# Fixture plugins.
#
# Each one is a specific way a plugin can be wrong. They live outside the
# directory the application scans, so the app never tries to load them, and
# they are built with the same helper real theories use -- which means this
# suite also proves that helper works.
# ---------------------------------------------------------------------------
set(MARKETSCAN_TEST_PLUGIN_DIR "${CMAKE_BINARY_DIR}/test_plugins")

marketscan_add_theory(test_plugin_bad_abi
    SOURCES plugins/bad_abi.cpp
    OUTPUT_DIRECTORY "${MARKETSCAN_TEST_PLUGIN_DIR}")
marketscan_add_theory(test_plugin_null_create
    SOURCES plugins/null_create.cpp
    OUTPUT_DIRECTORY "${MARKETSCAN_TEST_PLUGIN_DIR}")
marketscan_add_theory(test_plugin_empty_id
    SOURCES plugins/empty_id.cpp
    OUTPUT_DIRECTORY "${MARKETSCAN_TEST_PLUGIN_DIR}")
marketscan_add_theory(test_plugin_no_exports
    SOURCES plugins/no_exports.cpp
    OUTPUT_DIRECTORY "${MARKETSCAN_TEST_PLUGIN_DIR}")


# Every test binary is registered with ctest through catch_discover_tests so a
# failing assertion names the exact test case rather than just the executable.
function(marketscan_add_test name)
    add_executable(${name} ${ARGN})
    target_link_libraries(${name} PRIVATE
        marketscan::core
        marketscan::app
        marketscan::flags
        Catch2::Catch2WithMain)
    # Absolute paths, rather than depending on the working directory ctest
    # happens to launch the binary in.
    target_compile_definitions(${name} PRIVATE
        MARKETSCAN_TEST_DATA_DIR="${PROJECT_SOURCE_DIR}/data/samples"
        MARKETSCAN_THEORY_DIR="${MARKETSCAN_RUNTIME_DIR}/theories"
        MARKETSCAN_TEST_PLUGIN_DIR="${MARKETSCAN_TEST_PLUGIN_DIR}")
    set_target_properties(${name} PROPERTIES FOLDER "marketscan/tests")
    # The plugin tests load real DLLs, so those must exist before ctest runs.
    add_dependencies(${name}
        test_plugin_bad_abi
        test_plugin_null_create
        test_plugin_empty_id
        test_plugin_no_exports)
    if(TARGET buy_and_hold)
        add_dependencies(${name} buy_and_hold)
    endif()

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
marketscan_add_test(test_plugin_loader test_plugin_loader.cpp)
marketscan_add_test(test_metrics     test_metrics.cpp)
marketscan_add_test(test_session     test_session.cpp)
