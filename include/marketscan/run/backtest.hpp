#ifndef MARKETSCAN_RUN_BACKTEST_HPP
#define MARKETSCAN_RUN_BACKTEST_HPP

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "marketscan/exec/ledger.hpp"
#include "marketscan/metrics/correlation.hpp"
#include "marketscan/metrics/performance.hpp"
#include "marketscan/strategy/plugin_loader.hpp"
#include "marketscan/util/latency_histogram.hpp"

/// Running a backtest from start to finish, in one call.
///
/// This is the shape the command line wants, the shape the Python bindings
/// want, and the shape a batch job wants. The GUI needs something different --
/// it has to be able to stop halfway and be asked what is going on -- so it
/// drives the engine itself through Session. Both go through the same Engine,
/// the same Ledger and the same fill models; what differs is who is holding
/// the handle, which is exactly the distinction this whole project is built
/// around.
namespace marketscan::run {

struct BacktestConfig {
    /// A CSV to replay. Empty means generate synthetic data instead.
    std::filesystem::path data_file;

    /// Ticker to assume when a CSV has no symbol column, and the instrument
    /// the synthetic generator invents.
    std::string symbol = "SPY";

    std::uint32_t bars_per_session = 390;
    std::uint32_t sessions = 2;
    std::uint64_t seed = 0xC0FF'EE00'1234'5678ULL;

    Money starting_cash = Money::from_dollars(100'000);
    Duration equity_sample_interval = time::kMinute;

    /// Directories searched recursively for theory DLLs.
    std::vector<std::filesystem::path> theory_roots;

    /// Which theory everything else is measured against.
    std::string benchmark_id = "buy_and_hold";

    /// Use the optimistic fill model instead of the realistic one.
    ///
    /// The results are not a result. It is here so the two can be run over the
    /// same data and subtracted, which is the only way to see what a naive
    /// fill assumption is worth.
    bool optimistic_fills = false;

    /// Bar data carries no spread, and the spread-crossing model needs one to
    /// cross. Without this a market order against a bars-only file fills at
    /// the close, which is the optimistic model wearing a disguise.
    bool synthesize_quotes_for_bars = true;

    double spread_bps = 2.0;
};

/// What one theory did.
struct TheoryResult {
    std::string id;
    std::string display_name;
    std::string author;
    bool is_benchmark = false;

    Money starting_cash;
    Money final_equity;
    Money cash;
    Money realized_pnl;
    Money unrealized_pnl;
    Money commission;
    Money slippage;

    std::size_t trades = 0;
    std::uint64_t rejected = 0;

    metrics::PerformanceSummary summary;

    /// Seconds since the Unix epoch, for plotting.
    std::vector<double> times;
    std::vector<double> equity_curve;
    std::vector<double> returns;
    std::vector<double> drawdown;

    std::vector<TradeRecord> blotter;
    std::vector<Position> positions;

    /// Named scalars the theory published, in first-published order.
    std::vector<std::pair<std::string, double>> published;

    LatencyHistogram latency;

    /// True when the ledger's accounting identity held all the way through.
    /// False means value was created or destroyed, which is a bug.
    bool invariant_held = true;
};

struct BacktestResult {
    std::vector<TheoryResult> theories;

    /// Every plugin found, loaded or not, with its error if it was refused.
    std::vector<PluginInfo> plugins;

    std::vector<std::string> symbols;

    std::uint64_t events = 0;
    Timestamp first_ts = 0;
    Timestamp last_ts = 0;

    std::string fill_model;
    bool fill_model_realistic = true;

    metrics::CorrelationMatrix correlation;

    /// Set when the run could not start -- a missing file, an unreadable
    /// dataset. The theories list is empty in that case.
    std::string error;

    [[nodiscard]] bool ok() const noexcept { return error.empty(); }

    /// The benchmark's result, or null if none was loaded.
    [[nodiscard]] const TheoryResult* benchmark() const noexcept;

    [[nodiscard]] const TheoryResult* find(std::string_view id) const noexcept;
};

/// Loads every theory it can find, replays the data through all of them at
/// once, and returns what happened.
///
/// Every theory sees the same events in the same order and owns its own
/// account. None of them can see any other.
[[nodiscard]] BacktestResult run_backtest(const BacktestConfig& config);

}  // namespace marketscan::run

#endif  // MARKETSCAN_RUN_BACKTEST_HPP
