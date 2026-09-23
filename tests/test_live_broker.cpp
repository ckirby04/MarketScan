/*
 * The live broker's order lifecycle, driven without a network.
 *
 * NO SOCKET IS OPENED. Every message here is hand-written to the vendor's
 * documented shape and pushed through LiveBroker::apply_update, which is the
 * same function the socket pump calls. The cases are the ones Part 4 names as
 * the ones that matter: a rejection, a partial that then expires, a cancel
 * that loses its race, and a reconnect with orders outstanding.
 *
 * WHAT THESE CANNOT TEST
 *
 * That the venue actually sends these shapes. That is what the real session in
 * docs/EXECUTION.md is for, and it is reported there with its numbers rather
 * than asserted here.
 */

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include "marketscan/core/symbol_table.hpp"
#include "marketscan/live/live_broker.hpp"
#include "marketscan/live/order_map.hpp"
#include "marketscan/live/trade_updates.hpp"

using namespace marketscan;
using namespace marketscan::live;
using Catch::Matchers::WithinAbs;

namespace {

/// Records what a strategy would have been told.
class RecordingListener final : public IFillListener {
public:
    struct StatusEvent {
        OrderId order_id = 0;
        std::uint32_t state = 0;
        bool terminal = false;
        double filled = 0.0;
        std::string reason;
    };

    void on_fill(const MS_Fill& fill) override { fills.push_back(fill); }

    void on_order_status(const MS_OrderStatus& status) override {
        StatusEvent event;
        event.order_id = status.order_id;
        event.state = status.state;
        event.terminal = status.terminal != 0u;
        event.filled = status.filled_quantity;
        event.reason = status.reason != nullptr ? status.reason : "";
        statuses.push_back(std::move(event));
    }

    [[nodiscard]] std::size_t terminal_count() const {
        std::size_t n = 0;
        for (const StatusEvent& s : statuses) {
            n += s.terminal ? 1 : 0;
        }
        return n;
    }

    std::vector<MS_Fill> fills;
    std::vector<StatusEvent> statuses;
};

Credentials fake_credentials() {
    // Not a real key and never used: no test here opens a socket. The shape is
    // what LiveBroker's constructor checks.
    return Credentials{"TESTKEYNOTREAL0000000", "TESTSECRETNOTREAL000000000000000000000000", {}};
}

struct Harness {
    SymbolTable symbols;
    RecordingListener listener;
    std::unique_ptr<LiveBroker> broker;
    SymbolId aapl = kInvalidSymbol;

    explicit Harness(LiveBrokerConfig config = {}) {
        aapl = symbols.intern("AAPL");
        if (config.run_token.empty()) {
            config.run_token = "testrun";
        }
        broker = std::make_unique<LiveBroker>(config, fake_credentials(), &symbols);
        broker->set_fill_listener(&listener);
    }

    [[nodiscard]] OrderId place(Quantity quantity, Side side = MS_SIDE_BUY) {
        MS_OrderRequest request{};
        request.symbol = aapl;
        request.side = side;
        request.type = MS_ORD_LIMIT;
        request.limit_price = 100.0;
        request.quantity = quantity;
        request.tif = MS_TIF_DAY;
        request.client_order_id = 7;
        return broker->register_local_order_for_test(request, 1000);
    }

    [[nodiscard]] TradeUpdate update(OrderId id, UpdateKind kind) const {
        TradeUpdate u;
        u.kind = kind;
        u.client_order_id = broker->order_map().client_id_for(id);
        u.broker_order_id = "venue-" + std::to_string(id);
        u.symbol = "AAPL";
        u.ts = 2000;
        u.order_quantity = 10.0;
        u.side = MS_SIDE_BUY;
        return u;
    }
};

}  // namespace

// --------------------------------------------------------------------------
// The client order id, which is what survives a reconnect
// --------------------------------------------------------------------------

