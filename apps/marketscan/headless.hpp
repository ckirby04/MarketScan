#ifndef MARKETSCAN_APP_HEADLESS_HPP
#define MARKETSCAN_APP_HEADLESS_HPP

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace marketscan::app {

/// What the headless runner was asked to do.
struct HeadlessOptions {
    /// A CSV to replay. Empty means generate synthetic data instead.
    std::filesystem::path data_file;

    /// Ticker to assume when a CSV has no symbol column.
    std::string default_symbol = "SPY";

    /// Synthetic generator settings, used only when data_file is empty.
    std::uint32_t bars = 390;
    std::uint32_t sessions = 2;
    std::uint64_t seed = 0xC0FF'EE00'1234'5678ULL;

    double starting_cash = 100'000.0;

    /// List what is in theories/ and stop.
    bool list_only = false;

    /// Also run every theory against the optimistic fill model, and print the
    /// difference. This is the number that shows what a naive backtest is
    /// quietly awarding itself.
    bool compare_fill_models = false;

    bool verbose = false;
};

/// Parses argv. Returns false when the program should stop -- either because
/// help was asked for, or because an argument was wrong (in which case a
/// message has already been printed).
bool parse_arguments(int argc, char** argv, HeadlessOptions& out, int& exit_code);

/// Loads every theory it can find, replays the data through all of them, and
/// prints a summary. Returns a process exit code.
int run_headless(const HeadlessOptions& options, const std::filesystem::path& executable_dir);

/// Directory containing the running executable. Theories are looked for
/// beside it, which is what makes a built tree and an installed copy behave
/// the same way.
[[nodiscard]] std::filesystem::path executable_directory();

}  // namespace marketscan::app

#endif  // MARKETSCAN_APP_HEADLESS_HPP
