#include "headless.hpp"

#include <charconv>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <sstream>

#include "marketscan/data/bar_store.hpp"
#include "marketscan/data/identity_probe.hpp"
#include "marketscan/data/security_master.hpp"
#include "marketscan/data/universe.hpp"
#include "marketscan/metrics/performance.hpp"
#include "marketscan/run/backtest.hpp"
#include "marketscan/strategy/plugin_loader.hpp"
#include "marketscan/util/logging.hpp"
#include "marketscan/util/version.hpp"

#if defined(_WIN32)
#    include <windows.h>
#endif

namespace marketscan::app {
namespace {

void print_usage() {
    std::cout <<
        R"(marketscan -- headless runner

Discovers every theory in the theories/ directory beside this executable,
replays market data through all of them at once, and prints what happened.

  --data <file.csv>     Replay a CSV instead of generating synthetic data.
  --daily <dir>         Replay a directory of cached daily bars (.msb). Populate
                        one with: python scripts/fetch_stooq.py --symbols ...
  --cache-info <dir>    Report what a daily cache holds, then stop.
  --universe <file>     Membership history (date,TICKER,TICKER,... snapshots).
                        Turns on the survivorship guard: a strategy is never
                        shown a security that was not a member that day.
                        Populate with: python scripts/fetch_sp500.py
  --build-universe <f>  Load a membership history, write docs/IDENTITY_DECISIONS.md
                        and a summary, then stop.
  --prices <dir>        With --build-universe: measure every membership gap
                        against cached daily bars, so identity is observed
                        rather than inferred from a threshold. Overrides are
                        then written from measurements only.
  --no-universe         Replay every symbol in the data regardless of index
                        membership. Correct for intraday and for one
                        instrument; survivorship-unsafe for anything else.
  --symbol <TICKER>     Ticker to assume for a CSV with no symbol column.
                        Default: SPY
  --bars <n>            Synthetic bars per session. Default: 390
  --sessions <n>        Synthetic sessions. Default: 2
  --seed <n>            Synthetic generator seed.
  --cash <dollars>      Starting capital per theory. Default: 100000
  --compare-fills       Also run against the optimistic fill model and print
                        the difference. That difference is what a backtest
                        built on naive fills awards itself for free.
  --latency <ms>        Simulated delay between sending an order and its first
                        chance to fill. Default 0. An order in flight is still
                        exposed: it fills at the price when it arrives, not the
                        price when it was sent.
  --latency-jitter <ms> Uniform jitter added to that delay, drawn from the run
                        seed. Deterministic; a replay reproduces it exactly.
  --participation <f>   Largest fraction of the size shown at the touch that one
                        event may fill, e.g. 0.25. Default 0, meaning no cap.
                        With a cap, a large order arrives as several fills.
  --autoplay            Start replaying as soon as the window opens (GUI only).
  --speed <n>           Replay speed multiplier for the GUI. 0 is unthrottled.
  --headless            Print the results to the terminal instead of opening
                        the dashboard. Implied by builds without the GUI.
  --list                List the theories found, then stop.
  --verbose             Debug-level logging.
  -h, --help            This.
)";
}

[[nodiscard]] bool parse_u64(const char* text, std::uint64_t& out) {
    if (text == nullptr) {
        return false;
    }
    const std::size_t length = std::strlen(text);
    const auto [ptr, ec] = std::from_chars(text, text + length, out);
    return ec == std::errc{} && ptr == text + length;
}

[[nodiscard]] bool parse_double(const char* text, double& out) {
    if (text == nullptr) {
        return false;
    }
    const std::size_t length = std::strlen(text);
    const auto [ptr, ec] = std::from_chars(text, text + length, out);
    return ec == std::errc{} && ptr == text + length;
}

[[nodiscard]] std::string format_money(Money value) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(2) << value.to_double();
    return out.str();
}

[[nodiscard]] std::string format_percent(double fraction) {
    if (!metrics::is_defined(fraction)) {
        return "--";
    }
    std::ostringstream out;
    out << std::fixed << std::setprecision(2) << (fraction * 100.0) << "%";
    return out.str();
}

/// A statistic, or a dash. Every metric returns NaN rather than zero when it
/// cannot be computed, and printing that as "0.00" would put a number in a
/// column nobody could tell apart from a result.
[[nodiscard]] std::string format_metric(double value, int precision = 2) {
    if (!metrics::is_defined(value)) {
        return "--";
    }
    std::ostringstream out;
    out << std::fixed << std::setprecision(precision) << value;
    return out.str();
}

