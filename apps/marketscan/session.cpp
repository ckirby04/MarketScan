#include "session.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <utility>

#include "marketscan/data/csv_feed.hpp"
#include "marketscan/data/synthetic_feed.hpp"
#include "marketscan/metrics/rolling.hpp"
#include "marketscan/util/logging.hpp"

namespace marketscan::app {
namespace {

constexpr std::size_t kNoIndex = static_cast<std::size_t>(-1);

/// How often a snapshot is published.
///
/// Not every event: a replay running unthrottled dispatches millions a second,
/// and copying every equity curve that often would spend the whole machine on
/// bookkeeping. Not every frame either, because the GUI thread must not have
/// to wait for the engine. Fifty a second is smoother than a display and
/// costs a fraction of a percent.
constexpr auto kPublishInterval = std::chrono::milliseconds(20);

/// How many events to run between checks for a new command.
///
/// Small enough that Pause feels instant, large enough that the mutex is not
/// the bottleneck at full speed.
constexpr std::uint64_t kEventsPerSlice = 4'096;

[[nodiscard]] double to_plot_seconds(Timestamp ts) noexcept {
    return static_cast<double>(ts) / static_cast<double>(time::kSecond);
}

}  // namespace

std::string_view to_string(TransportState state) noexcept {
    switch (state) {
        case TransportState::Ready:
            return "ready";
        case TransportState::Running:
            return "running";
        case TransportState::Paused:
            return "paused";
        case TransportState::Finished:
            return "finished";
        case TransportState::Failed:
            return "failed";
    }
    return "?";
}

// ---------------------------------------------------------------------------
// Lifetime
// ---------------------------------------------------------------------------

Session::Session(SessionConfig config) : config_(std::move(config)) {
    // Publish something immediately so the GUI has a snapshot to draw on its
    // very first frame, before the worker has done anything.
    auto initial = std::make_shared<SessionSnapshot>();
    initial->state = TransportState::Ready;
    snapshot_ = std::move(initial);

    worker_ = std::thread([this] { worker_main(); });
}

Session::~Session() {
    quitting_.store(true, std::memory_order_relaxed);
    post(Request{Command::Quit, 0, 0.0, {}, {}});
    if (worker_.joinable()) {
        worker_.join();
    }
}

void Session::post(Request request) {
    {
        const std::lock_guard<std::mutex> guard(command_mutex_);
        commands_.push_back(std::move(request));
    }
    command_signal_.notify_one();
}

// ---------------------------------------------------------------------------
// Transport, called from the GUI thread
// ---------------------------------------------------------------------------

void Session::play() {
    post(Request{Command::Play, 0, 0.0, {}, {}});
}

void Session::pause() {
    post(Request{Command::Pause, 0, 0.0, {}, {}});
}

void Session::toggle_play() {
    const auto current = snapshot();
    if (current->state == TransportState::Running) {
        pause();
    } else {
        play();
    }
}

void Session::step_event() {
    post(Request{Command::StepEvent, 0, 0.0, {}, {}});
}

void Session::step_bar() {
    post(Request{Command::StepBar, 0, 0.0, {}, {}});
}

void Session::seek(Timestamp ts) {
    post(Request{Command::Seek, ts, 0.0, {}, {}});
}

void Session::set_speed(double multiplier) {
    speed_.store(multiplier < 0.0 ? 0.0 : multiplier, std::memory_order_relaxed);
}

void Session::restart() {
    post(Request{Command::Restart, 0, 0.0, {}, {}});
}

void Session::rescan_theories() {
    post(Request{Command::Rescan, 0, 0.0, {}, {}});
}

void Session::load_plugin(const std::filesystem::path& path) {
    post(Request{Command::LoadPlugin, 0, 0.0, path.string(), {}});
}

void Session::unload_plugin(const std::string& id) {
    post(Request{Command::UnloadPlugin, 0, 0.0, id, {}});
}

void Session::reload_plugin(const std::string& id) {
    post(Request{Command::ReloadPlugin, 0, 0.0, id, {}});
}

void Session::set_parameter(const std::string& strategy_id, const std::string& name, double value) {
    post(Request{Command::SetParameter, 0, value, strategy_id, name});
}

void Session::set_optimistic_fills(bool optimistic) {
    post(Request{Command::SetFillModel, 0, optimistic ? 1.0 : 0.0, {}, {}});
}

bool Session::theories_changed_on_disk() const {
    const std::lock_guard<std::mutex> guard(loader_mutex_);
    return loader_ != nullptr && loader_->changes_available();
}

std::vector<PluginInfo> Session::plugins() const {
    const std::lock_guard<std::mutex> guard(loader_mutex_);
    return loader_ == nullptr ? std::vector<PluginInfo>{} : loader_->plugins();
}

std::shared_ptr<const SessionSnapshot> Session::snapshot() const {
    const std::lock_guard<std::mutex> guard(snapshot_mutex_);
    return snapshot_;
}

// ---------------------------------------------------------------------------
// Worker thread
// ---------------------------------------------------------------------------

void Session::worker_main() {
    {
        const std::lock_guard<std::mutex> guard(loader_mutex_);
        loader_ = std::make_unique<PluginLoader>(config_.theory_roots);
        loader_->rescan();
        build_locked();
    }
    publish();

    while (!quitting_.load(std::memory_order_relaxed)) {
        // Drain every pending command before running anything, so a burst of
        // clicks is applied in the order they were made.
        std::deque<Request> batch;
        {
            std::unique_lock<std::mutex> lock(command_mutex_);
            if (commands_.empty() && !running_ && pending_events_ == 0) {
                // Nothing to do and nothing to run: sleep until told
                // otherwise, rather than spinning a core to draw nothing.
                command_signal_.wait_for(lock, std::chrono::milliseconds(100));
            }
            batch.swap(commands_);
        }

        for (const Request& request : batch) {
            if (request.command == Command::Quit) {
                teardown();
                return;
            }
            apply(request);
        }

        const bool did_work = pump();

        // Throttled replay spends most of its time ahead of schedule, waiting
        // for wall-clock time to catch up. Without this the loop would spin a
        // core at 100% doing nothing; a millisecond is short enough that Pause
        // still feels instant and long enough that the core is idle.
        if (!did_work && running_) {
            std::unique_lock<std::mutex> lock(command_mutex_);
            if (commands_.empty()) {
                command_signal_.wait_for(lock, std::chrono::milliseconds(1));
            }
        }

        const auto now = std::chrono::steady_clock::now();
        if (now - last_publish_ >= kPublishInterval) {
            publish();
            last_publish_ = now;
        }
    }
    teardown();
}

void Session::apply(const Request& request) {
    switch (request.command) {
        case Command::Play:
            if (state_ == TransportState::Finished || state_ == TransportState::Failed) {
                break;  // nothing left to play; Restart is the way back
            }
            running_ = true;
            state_ = TransportState::Running;
            pace_anchor_ = std::chrono::steady_clock::now();
            // Anchored to where the *data* starts, not to the clock's zero.
            // The engine clock reads 0 until the first event is dispatched,
            // and market timestamps are around 1.7e18 nanoseconds, so
            // anchoring at zero would make every throttled deadline fall
            // decades before the first bar and the replay would never move.
            pace_anchor_ts_ = engine_ == nullptr         ? 0
                              : engine_->stats().started ? engine_->now()
                                                         : span_.first;
            rate_anchor_ = pace_anchor_;
            rate_anchor_events_ = engine_ != nullptr ? engine_->stats().events_dispatched : 0;
            break;

        case Command::Pause:
            running_ = false;
            pending_events_ = 0;
            stepping_to_bar_ = false;
            if (state_ == TransportState::Running) {
                state_ = TransportState::Paused;
            }
            break;

        case Command::StepEvent:
            running_ = false;
            stepping_to_bar_ = false;
            pending_events_ = 1;
            break;

        case Command::StepBar:
            running_ = false;
            pending_events_ = 0;
            stepping_to_bar_ = engine_ != nullptr;
            step_bar_target_ = engine_ != nullptr ? engine_->stats().bars_dispatched + 1 : 0;
            break;

        case Command::Seek: {
            // Rebuild and replay. See the header for why this cannot be a
            // rewind.
            const Timestamp target = request.timestamp;
            build();
            if (engine_ != nullptr && state_ != TransportState::Failed) {
                try {
                    engine_->run_until(target);
                    state_ = TransportState::Paused;
                } catch (const std::exception& error) {
                    state_ = TransportState::Failed;
                    error_ = error.what();
                    log::error("session", error_);
                }
            }
            running_ = false;
            break;
        }

        case Command::Restart:
            build();
            break;

        case Command::Rescan: {
            // One critical section for the whole swap, so the GUI never reads
            // a plugin list that is halfway through being replaced.
            const std::lock_guard<std::mutex> guard(loader_mutex_);
            teardown_locked();
            loader_->rescan();
            build_locked();
            break;
        }

        case Command::LoadPlugin: {
            // One critical section for the whole swap, so the GUI never reads
            // a plugin list that is halfway through being replaced.
            const std::lock_guard<std::mutex> guard(loader_mutex_);
            teardown_locked();
            (void)loader_->load_file(request.text);
            build_locked();
            break;
        }

        case Command::UnloadPlugin: {
            // One critical section for the whole swap, so the GUI never reads
            // a plugin list that is halfway through being replaced.
            const std::lock_guard<std::mutex> guard(loader_mutex_);
            // Strategies hold the instances, so they go first: the loader
            // refuses to free a module anything is still using.
            teardown_locked();
            (void)loader_->unload(request.text);
            build_locked();
            break;
        }

        case Command::ReloadPlugin: {
            // One critical section for the whole swap, so the GUI never reads
            // a plugin list that is halfway through being replaced.
            const std::lock_guard<std::mutex> guard(loader_mutex_);
            teardown_locked();
            (void)loader_->reload(request.text);
            build_locked();
            break;
        }

        case Command::SetParameter:
            for (StrategyInstance& strategy : strategies_) {
                if (strategy.id() == request.text) {
                    strategy.set_parameter(request.text2.c_str(), request.value);
                    break;
                }
            }
            break;

        case Command::SetFillModel:
            config_.optimistic_fills = request.value != 0.0;
            build();
            break;

        case Command::Quit:
            break;
    }
}

void Session::teardown() {
    const std::lock_guard<std::mutex> guard(loader_mutex_);
    teardown_locked();
}

void Session::teardown_locked() {
    // Order matters: the engine holds raw pointers to the strategies, and the
    // strategies own the plugin instances whose destruction tells the loader
    // its modules are free again.
    engine_.reset();
    strategies_.clear();
    fill_model_.reset();
    symbols_.reset();
}

void Session::build() {
    const std::lock_guard<std::mutex> guard(loader_mutex_);
    build_locked();
}

void Session::build_locked() {
    teardown_locked();
    error_.clear();
    running_ = false;
    pending_events_ = 0;
    stepping_to_bar_ = false;
    events_per_second_ = 0.0;

    symbols_ = std::make_unique<SymbolTable>();
    engine_ = std::make_unique<Engine>();

    if (config_.optimistic_fills) {
        fill_model_ = std::make_unique<OptimisticFillModel>(config_.fills);
    } else {
        fill_model_ = std::make_unique<SpreadCrossingFillModel>(config_.fills);
    }

    try {
        std::unique_ptr<IMarketDataFeed> feed;
        if (!config_.data_file.empty()) {
            CsvFeedOptions options;
            options.default_symbol = config_.default_symbol;
            // Bar data carries no spread, and the model needs one to cross.
            options.synthesize_quotes = true;
            feed = std::make_unique<CsvFeed>(config_.data_file, *symbols_, options);
        } else {
            SyntheticFeedConfig options;
            options.instruments = {
                SyntheticInstrument{config_.default_symbol, 500.0, 0.05, 0.15, 90'000.0}};
            options.bars_per_session = config_.bars_per_session;
            options.sessions = config_.sessions;
            options.seed = config_.seed;
            Timestamp start = 0;
            (void)time::from_iso8601("2024-03-18T13:30:00Z", start);
            options.start = start;
            feed = std::make_unique<SyntheticFeed>(options, *symbols_);
        }
        span_ = feed->span();
        engine_->add_source(std::move(feed));
    } catch (const std::exception& error) {
        state_ = TransportState::Failed;
        error_ = error.what();
        log::error("session", error_);
        return;
    }

    StrategyRuntimeConfig runtime;
    runtime.starting_cash = config_.starting_cash;
    runtime.equity_sample_interval = config_.equity_sample_interval;
    runtime.run_seed = config_.seed;

    OrderId id_base = 0;
    for (const PluginInfo& info : loader_->plugins()) {
        if (!info.loaded()) {
            continue;
        }
        PluginInstance instance = loader_->create(info.id);
        if (!instance.valid()) {
            log::error("session", "could not instantiate '" + info.id + "': " + info.error);
            continue;
        }
        id_base += 1'000'000;
        runtime.order_id_base = id_base;
        strategies_.emplace_back(info.id,
                                 info.display_name,
                                 std::move(instance),
                                 info.required_symbols,
                                 runtime,
                                 fill_model_.get(),
                                 &engine_->market(),
                                 symbols_.get(),
                                 &engine_->clock());
    }

    for (StrategyInstance& strategy : strategies_) {
        strategy.resolve_subscriptions();
        engine_->add_sink(&strategy);
        strategy.start();
    }

    state_ = TransportState::Ready;
}

bool Session::pump() {
    if (engine_ == nullptr || state_ == TransportState::Failed) {
        return false;
    }

    std::uint64_t budget = 0;
    if (pending_events_ > 0) {
        budget = pending_events_;
    } else if (stepping_to_bar_) {
        budget = kEventsPerSlice;
    } else if (running_) {
        budget = kEventsPerSlice;

        const double speed = speed_.load(std::memory_order_relaxed);
        if (speed > 0.0) {
            // Throttled replay: only run as far as wall-clock time has earned.
            // Anchored to where playback started rather than to the previous
            // slice, so rounding does not accumulate into a drift.
            const auto elapsed = std::chrono::steady_clock::now() - pace_anchor_;
            const auto elapsed_nanos =
                std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count();
            const auto allowed = pace_anchor_ts_ +
                                 static_cast<Timestamp>(static_cast<double>(elapsed_nanos) * speed);
            try {
                const std::uint64_t dispatched = engine_->run_until(allowed);
                if (dispatched == 0 && !engine_->has_pending_event()) {
                    // Nothing dispatched and nothing left. Without the second
                    // half of that test this is indistinguishable from "the
                    // next event is not due yet", and stepping to find out
                    // would defeat the throttle it is trying to honour.
                    running_ = false;
                    state_ = TransportState::Finished;
                }
                return dispatched > 0;
            } catch (const std::exception& error) {
                state_ = TransportState::Failed;
                error_ = error.what();
                log::error("session", error_);
                running_ = false;
                return false;
            }
        }
    } else {
        return false;
    }

    try {
        std::uint64_t dispatched = 0;
        while (dispatched < budget) {
            if (!engine_->step()) {
                running_ = false;
                pending_events_ = 0;
                stepping_to_bar_ = false;
                state_ = TransportState::Finished;
                break;
            }
            ++dispatched;

            // Stop once a bar has actually gone past. Stepping events would
            // stop on the quote that precedes it, which is not what anyone
            // means by "step one bar".
            if (stepping_to_bar_ && engine_->stats().bars_dispatched >= step_bar_target_) {
                stepping_to_bar_ = false;
                state_ = TransportState::Paused;
                break;
            }
        }

        if (pending_events_ > 0) {
            pending_events_ = pending_events_ > dispatched ? pending_events_ - dispatched : 0;
            if (pending_events_ == 0 && state_ != TransportState::Finished) {
                state_ = TransportState::Paused;
            }
        }

        if (dispatched > 0 && running_) {
            const auto now = std::chrono::steady_clock::now();
            const auto window =
                std::chrono::duration_cast<std::chrono::duration<double>>(now - rate_anchor_)
                    .count();
            if (window >= 0.25) {
                const std::uint64_t events = engine_->stats().events_dispatched;
                events_per_second_ = static_cast<double>(events - rate_anchor_events_) / window;
                rate_anchor_ = now;
                rate_anchor_events_ = events;
            }
        }
        return dispatched > 0;
    } catch (const std::exception& error) {
        state_ = TransportState::Failed;
        error_ = error.what();
        log::error("session", error_);
        running_ = false;
        return false;
    }
}

// ---------------------------------------------------------------------------
// Snapshot
// ---------------------------------------------------------------------------

void Session::publish() {
    auto next = std::make_shared<SessionSnapshot>();
    next->state = state_;
    next->error = error_;
    next->speed = speed_.load(std::memory_order_relaxed);
    next->events_per_second = events_per_second_;

    if (fill_model_ != nullptr) {
        next->fill_model = std::string(fill_model_->name());
        next->fill_model_realistic = fill_model_->is_realistic();
    }

    if (engine_ != nullptr) {
        next->clock = engine_->now();
        next->events = engine_->stats().events_dispatched;
    }
    next->first_ts = span_.first;
    next->last_ts = span_.last;
    next->span_known = span_.known;
    next->total_events = span_.event_count;

    if (span_.known && span_.last > span_.first && next->clock > 0) {
        const double fraction = static_cast<double>(next->clock - span_.first) /
                                static_cast<double>(span_.last - span_.first);
        next->progress = std::clamp(fraction, 0.0, 1.0);
    } else if (state_ == TransportState::Finished) {
        next->progress = 1.0;
    }

    if (symbols_ != nullptr) {
        for (const SymbolId id : symbols_->ids()) {
            next->symbols.emplace_back(symbols_->name(id));
        }
    }

    // --- Per strategy ---------------------------------------------------
    next->strategies.reserve(strategies_.size());
    for (StrategyInstance& strategy : strategies_) {
        const Ledger& ledger = strategy.ledger();

        StrategySnapshot row;
        row.id = strategy.id();
        row.display_name = strategy.display_name();
        row.is_benchmark = row.id == config_.benchmark_id;
        row.starting_cash = ledger.starting_cash();
        row.equity = ledger.equity();
        row.cash = ledger.cash();
        row.realized_pnl = ledger.realized_pnl();
        row.unrealized_pnl = ledger.unrealized_pnl();
        row.commission = ledger.commission_paid();
        row.trades = ledger.blotter().size();
        row.rejected = strategy.broker().rejected_count();
        row.working_orders = strategy.broker().working_orders().size();
        row.latency = strategy.latency();

        for (const TradeRecord& record : ledger.blotter()) {
            row.slippage += record.slippage;
        }
        row.blotter = ledger.blotter();

        for (const Position* position : ledger.open_positions()) {
            row.positions.push_back(*position);
        }
        row.published = strategy.metrics();

        // Parameter descriptions come from the plugin's descriptor; their
        // current values come from the live instance, because the GUI's
        // sliders move them and the descriptor only ever holds defaults.
        {
            const std::lock_guard<std::mutex> guard(loader_mutex_);
            if (loader_ != nullptr) {
                if (const PluginInfo* info = loader_->find(row.id); info != nullptr) {
                    row.author = info->author;
                    row.params = info->params;
                    row.param_values.reserve(info->params.size());
                    for (const ParamInfo& param : info->params) {
                        row.param_values.push_back(strategy.parameter(param.name.c_str()));
                    }
                }
            }
        }

        const std::vector<EquityPoint>& curve = ledger.equity_curve();
        row.times.reserve(curve.size());
        row.equity_curve.reserve(curve.size());
        row.normalised.reserve(curve.size());
        const double base = row.starting_cash.to_double();
        for (const EquityPoint& point : curve) {
            row.times.push_back(to_plot_seconds(point.ts));
            row.equity_curve.push_back(point.equity.to_double());
            row.normalised.push_back(base > 0.0 ? point.equity.to_double() / base : 0.0);
        }
        row.drawdown = metrics::drawdown_series(std::span<const double>(row.equity_curve));
        row.returns = metrics::simple_returns(std::span<const double>(row.equity_curve));

        // Equity at the start of the current UTC day, for the day P&L column.
        if (!curve.empty()) {
            const std::int64_t today = time::day_number(curve.back().ts);
            Money open_of_day = curve.front().equity;
            for (const EquityPoint& point : curve) {
                if (time::day_number(point.ts) == today) {
                    break;
                }
                open_of_day = point.equity;
            }
            row.day_pnl = row.equity - open_of_day;
        }

        next->strategies.push_back(std::move(row));
    }

    // --- Benchmark-relative ---------------------------------------------
    for (std::size_t i = 0; i < next->strategies.size(); ++i) {
        if (next->strategies[i].is_benchmark) {
            next->benchmark_index = i;
            break;
        }
    }

    metrics::Annualisation annualisation;
    annualisation.periods_per_year =
        metrics::infer_periods_per_year(config_.equity_sample_interval);

    const std::vector<double>* benchmark_levels = nullptr;
    const std::vector<double>* benchmark_returns = nullptr;
    if (next->benchmark_index != kNoIndex) {
        const StrategySnapshot& benchmark = next->strategies[next->benchmark_index];
        if (benchmark.equity_curve.size() >= 2) {
            benchmark_levels = &benchmark.equity_curve;
            benchmark_returns = &benchmark.returns;
        }
    }

    for (StrategySnapshot& row : next->strategies) {
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

        const double years =
            static_cast<double>(row.times.back() - row.times.front()) / (365.2425 * 86400.0);
        row.summary.years = years;
        row.summary.cagr = metrics::cagr(row.equity_curve.front(), row.equity_curve.back(), years);
        row.summary.calmar = metrics::calmar(row.summary.cagr, row.summary.max_drawdown);

        if (benchmark_returns != nullptr && !row.is_benchmark) {
            const std::span<const double> bench(*benchmark_returns);
            const metrics::Regression regression = metrics::regress(returns, bench, annualisation);
            row.summary.beta = regression.beta;
            row.summary.alpha = regression.alpha;
            row.summary.r_squared = regression.r_squared;
            row.summary.information_ratio =
                metrics::information_ratio(returns, bench, annualisation);
            row.summary.tracking_error = metrics::tracking_error(returns, bench, annualisation);
            row.summary.correlation = metrics::correlation(returns, bench);
        }
        if (benchmark_levels != nullptr) {
            row.relative =
                metrics::relative_strength(levels, std::span<const double>(*benchmark_levels));
        }
    }

    // --- Correlation matrix ---------------------------------------------
    if (next->strategies.size() >= 2) {
        std::vector<std::vector<double>> series;
        std::vector<std::string> labels;
        series.reserve(next->strategies.size());
        labels.reserve(next->strategies.size());
        for (const StrategySnapshot& row : next->strategies) {
            series.push_back(row.returns);
            labels.push_back(row.display_name);
        }
        next->correlation = metrics::correlation_matrix(series, std::move(labels));
    }

    // --- Latency ---------------------------------------------------------
    for (const StrategySnapshot& row : next->strategies) {
        next->latency.merge(row.latency);
    }

    {
        const std::lock_guard<std::mutex> guard(snapshot_mutex_);
        snapshot_ = std::move(next);
    }
}

}  // namespace marketscan::app
