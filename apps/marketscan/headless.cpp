#include "headless.hpp"

#include <charconv>
#include <cstdio>
#include <cstring>
#include <deque>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>

#include "marketscan/core/engine.hpp"
#include "marketscan/data/csv_feed.hpp"
#include "marketscan/data/synthetic_feed.hpp"
#include "marketscan/metrics/performance.hpp"
#include "marketscan/strategy/plugin_loader.hpp"
#include "marketscan/strategy/strategy_instance.hpp"
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
  --symbol <TICKER>     Ticker to assume for a CSV with no symbol column.
                        Default: SPY
  --bars <n>            Synthetic bars per session. Default: 390
  --sessions <n>        Synthetic sessions. Default: 2
  --seed <n>            Synthetic generator seed. Default: 13835058055282163832
  --cash <dollars>      Starting capital per theory. Default: 100000
  --compare-fills       Also run against the optimistic fill model and print
                        the difference. That difference is what a backtest
                        built on naive fills awards itself for free.
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
    std::ostringstream out;
    out << std::fixed << std::setprecision(2) << (fraction * 100.0) << "%";
    return out.str();
}

void print_plugin_table(const PluginLoader& loader) {
    if (loader.plugins().empty()) {
        std::cout << "No theories found. Looked in:\n";
        for (const std::filesystem::path& root : loader.roots()) {
            std::cout << "  " << root.string() << "\n";
        }
        std::cout << "\nBuild one with cmake and drop the DLL there. "
                     "See theories/README.md.\n";
        return;
    }

    std::cout << "Theories\n";
    std::cout << "--------\n";
    for (const PluginInfo& info : loader.plugins()) {
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

/// Builds the feed the run replays. Throws with a readable message on bad
/// input rather than falling back to synthetic data, which would silently
/// answer a question nobody asked.
[[nodiscard]] std::unique_ptr<IMarketDataFeed> make_feed(const HeadlessOptions& options,
                                                         SymbolTable& symbols) {
    if (!options.data_file.empty()) {
        CsvFeedOptions csv;
        csv.default_symbol = options.default_symbol;
        // Bar data carries no spread, and the fill model needs one to cross.
        // Without this a market order would fill at the close, which is the
        // optimistic model wearing a disguise.
        csv.synthesize_quotes = true;
        return std::make_unique<CsvFeed>(options.data_file, symbols, csv);
    }

    SyntheticFeedConfig config;
    config.instruments = {SyntheticInstrument{options.default_symbol, 500.0, 0.05, 0.15, 90'000.0}};
    config.bars_per_session = options.bars;
    config.sessions = options.sessions;
    config.seed = options.seed;
    Timestamp start = 0;
    (void)time::from_iso8601("2024-03-18T13:30:00Z", start);
    config.start = start;
    return std::make_unique<SyntheticFeed>(config, symbols);
}

struct RunResult {
    std::string id;
    std::string display_name;
    Money starting_cash;
    Money final_equity;
    std::size_t trades = 0;
    std::uint64_t rejected = 0;
    Money commission;
    Money slippage;
    bool invariant_held = true;

    std::vector<EquityPoint> equity_curve;
    metrics::PerformanceSummary summary;
};

/// Formats a statistic, or a dash when it is undefined.
///
/// Every metric returns NaN rather than zero when it cannot be computed, and
/// printing that as "0.00" would put a number in a column that nobody could
/// tell apart from a result.
[[nodiscard]] std::string format_metric(double value, int precision = 2) {
    if (!metrics::is_defined(value)) {
        return "--";
    }
    std::ostringstream out;
    out << std::fixed << std::setprecision(precision) << value;
    return out.str();
}

/// One complete replay with one fill model. Returns a row per theory.
[[nodiscard]] std::vector<RunResult>
replay(const HeadlessOptions& options, PluginLoader& loader, const IFillModel& fill_model) {
    SymbolTable symbols;
    Engine engine;
    engine.add_source(make_feed(options, symbols));

    StrategyRuntimeConfig runtime;
    runtime.starting_cash = Money::from_double(options.starting_cash);

    // Deque rather than vector: the engine holds raw pointers to these, and a
    // vector would invalidate every one of them as it grew.
    std::deque<StrategyInstance> strategies;

    OrderId id_base = 0;
    for (const PluginInfo& info : loader.plugins()) {
        if (!info.loaded()) {
            continue;
        }
        PluginInstance instance = loader.create(info.id);
        if (!instance.valid()) {
            log::error("app", "could not instantiate '" + info.id + "': " + info.error);
            continue;
        }
        id_base += 1'000'000;
        runtime.order_id_base = id_base;
        strategies.emplace_back(info.id,
                                info.display_name,
                                std::move(instance),
                                info.required_symbols,
                                runtime,
                                &fill_model,
                                &engine.market(),
                                &symbols,
                                &engine.clock());
    }

    for (StrategyInstance& strategy : strategies) {
        // Re-resolve now that the feed has interned every ticker it knows.
        strategy.resolve_subscriptions();
        engine.add_sink(&strategy);
        strategy.start();
    }

    engine.run();

    std::vector<RunResult> results;
    for (StrategyInstance& strategy : strategies) {
        strategy.stop();
        const Ledger& ledger = strategy.ledger();

        RunResult row;
        row.id = strategy.id();
        row.display_name = strategy.display_name();
        row.starting_cash = ledger.starting_cash();
        row.final_equity = ledger.equity();
        row.trades = ledger.blotter().size();
        row.rejected = strategy.broker().rejected_count();
        row.commission = ledger.commission_paid();
        row.invariant_held = ledger.check_invariant();
        for (const TradeRecord& record : ledger.blotter()) {
            row.slippage += record.slippage;
        }
        row.equity_curve = ledger.equity_curve();
        results.push_back(std::move(row));
    }

    // The benchmark is whichever theory declares itself one, and buy_and_hold
    // is it by convention rather than by anything in the host. Measuring the
    // others against it is only fair because it went through this same engine,
    // paid this same spread and was charged this same commission.
    const std::vector<EquityPoint>* benchmark = nullptr;
    for (const RunResult& row : results) {
        if (row.id == "buy_and_hold" && row.equity_curve.size() >= 2) {
            benchmark = &row.equity_curve;
            break;
        }
    }

    metrics::Annualisation annualisation;
    annualisation.periods_per_year = 0.0;  // infer it from the curve's spacing
    for (RunResult& row : results) {
        row.summary = benchmark != nullptr
                          ? metrics::summarise(row.equity_curve, *benchmark, annualisation)
                          : metrics::summarise(row.equity_curve, annualisation);
    }
    return results;
}

void print_results(const std::vector<RunResult>& results, std::string_view heading) {
    std::cout << heading << "\n";
    std::cout << std::string(heading.size(), '-') << "\n";
    std::cout << "  " << std::left << std::setw(20) << "theory" << std::right << std::setw(13)
              << "equity" << std::setw(9) << "return" << std::setw(9) << "sharpe" << std::setw(9)
              << "sortino" << std::setw(9) << "max dd" << std::setw(8) << "IR" << std::setw(8)
              << "trades" << std::setw(11) << "slippage" << "\n";

    for (const RunResult& row : results) {
        const double total_return =
            row.starting_cash.is_zero()
                ? 0.0
                : (row.final_equity.to_double() / row.starting_cash.to_double()) - 1.0;
        std::cout << "  " << std::left << std::setw(20) << row.display_name << std::right
                  << std::setw(13) << format_money(row.final_equity) << std::setw(9)
                  << format_percent(total_return) << std::setw(9)
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

    PluginLoader loader(PluginLoader::default_roots(executable_dir));
    loader.rescan();
    print_plugin_table(loader);

    if (options.list_only) {
        return 0;
    }
    if (loader.loaded_count() == 0) {
        std::cout << "Nothing to run.\n";
        return 0;
    }

    try {
        const SpreadCrossingFillModel realistic;
        const std::vector<RunResult> honest = replay(options, loader, realistic);
        print_results(honest, "Results (spread-crossing fills)");

        if (options.compare_fill_models) {
            const OptimisticFillModel optimistic;
            const std::vector<RunResult> wishful = replay(options, loader, optimistic);
            print_results(wishful, "Results (OPTIMISTIC fills -- not realistic)");

            std::cout << "Cost of assuming optimistic fills\n";
            std::cout << "---------------------------------\n";
            for (std::size_t i = 0; i < honest.size() && i < wishful.size(); ++i) {
                const Money gap = wishful[i].final_equity - honest[i].final_equity;
                std::cout << "  " << std::left << std::setw(22) << honest[i].display_name
                          << std::right << std::setw(14) << format_money(gap)
                          << "  of imaginary profit\n";
            }
            std::cout << "\n";
        }

        bool all_sound = true;
        for (const RunResult& row : honest) {
            all_sound = all_sound && row.invariant_held;
        }
        return all_sound ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << "marketscan: " << error.what() << "\n";
        return 1;
    }
}

}  // namespace marketscan::app
