#ifndef MARKETSCAN_APPS_LIVE_CLI_HPP
#define MARKETSCAN_APPS_LIVE_CLI_HPP

/*
 * Argument parsing shared by ms_live's subcommands, and the one string that
 * identifies this program to the venue.
 *
 * WHY THIS IS A HEADER RATHER THAN SEVEN COPIES
 *
 * Because it was seven copies. ms_live grew a file per subcommand, and each new
 * file brought its own `option_value`, its own `has_flag`, and its own
 * `kUserAgent`:
 *
 *   option_value()  ms_live.cpp, equivalence_cmd.cpp, dryrun_cmd.cpp, orders_cmd.cpp
 *   has_flag()      equivalence_cmd.cpp, positions_cmd.cpp
 *   split_commas()  ms_live.cpp, equivalence_cmd.cpp
 *   kUserAgent      six files, all of them
 *
 * Every copy was compared before being merged, not assumed identical, because
 * two option parsers that had drifted apart would look exactly like two that
 * had not.
 *
 * kUserAgent IS DIFFERENT FROM A CONSTANT THAT HAPPENS TO REPEAT
 *
 * It says who is calling, to a venue that is entitled to a straight answer. All
 * six copies had to agree and there is exactly one correct value, so six
 * definitions were six chances to disagree about the identity of one program.
 * Contrast `kReceiveTimeoutMs` in the live layer, which is deliberately left
 * duplicated because two sockets may legitimately want different timeouts --
 * see include/marketscan/live/detail.hpp. Repetition is worth collapsing when
 * divergence would be a bug and worth keeping when divergence would be a
 * decision.
 *
 * ARGUMENT CONVENTIONS, SUCH AS THEY ARE
 *
 * `option_value` returns a pointer into argv or nullptr, so a missing option and
 * an option given an empty value are distinguishable; callers rely on that.
 * `has_flag` scans the whole of argv, `option_value` stops one short because it
 * needs a following token. Neither validates: ms_live is a research tool driven
 * by a person reading its usage text, and a wrong flag fails loudly downstream
 * rather than being guessed at.
 */

#include <cstring>
#include <string>
#include <vector>

namespace marketscan::live {

/// How ms_live identifies itself to the venue. One program, one answer.
constexpr const char* kUserAgent = "MarketScan/0.1 (research tool)";

/// The token after `name`, or nullptr when `name` is absent or last.
[[nodiscard]] inline const char* option_value(int argc, char** argv, const char* name) {
    for (int i = 0; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], name) == 0) {
            return argv[i + 1];
        }
    }
    return nullptr;
}

/// Whether `name` appears at all.
[[nodiscard]] inline bool has_flag(int argc, char** argv, const char* name) {
    for (int i = 0; i < argc; ++i) {
        if (std::strcmp(argv[i], name) == 0) {
            return true;
        }
    }
    return false;
}

/// Splits on commas, dropping empty fields, so "AAPL,,SPY," is two symbols.
[[nodiscard]] inline std::vector<std::string> split_commas(const std::string& text) {
    std::vector<std::string> out;
    std::string current;
    for (const char c : text) {
        if (c == ',') {
            if (!current.empty()) {
                out.push_back(current);
            }
            current.clear();
        } else {
            current.push_back(c);
        }
    }
    if (!current.empty()) {
        out.push_back(current);
    }
    return out;
}

}  // namespace marketscan::live

#endif  // MARKETSCAN_APPS_LIVE_CLI_HPP
