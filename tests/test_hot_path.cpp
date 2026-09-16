// The hot-path allocation budget, measured rather than asserted in a comment.
//
// The specification forbids allocating, locking a mutex or making a syscall on
// the per-event path. Through Spec 1 that rule was enforced by reading the
// code, which is a method that works right up until it does not: push_back is
// four characters and amortised O(1), and nothing about writing one looks like
// a violation.
//
// So this file counts. It replaces global operator new and delete, arms the
// counter around a measured region, and reports the number. Two claims come
// out of it:
//
//   - the engine's dispatch path -- select, advance the clock, apply to the
//     book, hand to every sink -- allocates exactly zero times per event, and
//   - a strategy's path allocates only where it is storing a result that has
//     to outlive the event (an order, a fill, an equity sample), and the count
//     per event falls towards zero as the containers stop growing.
//
// The second is deliberately not asserted as zero. A blotter that records
// twenty thousand fills has to put them somewhere, and pretending otherwise
// would mean either an arbitrary preallocated cap or a test that lies. What
// is asserted is that the cost is amortised: a second pass over the same
// events must allocate strictly less than the first.

#include <cstddef>
#include <cstdlib>
#include <memory>
#include <new>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "marketscan/core/engine.hpp"
#include "marketscan/core/symbol_table.hpp"
#include "marketscan/data/synthetic_feed.hpp"
#include "marketscan/exec/fill_model.hpp"
#include "marketscan/strategy/istrategy.hpp"
#include "marketscan/strategy/registry.hpp"
#include "marketscan/strategy/strategy_instance.hpp"

using namespace marketscan;

// --------------------------------------------------------------------------
// The counter
// --------------------------------------------------------------------------
//
// Not atomic, and deliberately so: everything measured here runs on one
// thread, and an atomic increment on the allocation path would be a cost the
// measurement itself imposed. Catch2 allocates freely around the measured
// region, which is why the counter is armed rather than read from zero.

namespace {

std::size_t g_allocations = 0;
bool g_counting = false;

/// Counts allocations inside its scope, and never past the end of it.
class AllocationCounter {
public:
    AllocationCounter() {
        g_allocations = 0;
        g_counting = true;
    }

    AllocationCounter(const AllocationCounter&) = delete;
    AllocationCounter& operator=(const AllocationCounter&) = delete;
    AllocationCounter(AllocationCounter&&) = delete;
    AllocationCounter& operator=(AllocationCounter&&) = delete;

    ~AllocationCounter() { g_counting = false; }

    /// Stops counting and returns the total. Safe to call before the scope
    /// ends, so a REQUIRE cannot itself be counted.
    std::size_t stop() noexcept {
        g_counting = false;
        return g_allocations;
    }
};

}  // namespace

// Replacing the global operator new is defined behaviour and is how this is
// meant to be done; the standard reserves exactly these signatures for it.
// Every overload has to be replaced together, or an allocation that goes
// through the one left behind is invisible to the count.
void* operator new(std::size_t size) {
    if (g_counting) {
        ++g_allocations;
    }
    if (size == 0) {
        size = 1;
    }
    void* memory = std::malloc(size);
    if (memory == nullptr) {
        throw std::bad_alloc{};
    }
    return memory;
}

void* operator new[](std::size_t size) {
    return operator new(size);
}

void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    if (g_counting) {
        ++g_allocations;
    }
    return std::malloc(size == 0 ? 1 : size);
}

void* operator new[](std::size_t size, const std::nothrow_t& tag) noexcept {
    return operator new(size, tag);
}

void operator delete(void* memory) noexcept {
    std::free(memory);
}

void operator delete[](void* memory) noexcept {
    std::free(memory);
}

void operator delete(void* memory, std::size_t) noexcept {
    std::free(memory);
}

void operator delete[](void* memory, std::size_t) noexcept {
    std::free(memory);
}

void operator delete(void* memory, const std::nothrow_t&) noexcept {
    std::free(memory);
}

void operator delete[](void* memory, const std::nothrow_t&) noexcept {
    std::free(memory);
}

// --------------------------------------------------------------------------
// Fixtures
// --------------------------------------------------------------------------

