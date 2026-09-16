#ifndef MARKETSCAN_CORE_ENGINE_HPP
#define MARKETSCAN_CORE_ENGINE_HPP

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "marketscan/book/order_book.hpp"
#include "marketscan/core/clock.hpp"
#include "marketscan/core/event.hpp"
#include "marketscan/core/types.hpp"

namespace marketscan {

/// A stream of market events in non-decreasing timestamp order.
///
/// Ordering is the source's responsibility, not the engine's. A feed that
/// cannot guarantee it must sort before handing anything over: the engine
/// refuses an out-of-order event rather than quietly dispatching it, because
/// a replay that silently reorders is a replay that silently lies.
class IEventSource {
public:
    IEventSource() = default;
    IEventSource(const IEventSource&) = delete;
    IEventSource& operator=(const IEventSource&) = delete;
    IEventSource(IEventSource&&) = delete;
    IEventSource& operator=(IEventSource&&) = delete;
    virtual ~IEventSource() = default;

    /// Writes the next event and returns true, or returns false at end of
    /// stream. A live source returns false when it has nothing *right now*.
    [[nodiscard]] virtual bool next(MarketEvent& out) = 0;

    /// Whether this source can be rewound and replayed. A live transport
    /// cannot, which is what stops the transport panel from offering to scrub
    /// backwards through one.
    ///
    /// Separate from reset() so the engine can check every source before
    /// rewinding any of them, and leave itself untouched when one says no.
    [[nodiscard]] virtual bool can_reset() const noexcept { return false; }

    /// Rewind to the beginning. Only called when can_reset() is true.
    virtual void reset() {}

    [[nodiscard]] virtual std::string_view name() const noexcept { return "source"; }
};

/// Receives events from the engine.
///
/// Host-side and C++; the plugin boundary sits one layer further out, in
/// StrategyInstance, which implements this interface by forwarding through the
/// C vtable. Everything the engine dispatches to goes through here, so there is
/// exactly one dispatch path regardless of where the code came from.
class IEventSink {
public:
    IEventSink() = default;
    IEventSink(const IEventSink&) = delete;
    IEventSink& operator=(const IEventSink&) = delete;
    IEventSink(IEventSink&&) = delete;
    IEventSink& operator=(IEventSink&&) = delete;
    virtual ~IEventSink() = default;

    virtual void on_bar(const MS_Bar&) {}

    virtual void on_quote(const MS_Quote&) {}

    virtual void on_trade(const MS_Trade&) {}

    /// Called after every event has been delivered to every sink, so that work
    /// which must observe the whole tick -- matching resting orders, marking
    /// positions -- happens at a well defined point.
    virtual void on_event_complete(const MarketEvent&) {}
};

struct EngineConfig {
    /// Seeds the per-strategy random streams. Two runs with the same seed and
    /// the same input produce byte-identical order logs; that is a test, not a
    /// hope. See tests/test_determinism.cpp.
    std::uint64_t rng_seed = 0x5EED'1234'ABCD'0001ULL;
};

struct EngineStats {
    std::uint64_t events_dispatched = 0;

    /// Bars specifically. The transport's "step one bar" needs this: quotes
    /// outnumber bars, so stepping events would stop somewhere a user did not
    /// ask for.
    std::uint64_t bars_dispatched = 0;

    Timestamp first_event_ts = 0;
    Timestamp last_event_ts = 0;
    bool started = false;
};

/// The event loop.
///
/// Single threaded by construction. In replay that is the whole story. Live,
/// a feed thread fills an SpscRingBuffer and a source drains it on this
/// thread, so the loop below does not change shape at all -- only where the
/// events came from and which clock is installed.
///
/// Events from several sources are merged into one non-decreasing timestamp
/// order. Ties break by source registration order, which is stable across
/// runs; nothing here iterates a hash container, for the same reason.
class Engine {
public:
    explicit Engine(EngineConfig config = {});
    ~Engine();

    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;
    Engine(Engine&&) = delete;
    Engine& operator=(Engine&&) = delete;

    /// Sources are merged in registration order, which is also the tie-break
    /// when two sources offer the same timestamp.
    void add_source(std::unique_ptr<IEventSource> source);

    /// Sinks are not owned and must outlive the engine. They receive events in
    /// registration order, and that order is part of the determinism contract.
    void add_sink(IEventSink* sink);

    /// Dispatch exactly one event. Returns false once every source is drained.
    bool step();

    /// Dispatch until the sources are drained or `max_events` have been
    /// dispatched. Returns the number dispatched.
    std::uint64_t run(std::uint64_t max_events = UINT64_MAX);

    /// Dispatch until the clock passes `ts`, or the sources drain. The event
    /// exactly at `ts` is included. Returns the number dispatched.
    std::uint64_t run_until(Timestamp ts);

    /// Whether any source still has an event waiting.
    ///
    /// Not const: answering it requires the one-event lookahead to be primed,
    /// and priming reads from the sources. A throttled replay needs this to
    /// tell "nothing is due yet" apart from "there is nothing left", which
    /// otherwise look identical from the outside -- both are simply zero
    /// events dispatched.
    [[nodiscard]] bool has_pending_event();

    /// Timestamp of the next event, or kForever when the sources are drained.
    [[nodiscard]] Timestamp next_event_ts();

    /// Rewind every source and clear the clock, so a run can be repeated from
    /// the beginning. Returns false -- and changes nothing -- if any source
    /// cannot be rewound.
    ///
    /// Sinks are *not* reset: the caller owns their lifetime and their state.
    /// Scrubbing the transport backwards rebuilds them.
    bool reset();

    [[nodiscard]] Timestamp now() const noexcept { return clock_.now(); }

    [[nodiscard]] const SimulatedClock& clock() const noexcept { return clock_; }

    [[nodiscard]] SimulatedClock& clock() noexcept { return clock_; }

    /// Every instrument's book, as of the event currently being dispatched.
    ///
    /// Updated before any sink sees the event, so a strategy asking about the
    /// market inside a callback is told the state as of that event -- and the
    /// fill model prices its order against that same state. That is what makes
    /// "the quote in effect at the signal timestamp" a guarantee rather than a
    /// comment.
    [[nodiscard]] const MarketView& market() const noexcept { return market_; }

    [[nodiscard]] const EngineConfig& config() const noexcept { return config_; }

    [[nodiscard]] const EngineStats& stats() const noexcept { return stats_; }

    [[nodiscard]] std::size_t source_count() const noexcept { return sources_.size(); }

    [[nodiscard]] std::size_t sink_count() const noexcept { return sinks_.size(); }

private:
    /// One source plus its one-event lookahead. The lookahead is what makes
    /// the k-way merge possible without asking a source to peek.
    struct SourceSlot {
        std::unique_ptr<IEventSource> source;
        MarketEvent pending{};
        Timestamp last_ts = kNever;
        bool has_pending = false;
        bool exhausted = false;
    };

    /// Pulls the next event from `slot`, validating that the source has not
    /// gone backwards. Throws std::runtime_error if it has.
    void refill(SourceSlot& slot);

    /// Index of the slot holding the earliest pending event, or npos when
    /// every source is drained.
    [[nodiscard]] std::size_t select_next() const noexcept;

    void dispatch(const MarketEvent& event);

    EngineConfig config_;
    SimulatedClock clock_;
    MarketView market_;
    std::vector<SourceSlot> sources_;
    std::vector<IEventSink*> sinks_;
    EngineStats stats_;
    bool primed_ = false;
};

}  // namespace marketscan

#endif  // MARKETSCAN_CORE_ENGINE_HPP
