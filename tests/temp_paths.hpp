#ifndef MARKETSCAN_TESTS_TEMP_PATHS_HPP
#define MARKETSCAN_TESTS_TEMP_PATHS_HPP

// A temporary path no other test process can be using.
//
// catch_discover_tests runs every test case as its own process, and ctest runs
// those in parallel. A name built from a per-process counter therefore starts
// at 1 in every process, and two cases writing "marketscan_test_1.csv" at the
// same moment read each other's files: "a close-only file is a valid price
// series" failed with "contains no header row" because a sibling had just
// truncated the file it was reading. The process id is what makes the name
// unique across processes; the counter keeps it unique within one.

#include <filesystem>
#include <string>
#include <string_view>

#ifdef _WIN32
#    include <process.h>
#else
#    include <unistd.h>
#endif

namespace marketscan::test {

[[nodiscard]] inline std::filesystem::path unique_temp_path(std::string_view prefix,
                                                            std::string_view suffix = {}) {
    static int counter = 0;
#ifdef _WIN32
    const int pid = _getpid();
#else
    const int pid = static_cast<int>(getpid());
#endif
    return std::filesystem::temp_directory_path() /
           (std::string(prefix) + std::to_string(pid) + "_" + std::to_string(++counter) +
            std::string(suffix));
}

}  // namespace marketscan::test

#endif  // MARKETSCAN_TESTS_TEMP_PATHS_HPP