namespace {

/// Proves the counter is measuring what it claims to.
///
/// Not asserted as exactly one. MSVC's debug allocator gives a vector a second
/// allocation for the checked-iterator bookkeeping, so "one buffer, one
/// allocation" is a Release-only fact and asserting it would make this
/// calibration test fail for a reason that has nothing to do with the thing
/// being calibrated.
TEST_CASE("the allocation counter sees an allocation", "[hotpath]") {
    AllocationCounter counter;
    std::vector<int> values;
    values.reserve(64);
    const std::size_t seen = counter.stop();

    REQUIRE(seen >= 1);
    REQUIRE(values.capacity() >= 64);
}

TEST_CASE("the allocation counter sees nothing when nothing allocates", "[hotpath]") {
    int total = 0;
    AllocationCounter counter;
    for (int i = 0; i < 1'000; ++i) {
        total += i;
    }
    const std::size_t seen = counter.stop();

    REQUIRE(seen == 0);
    REQUIRE(total == 499'500);
}

/// A sink that does the minimum an event sink can do.
///
/// Stands in for "the engine's own dispatch cost", with no strategy, no
/// broker and no ledger in the measurement.
class TallySink final : public IEventSink {
public:
    void on_bar(const MS_Bar& bar) override {
        (void)bar;
        ++bars_;
    }

    void on_quote(const MS_Quote& quote) override {
        (void)quote;
        ++quotes_;
    }

    void on_trade(const MS_Trade& trade) override {
        (void)trade;
        ++trades_;
    }

    [[nodiscard]] long long total() const noexcept { return bars_ + quotes_ + trades_; }

private:
    long long bars_ = 0;
    long long quotes_ = 0;
    long long trades_ = 0;
};

[[nodiscard]] SyntheticFeedConfig bars(std::uint32_t per_session, std::uint32_t sessions) {
    SyntheticFeedConfig config;
    config.instruments = {SyntheticInstrument{"SPY", 500.0, 0.05, 0.15, 100'000.0}};
    config.bars_per_session = per_session;
    config.sessions = sessions;
    config.seed = 7;
    return config;
}

/// A strategy that trades on a schedule, so the measured path includes order
/// submission, the fill model and the ledger rather than dispatch alone.
class Metronome final : public marketscan::IStrategy {
public:
    static constexpr const char* kDescription = "Buys and sells on a fixed cadence.";

    void on_bar(const MS_Bar& bar) override {
        ++bars_;
        if (bars_ % 20 != 0) {
            return;
        }
        submit_market(bar.symbol, long_ ? MS_SIDE_SELL : MS_SIDE_BUY, 10.0);
        long_ = !long_;
    }

private:
    long long bars_ = 0;
    bool long_ = false;
};

}  // namespace

MARKETSCAN_REGISTER_STRATEGY(Metronome, "metronome", "Metronome", "test")

// --------------------------------------------------------------------------
// The claims
// --------------------------------------------------------------------------

TEST_CASE("the engine's dispatch path allocates nothing per event", "[hotpath][engine]") {
    SymbolTable symbols;
    Engine engine;
    engine.add_source(std::make_unique<SyntheticFeed>(bars(390, 6), symbols));

    TallySink sink;
    engine.add_sink(&sink);

    // Prime outside the measurement. The first step pulls each source's first
    // event, which is a feed's own business and is allowed to allocate.
    REQUIRE(engine.step());

    std::size_t seen = 0;
    std::uint64_t dispatched = 0;
    {
        AllocationCounter counter;
        dispatched = engine.run();
        seen = counter.stop();
    }

    INFO("dispatched " << dispatched << " events");
    REQUIRE(dispatched > 1'000);
    REQUIRE(seen == 0);
    REQUIRE(sink.total() > 0);
}

TEST_CASE("a strategy's path allocates only to keep what outlives the event",
          "[hotpath][strategy]") {
    SymbolTable symbols;
    Engine engine;
    engine.add_source(std::make_unique<SyntheticFeed>(bars(390, 6), symbols));

    const SpreadCrossingFillModel fill_model;
    StrategyRuntimeConfig config;
    config.starting_cash = Money::from_dollars(1'000'000);

    StrategyInstance theory("metronome",
                            "Metronome",
                            StrategyRegistry::instance().create("metronome"),
                            {},
                            config,
                            &fill_model,
                            &engine.market(),
                            &symbols,
                            &engine.clock());
    engine.add_sink(&theory);
    theory.start();

    REQUIRE(engine.step());

    std::size_t seen = 0;
    std::uint64_t dispatched = 0;
    {
        AllocationCounter counter;
        dispatched = engine.run();
        seen = counter.stop();
    }
    theory.stop();

    // The numbers, in the output, because the point of this test is the
    // measurement and not only the inequality.
    INFO("dispatched " << dispatched << " events");
    INFO("allocations " << seen);
    INFO("trades " << theory.ledger().blotter().size());
    REQUIRE(dispatched > 1'000);
    REQUIRE(theory.ledger().blotter().size() > 10);

    // Far below one per event. A per-event allocation -- a std::string built
    // for a log line, a vector rebuilt to answer a query -- would land here
    // with a count in the thousands rather than the dozens.
    REQUIRE(seen * 20 < dispatched);
}

TEST_CASE("the second pass over the same events allocates less than the first",
          "[hotpath][strategy]") {
    SymbolTable symbols;
    Engine engine;
    engine.add_source(std::make_unique<SyntheticFeed>(bars(390, 3), symbols));

    const SpreadCrossingFillModel fill_model;
    StrategyRuntimeConfig config;
    config.starting_cash = Money::from_dollars(1'000'000);

    StrategyInstance theory("metronome",
                            "Metronome",
                            StrategyRegistry::instance().create("metronome"),
                            {},
                            config,
                            &fill_model,
                            &engine.market(),
                            &symbols,
                            &engine.clock());
    engine.add_sink(&theory);
    theory.start();

    REQUIRE(engine.step());

    std::size_t first = 0;
    {
        AllocationCounter counter;
        engine.run();
        first = counter.stop();
    }

    // Rewind and replay the identical events. Nothing new is learned about the
    // market; what changes is that every container is already large enough.
    REQUIRE(engine.reset());
    REQUIRE(engine.step());

    std::size_t second = 0;
    {
        AllocationCounter counter;
        engine.run();
        second = counter.stop();
    }
    theory.stop();

    INFO("first pass " << first << ", second pass " << second);
    REQUIRE(first > 0);
    REQUIRE(second < first);
}
