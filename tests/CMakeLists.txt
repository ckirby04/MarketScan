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

# The ABI v1 migration promise, as something that runs. Declares version 1 by
# hand and fills exactly the nine slots v1 had; the host must keep loading it
# and must never deliver the tenth callback. Source rather than a committed DLL,
# because a binary nobody can rebuild is a fixture nobody can audit.
marketscan_add_theory(test_plugin_v1_reference
    SOURCES plugins/v1_reference.cpp
    OUTPUT_DIRECTORY "${MARKETSCAN_TEST_PLUGIN_DIR}")

# Its twin at the current ABI. Same body, different declared version, so any
# difference the tests measure is attributable to the version branch and to
# nothing else.
marketscan_add_theory(test_plugin_v2_reference
    SOURCES plugins/v2_reference.cpp
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
        MARKETSCAN_RUNTIME_DIR="${MARKETSCAN_RUNTIME_DIR}"
        MARKETSCAN_TEST_PLUGIN_DIR="${MARKETSCAN_TEST_PLUGIN_DIR}"
        MARKETSCAN_TEMPLATE_DLL="${CMAKE_BINARY_DIR}/theory_template/my_theory.dll"
        MARKETSCAN_PARITY_THEORY_DIR="${CMAKE_BINARY_DIR}/parity_theory")
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
marketscan_add_test(test_universe    test_universe.cpp)
marketscan_add_test(test_identity_probe test_identity_probe.cpp)
marketscan_add_test(test_determinism test_determinism.cpp)
marketscan_add_test(test_fill_model  test_fill_model.cpp)
marketscan_add_test(test_ledger      test_ledger.cpp)
marketscan_add_test(test_execution   test_execution.cpp)
marketscan_add_test(test_plugin_loader test_plugin_loader.cpp)
marketscan_add_test(test_metrics     test_metrics.cpp)
marketscan_add_test(test_session     test_session.cpp)
marketscan_add_test(test_view_modes  test_view_modes.cpp)
marketscan_add_test(test_sessions    test_sessions.cpp)
marketscan_add_test(test_hot_path    test_hot_path.cpp)

# The sizing rule, cross-tested against research/sizing.py through one committed
# grid of cases. docs/SIZING.md is the specification both implement.
marketscan_add_test(test_sizing      test_sizing.cpp)

# Paper only, structurally. Scans the built binaries, which is the half a
# source grep cannot do; scripts/check_no_live_endpoints.py does the tree.
marketscan_add_test(test_paper_only  test_paper_only.cpp)

# The live seam's offline half. The parser, the monotonic gate, the recorder
# round trip and the calendar, none of which touch a network: every message in
# these is hand-written to the vendor's documented shape with invented prices.
# Part 0 of Session F2: a refusal and a never-sent order stopped sharing one
# sentinel. Header-only IStrategy, instantiated directly -- no DLL, no engine.
marketscan_add_test(test_submit_outcome test_submit_outcome.cpp)

marketscan_add_test(test_stream_parser test_stream_parser.cpp)
marketscan_add_test(test_calendar      test_calendar.cpp)

# The live broker's order lifecycle, driven through the same apply_update the
# socket pump calls. No socket is opened.
marketscan_add_test(test_live_broker   test_live_broker.cpp)

# Part 1 of Session F2: a recording carries the venue side, and a replay hands a
# strategy exactly what the venue handed it. No socket is opened.
marketscan_add_test(test_recorded_broker test_recorded_broker.cpp)

# ABI v2, and the promise the version-tolerant loader was built to keep.
marketscan_add_test(test_abi_v2      test_abi_v2.cpp)
if(TARGET test_plugin_v1_reference)
    add_dependencies(test_abi_v2 test_plugin_v1_reference)
endif()
if(TARGET test_plugin_v2_reference)
    add_dependencies(test_abi_v2 test_plugin_v2_reference)
endif()

# M9. Replays the committed fixture through the engine and compares the equity
# curve to the one research/parity.py produced from the same file, so it needs
# the decile_parity theory to exist in the scanned directory.
marketscan_add_test(test_parity      test_parity.cpp)
if(TARGET decile_parity)
    add_dependencies(test_parity decile_parity)
endif()

# ---------------------------------------------------------------------------
# The negative control.
#
# "All green" is only information if red is reachable. This binary contains one
# deliberately false assertion, and the test passes only when CTest finds the
# failure in its output -- so the suite's pass condition includes Catch2 having
# evaluated a false expression and reported it.
#
# PASS_REGULAR_EXPRESSION rather than WILL_FAIL, deliberately. WILL_FAIL only
# checks that the process exited non-zero, which a crash on startup also does;
# matching the reported failure proves assertions specifically still work.
# Setting PASS_REGULAR_EXPRESSION makes CTest ignore the exit code, which is what
# lets a binary that is supposed to fail be a test that is supposed to pass.
#
# Registered by hand rather than through marketscan_add_test, because
# catch_discover_tests would register the deliberate failure as an ordinary test
# case and turn the whole suite red.
# ---------------------------------------------------------------------------
add_executable(negative_control negative_control.cpp)
target_link_libraries(negative_control PRIVATE marketscan::flags Catch2::Catch2WithMain)
set_target_properties(negative_control PROPERTIES FOLDER "tests")

add_test(NAME "the harness can still fail" COMMAND negative_control)
set_tests_properties("the harness can still fail" PROPERTIES
    PASS_REGULAR_EXPRESSION "NEGATIVE CONTROL: this failure is deliberate"
    LABELS "unit")