void print_plugin_table(const std::vector<PluginInfo>& plugins,
                        const std::vector<std::filesystem::path>& roots) {
    if (plugins.empty()) {
        std::cout << "No theories found. Looked in:\n";
        for (const std::filesystem::path& root : roots) {
            std::cout << "  " << root.string() << "\n";
        }
        std::cout << "\nBuild one and drop the DLL there. See theories/README.md.\n";
        return;
    }

    std::cout << "Theories\n";
    std::cout << "--------\n";
    for (const PluginInfo& info : plugins) {
        std::cout << "  " << std::left << std::setw(20) << info.id << "  " << std::setw(10)
                  << to_string(info.state);
        if (info.state == PluginState::Loaded) {
            std::cout << "  " << info.display_name;
            if (!info.author.empty()) {
                std::cout << "  (" << info.author << ")";
            }
            if (!info.params.empty()) {
                std::cout << "  [" << info.params.size() << " param(s)]";
            }
        } else if (!info.error.empty()) {
            std::cout << "  " << info.path.filename().string() << ": " << info.error;
        }
        std::cout << "\n";
    }
    std::cout << "\n";
}

/// Loads a membership history, writes the decision report, and says what it
/// found.
///
/// Separated from a replay on purpose. Building the universe is where every
/// identity judgment is made, and those judgments deserve to be looked at
/// before anything is run on top of them rather than scrolling past on the way
/// to a number.
int build_universe(const std::filesystem::path& history_path,
                   const std::filesystem::path& overrides_path,
                   const std::filesystem::path& report_path,
                   const std::filesystem::path& price_cache) {
    SymbolTable symbols;
    SecurityMaster master(symbols);

    const IdentityRules rules = load_overrides(overrides_path);
    const SnapshotMembershipSource source(history_path);
    MembershipHistory history = build_membership(source, master, rules);

    const bool measuring = !price_cache.empty();
    const IdentityProbeConfig probe;
    std::size_t settled = 0;
    std::size_t disagreements = 0;
    if (measuring) {
        measure_identities(history, price_cache, probe);
        for (const IdentityDecision& decision : history.decisions) {
            settled += decision.measured.resolved() ? 1 : 0;
            disagreements += decision.disagrees() ? 1 : 0;
        }
    }

    write_decision_report(report_path, history, master, rules, source.describe(), probe, measuring);

    std::size_t splits = 0;
    std::size_t near_misses = 0;
    for (const IdentityDecision& decision : history.decisions) {
        splits += decision.split ? 1 : 0;
        near_misses += decision.rule == "near miss" ? 1 : 0;
    }

    std::cout << "Universe\n--------\n";
    std::cout << "  source            " << source.describe() << "\n";
    std::cout << "  snapshots         " << source.snapshots() << "\n";
    std::cout << "  securities        " << master.size() << "\n";
    std::cout << "  membership spans  " << history.universe.size() << "\n";
    std::cout << "  covers            " << time::to_iso8601(history.universe.first()).substr(0, 10)
              << " .. ";
    if (history.universe.last() == kForever) {
        std::cout << "current";
    } else {
        std::cout << time::to_iso8601(history.universe.last()).substr(0, 10);
    }
    std::cout << "\n";
    std::cout << "  members today     "
              << history.universe.members_on(history.universe.last() - 1).size() << "\n\n";

    std::cout << "Identity\n--------\n";
    std::cout << "  overrides loaded  " << rules.overrides.size() << "\n";
    std::cout << "  gaps considered   " << history.decisions.size() << "\n";
    std::cout << "  splits made       " << splits << "\n";
    std::cout << "  near misses       " << near_misses << "\n\n";
    if (measuring) {
        std::cout << "Measurement\n-----------\n";
        std::cout << "  prices from       " << price_cache.string() << "\n";
        std::cout << "  settled by data   " << settled << " of " << history.decisions.size()
                  << "\n";
        std::cout << "  disagreements     " << disagreements << "\n";

        if (settled > 0) {
            const std::size_t rows =
                write_measured_overrides(overrides_path, history, probe, source.describe());
            std::cout << "  overrides written " << rows << " (measurements only)\n\n";
        } else {
            // Nothing settled means nothing to write. Overwriting a file that
            // may hold hand-written corrections with a header and no rows would
            // destroy them in order to record an absence.
            std::cout << "  overrides written 0 -- nothing was settled, so "
                      << overrides_path.string() << " is left alone\n\n";
        }
    }

    std::cout << "  Every one of those calls is in " << report_path.string() << ".\n";
    std::cout << "  The threshold is a judgment, not a constant. Corrections go in "
              << overrides_path.string() << ".\n\n";

    if (!history.rejected.empty()) {
        std::cout << "  " << history.rejected.size()
                  << " row(s) the source could not supply; see the report.\n\n";
    }
    return 0;
}