TEST_CASE("a client order id round trips to its engine order id", "[live][broker][ids]") {
    const OrderMap map("run42");
    for (const OrderId id : {OrderId{1}, OrderId{2}, OrderId{999'999}, OrderId{1ULL << 40}}) {
        const std::string client = map.client_id_for(id);
        INFO(client);
        REQUIRE(map.engine_id_for(client) == id);
    }
}

TEST_CASE("a client id is the same string before and after a restart", "[live][broker][ids]") {
    // The whole point. Nothing is stored: two maps built from the same run
    // token produce identical ids, so a process that died between sending an
    // order and hearing about it can still name that order.
    const OrderMap before("run42");
    const OrderMap after("run42");
    REQUIRE(before.client_id_for(17) == after.client_id_for(17));
    REQUIRE(after.engine_id_for(before.client_id_for(17)) == OrderId{17});
}

TEST_CASE("another run's client id is not adopted", "[live][broker][ids]") {
    // The failure this prevents is booking somebody else's fills into this
    // ledger, which would be silent and would be wrong from that point on.
    const OrderMap mine("runA");
    const OrderMap theirs("runB");
    REQUIRE_FALSE(mine.engine_id_for(theirs.client_id_for(5)).has_value());
    REQUIRE_FALSE(mine.engine_id_for("some-manual-order").has_value());
    REQUIRE_FALSE(mine.engine_id_for("ms-runA").has_value());
    REQUIRE_FALSE(mine.engine_id_for("ms-runA-").has_value());
    REQUIRE_FALSE(mine.engine_id_for("ms-runA-notanumber").has_value());
    REQUIRE_FALSE(mine.engine_id_for("ms-runA-12x").has_value());
    REQUIRE_FALSE(mine.engine_id_for("ms-runA-0").has_value());
    REQUIRE_FALSE(mine.engine_id_for("").has_value());
}

TEST_CASE("a run token that cannot be read back is refused", "[live][broker][ids]") {
    // A token with a separator in it produces ids this map could not parse,
    // which means orders it could not reclaim -- the exact failure the client
    // order id exists to prevent, arriving through the back door.
    REQUIRE_THROWS_AS(OrderMap(""), std::invalid_argument);
    REQUIRE_THROWS_AS(OrderMap("has-a-dash"), std::invalid_argument);
}

TEST_CASE("unacknowledged orders are the ones to go looking for", "[live][broker][ids]") {
    OrderMap map("run42");
    map.record_submitted(1);
    map.record_submitted(2);
    map.record_acknowledged(2, "venue-2");

    REQUIRE(map.unacknowledged() == std::vector<OrderId>{1});

    // Re-recording a submit must not un-acknowledge a confirmed order: that
    // would send a reconnect looking for something it already knows.
    map.record_submitted(2);
    REQUIRE(map.unacknowledged() == std::vector<OrderId>{1});

    map.retire(2);
    REQUIRE(map.size() == 1);
}

// --------------------------------------------------------------------------
// Parsing what the venue says
// --------------------------------------------------------------------------

TEST_CASE("a fill message parses into a fill", "[live][broker][updates]") {
    const TradeUpdateParse parsed = parse_trade_update(
        R"({"stream":"trade_updates","data":{"event":"fill","price":"189.52","qty":"3",)"
        R"("timestamp":"2026-09-21T18:36:33.355534795Z","order":{"id":"abc","client_order_id":)"
        R"("ms-testrun-1","symbol":"AAPL","qty":"3","filled_qty":"3","filled_avg_price":"189.52",)"
        R"("side":"buy"}}})");

    REQUIRE(parsed.error.empty());
    REQUIRE(parsed.updates.size() == 1);

    const TradeUpdate& update = parsed.updates.front();
    REQUIRE(update.kind == UpdateKind::Fill);
    REQUIRE(update.terminal());
    REQUIRE(update.client_order_id == "ms-testrun-1");
    REQUIRE(update.broker_order_id == "abc");
    REQUIRE_THAT(update.fill_price, WithinAbs(189.52, 1e-9));
    REQUIRE_THAT(update.fill_quantity, WithinAbs(3.0, 1e-9));
    REQUIRE_THAT(update.cumulative_filled, WithinAbs(3.0, 1e-9));

    Timestamp expected = 0;
    REQUIRE(time::from_iso8601("2026-09-21T18:36:33.355534795Z", expected));
    REQUIRE(update.ts == expected);
}

TEST_CASE("an offset timestamp is read as the same instant", "[live][broker][updates]") {
    // The venue sends RFC3339 with a numeric offset, which from_iso8601
    // refuses on purpose. Getting this wrong by four hours would put every
    // fill in the wrong session.
    const TradeUpdateParse parsed = parse_trade_update(
        R"({"stream":"trade_updates","data":{"event":"fill","price":"1","qty":"1",)"
        R"("timestamp":"2026-09-21T14:36:33.000000000-04:00","order":{"id":"a",)"
        R"("client_order_id":"ms-testrun-1","symbol":"AAPL","qty":"1","filled_qty":"1"}}})");

    REQUIRE(parsed.updates.size() == 1);
    Timestamp expected = 0;
    REQUIRE(time::from_iso8601("2026-09-21T18:36:33Z", expected));
    REQUIRE(parsed.updates.front().ts == expected);
}

TEST_CASE("an unknown event is unknown, not harmless", "[live][broker][updates]") {
    const TradeUpdateParse parsed = parse_trade_update(
        R"({"stream":"trade_updates","data":{"event":"something_new","order":{"id":"a",)"
        R"("client_order_id":"ms-testrun-1"}}})");

    REQUIRE(parsed.updates.size() == 1);
    REQUIRE(parsed.updates.front().kind == UpdateKind::Unknown);
    REQUIRE(parsed.updates.front().raw_event == "something_new");
    REQUIRE_FALSE(parsed.updates.front().terminal());
}

TEST_CASE("a refused authorization is fatal and says so", "[live][broker][updates]") {
    const TradeUpdateParse refused = parse_trade_update(
        R"({"stream":"authorization","data":{"status":"unauthorized","action":"authenticate"}})");
    REQUIRE(refused.unauthorised);

    const TradeUpdateParse granted = parse_trade_update(
        R"({"stream":"authorization","data":{"status":"authorized","action":"authenticate"}})");
    REQUIRE_FALSE(granted.unauthorised);

    // Anything that is not the exact success string is a refusal. Defaulting
    // the other way would read a typo as permission.
    const TradeUpdateParse odd =
        parse_trade_update(R"({"stream":"authorization","data":{"status":"AUTHORIZED"}})");
    REQUIRE(odd.unauthorised);
}

// --------------------------------------------------------------------------
// The lifecycles Part 4 names
// --------------------------------------------------------------------------

TEST_CASE("a rejection reaches the strategy as a terminal status", "[live][broker][lifecycle]") {
    Harness h;
    const OrderId id = h.place(3.0);

    TradeUpdate rejected = h.update(id, UpdateKind::Rejected);
    rejected.raw_event = "rejected";
    rejected.reason = "insufficient buying power";
    h.broker->apply_update(rejected, 2000);

    const Order* order = h.broker->find_order(id);
    REQUIRE(order != nullptr);
    REQUIRE(order->status == OrderStatus::Rejected);
    REQUIRE(order->is_terminal());

    REQUIRE(h.listener.fills.empty());
    REQUIRE(h.listener.terminal_count() == 1);
    REQUIRE(h.listener.statuses.back().state == static_cast<std::uint32_t>(MS_ORDER_REJECTED));
    REQUIRE(h.listener.statuses.back().reason == "insufficient buying power");
    REQUIRE(h.broker->stats().rejected == 1);

    // The link is gone, so a late duplicate cannot reopen it.
    REQUIRE(h.broker->order_map().find(id) == nullptr);
}

TEST_CASE("a partial that then expires books one fill and one terminal status",
          "[live][broker][lifecycle]") {
    // The case a zero-latency simulator cannot produce: an order that got some
    // of what it asked for and then ran out of session.
    Harness h;
    const OrderId id = h.place(10.0);

    TradeUpdate partial = h.update(id, UpdateKind::PartialFill);
    partial.raw_event = "partial_fill";
    partial.fill_quantity = 4.0;
    partial.fill_price = 100.25;
    partial.cumulative_filled = 4.0;
    partial.average_fill_price = 100.25;
    h.broker->apply_update(partial, 2000);

    const Order* order = h.broker->find_order(id);
    REQUIRE(order->status == OrderStatus::PartiallyFilled);
    REQUIRE_THAT(order->filled_quantity, WithinAbs(4.0, 1e-9));
    REQUIRE_THAT(order->remaining(), WithinAbs(6.0, 1e-9));
    REQUIRE(h.listener.fills.size() == 1);
    // Still alive: a partial is not terminal and a strategy must not be told
    // the order is over while six shares are still working.
    REQUIRE(h.listener.terminal_count() == 0);

    TradeUpdate expired = h.update(id, UpdateKind::Expired);
    expired.raw_event = "expired";
    expired.cumulative_filled = 4.0;
    h.broker->apply_update(expired, 3000);

    REQUIRE(order->status == OrderStatus::Expired);
    REQUIRE(h.listener.fills.size() == 1);
    REQUIRE(h.listener.terminal_count() == 1);
    REQUIRE(h.listener.statuses.back().state == static_cast<std::uint32_t>(MS_ORDER_EXPIRED));
    // The terminal status carries how much did fill, so a strategy learns the
    // outcome and the amount in one callback.
    REQUIRE_THAT(h.listener.statuses.back().filled, WithinAbs(4.0, 1e-9));

    // The ledger holds the four shares and nothing more.
    REQUIRE_THAT(h.broker->ledger().position(h.aapl), WithinAbs(4.0, 1e-9));
}

TEST_CASE("a cancel that loses its race is counted, not hidden", "[live][broker][lifecycle]") {
    Harness h;
    const OrderId id = h.place(2.0);

    TradeUpdate filled = h.update(id, UpdateKind::Fill);
    filled.raw_event = "fill";
    filled.fill_quantity = 2.0;
    filled.fill_price = 99.5;
    filled.cumulative_filled = 2.0;
    filled.average_fill_price = 99.5;
    h.broker->apply_update(filled, 2000);

    REQUIRE(h.broker->find_order(id)->status == OrderStatus::Filled);

    // The cancel the strategy sent a moment too late. False means "this order
    // is already over", and the count says how often that happens -- a
    // strategy whose cancels are routinely late is assuming a speed it does
    // not have.
    REQUIRE_FALSE(h.broker->cancel(id, 2500));
    REQUIRE(h.broker->stats().lost_cancel_races == 1);
    REQUIRE(h.broker->late_cancel_count() == 1);

    // And the fill still stands. A lost race must never unwind one.
    REQUIRE(h.listener.fills.size() == 1);
    REQUIRE_THAT(h.broker->ledger().position(h.aapl), WithinAbs(2.0, 1e-9));
}

TEST_CASE("fills are booked from the venue's cumulative figure", "[live][broker][lifecycle]") {
    // If a partial message were lost, a running sum of our own would be short
    // by that amount forever. Reading the cumulative figure means the next
    // message repairs the position by itself.
    Harness h;
    const OrderId id = h.place(10.0);

    TradeUpdate first = h.update(id, UpdateKind::PartialFill);
    first.fill_quantity = 3.0;
    first.fill_price = 100.0;
    first.cumulative_filled = 3.0;
    h.broker->apply_update(first, 2000);
    REQUIRE_THAT(h.broker->ledger().position(h.aapl), WithinAbs(3.0, 1e-9));

    // The message for shares four and five never arrived. This one says eight.
    TradeUpdate later = h.update(id, UpdateKind::PartialFill);
    later.fill_quantity = 3.0;
    later.fill_price = 100.0;
    later.cumulative_filled = 8.0;
    h.broker->apply_update(later, 3000);

    REQUIRE_THAT(h.broker->find_order(id)->filled_quantity, WithinAbs(8.0, 1e-9));
    REQUIRE_THAT(h.broker->ledger().position(h.aapl), WithinAbs(8.0, 1e-9));
    // Two blotter rows, not three: one message's granularity was lost, and the
    // position was not.
    REQUIRE(h.listener.fills.size() == 2);
    REQUIRE_THAT(h.listener.fills.back().quantity, WithinAbs(5.0, 1e-9));
}

TEST_CASE("an update for another run's order is counted and ignored", "[live][broker][lifecycle]") {
    Harness h;
    const OrderId id = h.place(1.0);

    TradeUpdate foreign = h.update(id, UpdateKind::Fill);
    foreign.client_order_id = "ms-someoneelse-1";
    foreign.fill_quantity = 1.0;
    foreign.fill_price = 100.0;
    foreign.cumulative_filled = 1.0;
    h.broker->apply_update(foreign, 2000);

    REQUIRE(h.broker->stats().foreign_updates == 1);
    REQUIRE(h.listener.fills.empty());
    REQUIRE(h.broker->find_order(id)->status == OrderStatus::Working);
    REQUIRE_THAT(h.broker->ledger().position(h.aapl), WithinAbs(0.0, 1e-12));
}

TEST_CASE("an unknown update leaves the order working", "[live][broker][lifecycle]") {
    // The failure this prevents: an order quietly treated as finished because
    // a message arrived that nobody had taught this build about. A strategy
    // would then stop waiting for something that is still live at the venue.
    Harness h;
    const OrderId id = h.place(1.0);

    TradeUpdate mystery = h.update(id, UpdateKind::Unknown);
    mystery.raw_event = "some_new_venue_state";
    h.broker->apply_update(mystery, 2000);

    REQUIRE(h.broker->find_order(id)->status == OrderStatus::Working);
    REQUIRE(h.broker->find_order(id)->is_working());
    REQUIRE(h.broker->stats().unknown_updates == 1);
    REQUIRE(h.listener.terminal_count() == 0);
}

TEST_CASE("a transient state is not an outcome", "[live][broker][lifecycle]") {
    Harness h;
    const OrderId id = h.place(1.0);

    TradeUpdate pending = h.update(id, UpdateKind::Transient);
    pending.raw_event = "pending_cancel";
    h.broker->apply_update(pending, 2000);

    REQUIRE(h.broker->find_order(id)->status == OrderStatus::Working);
    REQUIRE(h.listener.statuses.empty());
    REQUIRE(h.broker->stats().unknown_updates == 0);
}

// --------------------------------------------------------------------------
// The rails
// --------------------------------------------------------------------------

TEST_CASE("an order over the size cap never reaches the venue", "[live][broker][rails]") {
    // This test opens no socket, so "never reaches the venue" is enforced by
    // the order being refused before any request is built: a submit that got
    // past the rail would try to POST and fail the test by hanging or erroring
    // rather than returning kInvalidOrderId.
    LiveBrokerConfig config;
    config.run_token = "testrun";
    config.max_order_quantity = 5.0;
    Harness h(config);

    MS_OrderRequest request{};
    request.symbol = h.aapl;
    request.side = MS_SIDE_BUY;
    request.type = MS_ORD_MARKET;
    request.quantity = 500.0;
    request.tif = MS_TIF_DAY;

    REQUIRE(h.broker->submit(request, 1000) == kInvalidOrderId);
    REQUIRE(h.broker->stats().refused_by_rail == 1);
    REQUIRE(h.broker->stats().submitted == 0);

    // Refused, and visible: the order is in the log with its reason rather
    // than simply absent.
    REQUIRE(h.broker->orders().size() == 1);
    REQUIRE(h.broker->orders().front().status == OrderStatus::Rejected);
    REQUIRE(h.listener.terminal_count() == 1);
}

TEST_CASE("a live account refuses to be rewound", "[live][broker][rails]") {
    // reset() is a replay operation. Doing nothing would let a caller believe
    // the account had been cleared when it still holds positions.
    Harness h;
    REQUIRE_THROWS_AS(h.broker->reset(), std::logic_error);
}

TEST_CASE("the live broker and the simulator are the same interface", "[live][broker][seam]") {
    // The milestone's own test of itself. If this ever needs a cast or a
    // branch, the seam has leaked.
    Harness h;
    IBroker& broker = *h.broker;

    REQUIRE(broker.venue() == "alpaca-paper");
    REQUIRE(broker.orders().empty());
    REQUIRE(broker.working_orders().empty());
    REQUIRE(broker.rejected_count() == 0);
    REQUIRE(broker.late_cancel_count() == 0);
    REQUIRE(broker.ledger().cash() == Money::from_dollars(100'000));

    const OrderId id = h.place(1.0);
    REQUIRE(broker.find_order(id) != nullptr);
    REQUIRE(broker.working_orders().size() == 1);

    // And the same call the engine makes after every event, which for a live
    // broker drains the venue's inbox rather than retrying resting orders.
    broker.on_market_event(h.aapl, 2000);
    REQUIRE(broker.working_orders().size() == 1);
}

// --------------------------------------------------------------------------
// Reconciliation
// --------------------------------------------------------------------------
//
// Tested through compare_with, which is the comparison without the fetch. The
// live check can only be seen agreeing when the account happens to be flat,
// and an assertion that only ever runs in one direction is not an assertion --
// which is exactly the shape of vacuous test this session set out to remove.

TEST_CASE("reconciliation agrees when the venue agrees", "[live][broker][reconcile]") {
    Harness h;
    const OrderId id = h.place(4.0);

    TradeUpdate filled = h.update(id, UpdateKind::Fill);
    filled.fill_quantity = 4.0;
    filled.fill_price = 100.0;
    filled.cumulative_filled = 4.0;
    h.broker->apply_update(filled, 2000);

    const Reconciliation result =
        h.broker->compare_with(R"({"status":"ACTIVE","equity":"100000","cash":"99600"})",
                               R"([{"symbol":"AAPL","qty":"4","avg_entry_price":"100.00"}])");

    REQUIRE(result.checked);
    REQUIRE(result.error.empty());
    REQUIRE(result.divergences.empty());
    REQUIRE(result.agreed());
}

TEST_CASE("reconciliation reports a quantity that does not match", "[live][broker][reconcile]") {
    Harness h;
    const OrderId id = h.place(4.0);

    TradeUpdate filled = h.update(id, UpdateKind::Fill);
    filled.fill_quantity = 4.0;
    filled.fill_price = 100.0;
    filled.cumulative_filled = 4.0;
    h.broker->apply_update(filled, 2000);

    const Reconciliation result =
        h.broker->compare_with(R"({"status":"ACTIVE"})", R"([{"symbol":"AAPL","qty":"7"}])");

    REQUIRE(result.checked);
    REQUIRE_FALSE(result.agreed());
    REQUIRE(result.divergences.size() == 1);
    REQUIRE(result.divergences.front().what == "position in AAPL");
    REQUIRE(result.divergences.front().ours == "4.0000");
    REQUIRE(result.divergences.front().theirs == "7.0000");
}

TEST_CASE("a position this run never opened is reported, not adopted",
          "[live][broker][reconcile]") {
    // This is the case a real session hit: a previous run was killed after its
    // orders filled, and six shares of AAPL sat on the account. Adopting them
    // would make the ledger agree and hide the fact that a run had died
    // holding a position.
    Harness h;
    const Reconciliation result =
        h.broker->compare_with(R"({"status":"ACTIVE"})", R"([{"symbol":"AAPL","qty":"6"}])");

    REQUIRE(result.checked);
    REQUIRE_FALSE(result.agreed());
    REQUIRE(result.divergences.size() == 1);
    REQUIRE(result.divergences.front().what == "position in AAPL (unknown to this run)");
    REQUIRE(result.divergences.front().theirs == "6.0000");

    // And the ledger is untouched: reporting is not correcting.
    REQUIRE_THAT(h.broker->ledger().position(h.aapl), WithinAbs(0.0, 1e-12));
}

TEST_CASE("a position the venue does not have is reported too", "[live][broker][reconcile]") {
    Harness h;
    const OrderId id = h.place(2.0);
    TradeUpdate filled = h.update(id, UpdateKind::Fill);
    filled.fill_quantity = 2.0;
    filled.fill_price = 50.0;
    filled.cumulative_filled = 2.0;
    h.broker->apply_update(filled, 2000);

    const Reconciliation result = h.broker->compare_with(R"({"status":"ACTIVE"})", R"([])");

    REQUIRE_FALSE(result.agreed());
    REQUIRE(result.divergences.size() == 1);
    REQUIRE(result.divergences.front().ours == "2.0000");
    REQUIRE(result.divergences.front().theirs == "0.0000");
}

TEST_CASE("an unreadable response is not agreement", "[live][broker][reconcile]") {
    // The distinction that matters: "I could not check" must never be reported
    // as "they agree". agreed() requires checked and no error.
    Harness h;

    const Reconciliation bad_account = h.broker->compare_with("not json", R"([])");
    REQUIRE_FALSE(bad_account.checked);
    REQUIRE_FALSE(bad_account.agreed());
    REQUIRE_FALSE(bad_account.error.empty());

    const Reconciliation bad_positions =
        h.broker->compare_with(R"({"status":"ACTIVE"})", R"({"not":"an array"})");
    REQUIRE_FALSE(bad_positions.checked);
    REQUIRE_FALSE(bad_positions.agreed());
    REQUIRE_FALSE(bad_positions.error.empty());
}

TEST_CASE("a second terminal update for one order is dropped", "[live][broker][lifecycle]") {
    // The reclaim after a reconnect re-reads every order the venue knows,
    // including ones this run already saw end. Applying the terminal state
    // twice would call on_order_status twice for one order, and a strategy
    // that released state on the first would release it again.
    Harness h;
    const OrderId id = h.place(1.0);

    TradeUpdate cancelled = h.update(id, UpdateKind::Cancelled);
    cancelled.raw_event = "canceled";
    h.broker->apply_update(cancelled, 2000);

    REQUIRE(h.listener.terminal_count() == 1);
    REQUIRE(h.broker->stats().cancelled == 1);

    // The same message again, as a reconnect would replay it.
    h.broker->apply_update(cancelled, 3000);

    REQUIRE(h.listener.terminal_count() == 1);
    REQUIRE(h.broker->stats().cancelled == 1);
    REQUIRE(h.broker->stats().stale_updates == 1);
}

TEST_CASE("a fill after a terminal state cannot reopen an order", "[live][broker][lifecycle]") {
    // The same guard, in the direction that would move money: a late fill for
    // a cancelled order must not book a position.
    Harness h;
    const OrderId id = h.place(2.0);

    TradeUpdate cancelled = h.update(id, UpdateKind::Cancelled);
    h.broker->apply_update(cancelled, 2000);

    TradeUpdate late = h.update(id, UpdateKind::Fill);
    late.fill_quantity = 2.0;
    late.fill_price = 100.0;
    late.cumulative_filled = 2.0;
    h.broker->apply_update(late, 3000);

    REQUIRE(h.broker->find_order(id)->status == OrderStatus::Cancelled);
    REQUIRE(h.listener.fills.empty());
    REQUIRE_THAT(h.broker->ledger().position(h.aapl), WithinAbs(0.0, 1e-12));
    REQUIRE(h.broker->stats().stale_updates == 1);
}

TEST_CASE("a null field in a venue response does not throw", "[live][broker][updates]") {
    // The venue sends "filled_avg_price": null for an order that has not
    // filled, and nlohmann::json::value throws on a present-but-null key with
    // a string default. That threw inside the reclaim-after-reconnect path --
    // the one path the run was exercising -- and ended the process.
    const TradeUpdateParse parsed = parse_trade_update(
        R"({"stream":"trade_updates","data":{"event":"new","timestamp":null,"qty":null,)"
        R"("price":null,"order":{"id":"a","client_order_id":"ms-testrun-1","symbol":"QQQ",)"
        R"("qty":"1","filled_qty":"0","filled_avg_price":null,"side":"buy"}}})");

    REQUIRE(parsed.error.empty());
    REQUIRE(parsed.updates.size() == 1);

    const TradeUpdate& update = parsed.updates.front();
    REQUIRE(update.kind == UpdateKind::Accepted);
    REQUIRE_THAT(update.average_fill_price, WithinAbs(0.0, 1e-12));
    REQUIRE_THAT(update.fill_price, WithinAbs(0.0, 1e-12));
    REQUIRE_THAT(update.cumulative_filled, WithinAbs(0.0, 1e-12));
    // No timestamp anywhere, so the caller's clock is used rather than zero
    // being passed off as an instant.
    REQUIRE(update.ts == 0);
}

TEST_CASE("the notional rail refuses an order the share cap would allow", "[live][broker][rails]") {
    // The reason the share cap could be raised from 10 to 300 at all. These
    // instruments span a fourfold price range, so a share count is not a safety
    // limit: 300 shares is $54k of one and $232k of another, and only one of
    // those is a sane order on a $100k account.
    //
    // Asserted through check_rails rather than submit(), because a rail that
    // *allows* an order can only be observed through submit() by letting that
    // order reach the venue -- and no test here touches the network.
    LiveBrokerConfig config;
    config.run_token = "testrun";
    config.max_order_quantity = 300.0;
    config.max_order_notional = Money::from_dollars(60'000);
    Harness h(config);

    MS_OrderRequest request{};
    request.symbol = h.aapl;
    request.side = MS_SIDE_BUY;
    request.type = MS_ORD_LIMIT;
    request.tif = MS_TIF_DAY;

    // Well inside the share cap, well outside the notional cap.
    request.quantity = 200.0;
    request.limit_price = 800.0;  // $160,000
    REQUIRE(h.broker->check_rails(request) == RejectReason::InvalidQuantity);

    // The same share count on a cheaper instrument is allowed through.
    request.limit_price = 100.0;  // $20,000
    REQUIRE(h.broker->check_rails(request) == RejectReason::None);

    // And exactly at the cap is allowed: the rail refuses more than, not equal.
    request.quantity = 600.0;
    request.limit_price = 100.0;  // $60,000 exactly
    config.max_order_quantity = 1000.0;
    Harness generous(config);
    REQUIRE(generous.broker->check_rails(request) == RejectReason::None);
}

TEST_CASE("the share cap still fires inside the notional cap", "[live][broker][rails]") {
    // Both rails matter, and each has a case the other misses. This is the
    // fat-finger-in-a-cheap-instrument case: trivial notional, absurd size.
    LiveBrokerConfig config;
    config.run_token = "testrun";
    config.max_order_quantity = 300.0;
    config.max_order_notional = Money::from_dollars(60'000);
    Harness h(config);

    MS_OrderRequest request{};
    request.symbol = h.aapl;
    request.side = MS_SIDE_BUY;
    request.type = MS_ORD_LIMIT;
    request.tif = MS_TIF_DAY;
    request.quantity = 5'000.0;
    request.limit_price = 1.0;  // $5,000

    REQUIRE(h.broker->check_rails(request) == RejectReason::InvalidQuantity);
}

TEST_CASE("a zero notional rail disables the check", "[live][broker][rails]") {
    LiveBrokerConfig config;
    config.run_token = "testrun";
    config.max_order_quantity = 300.0;
    config.max_order_notional = Money{};
    Harness h(config);

    MS_OrderRequest request{};
    request.symbol = h.aapl;
    request.side = MS_SIDE_BUY;
    request.type = MS_ORD_LIMIT;
    request.tif = MS_TIF_DAY;
    request.quantity = 300.0;
    request.limit_price = 9'000.0;  // $2.7m, and deliberately allowed

    REQUIRE(h.broker->check_rails(request) == RejectReason::None);
}

TEST_CASE("a market order passes the notional rail on the share cap alone",
          "[live][broker][rails]") {
    // Stated in the header and asserted here, because the alternative -- a
    // silent gap in a safety rail -- is only found when it matters. LiveBroker
    // has no MarketView, so a market order carries no price it could check.
    LiveBrokerConfig config;
    config.run_token = "testrun";
    config.max_order_quantity = 300.0;
    config.max_order_notional = Money::from_dollars(1);
    Harness h(config);

    MS_OrderRequest request{};
    request.symbol = h.aapl;
    request.side = MS_SIDE_BUY;
    request.type = MS_ORD_MARKET;
    request.tif = MS_TIF_DAY;
    request.quantity = 300.0;

    REQUIRE(h.broker->check_rails(request) == RejectReason::None);

    // The share cap still holds for it.
    request.quantity = 301.0;
    REQUIRE(h.broker->check_rails(request) == RejectReason::InvalidQuantity);
}

TEST_CASE("the rails refuse a price or quantity that is not a number", "[live][broker][rails]") {
    Harness h;

    MS_OrderRequest request{};
    request.symbol = h.aapl;
    request.side = MS_SIDE_BUY;
    request.type = MS_ORD_LIMIT;
    request.tif = MS_TIF_DAY;
    request.quantity = 10.0;
    request.limit_price = 100.0;
    REQUIRE(h.broker->check_rails(request) == RejectReason::None);

    request.quantity = 0.0;
    REQUIRE(h.broker->check_rails(request) == RejectReason::InvalidQuantity);

    request.quantity = std::numeric_limits<double>::quiet_NaN();
    REQUIRE(h.broker->check_rails(request) == RejectReason::InvalidQuantity);

    request.quantity = 10.0;
    request.limit_price = -1.0;
    REQUIRE(h.broker->check_rails(request) == RejectReason::InvalidPrice);

    request.limit_price = std::numeric_limits<double>::infinity();
    REQUIRE(h.broker->check_rails(request) == RejectReason::InvalidPrice);
}
