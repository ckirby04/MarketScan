#ifndef MARKETSCAN_APP_SESSION_HPP
#define MARKETSCAN_APP_SESSION_HPP

#include <atomic>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "marketscan/core/engine.hpp"
#include "marketscan/data/feed.hpp"
#include "marketscan/exec/fill_model.hpp"
#include "marketscan/metrics/correlation.hpp"
#include "marketscan/metrics/performance.hpp"
#include "marketscan/strategy/plugin_loader.hpp"
#include "marketscan/strategy/strategy_instance.hpp"
#include "marketscan/util/latency_histogram.hpp"

namespace marketscan::app {

enum class TransportState : std::uint8_t {
    /// Built but not started.
    Ready = 0,
    Running,
    Paused,
    /// The feed is drained.
    Finished,
    /// Something went wrong; `error` in the snapshot says what.
    Failed,
};

[[nodiscard]] std::string_view to_string(TransportState state) noexcept;

struct SessionConfig {
    /// A CSV to replay. Empty means generate synthetic data.
    std::filesystem::path data_file;
    std::string default_symbol = "SPY";

    std::uint32_t bars_per_session = 390;
    std::uint32_t sessions = 2;
    std::uint64_t seed = 0xC0FF'EE00'1234'5678ULL;

    Money starting_cash = Money::from_dollars(100'000);
    Duration equity_sample_interval = time::kMinute;

    std::vector<std::filesystem::path> theory_roots;

    /// Which theory the leaderboard measures the others against.
    std::string benchmark_id = "buy_and_hold";

    /// Swap the realistic fill model for the optimistic one. Off by default,
    /// and the UI says so loudly when it is on.
    bool optimistic_fills = false;

    /// Execution realism. Default-constructed is an instant simulator; see
    /// FillModelConfig.
    FillModelConfig fills;
};

/// One theory's state, copied out of the engine for the GUI to draw.
///
/// A copy rather than a pointer into live state: the engine thread is still
/// running while the GUI walks this, and a vector it is appending to is not
/// something another thread may iterate.
struct StrategySnapshot {
    std::string id;
    std::string display_name;
    std::string author;
    bool is_benchmark = false;

    Money starting_cash;
    Money equity;
    Money cash;
    Money realized_pnl;
    Money unrealized_pnl;
    Money commission;
    Money slippage;

    /// Change in equity since the start of the current UTC day.
    Money day_pnl;

    std::size_t trades = 0;
    std::uint64_t rejected = 0;
    std::size_t working_orders = 0;

    metrics::PerformanceSummary summary;

    /// Plot-ready series. X is seconds since the Unix epoch, which is what
    /// ImPlot's time axis expects.
    std::vector<double> times;
    std::vector<double> equity_curve;
    std::vector<double> normalised;  // rebased to the starting capital
    std::vector<double> drawdown;    // underwater, negative fractions
    std::vector<double> relative;    // vs the benchmark, both rebased to 1

    /// Period returns, for the correlation matrix.
    std::vector<double> returns;

    std::vector<PublishedMetric> published;
    std::vector<TradeRecord> blotter;
    std::vector<Position> positions;

    /// Parameters, and their values right now.
    std::vector<ParamInfo> params;
    std::vector<double> param_values;

    LatencyHistogram latency;
};

/// Everything the GUI draws, as of one instant.
struct SessionSnapshot {
    TransportState state = TransportState::Ready;
    std::string error;

    Timestamp clock = 0;
    Timestamp first_ts = 0;
    Timestamp last_ts = 0;
    bool span_known = false;

    /// 0..1 through the dataset.
    double progress = 0.0;

    std::uint64_t events = 0;
    std::uint64_t total_events = 0;
    double events_per_second = 0.0;

    /// Requested replay speed. Zero means "as fast as possible".
    double speed = 0.0;

    std::string fill_model;
    bool fill_model_realistic = true;

    std::vector<StrategySnapshot> strategies;
    std::size_t benchmark_index = static_cast<std::size_t>(-1);

    metrics::CorrelationMatrix correlation;
    LatencyHistogram latency;

    std::vector<std::string> symbols;
};

/// The engine, on its own thread.
///
/// The GUI never touches the engine. It posts commands and reads snapshots,
/// and that is the whole interface. Anything else would put a 16-millisecond
/// frame budget in the same lock as a replay running at ten thousand times
/// speed, and the frame budget would lose.
///
/// Snapshots are published on a throttle rather than every event, and swapped
/// in behind a shared_ptr, so the GUI reads a complete and internally
/// consistent picture that cannot change while it is being drawn.
class Session {
public:
    explicit Session(SessionConfig config);
    ~Session();

    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
    Session(Session&&) = delete;
    Session& operator=(Session&&) = delete;