/// What a daily cache holds, without replaying it.
///
/// The cache is binary and therefore not greppable, which is a real cost of
/// the format. This is the answer to it: the question "what have I actually
/// got" should not require writing a program.
int print_cache(const std::filesystem::path& directory) {
    const std::vector<std::filesystem::path> files = bar_store::list(directory);
    if (files.empty()) {
        std::cerr << "marketscan: no .msb files in '" << directory.string() << "'\n";
        return 1;
    }

    std::cout << directory.string() << "\n\n";
    std::cout << "  " << std::left << std::setw(10) << "symbol" << std::right << std::setw(10)
              << "bars" << "  " << std::left << std::setw(12) << "first" << std::setw(12) << "last"
              << "\n";

    std::uint64_t total = 0;
    Timestamp earliest = kForever;
    Timestamp latest = kNever;
    for (const std::filesystem::path& file : files) {
        const bar_store::Summary summary = bar_store::summarise(file);
        total += summary.bars;
        if (summary.bars > 0) {
            earliest = std::min(earliest, summary.first);
            latest = std::max(latest, summary.last);
        }
        std::cout << "  " << std::left << std::setw(10) << summary.ticker << std::right
                  << std::setw(10) << summary.bars << "  " << std::left << std::setw(12)
                  << (summary.bars > 0 ? time::to_iso8601(summary.first).substr(0, 10) : "--")
                  << std::setw(12)
                  << (summary.bars > 0 ? time::to_iso8601(summary.last).substr(0, 10) : "--")
                  << "\n";
    }

    std::cout << "\n  " << files.size() << " symbols, " << total << " bars";
    if (total > 0) {
        std::cout << ", " << time::to_iso8601(earliest).substr(0, 10) << " .. "
                  << time::to_iso8601(latest).substr(0, 10);
    }
    std::cout << "\n\nNothing here is research-grade. See docs/DATA.md.\n";
    return 0;
}

void print_results(const run::BacktestResult& result, std::string_view heading) {
    std::cout << heading << "\n";
    std::cout << std::string(heading.size(), '-') << "\n";
    std::cout << "  " << std::left << std::setw(20) << "theory" << std::right << std::setw(13)
              << "equity" << std::setw(9) << "return" << std::setw(9) << "sharpe" << std::setw(9)
              << "sortino" << std::setw(9) << "max dd" << std::setw(8) << "IR" << std::setw(8)
              << "trades" << std::setw(11) << "slippage" << "\n";

    for (const run::TheoryResult& row : result.theories) {
        std::cout << "  " << std::left << std::setw(20) << row.display_name << std::right
                  << std::setw(13) << format_money(row.final_equity) << std::setw(9)
                  << format_percent(row.summary.total_return) << std::setw(9)
                  << format_metric(row.summary.sharpe) << std::setw(9)
                  << format_metric(row.summary.sortino) << std::setw(9)
                  << format_metric(row.summary.max_drawdown * 100.0) << std::setw(8)
                  << format_metric(row.summary.information_ratio) << std::setw(8) << row.trades
                  << std::setw(11) << format_money(row.slippage) << "\n";
        if (!row.invariant_held) {
            std::cout << "    *** LEDGER INVARIANT VIOLATED -- this is a bug ***\n";
        }
    }
    std::cout << "\n  max dd is a percentage. A dash means the statistic is not "
                 "defined for this run.\n\n";
}

}  // namespace

std::filesystem::path executable_directory() {
#if defined(_WIN32)
    wchar_t buffer[MAX_PATH * 4];
    const DWORD length =
        ::GetModuleFileNameW(nullptr, buffer, static_cast<DWORD>(std::size(buffer)));
    if (length == 0) {
        return std::filesystem::current_path();
    }
    return std::filesystem::path(std::wstring(buffer, length)).parent_path();
#else
    return std::filesystem::current_path();
#endif
}

