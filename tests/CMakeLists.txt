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
    set_target_properties(${name} PROPERTIES FOLDER "marketscan/tests")
    catch_discover_tests(${name}
        WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
        PROPERTIES LABELS "unit")
endfunction()

marketscan_add_test(test_smoke test_smoke.cpp)
