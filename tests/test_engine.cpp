#include <memory>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "marketscan/core/engine.hpp"

using namespace marketscan;

namespace {

/// A replayable source backed by a vector, standing in for the feeds that
/// arrive in M2.
class VectorSource final : public IEventSource {
public:
    VectorSource(std::string name, std::vector<MarketEvent> events, bool replayable = true)
        : name_(std::move(name)), events_(std::move(events)), replayable_(replayable) {}

    bool next(MarketEvent& out) override {
        if (cursor_ >= events_.size()) {
            return false;
        }
        out = events_[cursor_++];
        return true;
    }

    [[nodiscard]] bool can_reset() const noexcept override { return replayable_; }

    void reset() override { cursor_ = 0; }

    [[nodiscard]] std::string_view name() const noexcept override { return name_; }

private:
    std::string name_;
    std::vector<MarketEvent> events_;
    std::size_t cursor_ = 0;
    bool replayable_;
};

MarketEvent make_bar(Timestamp ts, SymbolId symbol, double close = 100.0) {
    MS_Bar bar{};
    bar.ts = ts;
    bar.symbol = symbol;
    bar.open = bar.high = bar.low = bar.close = close;
    bar.volume = 1'000.0;
    return MarketEvent::from(bar);
}

MarketEvent make_quote(Timestamp ts, SymbolId symbol) {
    MS_Quote quote{};
    quote.ts = ts;
    quote.symbol = symbol;
    quote.bid_px = 99.0;
    quote.ask_px = 101.0;
    quote.bid_sz = quote.ask_sz = 100.0;
    return MarketEvent::from(quote);
}

/// Records what it was shown and when, including the clock reading at the time
/// each callback ran.
class RecordingSink final : public IEventSink {
public:
    explicit RecordingSink(std::string label, const Engine* engine = nullptr)
        : label_(std::move(label)), engine_(engine) {}

    void on_bar(const MS_Bar& bar) override { record(EventType::Bar, bar.ts, bar.symbol); }

    void on_quote(const MS_Quote& quote) override {
        record(EventType::Quote, quote.ts, quote.symbol);
    }

    void on_trade(const MS_Trade& trade) override {
        record(EventType::Trade, trade.ts, trade.symbol);
    }

    void on_event_complete(const MarketEvent&) override { ++completions; }

    struct Seen {
        EventType type;
        Timestamp ts;
        SymbolId symbol;
        Timestamp clock_at_dispatch;
    };

    std::vector<Seen> seen;
    int completions = 0;

    [[nodiscard]] const std::string& label() const { return label_; }

private:
    void record(EventType type, Timestamp ts, SymbolId symbol) {
        seen.push_back(Seen{type, ts, symbol, engine_ != nullptr ? engine_->now() : 0});
    }

    std::string label_;
    const Engine* engine_;
};

/// Shared across sinks so that dispatch order is observable.
std::vector<std::string>* g_call_order = nullptr;

class OrderWitnessSink final : public IEventSink {
public:
    explicit OrderWitnessSink(std::string label) : label_(std::move(label)) {}

    void on_bar(const MS_Bar&) override {
        if (g_call_order != nullptr) {
            g_call_order->push_back(label_);
        }
    }

private:
    std::string label_;
};

}  // namespace

TEST_CASE("events from one source arrive in order", "[engine]") {
    Engine engine;
    RecordingSink sink("only", &engine);

    engine.add_source(std::make_unique<VectorSource>("s",
                                                     std::vector<MarketEvent>{
                                                         make_bar(100, 1),
                                                         make_quote(200, 1),
                                                         make_bar(300, 1),
                                                     }));
    engine.add_sink(&sink);

    REQUIRE(engine.run() == 3);
    REQUIRE(sink.seen.size() == 3);
    REQUIRE(sink.seen[0].ts == 100);
    REQUIRE(sink.seen[1].ts == 200);
    REQUIRE(sink.seen[2].ts == 300);
    REQUIRE(sink.seen[1].type == EventType::Quote);
    REQUIRE(sink.completions == 3);

    REQUIRE(engine.stats().events_dispatched == 3);
    REQUIRE(engine.stats().first_event_ts == 100);
    REQUIRE(engine.stats().last_event_ts == 300);
}

TEST_CASE("several sources are merged into one timestamp order", "[engine]") {
    Engine engine;
    RecordingSink sink("merged", &engine);

    engine.add_source(std::make_unique<VectorSource>("a",
                                                     std::vector<MarketEvent>{
                                                         make_bar(100, 1),
                                                         make_bar(400, 1),
                                                         make_bar(500, 1),
                                                     }));
    engine.add_source(std::make_unique<VectorSource>("b",
                                                     std::vector<MarketEvent>{
                                                         make_bar(200, 2),
                                                         make_bar(300, 2),
                                                         make_bar(600, 2),
                                                     }));
    engine.add_sink(&sink);

    REQUIRE(engine.run() == 6);

    std::vector<Timestamp> timestamps;
    for (const auto& s : sink.seen) {
        timestamps.push_back(s.ts);
    }
    REQUIRE(timestamps == std::vector<Timestamp>{100, 200, 300, 400, 500, 600});
}

