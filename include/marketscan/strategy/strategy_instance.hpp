#ifndef MARKETSCAN_STRATEGY_STRATEGY_INSTANCE_HPP
#define MARKETSCAN_STRATEGY_STRATEGY_INSTANCE_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "marketscan/core/engine.hpp"
#include "marketscan/core/random.hpp"
#include "marketscan/core/symbol_table.hpp"
#include "marketscan/exec/broker.hpp"
#include "marketscan/strategy/host_context.hpp"
#include "marketscan/strategy/plugin_loader.hpp"
#include "marketscan/util/latency_histogram.hpp"

namespace marketscan {

struct StrategyRuntimeConfig {
    Money starting_cash = Money::from_dollars(100'000);

    /// How often the equity curve is sampled. Every event would produce a
    /// series nobody can plot; once a minute is enough to see the shape.
    Duration equity_sample_interval = time::kMinute;

    /// Seeds this strategy's own random stream, via derive_seed(seed, id).
    /// Per-strategy rather than shared, so enabling one theory cannot change
    /// the numbers another one draws.
    std::uint64_t run_seed = 0x5EED'1234'ABCD'0001ULL;

    /// Order ids start here. The engine gives each strategy a distinct range
    /// so a blotter merged across theories has no collisions.
    OrderId order_id_base = 0;
};

/// A named scalar a strategy published. The GUI plots these without being told.
struct PublishedMetric {
    std::string name;
    double value = 0.0;
    std::uint64_t updates = 0;
};

/// One theory, running.
///
/// This is where a plugin stops being a DLL and becomes something the engine
/// cannot distinguish from built-in code: it is an IEventSink like any other,
/// it owns a Broker and therefore its own virtual account, and it implements
/// HostServices so the C table handed across the boundary has somewhere to
/// land.
///
/// Events reach the plugin in this order, per market event:
///
///   1. on_bar / on_quote / on_trade -- the strategy may submit orders, which
///      are priced immediately against the book as of this event.
///   2. on_event_complete -- resting orders are retried, DAY orders expire,
///      positions are marked and the equity curve is sampled.
///
/// Step 2 happens after every sink has seen step 1, so a resting order can
/// never fill against a book that a later strategy has not yet reacted to.
class StrategyInstance final : public IEventSink, public IFillListener, public HostServices {
public:
    StrategyInstance(std::string id,
                     std::string display_name,
                     PluginInstance plugin,
                     std::vector<std::string> required_symbols,
                     StrategyRuntimeConfig config,
                     const IFillModel* fill_model,
                     const MarketView* market,
                     SymbolTable* symbols,
                     const IClock* clock);

    ~StrategyInstance() override;

    StrategyInstance(const StrategyInstance&) = delete;
    StrategyInstance& operator=(const StrategyInstance&) = delete;
    StrategyInstance(StrategyInstance&&) = delete;
    StrategyInstance& operator=(StrategyInstance&&) = delete;

    /// Hands the strategy its host context and calls on_start. Must be called
    /// before the first event.
    void start();

    /// Calls on_stop. Idempotent.
    void stop();

    // --- IEventSink -----------------------------------------------------

    void on_bar(const MS_Bar& bar) override;
    void on_quote(const MS_Quote& quote) override;
    void on_trade(const MS_Trade& trade) override;
    void on_event_complete(const MarketEvent& event) override;

    // --- IFillListener --------------------------------------------------

    void on_fill(const MS_Fill& fill) override;

    // --- HostServices ---------------------------------------------------

    std::uint64_t submit_order(const MS_OrderRequest& request) override;
    void cancel_order(std::uint64_t order_id) override;
    [[nodiscard]] Timestamp now() const override;
    double random_uniform() override;
    [[nodiscard]] double position(SymbolId symbol) const override;
    [[nodiscard]] Money cash() const override;
    [[nodiscard]] Money equity() const override;
    [[nodiscard]] SymbolId symbol_id(const char* ticker) const override;
    [[nodiscard]] const char* symbol_name(SymbolId symbol) const override;
    [[nodiscard]] std::uint32_t symbol_count() const override;
    void log(LogLevel level, const char* message) override;
    void publish_metric(const char* name, double value) override;

    // --- Inspection -----------------------------------------------------

    [[nodiscard]] const std::string& id() const noexcept { return id_; }

    [[nodiscard]] const std::string& display_name() const noexcept { return display_name_; }

    [[nodiscard]] const Ledger& ledger() const noexcept { return broker_.ledger(); }

    [[nodiscard]] const Broker& broker() const noexcept { return broker_; }

    [[nodiscard]] Broker& broker() noexcept { return broker_; }

    /// Published metrics, in first-published order. Stable across runs, which
    /// a hash map would not be.
    [[nodiscard]] const std::vector<PublishedMetric>& metrics() const noexcept { return metrics_; }

    [[nodiscard]] double parameter(const char* name) const { return plugin_.get_param(name); }

    void set_parameter(const char* name, double value) { plugin_.set_param(name, value); }

    /// Symbols this strategy asked for. Empty means every symbol.
    [[nodiscard]] const std::vector<SymbolId>& subscribed() const noexcept { return subscribed_; }

    [[nodiscard]] bool started() const noexcept { return started_; }

    /// Time from an event arriving at this strategy to its callback
    /// returning, which is the event-in to order-out path: any order the
    /// strategy submits is priced and booked inside that callback.
    ///
    /// In replay this measures the machine, not the market. It is here from
    /// the start so the path is already instrumented when it means something.
    [[nodiscard]] const LatencyHistogram& latency() const noexcept { return latency_; }

    /// Resolves the required tickers against the symbol table. Called again
    /// after a feed interns new symbols; a required ticker the data never
    /// mentions is logged once rather than being silently ignored.
    void resolve_subscriptions();

private:
    [[nodiscard]] bool wants(SymbolId symbol) const noexcept;

    std::string id_;
    std::string display_name_;
    PluginInstance plugin_;
    std::vector<std::string> required_symbols_;
    std::vector<SymbolId> subscribed_;

    StrategyRuntimeConfig config_;
    const MarketView* market_;
    SymbolTable* symbols_;
    const IClock* clock_;

    Broker broker_;
    HostContext host_;
    DeterministicRng rng_;

    std::vector<PublishedMetric> metrics_;
    LatencyHistogram latency_;

    bool started_ = false;
    bool stopped_ = false;
    bool warned_missing_symbols_ = false;
};

}  // namespace marketscan

#endif  // MARKETSCAN_STRATEGY_STRATEGY_INSTANCE_HPP
