#include "marketscan/run/backtest.hpp"

#include <algorithm>
#include <deque>
#include <memory>
#include <span>

#include "marketscan/core/engine.hpp"
#include "marketscan/data/csv_feed.hpp"
#include "marketscan/data/daily_feed.hpp"
#include "marketscan/data/synthetic_feed.hpp"
#include "marketscan/exec/fill_model.hpp"
#include "marketscan/metrics/rolling.hpp"
#include "marketscan/strategy/strategy_instance.hpp"
#include "marketscan/util/logging.hpp"

namespace marketscan::run {
namespace {

constexpr std::size_t kNoIndex = static_cast<std::size_t>(-1);

[[nodiscard]] double to_plot_seconds(Timestamp ts) noexcept {
    return static_cast<double>(ts) / static_cast<double>(time::kSecond);
}

[[nodiscard]] std::unique_ptr<IMarketDataFeed> make_feed(const BacktestConfig& config,
                                                         SymbolTable& symbols) {
    if (!config.daily_dir.empty()) {
        DailyFeedOptions options;
        options.synthesize_quotes = config.synthesize_quotes_for_bars;
        options.spread_bps = config.spread_bps;
        return std::make_unique<DailyBarFeed>(config.daily_dir, symbols, options);
    }

    if (!config.data_file.empty()) {
        CsvFeedOptions options;
        options.default_symbol = config.symbol;
        options.synthesize_quotes = config.synthesize_quotes_for_bars;
        options.spread_bps = config.spread_bps;
        return std::make_unique<CsvFeed>(config.data_file, symbols, options);
    }

    SyntheticFeedConfig options;
    options.instruments = {SyntheticInstrument{config.symbol, 500.0, 0.05, 0.15, 90'000.0}};
    options.bars_per_session = config.bars_per_session;
    options.sessions = config.sessions;
    options.seed = config.seed;
    options.spread_bps = config.spread_bps;
    Timestamp start = 0;
    (void)time::from_iso8601("2024-03-18T13:30:00Z", start);
    options.start = start;
    return std::make_unique<SyntheticFeed>(options, symbols);
}

}  // namespace

const TheoryResult* BacktestResult::benchmark() const noexcept {
    for (const TheoryResult& theory : theories) {
        if (theory.is_benchmark) {
            return &theory;
        }
    }
    return nullptr;
}

const TheoryResult* BacktestResult::find(std::string_view id) const noexcept {
    for (const TheoryResult& theory : theories) {
        if (theory.id == id) {
            return &theory;
        }
    }
    return nullptr;
}

BacktestResult run_backtest(const BacktestConfig& config) {
    BacktestResult result;

    PluginLoader loader(config.theory_roots);
    loader.rescan();
    result.plugins = loader.plugins();

    SymbolTable symbols;
    Engine engine;

    try {
        std::unique_ptr<IMarketDataFeed> feed = make_feed(config, symbols);
        const FeedSpan span = feed->span();
        result.first_ts = span.first;
        result.last_ts = span.last;
        engine.add_source(std::move(feed));
    } catch (const std::exception& error) {
        result.error = error.what();
        return result;
    }

    std::unique_ptr<IFillModel> fill_model;
    if (config.optimistic_fills) {
        fill_model = std::make_unique<OptimisticFillModel>(config.fills);
    } else {
        fill_model = std::make_unique<SpreadCrossingFillModel>(config.fills);
    }
    result.fill_model = std::string(fill_model->name());
    result.fill_model_realistic = fill_model->is_realistic();

    StrategyRuntimeConfig runtime;
    runtime.starting_cash = config.starting_cash;
    runtime.equity_sample_interval = config.equity_sample_interval;
    runtime.run_seed = config.seed;

    // A deque, not a vector: the engine holds raw pointers to these and a
    // vector would invalidate every one of them as it grew.
    std::deque<StrategyInstance> strategies;

    OrderId id_base = 0;
    for (const PluginInfo& info : loader.plugins()) {
        if (!info.loaded()) {
            continue;
        }
        PluginInstance instance = loader.create(info.id);
        if (!instance.valid()) {
            log::error("backtest", "could not instantiate '" + info.id + "': " + info.error);
            continue;
        }
        id_base += 1'000'000;
        runtime.order_id_base = id_base;
        strategies.emplace_back(info.id,
                                info.display_name,
                                std::move(instance),
                                info.required_symbols,
                                runtime,
                                fill_model.get(),
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

    try {
        engine.run();
    } catch (const std::exception& error) {
        result.error = error.what();
        return result;
    }
    result.events = engine.stats().events_dispatched;

    for (const SymbolId id : symbols.ids()) {
        result.symbols.emplace_back(symbols.name(id));
    }

    // --- Collect ---------------------------------------------------------
    for (StrategyInstance& strategy : strategies) {
        strategy.stop();
        const Ledger& ledger = strategy.ledger();

        TheoryResult row;
        row.id = strategy.id();
        row.display_name = strategy.display_name();
        row.is_benchmark = row.id == config.benchmark_id;
        row.starting_cash = ledger.starting_cash();
        row.final_equity = ledger.equity();
        row.cash = ledger.cash();
        row.realized_pnl = ledger.realized_pnl();
        row.unrealized_pnl = ledger.unrealized_pnl();
        row.commission = ledger.commission_paid();
        row.trades = ledger.blotter().size();
        row.rejected = strategy.broker().rejected_count();
        row.invariant_held = ledger.check_invariant();
        row.latency = strategy.latency();
        row.blotter = ledger.blotter();

        for (const TradeRecord& record : ledger.blotter()) {
            row.slippage += record.slippage;
        }
        for (const Position* position : ledger.open_positions()) {
            row.positions.push_back(*position);
        }
        for (const PublishedMetric& metric : strategy.metrics()) {
            row.published.emplace_back(metric.name, metric.value);
        }

        for (const EquityPoint& point : ledger.equity_curve()) {
            row.times.push_back(to_plot_seconds(point.ts));
            row.equity_curve.push_back(point.equity.to_double());
        }
        row.returns = metrics::simple_returns(std::span<const double>(row.equity_curve));
        row.drawdown = metrics::drawdown_series(std::span<const double>(row.equity_curve));

        if (const PluginInfo* info = loader.find(row.id); info != nullptr) {
            row.author = info->author;
        }

        result.theories.push_back(std::move(row));
    }

    // --- Statistics ------------------------------------------------------
    metrics::Annualisation annualisation;
    annualisation.periods_per_year = metrics::infer_periods_per_year(config.equity_sample_interval);

    std::size_t benchmark_index = kNoIndex;
    for (std::size_t i = 0; i < result.theories.size(); ++i) {
        if (result.theories[i].is_benchmark && result.theories[i].returns.size() >= 2) {
            benchmark_index = i;
            break;
        }
    }

    for (std::size_t i = 0; i < result.theories.size(); ++i) {
        TheoryResult& row = result.theories[i];
        if (row.equity_curve.size() < 2) {
            continue;
        }
        const std::span<const double> levels(row.equity_curve);
        const std::span<const double> returns(row.returns);

        row.summary.samples = row.equity_curve.size();
        row.summary.starting_equity = row.equity_curve.front();
        row.summary.final_equity = row.equity_curve.back();
        row.summary.total_return = metrics::total_return(levels);
        row.summary.volatility = metrics::annualised_volatility(returns, annualisation);
        row.summary.sharpe = metrics::sharpe(returns, annualisation);
        row.summary.sortino = metrics::sortino(returns, annualisation);
        row.summary.max_drawdown = metrics::max_drawdown(levels).max_drawdown;

        const double years = (row.times.back() - row.times.front()) / (365.2425 * 86400.0);
        row.summary.years = years;
        row.summary.span = static_cast<Duration>((row.times.back() - row.times.front()) *
                                                 static_cast<double>(time::kSecond));
        row.summary.cagr = metrics::cagr(row.equity_curve.front(), row.equity_curve.back(), years);
        row.summary.calmar = metrics::calmar(row.summary.cagr, row.summary.max_drawdown);

        if (benchmark_index != kNoIndex && benchmark_index != i) {
            const std::span<const double> bench(result.theories[benchmark_index].returns);
            const metrics::Regression regression = metrics::regress(returns, bench, annualisation);
            row.summary.beta = regression.beta;
            row.summary.alpha = regression.alpha;
            row.summary.r_squared = regression.r_squared;
            row.summary.information_ratio =
                metrics::information_ratio(returns, bench, annualisation);
            row.summary.tracking_error = metrics::tracking_error(returns, bench, annualisation);
            row.summary.correlation = metrics::correlation(returns, bench);
        }
    }

    if (result.theories.size() >= 2) {
        std::vector<std::vector<double>> series;
        std::vector<std::string> labels;
        for (const TheoryResult& row : result.theories) {
            series.push_back(row.returns);
            labels.push_back(row.display_name);
        }
        result.correlation = metrics::correlation_matrix(series, std::move(labels));
    }

    return result;
}

}  // namespace marketscan::run