bool parse_arguments(int argc, char** argv, HeadlessOptions& out, int& exit_code) {
    exit_code = 0;

    const auto need_value = [&](int& i, const char* flag) -> const char* {
        if (i + 1 >= argc) {
            std::cerr << "marketscan: " << flag << " needs a value\n";
            exit_code = 2;
            return nullptr;
        }
        return argv[++i];
    };

    for (int i = 1; i < argc; ++i) {
        const std::string_view argument = argv[i];

        if (argument == "-h" || argument == "--help") {
            print_usage();
            return false;
        }
        if (argument == "--list") {
            out.list_only = true;
        } else if (argument == "--verbose") {
            out.verbose = true;
        } else if (argument == "--compare-fills") {
            out.compare_fill_models = true;
        } else if (argument == "--autoplay") {
            out.autoplay = true;
        } else if (argument == "--speed") {
            const char* value = need_value(i, "--speed");
            if (value == nullptr) {
                return false;
            }
            if (!parse_double(value, out.speed)) {
                std::cerr << "marketscan: '" << value << "' is not a number\n";
                exit_code = 2;
                return false;
            }
        } else if (argument == "--prices") {
            const char* value = need_value(i, "--prices");
            if (value == nullptr) {
                return false;
            }
            out.price_cache = value;
        } else if (argument == "--no-universe") {
            out.no_universe = true;
        } else if (argument == "--universe" || argument == "--build-universe") {
            const char* value = need_value(i, argv[i]);
            if (value == nullptr) {
                return false;
            }
            out.universe_file = value;
            out.build_universe = argument == "--build-universe";
        } else if (argument == "--daily" || argument == "--cache-info") {
            const char* value = need_value(i, argv[i]);
            if (value == nullptr) {
                return false;
            }
            out.daily_dir = value;
            out.inspect_cache = argument == "--cache-info";
        } else if (argument == "--data") {
            const char* value = need_value(i, "--data");
            if (value == nullptr) {
                return false;
            }
            out.data_file = value;
        } else if (argument == "--symbol") {
            const char* value = need_value(i, "--symbol");
            if (value == nullptr) {
                return false;
            }
            out.default_symbol = value;
        } else if (argument == "--bars" || argument == "--sessions" || argument == "--seed") {
            const char* value = need_value(i, argv[i]);
            if (value == nullptr) {
                return false;
            }
            std::uint64_t parsed = 0;
            if (!parse_u64(value, parsed)) {
                std::cerr << "marketscan: '" << value << "' is not a number\n";
                exit_code = 2;
                return false;
            }
            if (argument == "--bars") {
                out.bars = static_cast<std::uint32_t>(parsed);
            } else if (argument == "--sessions") {
                out.sessions = static_cast<std::uint32_t>(parsed);
            } else {
                out.seed = parsed;
            }
        } else if (argument == "--latency" || argument == "--latency-jitter" ||
                   argument == "--participation") {
            const char* value = need_value(i, argv[i]);
            if (value == nullptr) {
                return false;
            }
            double parsed = 0.0;
            if (!parse_double(value, parsed) || parsed < 0.0) {
                std::cerr << "marketscan: '" << value << "' is not a non-negative number\n";
                exit_code = 2;
                return false;
            }
            if (argument == "--latency") {
                out.latency_ms = parsed;
            } else if (argument == "--latency-jitter") {
                out.latency_jitter_ms = parsed;
            } else {
                out.participation = parsed;
            }
        } else if (argument == "--cash") {
            const char* value = need_value(i, "--cash");
            if (value == nullptr) {
                return false;
            }
            if (!parse_double(value, out.starting_cash)) {
                std::cerr << "marketscan: '" << value << "' is not a number\n";
                exit_code = 2;
                return false;
            }
        } else {
            std::cerr << "marketscan: unrecognised argument '" << argument << "'\n";
            std::cerr << "Try --help.\n";
            exit_code = 2;
            return false;
        }
    }
    return true;
}