    // --- Transport ------------------------------------------------------
    //
    // All of these return immediately; the worker thread picks them up.

    void play();
    void pause();
    void toggle_play();

    /// One event, then pause.
    void step_event();

    /// Events until the next bar has been dispatched, then pause.
    void step_bar();

    /// Rebuild and replay to `ts`.
    ///
    /// Scrubbing backwards cannot rewind the strategies -- they hold state
    /// from a future they are about to be shown again -- so the run is rebuilt
    /// from the start and replayed. That is deterministic by construction,
    /// which is the only reason the scrub bar can be trusted at all.
    void seek(Timestamp ts);

    /// Replay speed as a multiple of real time. Zero is unthrottled.
    void set_speed(double multiplier);

    /// Rebuild from the beginning with the current theories.
    void restart();

    // --- Theories -------------------------------------------------------

    /// Rescan theories/ and rebuild the run. The only way a plugin is ever
    /// reloaded: never automatically, because swapping a strategy's code out
    /// mid-run would invalidate every number on screen without saying so.
    void rescan_theories();

    void load_plugin(const std::filesystem::path& path);
    void unload_plugin(const std::string& id);
    void reload_plugin(const std::string& id);

    /// Cheap enough to poll once a second; raises the GUI's reload badge.
    [[nodiscard]] bool theories_changed_on_disk() const;

    /// A copy, because the worker thread mutates the loader's state.
    [[nodiscard]] std::vector<PluginInfo> plugins() const;

    void set_parameter(const std::string& strategy_id, const std::string& name, double value);

    void set_optimistic_fills(bool optimistic);

    // --- Reading --------------------------------------------------------

    /// Never null. The same pointer stays valid however long it is held.
    [[nodiscard]] std::shared_ptr<const SessionSnapshot> snapshot() const;

    [[nodiscard]] const SessionConfig& config() const noexcept { return config_; }

private:
    enum class Command : std::uint8_t {
        Play,
        Pause,
        StepEvent,
        StepBar,
        Seek,
        Restart,
        Rescan,
        LoadPlugin,
        UnloadPlugin,
        ReloadPlugin,
        SetParameter,
        SetFillModel,
        Quit,
    };

    struct Request {
        Command command = Command::Pause;
        Timestamp timestamp = 0;
        double value = 0.0;
        std::string text;
        std::string text2;
    };

    void post(Request request);

    // --- Worker thread --------------------------------------------------

    void worker_main();
    void apply(const Request& request);
    void publish();
    [[nodiscard]] bool pump();

    /// Tearing down and rebuilding both reach into the loader: dropping a
    /// strategy releases its plugin instance, and building one creates a new
    /// one. Both mutate state the GUI thread reads through plugins(), so both
    /// take loader_mutex_ -- and the _locked variants exist for the command
    /// handlers that need to rescan or unload in the same critical section,
    /// because std::mutex is not recursive.
    void build();
    void build_locked();
    void teardown();
    void teardown_locked();

    SessionConfig config_;

    mutable std::mutex command_mutex_;
    std::condition_variable command_signal_;
    std::deque<Request> commands_;
    std::atomic<bool> quitting_{false};

    mutable std::mutex snapshot_mutex_;
    std::shared_ptr<const SessionSnapshot> snapshot_;

    std::atomic<double> speed_{0.0};

    // Worker-owned. Nothing below is touched from any other thread.
    std::unique_ptr<PluginLoader> loader_;
    std::unique_ptr<SymbolTable> symbols_;
    std::unique_ptr<Engine> engine_;
    std::unique_ptr<IFillModel> fill_model_;
    std::deque<StrategyInstance> strategies_;
    FeedSpan span_{};
    TransportState state_ = TransportState::Ready;
    std::string error_;
    bool running_ = false;
    std::uint64_t pending_events_ = 0;
    bool stepping_to_bar_ = false;

    /// Bar count to stop at, for "step one bar".
    std::uint64_t step_bar_target_ = 0;

    /// Wall clock and engine clock at the point playback last started, so a
    /// speed multiplier can be honoured without drifting.
    std::chrono::steady_clock::time_point pace_anchor_{};
    Timestamp pace_anchor_ts_ = 0;

    std::chrono::steady_clock::time_point last_publish_{};
    std::chrono::steady_clock::time_point rate_anchor_{};
    std::uint64_t rate_anchor_events_ = 0;
    double events_per_second_ = 0.0;

    mutable std::mutex loader_mutex_;

    std::thread worker_;
};

}  // namespace marketscan::app

#endif  // MARKETSCAN_APP_SESSION_HPP