// Ties must break the same way on every run, or the order log is not
// reproducible. Registration order is the rule.
TEST_CASE("simultaneous events break ties by source registration order", "[engine][determinism]") {
    Engine engine;
    RecordingSink sink("ties", &engine);

    engine.add_source(std::make_unique<VectorSource>(
        "first", std::vector<MarketEvent>{make_bar(100, 11), make_bar(100, 12)}));
    engine.add_source(std::make_unique<VectorSource>(
        "second", std::vector<MarketEvent>{make_bar(100, 21), make_bar(100, 22)}));
    engine.add_sink(&sink);

    REQUIRE(engine.run() == 4);

    std::vector<SymbolId> symbols;
    for (const auto& s : sink.seen) {
        symbols.push_back(s.symbol);
    }
    // The first source is drained of everything at this timestamp before the
    // second is consulted, because ties always resolve to the lower index.
    REQUIRE(symbols == std::vector<SymbolId>{11, 12, 21, 22});
}

TEST_CASE("the clock reads the current event's timestamp during dispatch", "[engine][clock]") {
    Engine engine;
    RecordingSink sink("clock", &engine);

    REQUIRE(engine.now() == 0);

    engine.add_source(std::make_unique<VectorSource>(
        "s", std::vector<MarketEvent>{make_bar(1'000, 1), make_bar(2'000, 1)}));
    engine.add_sink(&sink);
    engine.run();

    REQUIRE(sink.seen[0].clock_at_dispatch == 1'000);
    REQUIRE(sink.seen[1].clock_at_dispatch == 2'000);
    REQUIRE(engine.now() == 2'000);
}

TEST_CASE("sinks are called in registration order", "[engine][determinism]") {
    std::vector<std::string> order;
    g_call_order = &order;

    Engine engine;
    OrderWitnessSink first("first");
    OrderWitnessSink second("second");
    OrderWitnessSink third("third");

    engine.add_source(
        std::make_unique<VectorSource>("s", std::vector<MarketEvent>{make_bar(1, 1)}));
    engine.add_sink(&first);
    engine.add_sink(&second);
    engine.add_sink(&third);
    engine.run();

    g_call_order = nullptr;
    REQUIRE(order == std::vector<std::string>{"first", "second", "third"});
}

TEST_CASE("step processes exactly one event", "[engine]") {
    Engine engine;
    RecordingSink sink("stepper", &engine);

    engine.add_source(std::make_unique<VectorSource>(
        "s", std::vector<MarketEvent>{make_bar(10, 1), make_bar(20, 1), make_bar(30, 1)}));
    engine.add_sink(&sink);

    REQUIRE(engine.step());
    REQUIRE(sink.seen.size() == 1);
    REQUIRE(engine.now() == 10);

    REQUIRE(engine.step());
    REQUIRE(engine.step());
    REQUIRE(sink.seen.size() == 3);

    REQUIRE_FALSE(engine.step());
    REQUIRE(sink.seen.size() == 3);
}

TEST_CASE("run_until stops at a timestamp and includes it", "[engine]") {
    Engine engine;
    RecordingSink sink("until", &engine);

    engine.add_source(std::make_unique<VectorSource>("s",
                                                     std::vector<MarketEvent>{
                                                         make_bar(10, 1),
                                                         make_bar(20, 1),
                                                         make_bar(30, 1),
                                                         make_bar(40, 1),
                                                     }));
    engine.add_sink(&sink);

    REQUIRE(engine.run_until(25) == 2);
    REQUIRE(engine.now() == 20);

    REQUIRE(engine.run_until(30) == 1);
    REQUIRE(sink.seen.size() == 3);
    REQUIRE(sink.seen.back().ts == 30);

    REQUIRE(engine.run_until(kForever) == 1);
    REQUIRE(sink.seen.size() == 4);
}

TEST_CASE("run honours an event budget", "[engine]") {
    Engine engine;
    RecordingSink sink("budget", &engine);

    engine.add_source(std::make_unique<VectorSource>("s",
                                                     std::vector<MarketEvent>{
                                                         make_bar(1, 1),
                                                         make_bar(2, 1),
                                                         make_bar(3, 1),
                                                         make_bar(4, 1),
                                                     }));
    engine.add_sink(&sink);

    REQUIRE(engine.run(2) == 2);
    REQUIRE(sink.seen.size() == 2);
    REQUIRE(engine.run() == 2);
    REQUIRE(sink.seen.size() == 4);
}

// A source that goes backwards would push a decreasing timestamp through the
// clock and the ledger. Fail at the seam, loudly.
TEST_CASE("an out-of-order source is rejected", "[engine]") {
    Engine engine;
    RecordingSink sink("bad", &engine);

    engine.add_source(std::make_unique<VectorSource>(
        "scrambled",
        std::vector<MarketEvent>{make_bar(100, 1), make_bar(300, 1), make_bar(200, 1)}));
    engine.add_sink(&sink);

    REQUIRE_THROWS_AS(engine.run(), std::runtime_error);
}

TEST_CASE("a replayable engine rewinds to the start", "[engine][determinism]") {
    Engine engine;
    RecordingSink first("first", &engine);

    engine.add_source(std::make_unique<VectorSource>(
        "s", std::vector<MarketEvent>{make_bar(10, 1), make_bar(20, 1), make_bar(30, 1)}));
    engine.add_sink(&first);

    REQUIRE(engine.run() == 3);
    REQUIRE(engine.reset());
    REQUIRE(engine.now() == 0);
    REQUIRE(engine.stats().events_dispatched == 0);

    // The same input replays identically. Sinks keep their own state, which is
    // why this one has now seen everything twice.
    REQUIRE(engine.run() == 3);
    REQUIRE(first.seen.size() == 6);
    REQUIRE(first.seen[0].ts == first.seen[3].ts);
    REQUIRE(first.seen[1].ts == first.seen[4].ts);
    REQUIRE(first.seen[2].ts == first.seen[5].ts);
}

TEST_CASE("an engine holding a non-replayable source refuses to rewind", "[engine]") {
    Engine engine;

    engine.add_source(std::make_unique<VectorSource>(
        "replayable", std::vector<MarketEvent>{make_bar(10, 1)}, true));
    engine.add_source(
        std::make_unique<VectorSource>("live", std::vector<MarketEvent>{make_bar(20, 1)}, false));

    REQUIRE(engine.run() == 2);
    REQUIRE_FALSE(engine.reset());

    // Refusing left the engine alone rather than half rewound.
    REQUIRE(engine.stats().events_dispatched == 2);
    REQUIRE(engine.now() == 20);
}

TEST_CASE("an engine with no sources finishes immediately", "[engine]") {
    Engine engine;
    REQUIRE_FALSE(engine.step());
    REQUIRE(engine.run() == 0);
    REQUIRE(engine.stats().events_dispatched == 0);
}

TEST_CASE("null sources and sinks are rejected", "[engine]") {
    Engine engine;
    REQUIRE_THROWS_AS(engine.add_source(nullptr), std::invalid_argument);
    REQUIRE_THROWS_AS(engine.add_sink(nullptr), std::invalid_argument);
}

// --------------------------------------------------------------------------
// The backtest/live seam
// --------------------------------------------------------------------------
//
// "A strategy cannot tell whether it is being backtested or trading live"
// rested for nine milestones on there being no code that branched on it. True,
// and unfalsifiable: with one clock type hard-wired into the engine there was
// nothing to swap and nowhere to put it, so the property could not be shown
// either way.
//
// Engine is now EngineT<SimulatedClock> and LiveEngine is EngineT<SystemClock>,
// both instantiated in engine.cpp. The event loop is one body of code compiled
// against both, and this is where the second one is driven -- with a canned
// source, because a live transport does not exist yet and inventing one to
// test the clock would be testing the invention.

TEST_CASE("the same engine runs on a wall clock", "[engine][seam]") {
    const SymbolId spy = 1;
    std::vector<MarketEvent> events;
    for (int i = 0; i < 20; ++i) {
        events.push_back(make_bar(1'000 + i * 100, spy, 100.0 + i));
    }

    LiveEngine engine;
    engine.add_source(std::make_unique<VectorSource>("canned", events));

    RecordingSink sink("live");
    engine.add_sink(&sink);

    const std::uint64_t dispatched = engine.run();

    // The loop behaves identically: every event, in order, once.
    REQUIRE(dispatched == 20);
    REQUIRE(sink.seen.size() == 20);
    REQUIRE(sink.seen.front().ts == 1'000);
    REQUIRE(sink.seen.back().ts == 1'000 + 19 * 100);

    // And the clock is genuinely a different one. A simulated clock would be
    // sitting on the last event's timestamp, which is 2,900 nanoseconds after
    // the Unix epoch; a wall clock is somewhere in this century.
    REQUIRE(engine.now() > 1'600'000'000LL * 1'000'000'000LL);
    REQUIRE(engine.now() != sink.seen.back().ts);
}

TEST_CASE("a wall-clock engine refuses to rewind a source that cannot", "[engine][seam]") {
    // The reset path is the one place the two clocks behave differently --
    // there is no beginning for a wall clock to go back to -- so it is worth
    // knowing the rest of reset() still does what it says.
    LiveEngine engine;
    std::vector<MarketEvent> one{make_bar(1'000, 1)};
    engine.add_source(std::make_unique<VectorSource>("live", one, /*replayable=*/false));

    RecordingSink sink("live");
    engine.add_sink(&sink);
    REQUIRE(engine.run() == 1);

    REQUIRE_FALSE(engine.reset());
    REQUIRE(engine.stats().events_dispatched == 1);
}
