// Submit-to-fill latency, partial fills, and the cancel/fill race.
//
// The ABI has always had the right shape -- submit returns an id, on_fill is a
// separate callback -- but a simulator that answers instantly means no
// strategy written against this engine has ever experienced a pending order.
// Every one of them has been trained on an assumption that breaks the first
// time it trades anywhere real.
//
// So: an accepted order can now be in flight, exposed to the market the whole
// time and priced when it arrives rather than when it was sent; a large order
// can arrive in pieces; and a cancel can lose a race it used to always win by
// default. Everything here is opt-in, and everything here is deterministic --
// the latency is in simulated nanoseconds and the jitter comes out of the
// run's seeded generator, because a delay measured from the machine would make
// a replay depend on how busy the machine was.

#include <cmath>
#include <cstdint>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "marketscan/abi/abi_compat.hpp"
#include "marketscan/book/order_book.hpp"
#include "marketscan/core/random.hpp"
#include "marketscan/exec/broker.hpp"
#include "marketscan/exec/fill_model.hpp"

using namespace marketscan;

namespace {

constexpr Duration kMillis = 1'000'000;

/// A market whose book one test can move by hand.
///
/// Latency only means something if the price can change while an order is in
/// the air, so the ability to move it between events is the fixture.
class Market {
public:
    explicit Market(SymbolId symbol = 1) : symbol_(symbol) {}

    void quote(Price bid, Quantity bid_size, Price ask, Quantity ask_size) {
        MarketEvent event;
        event.type = EventType::Quote;
        event.quote.ts = ts_;
        event.quote.symbol = symbol_;
        event.quote.bid_px = bid;
        event.quote.ask_px = ask;
        event.quote.bid_sz = bid_size;
        event.quote.ask_sz = ask_size;
        view_.apply(event);
    }

    void advance(Duration by) noexcept { ts_ += by; }

    [[nodiscard]] Timestamp now() const noexcept { return ts_; }

    [[nodiscard]] SymbolId symbol() const noexcept { return symbol_; }

    [[nodiscard]] const MarketView& view() const noexcept { return view_; }

private:
    SymbolId symbol_;
    MarketView view_;
    Timestamp ts_ = 1'700'000'000LL * 1'000'000'000LL;
};

[[nodiscard]] MS_OrderRequest market_order(SymbolId symbol, Side side, Quantity quantity) {
    MS_OrderRequest request{};
    request.symbol = symbol;
    request.side = side;
    request.type = MS_ORD_MARKET;
    request.tif = MS_TIF_DAY;
    request.quantity = quantity;
    return request;
}

/// Records every fill the broker emits, in order.
class FillLog final : public IFillListener {
public:
    void on_fill(const MS_Fill& fill) override { fills.push_back(fill); }

    [[nodiscard]] Quantity total_quantity() const noexcept {
        Quantity total = 0.0;
        for (const MS_Fill& fill : fills) {
            total += fill.quantity;
        }
        return total;
    }

    /// The price one fill of the whole quantity would have had to be for the
    /// cash to come out the same.
    [[nodiscard]] Price weighted_price() const noexcept {
        double gross = 0.0;
        Quantity total = 0.0;
        for (const MS_Fill& fill : fills) {
            gross += fill.price * fill.quantity;
            total += fill.quantity;
        }
        return total > 0.0 ? gross / total : 0.0;
    }

    std::vector<MS_Fill> fills;
};

[[nodiscard]] FillModelConfig bare() {
    FillModelConfig config;
    config.commission = CommissionSchedule{};
    config.commission.per_share = Money{};
    return config;
}

}  // namespace

// --------------------------------------------------------------------------
// Latency
// --------------------------------------------------------------------------