int run_headless(const HeadlessOptions& options, const std::filesystem::path& executable_dir) {
    log::init(options.verbose ? MS_LOG_DEBUG : MS_LOG_INFO);

    std::cout << "MarketScan " << version::kString << "  (" << version::kBuildType << ", ABI "
              << MS_ABI_VERSION << ")\n\n";

    // The same run_backtest the Python bindings call. One implementation, so a
    // notebook and the command line cannot disagree about what happened.
    if (options.inspect_cache) {
        return print_cache(options.daily_dir);
    }
    if (options.build_universe) {
        // Relative to the working directory rather than to the executable:
        // both of these are repository artefacts that get reviewed and
        // committed, and they belong next to the source rather than beside a
        // build output.
        return build_universe(options.universe_file,
                              "data/security_overrides.csv",
                              "docs/IDENTITY_DECISIONS.md",
                              options.price_cache);
    }

    run::BacktestConfig config;
    config.data_file = options.data_file;
    config.daily_dir = options.daily_dir;
    config.symbol = options.default_symbol;
    config.bars_per_session = options.bars;
    config.sessions = options.sessions;
    config.seed = options.seed;
    config.starting_cash = Money::from_double(options.starting_cash);
    config.theory_roots = PluginLoader::default_roots(executable_dir);
    config.fills.submit_latency =
        static_cast<Duration>(options.latency_ms * static_cast<double>(time::kMillisecond));
    config.fills.latency_jitter =
        static_cast<Duration>(options.latency_jitter_ms * static_cast<double>(time::kMillisecond));
    config.fills.max_participation = options.participation;
    if (!options.no_universe) {
        config.universe_file = options.universe_file;
        config.security_overrides = "data/security_overrides.csv";
    }

    if (options.list_only) {
        PluginLoader loader(config.theory_roots);
        loader.rescan();
        print_plugin_table(loader.plugins(), config.theory_roots);
        return 0;
    }

    const run::BacktestResult honest = run::run_backtest(config);
    print_plugin_table(honest.plugins, config.theory_roots);

    if (!honest.ok()) {
        std::cerr << "marketscan: " << honest.error << "\n";
        return 1;
    }
    if (honest.theories.empty()) {
        std::cout << "Nothing to run.\n";
        return 0;
    }

    if (config.fills.models_latency() || config.fills.models_partial_fills()) {
        // Said out loud, because these numbers are not comparable with the
        // ones in the README and nothing else on the page would say so.
        std::cout << "Execution: ";
        if (config.fills.models_latency()) {
            std::cout << options.latency_ms << "ms latency";
            if (options.latency_jitter_ms > 0.0) {
                std::cout << " +/- " << options.latency_jitter_ms << "ms jitter";
            }
        }
        if (config.fills.models_partial_fills()) {
            if (config.fills.models_latency()) {
                std::cout << ", ";
            }
            std::cout << options.participation << " participation cap";
        }
        std::cout << "\n\n";
    }

    // Said before the numbers, because a cross-sectional result produced
    // without a universe is survivorship-inflated and a reader should not have
    // to remember which kind of run they are looking at.
    if (honest.universe_active) {
        std::cout << "Universe: " << honest.universe_securities << " securities, "
                  << honest.events_withheld << " event(s) withheld from strategies, "
                  << honest.forced_exits << " forced exit(s)\n";
        std::cout << "          " << honest.bars_unidentified
                  << " bar(s) fell outside every membership period and were dropped.\n\n";
    } else if (!honest.theories.empty()) {
        std::cout << "Universe: none. Every symbol in the data is in scope, so a "
                     "cross-sectional\n          result from this run is "
                     "survivorship-inflated. Pass --universe.\n\n";
    }

    print_results(honest, "Results (spread-crossing fills)");

    if (options.compare_fill_models) {
        run::BacktestConfig optimistic_config = config;
        optimistic_config.optimistic_fills = true;
        const run::BacktestResult wishful = run::run_backtest(optimistic_config);

        print_results(wishful, "Results (OPTIMISTIC fills -- not realistic)");

        std::cout << "Cost of assuming optimistic fills\n";
        std::cout << "---------------------------------\n";
        for (const run::TheoryResult& row : honest.theories) {
            const run::TheoryResult* other = wishful.find(row.id);
            if (other == nullptr) {
                continue;
            }
            const Money gap = other->final_equity - row.final_equity;
            std::cout << "  " << std::left << std::setw(22) << row.display_name << std::right
                      << std::setw(14) << format_money(gap) << "  of imaginary profit\n";
        }
        std::cout << "\n";
    }

    bool all_sound = true;
    for (const run::TheoryResult& row : honest.theories) {
        all_sound = all_sound && row.invariant_held;
    }
    return all_sound ? 0 : 1;
}

}  // namespace marketscan::app
