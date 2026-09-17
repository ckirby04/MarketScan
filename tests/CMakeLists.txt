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

# Not a broken plugin: the one that exercises the host's random stream, which
# neither reference theory touches.
marketscan_add_theory(test_plugin_dice_roller
    SOURCES plugins/dice_roller.cpp
    OUTPUT_DIRECTORY "${MARKETSCAN_TEST_PLUGIN_DIR}")

# Session boundaries are detected inside the plugin's own shim layer, so the
# only honest way to test them is across a real DLL boundary.
marketscan_add_theory(test_plugin_session_watcher
    SOURCES plugins/session_watcher.cpp
    OUTPUT_DIRECTORY "${MARKETSCAN_TEST_PLUGIN_DIR}")
marketscan_add_theory(test_plugin_cross_section
    SOURCES plugins/cross_section_probe.cpp
    OUTPUT_DIRECTORY "${MARKETSCAN_TEST_PLUGIN_DIR}")

# A strategy that has to cope with an order that has not filled yet, which is
# unreachable against a simulator that answers instantly.
marketscan_add_theory(test_plugin_order_lifecycle
    SOURCES plugins/order_lifecycle.cpp
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
        MARKETSCAN_TEST_PLUGIN_DIR="${MARKETSCAN_TEST_PLUGIN_DIR}"
        MARKETSCAN_TEMPLATE_DLL="${CMAKE_BINARY_DIR}/theory_template/my_theory.dll")
    set_target_properties(${name} PROPERTIES FOLDER "marketscan/tests")
    # The plugin tests load real DLLs, so those must exist before ctest runs.
    add_dependencies(${name}
        test_plugin_bad_abi
        test_plugin_null_create
        test_plugin_empty_id
        test_plugin_no_exports
        test_plugin_dice_roller
        test_plugin_session_watcher
        test_plugin_cross_section
        test_plugin_order_lifecycle)
    if(TARGET buy_and_hold)
        add_dependencies(${name} buy_and_hold)
    endif()
    if(TARGET my_theory)
        add_dependencies(${name} my_theory)
    endif()

    catch_discover_tests(${name}
        WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
        PROPERTIES LABELS "unit")
endfunction()

marketscan_add_test(test_smoke       test_smoke.cpp)
marketscan_add_test(test_money       test_money.cpp)
marketscan_add_test(test_time        test_time.cpp)
marketscan_add_test(test_session_calendar test_session_calendar.cpp)
marketscan_add_test(test_ring_buffer test_ring_buffer.cpp)
marketscan_add_test(test_engine      test_engine.cpp)
marketscan_add_test(test_feeds       test_feeds.cpp)
marketscan_add_test(test_daily       test_daily.cpp)
marketscan_add_test(test_determinism test_determinism.cpp)
marketscan_add_test(test_fill_model  test_fill_model.cpp)
marketscan_add_test(test_ledger      test_ledger.cpp)
marketscan_add_test(test_execution   test_execution.cpp)
marketscan_add_test(test_plugin_loader test_plugin_loader.cpp)
marketscan_add_test(test_metrics     test_metrics.cpp)
marketscan_add_test(test_session     test_session.cpp)
marketscan_add_test(test_sessions    test_sessions.cpp)
marketscan_add_test(test_hot_path    test_hot_path.cpp)