TEST_CASE("with no latency configured nothing changes", "[exec][latency]") {
    // The compatibility floor. Every result this project has published was
    // produced with an instant simulator, and the default must still be it.
    Market market;
    market.quote(99.95, 500, 100.05, 500);

    const SpreadCrossingFillModel model(bare());
    REQUIRE_FALSE(model.config().models_latency());

    Broker broker(Money::from_dollars(1'000'000), &model, &market.view());
    FillLog log;
    broker.set_fill_listener(&log);

    const OrderId id = broker.submit(market_order(market.symbol(), MS_SIDE_BUY, 100), market.now());

    REQUIRE(id != kInvalidOrderId);
    REQUIRE(log.fills.size() == 1);
    REQUIRE(broker.find_order(id)->status == OrderStatus::Filled);
    REQUIRE(broker.in_flight_count(market.now()) == 0);
}

TEST_CASE("an order with latency does not fill on the event that sent it", "[exec][latency]") {
    Market market;
    market.quote(99.95, 500, 100.05, 500);

    FillModelConfig config = bare();
    config.submit_latency = 50 * kMillis;
    const SpreadCrossingFillModel model(config);

    Broker broker(Money::from_dollars(1'000'000), &model, &market.view());
    FillLog log;
    broker.set_fill_listener(&log);

    const OrderId id = broker.submit(market_order(market.symbol(), MS_SIDE_BUY, 100), market.now());

    // Accepted, alive, and not filled. That combination is new, and it is the
    // state every live order spends its first moments in.
    REQUIRE(id != kInvalidOrderId);
    REQUIRE(log.fills.empty());
    REQUIRE(broker.find_order(id)->status == OrderStatus::Working);
    REQUIRE(broker.in_flight_count(market.now()) == 1);

    // An event before it arrives changes nothing.
    market.advance(20 * kMillis);
    market.quote(99.95, 500, 100.05, 500);
    broker.on_market_event(market.symbol(), market.now());
    REQUIRE(log.fills.empty());
    REQUIRE(broker.in_flight_count(market.now()) == 1);

    // The first event at or after the arrival time fills it.
    market.advance(40 * kMillis);
    market.quote(99.95, 500, 100.05, 500);
    broker.on_market_event(market.symbol(), market.now());

    REQUIRE(log.fills.size() == 1);
    REQUIRE(broker.find_order(id)->status == OrderStatus::Filled);
    REQUIRE(broker.in_flight_count(market.now()) == 0);
}

// The reason latency is worth simulating at all.
TEST_CASE("an order in flight is exposed to the market", "[exec][latency]") {
    Market market;
    market.quote(99.95, 500, 100.05, 500);

    FillModelConfig config = bare();
    config.submit_latency = 50 * kMillis;
    const SpreadCrossingFillModel model(config);

    Broker broker(Money::from_dollars(1'000'000), &model, &market.view());
    FillLog log;
    broker.set_fill_listener(&log);

    const Price ask_when_sent = 100.05;
    broker.submit(market_order(market.symbol(), MS_SIDE_BUY, 100), market.now());

    // The market runs away while the order is crossing the wire.
    market.advance(60 * kMillis);
    market.quote(100.45, 500, 100.55, 500);
    broker.on_market_event(market.symbol(), market.now());

    REQUIRE(log.fills.size() == 1);

    // Filled at the price on arrival, not the price on the screen when the
    // decision was made. An instant simulator hands the strategy the second
    // one for free, every time, and calls the difference alpha.
    REQUIRE(log.fills.front().price == Catch::Approx(100.55));
    REQUIRE(log.fills.front().price > ask_when_sent);
}

