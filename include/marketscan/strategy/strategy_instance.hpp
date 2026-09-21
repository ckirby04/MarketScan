#ifndef MARKETSCAN_STRATEGY_STRATEGY_INSTANCE_HPP
#define MARKETSCAN_STRATEGY_STRATEGY_INSTANCE_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "marketscan/core/engine.hpp"
#include "marketscan/core/random.hpp"
#include "marketscan/core/session_calendar.hpp"
#include "marketscan/core/symbol_table.hpp"
#include "marketscan/data/security_master.hpp"
#include "marketscan/data/universe.hpp"
#include "marketscan/exec/broker.hpp"
#include "marketscan/exec/delisting.hpp"
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

    /// Which wall clock DAY orders expire against.
    ///
    /// Has to match the zone the strategy's own session clock uses, or the
    /// host and the plugin disagree about what day it is -- the host expiring
    /// an order on one boundary while the strategy believes it is still in the
    /// session that placed it. Both default to US Eastern.
    ExchangeZone exchange_zone = ExchangeZone::UsEastern;
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

    // --- The survivorship guard -----------------------------------------
    //
    // A backtest run over today's index members, applied to history, is a
    // backtest over the companies that survived, and it reports a premium
    // nobody could have earned because nobody knew in 1999 which names would
    // still be there.
    //
    // The defence is not a rule a strategy author is asked to remember. When a
    // universe is installed here, this instance is the only path from an event
    // to a plugin and the only implementation of the host context that plugin
    // has -- so filtering here is not a policy applied at a checkpoint, it is
    // the absence of a route. There is no call a strategy can make that
    // returns a non-member.
    //
    // Both are borrowed and must outlive this instance.

    void set_universe(const Universe* universe, const SecurityMaster* master) noexcept {
        universe_ = universe;
        master_ = master;
    }

    [[nodiscard]] const Universe* universe() const noexcept { return universe_; }

    /// Securities force-liquidated because they left the universe while held.
    [[nodiscard]] std::uint64_t forced_exits() const noexcept { return forced_exits_; }

    // --- The assumed exit ------------------------------------------------
    //
    // A forced exit liquidates at the last price the book holds, which is an
    // optimistic approximation and the single largest thing this data layer gets
    // wrong: a name leaves an index either because a bigger one replaced it or
    // because it collapsed, and only the first liquidates near its last print.
    //
    // `include/marketscan/exec/delisting.hpp` names the two choices and
    // `research/delisting.py` is the research layer's half of the same switch.
    // The default is the optimistic one on purpose -- changing a default silently
    // moves every committed number measured under it.

    /// Installs the delisting model and the table it reads.
    ///
    /// `returns` is borrowed and must outlive this instance. Null is legal and
    /// means the data supplies nothing, which liquidates at the last price under
    /// either model.
    void set_delisting_model(DelistingReturnModel model, const DelistingReturns* returns) noexcept {
        delisting_model_ = model;
        delisting_returns_ = returns;
    }

    [[nodiscard]] DelistingReturnModel delisting_model() const noexcept { return delisting_model_; }

    [[nodiscard]] const DelistingReturns* delisting_returns() const noexcept {
        return delisting_returns_;
    }

    /// Forced exits that found a delisting return in the table and booked it.
    ///
    /// Counted separately from `forced_exits` so that a run configured for
    /// `apply_true_return` against a table that covers none of its departures is
    /// visible as such, rather than looking like a universe whose delistings
    /// happened to cost nothing.
    [[nodiscard]] std::uint64_t delistings_applied() const noexcept { return delistings_applied_; }

    /// Events withheld from the strategy because the security was not a member
    /// that day. Counted so the guard's effect is a number rather than a
    /// claim: a run where this is zero has not been filtered at all.
    [[nodiscard]] std::uint64_t events_withheld() const noexcept { return events_withheld_; }

private:
    [[nodiscard]] bool wants(SymbolId symbol) const noexcept;

    /// Whether this security may be shown to the strategy at this instant.
    [[nodiscard]] bool visible(SymbolId symbol, Timestamp ts) const noexcept;

    /// Force-liquidates anything held that has left the universe.
    void sweep_removals(Timestamp ts);

    std::string id_;
    std::string display_name_;
    PluginInstance plugin_;
    std::vector<std::string> required_symbols_;
    std::vector<SymbolId> subscribed_;

    StrategyRuntimeConfig config_;
    const MarketView* market_;
    SymbolTable* symbols_;
    const IClock* clock_;

    const Universe* universe_ = nullptr;
    const SecurityMaster* master_ = nullptr;
    std::uint64_t forced_exits_ = 0;
    DelistingReturnModel delisting_model_ = kDefaultDelistingReturnModel;
    const DelistingReturns* delisting_returns_ = nullptr;
    std::uint64_t delistings_applied_ = 0;
    std::uint64_t events_withheld_ = 0;

    /// Members as of the last event, so a removal is noticed on the event that
    /// makes it true rather than by rescanning the whole ledger every tick.
    std::vector<SymbolId> last_members_;
    Timestamp last_membership_ts_ = kNever;

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