// Regression: before this, an in-flight order the cash check refused on arrival
// was marked Rejected and nothing else -- no status event and no count -- so a
// strategy waiting on it waited forever. Found by the research job's
// drawdown_entry cross-test (tests/test_research_rule.cpp), whose strategy
// stopped trading after the first gap-up.
TEST_CASE("an order refused on arrival is reported and counted", "[exec][latency][reject]") {
    Market market;
    market.quote(99.95, 500, 100.05, 500);

    FillModelConfig config = bare();
    config.submit_latency = 50 * kMillis;
    const SpreadCrossingFillModel model(config);

    // Enough for 100 shares at the ask when sent, not after the gap.
    Broker broker(Money::from_dollars(10'100), &model, &market.view());

    class StatusLog final : public IFillListener {
    public:
        void on_fill(const MS_Fill&) override { ++fills; }

        void on_order_status(const MS_OrderStatus& status) override { statuses.push_back(status); }

        int fills = 0;
        std::vector<MS_OrderStatus> statuses;
    } log;

    broker.set_fill_listener(&log);

    const OrderId id = broker.submit(market_order(market.symbol(), MS_SIDE_BUY, 100), market.now());
    REQUIRE(id != kInvalidOrderId);
    REQUIRE(broker.rejected_count() == 0);

    market.advance(60 * kMillis);
    market.quote(102.95, 500, 103.05, 500);
    broker.on_market_event(market.symbol(), market.now());

    CHECK(log.fills == 0);
    CHECK(broker.find_order(id)->status == OrderStatus::Rejected);
    CHECK(broker.rejected_count() == 1);
    CHECK(broker.in_flight_count(market.now()) == 0);
    REQUIRE(log.statuses.size() == 1);
    CHECK(log.statuses[0].order_id == id);
    CHECK(log.statuses[0].state == MS_ORDER_REJECTED);
    CHECK(log.statuses[0].terminal != 0);
    CHECK(log.statuses[0].filled_quantity == 0.0);
}

// The other way an order can be refused on arrival: nothing on its side of the
// book to cross. It was always reported; it was not always counted.
TEST_CASE("an order with nothing to fill against on arrival is counted as refused",
          "[exec][latency][reject]") {
    Market market;
    market.quote(99.95, 500, 100.05, 500);

    FillModelConfig config = bare();
    config.submit_latency = 50 * kMillis;
    const SpreadCrossingFillModel model(config);
    Broker broker(Money::from_dollars(1'000'000), &model, &market.view());

    const OrderId id = broker.submit(market_order(market.symbol(), MS_SIDE_BUY, 100), market.now());
    REQUIRE(id != kInvalidOrderId);

    // The offer is gone by the time the order arrives.
    market.advance(60 * kMillis);
    market.quote(99.95, 500, 0.0, 0.0);
    broker.on_market_event(market.symbol(), market.now());

    CHECK(broker.find_order(id)->status == OrderStatus::Rejected);
    CHECK(broker.rejected_count() == 1);
}

TEST_CASE("latency is deterministic, jitter included", "[exec][latency][determinism]") {
    const auto run = [](std::uint64_t seed) {
        Market market;
        market.quote(99.95, 500, 100.05, 500);

        FillModelConfig config = bare();
        config.submit_latency = 10 * kMillis;
        config.latency_jitter = 40 * kMillis;
        const SpreadCrossingFillModel model(config);

        DeterministicRng rng(seed);
        Broker broker(Money::from_dollars(1'000'000), &model, &market.view());
        broker.set_jitter_source(&rng);
        FillLog log;
        broker.set_fill_listener(&log);

        std::vector<Timestamp> eligible;
        for (int i = 0; i < 40; ++i) {
            const OrderId id = broker.submit(
                market_order(market.symbol(), i % 2 == 0 ? MS_SIDE_BUY : MS_SIDE_SELL, 10),
                market.now());
            if (id != kInvalidOrderId) {
                eligible.push_back(broker.find_order(id)->eligible_ts);
            }
            market.advance(7 * kMillis);
            market.quote(99.95, 500, 100.05, 500);
            broker.on_market_event(market.symbol(), market.now());
        }
        return eligible;
    };

    const std::vector<Timestamp> first = run(12345);
    const std::vector<Timestamp> second = run(12345);
    const std::vector<Timestamp> different = run(999);

    REQUIRE(first.size() == 40);
    REQUIRE(first == second);

    // Not vacuous twice over: the jitter has to actually vary, and it has to
    // depend on the seed. A jitter that quietly returned zero would satisfy
    // "the same seed replays" and nothing else.
    REQUIRE(first != different);
    bool varies = false;
    for (std::size_t i = 1; i < first.size(); ++i) {
        varies = varies || (first[i] - first[i - 1] != first[1] - first[0]);
    }
    REQUIRE(varies);
}

TEST_CASE("jitter stays inside its bound", "[exec][latency]") {
    Market market;
    market.quote(99.95, 500, 100.05, 500);

    FillModelConfig config = bare();
    config.submit_latency = 10 * kMillis;
    config.latency_jitter = 40 * kMillis;
    const SpreadCrossingFillModel model(config);

    DeterministicRng rng(7);
    Broker broker(Money::from_dollars(10'000'000), &model, &market.view());
    broker.set_jitter_source(&rng);

    for (int i = 0; i < 500; ++i) {
        const Timestamp sent = market.now();
        const OrderId id = broker.submit(market_order(market.symbol(), MS_SIDE_BUY, 1), sent);
        REQUIRE(id != kInvalidOrderId);
        const Duration flight = broker.find_order(id)->eligible_ts - sent;
        INFO("order " << i << " flight " << flight);
        REQUIRE(flight >= 10 * kMillis);
        REQUIRE(flight < 50 * kMillis);
    }
}

// --------------------------------------------------------------------------
// Partial fills
// --------------------------------------------------------------------------

TEST_CASE("a large order arrives in pieces", "[exec][partial]") {
    Market market;
    market.quote(99.95, 100, 100.05, 100);

    FillModelConfig config = bare();
    config.max_participation = 1.0;  // at most the displayed size per event
    config.impact_coefficient_bps = 0.0;
    const SpreadCrossingFillModel model(config);

    Broker broker(Money::from_dollars(1'000'000), &model, &market.view());
    FillLog log;
    broker.set_fill_listener(&log);

    const OrderId id = broker.submit(market_order(market.symbol(), MS_SIDE_BUY, 350), market.now());
    REQUIRE(id != kInvalidOrderId);

    // One event, one hundred shares, and the rest still working -- which is
    // what a market order for three and a half times the displayed size
    // actually does.
    REQUIRE(log.fills.size() == 1);
    REQUIRE(log.fills.front().quantity == Catch::Approx(100.0));
    REQUIRE(broker.find_order(id)->status == OrderStatus::PartiallyFilled);

    for (int i = 0; i < 4; ++i) {
        market.advance(kMillis);
        market.quote(99.95, 100, 100.05, 100);
        broker.on_market_event(market.symbol(), market.now());
    }

    REQUIRE(broker.find_order(id)->status == OrderStatus::Filled);
    REQUIRE(log.fills.size() == 4);
    REQUIRE(log.total_quantity() == Catch::Approx(350.0));

    // Each piece is its own callback with its own quantity. Emitting one fill
    // and adjusting it afterwards would give a strategy a fill that was never
    // true when it was delivered.
    REQUIRE(log.fills[0].quantity == Catch::Approx(100.0));
    REQUIRE(log.fills[3].quantity == Catch::Approx(50.0));
}

// The reconciliation the specification asks for by name.
TEST_CASE("partials reconcile to one fill at the weighted average", "[exec][partial][ledger]") {
    // The two runs differ in how much size is *shown*, not in the price of
    // it. Both fill 350 shares at 100.05 and neither reaches the impact curve;
    // one does it in four prints and the other in one. That is the comparison
    // the claim is about, and showing 100 in both would compare four fills
    // against an order that could not finish.
    const auto final_state = [](bool partials) {
        Market market;
        market.quote(99.95, 1'000, 100.05, partials ? 100 : 1'000);

        FillModelConfig config = bare();
        config.impact_coefficient_bps = 0.0;
        config.max_participation = partials ? 1.0 : 0.0;
        const SpreadCrossingFillModel model(config);

        Broker broker(Money::from_dollars(1'000'000), &model, &market.view());
        FillLog log;
        broker.set_fill_listener(&log);

        broker.submit(market_order(market.symbol(), MS_SIDE_BUY, 350), market.now());
        for (int i = 0; i < 6; ++i) {
            market.advance(kMillis);
            market.quote(99.95, 1'000, 100.05, partials ? 100 : 1'000);
            broker.on_market_event(market.symbol(), market.now());
        }
        struct Outcome {
            Money cash;
            Quantity position;
            std::size_t fills;
            Price average;
        };
        return Outcome{broker.ledger().cash(),
                       broker.ledger().position(market.symbol()),
                       log.fills.size(),
                       log.weighted_price()};
    };

    const auto in_pieces = final_state(true);
    const auto in_one = final_state(false);

    // Four fills against one, so the comparison is not between two identical
    // runs.
    REQUIRE(in_pieces.fills == 4);
    REQUIRE(in_one.fills == 1);

    // Both bought the whole 350.
    REQUIRE(in_pieces.position == Catch::Approx(350.0));
    REQUIRE(in_one.position == Catch::Approx(350.0));

    // The book never moved, so the pieces average to the same price and the
    // ledger has to land in exactly the same place -- to the micro-dollar,
    // not to within a tolerance. An averaging error of a hundredth of a cent
    // per share is invisible in a chart and is still money.
    REQUIRE(in_pieces.average == Catch::Approx(in_one.average));
    REQUIRE(in_pieces.cash.micros() == in_one.cash.micros());
}

TEST_CASE("the ledger's average cost follows the pieces, not the first one",
          "[exec][partial][ledger]") {
    Market market;
    market.quote(99.95, 100, 100.05, 100);

    FillModelConfig config = bare();
    config.impact_coefficient_bps = 0.0;
    config.max_participation = 1.0;
    const SpreadCrossingFillModel model(config);

    Broker broker(Money::from_dollars(1'000'000), &model, &market.view());
    FillLog log;
    broker.set_fill_listener(&log);

    broker.submit(market_order(market.symbol(), MS_SIDE_BUY, 200), market.now());

    // The price moves between the two pieces, so a ledger that used the first
    // fill's price for the whole order would be visibly wrong.
    market.advance(kMillis);
    market.quote(100.95, 100, 101.05, 100);
    broker.on_market_event(market.symbol(), market.now());

    REQUIRE(log.fills.size() == 2);
    REQUIRE(log.fills[0].price == Catch::Approx(100.05));
    REQUIRE(log.fills[1].price == Catch::Approx(101.05));

    const Position* position = broker.ledger().find_position(market.symbol());
    REQUIRE(position != nullptr);
    REQUIRE(position->quantity == Catch::Approx(200.0));
    REQUIRE(position->average_cost() == Catch::Approx(100.55));
    REQUIRE(broker.ledger().check_invariant());
}

// --------------------------------------------------------------------------
// Cancels
// --------------------------------------------------------------------------

TEST_CASE("cancelling a part-filled order keeps the fills", "[exec][cancel]") {
    Market market;
    market.quote(99.95, 100, 100.05, 100);

    FillModelConfig config = bare();
    config.impact_coefficient_bps = 0.0;
    config.max_participation = 1.0;
    const SpreadCrossingFillModel model(config);

    Broker broker(Money::from_dollars(1'000'000), &model, &market.view());
    FillLog log;
    broker.set_fill_listener(&log);

    const OrderId id = broker.submit(market_order(market.symbol(), MS_SIDE_BUY, 500), market.now());
    REQUIRE(log.fills.size() == 1);

    market.advance(kMillis);
    REQUIRE(broker.cancel(id, market.now()));

    const Order* order = broker.find_order(id);
    REQUIRE(order->status == OrderStatus::Cancelled);
    REQUIRE(order->filled_quantity == Catch::Approx(100.0));

    // The hundred shares are owned. A cancel reaches the remainder and
    // nothing else; no instruction sent afterwards makes a trade not have
    // happened.
    REQUIRE(broker.ledger().position(market.symbol()) == Catch::Approx(100.0));
    REQUIRE(broker.ledger().check_invariant());

    // And the remainder really is gone: later events do not resurrect it.
    market.advance(kMillis);
    market.quote(99.95, 100, 100.05, 100);
    broker.on_market_event(market.symbol(), market.now());
    REQUIRE(log.fills.size() == 1);
}

TEST_CASE("a cancel that beats the fill wins", "[exec][cancel][latency]") {
    Market market;
    market.quote(99.95, 500, 100.05, 500);

    FillModelConfig config = bare();
    config.submit_latency = 50 * kMillis;
    const SpreadCrossingFillModel model(config);

    Broker broker(Money::from_dollars(1'000'000), &model, &market.view());
    FillLog log;
    broker.set_fill_listener(&log);

    const OrderId id = broker.submit(market_order(market.symbol(), MS_SIDE_BUY, 100), market.now());

    // Cancelled while still crossing the wire.
    market.advance(10 * kMillis);
    REQUIRE(broker.cancel(id, market.now()));

    market.advance(100 * kMillis);
    market.quote(99.95, 500, 100.05, 500);
    broker.on_market_event(market.symbol(), market.now());

    REQUIRE(log.fills.empty());
    REQUIRE(broker.find_order(id)->status == OrderStatus::Cancelled);
    REQUIRE(broker.ledger().position(market.symbol()) == Catch::Approx(0.0));
    REQUIRE(broker.late_cancel_count() == 0);
}

TEST_CASE("a cancel that loses the race is a counted no-op", "[exec][cancel][latency]") {
    Market market;
    market.quote(99.95, 500, 100.05, 500);

    FillModelConfig config = bare();
    config.submit_latency = 50 * kMillis;
    const SpreadCrossingFillModel model(config);

    Broker broker(Money::from_dollars(1'000'000), &model, &market.view());
    FillLog log;
    broker.set_fill_listener(&log);

    const OrderId id = broker.submit(market_order(market.symbol(), MS_SIDE_BUY, 100), market.now());

    // The order arrives and fills first.
    market.advance(60 * kMillis);
    market.quote(99.95, 500, 100.05, 500);
    broker.on_market_event(market.symbol(), market.now());
    REQUIRE(log.fills.size() == 1);

    // The cancel turns up afterwards, and changes nothing.
    market.advance(kMillis);
    REQUIRE_FALSE(broker.cancel(id, market.now()));

    REQUIRE(broker.find_order(id)->status == OrderStatus::Filled);
    REQUIRE(broker.ledger().position(market.symbol()) == Catch::Approx(100.0));

    // Counted rather than swallowed: live this is a race that happened, and a
    // strategy losing it routinely is one whose logic assumes a speed it does
    // not have.
    REQUIRE(broker.late_cancel_count() == 1);
}

// The property the specification asks to be tested explicitly.
TEST_CASE("exactly one of filled or cancelled happens, whenever the cancel lands",
          "[exec][cancel][latency]") {
    // The cancel is walked across the whole flight window and past it, so it
    // lands before, exactly on, and after the arrival. At no offset may both
    // happen, and at none may neither.
    for (Duration offset = 0; offset <= 120 * kMillis; offset += 10 * kMillis) {
        Market market;
        market.quote(99.95, 500, 100.05, 500);

        FillModelConfig config = bare();
        config.submit_latency = 50 * kMillis;
        const SpreadCrossingFillModel model(config);

        Broker broker(Money::from_dollars(1'000'000), &model, &market.view());
        FillLog log;
        broker.set_fill_listener(&log);

        const Timestamp sent = market.now();
        const OrderId id = broker.submit(market_order(market.symbol(), MS_SIDE_BUY, 100), sent);

        // Events every 10ms; the cancel is issued when the clock reaches the
        // offset under test.
        bool cancelled_here = false;
        for (Duration elapsed = 10 * kMillis; elapsed <= 130 * kMillis; elapsed += 10 * kMillis) {
            market.advance(10 * kMillis);
            if (!cancelled_here && elapsed >= offset) {
                cancelled_here = true;
                (void)broker.cancel(id, market.now());
            }
            market.quote(99.95, 500, 100.05, 500);
            broker.on_market_event(market.symbol(), market.now());
        }

        const Order* order = broker.find_order(id);
        const bool filled = order->status == OrderStatus::Filled;
        const bool cancelled = order->status == OrderStatus::Cancelled;

        INFO("cancel offset " << offset << " status " << to_string(order->status));
        REQUIRE(filled != cancelled);  // exactly one
        REQUIRE(log.fills.size() == (filled ? 1U : 0U));
        REQUIRE(broker.ledger().position(market.symbol()) == Catch::Approx(filled ? 100.0 : 0.0));
        REQUIRE(broker.ledger().check_invariant());
    }
}

// --------------------------------------------------------------------------
// ABI version tolerance
// --------------------------------------------------------------------------
//
// Nothing needs this yet: MS_ABI_VERSION is 1 and there is exactly one layout.
// It is built and tested now because the thing it prevents -- a plugin writing
// a vtable larger than the struct the host handed it -- is silent memory
// corruption that happens before any callback runs, and the worst time to
// think about it for the first time is while shipping the change that
// introduces it.

TEST_CASE("the vtable layout table knows which versions exist", "[exec][abi]") {
    STATIC_REQUIRE(abi::slot_count_for(1) == abi::kV1SlotCount);
    STATIC_REQUIRE(abi::is_supported(1));

    // Zero is not a version anything ever shipped with.
    STATIC_REQUIRE_FALSE(abi::is_supported(0));

    // Newer than this host: the layout is unknown, so it is refused rather
    // than guessed at.
    STATIC_REQUIRE_FALSE(abi::is_supported(MS_ABI_VERSION + 1));
    STATIC_REQUIRE_FALSE(abi::is_supported(1000));
}

TEST_CASE("trimming a vtable nulls everything past the layout", "[exec][abi]") {
    // Stand-ins. They are never called; only their addresses matter.
    const auto destroy = [](void*) {};
    const auto on_start = [](void*, const MS_HostContext*) {};
    const auto on_bar = [](void*, const MS_Bar*) {};
    const auto on_quote = [](void*, const MS_Quote*) {};
    const auto on_trade = [](void*, const MS_Trade*) {};
    const auto on_fill = [](void*, const MS_Fill*) {};
    const auto on_stop = [](void*) {};
    const auto set_param = [](void*, const char*, double) {};
    const auto get_param = [](void*, const char*) { return 0.0; };

    MS_StrategyVTable full{};
    full.destroy = destroy;
    full.on_start = on_start;
    full.on_bar = on_bar;
    full.on_quote = on_quote;
    full.on_trade = on_trade;
    full.on_fill = on_fill;
    full.on_stop = on_stop;
    full.set_param = set_param;
    full.get_param = get_param;

    SECTION("a v1 layout keeps all of it") {
        MS_StrategyVTable table = full;
        abi::trim_to_version(table, 1);
        REQUIRE(table.destroy != nullptr);
        REQUIRE(table.on_bar != nullptr);
        REQUIRE(table.get_param != nullptr);
    }

    SECTION("a smaller layout drops the tail") {
        // What a hypothetical earlier version would look like: the first three
        // slots defined and nothing else callable.
        MS_StrategyVTable table = full;
        abi::trim_to(table, 3);
        REQUIRE(table.destroy != nullptr);
        REQUIRE(table.on_start != nullptr);
        REQUIRE(table.on_bar != nullptr);
        REQUIRE(table.on_quote == nullptr);
        REQUIRE(table.on_trade == nullptr);
        REQUIRE(table.on_fill == nullptr);
        REQUIRE(table.on_stop == nullptr);
        REQUIRE(table.set_param == nullptr);
        REQUIRE(table.get_param == nullptr);
    }

    SECTION("an unsupported version leaves nothing callable") {
        MS_StrategyVTable table = full;
        abi::trim_to_version(table, 1000);
        REQUIRE(table.destroy == nullptr);
        REQUIRE(table.on_start == nullptr);
        REQUIRE(table.get_param == nullptr);
    }
}
