// Do the tallies agree with the orders they claim to count?
//
// A run once reported "128 submitted, 124 accepted, 0 rejected" while four
// orders sat Rejected in the log with their reasons. Nothing was lost. The
// summary was simply untrue, and the only thing that caught it was a person
// doing the subtraction -- which is the vacuous-pass law's corollary in
// docs/AUDIT.md: attention is a fine detector and a useless control.
//
// OrderTracker::reconcile() is the control. These cases prove it fires, and the
// way they prove it is by reproducing the original bug rather than by asserting
// that a healthy tracker is healthy. A guard tested only against the passing
// case is a guard whose output looks the same when it is absent.

#include <catch2/catch_test_macros.hpp>

#include <string>

#include "marketscan/exec/ledger.hpp"
#include "marketscan/live/order_tracker.hpp"
#include "marketscan/live/summary_audit.hpp"

using namespace marketscan;
using namespace marketscan::live;

namespace {

constexpr Timestamp kNow = 1'700'000'000'000'000'000LL;

MS_OrderRequest make_request(Quantity quantity = 10.0, Side side = MS_SIDE_BUY) {
    MS_OrderRequest request{};
    request.symbol = 1;
    request.side = side;
    request.quantity = quantity;
    request.type = MS_ORD_MARKET;
    request.tif = MS_TIF_DAY;
    request.limit_price = 0.0;
    request.client_order_id = 0;
    return request;
}

/// A tracker with its own ledger, as every broker that owns one has.
struct Fixture {
    Ledger ledger{Money::from_dollars(100'000)};
    OrderTracker tracker{&ledger, "test"};

    OrderId submit(Quantity quantity = 10.0) {
        return tracker.register_order(make_request(quantity), kNow, kNow, 0);
    }
};

/// An update the tracker will actually route.
///
/// The client order id has to be the one the OrderMap minted -- `ms-<run>-<engine
/// id>` -- because that is the only handle the venue and this process share. A
/// made-up id is routed to `foreign_updates` and changes nothing, which is
/// precisely the misroute that produced a false EQUIVALENT once; asking the map
/// for the id rather than spelling it out means these cases cannot repeat it.
TradeUpdate update_for(OrderTracker& tracker, OrderId id, UpdateKind kind) {
    TradeUpdate update{};
    update.kind = kind;
    update.client_order_id = tracker.order_map().client_id_for(id);
    update.ts = kNow;
    return update;
}

}  // namespace

TEST_CASE("a fresh tracker reconciles, and says how many identities it checked",
          "[live][tracker][reconcile]") {
    Fixture f;
    const StatsReconciliation empty = f.tracker.reconcile();

    REQUIRE(empty.ok());
    // The count is asserted because a reconciliation that checked nothing also
    // reports ok(). Six identities are defined; if one is ever removed this
    // fails rather than quietly measuring less.
    REQUIRE(empty.identities.size() == 6);
    REQUIRE(empty.describe().empty());

    f.submit();
    f.submit();
    const StatsReconciliation two = f.tracker.reconcile();
    REQUIRE(two.ok());
    REQUIRE(two.working == 2);
    REQUIRE(two.identities.size() == 6);
}

TEST_CASE("the 128/124/0 bug: a Rejected order that was never counted fails the identity",
          "[live][tracker][reconcile][regression]") {
    // The historical shape exactly. live_broker.cpp used to set the order's
    // status here and call a separate no-argument counter; a refactor kept the
    // first and lost the second. So: mark an order Rejected without counting it,
    // which is what that code did, and require that reconcile() refuses.
    Fixture f;
    const OrderId first = f.submit();
    f.submit();
    f.submit();
    f.submit();

    REQUIRE(f.tracker.reconcile().ok());

    Order* order = f.tracker.mutable_order(first);
    REQUIRE(order != nullptr);
    order->status = OrderStatus::Rejected;

    const StatsReconciliation broken = f.tracker.reconcile();
    REQUIRE_FALSE(broken.ok());

    // The census sees the refusal; the counter does not. That difference is the
    // whole bug.
    REQUIRE(broken.rejected == 1);
    REQUIRE(f.tracker.stats().rejected == 0);

    // And the failure names the identity rather than merely failing.
    const std::string message = broken.describe();
    REQUIRE_FALSE(message.empty());
    REQUIRE(message.find("orders in Rejected == rejected") != std::string::npos);
    REQUIRE(message.find("1 != 0") != std::string::npos);
}

TEST_CASE("note_venue_refusal ends the order and counts it in one call",
          "[live][tracker][reconcile]") {
    // The guarantee, as opposed to the check above. One call does both things,
    // so the two cannot come apart the way they did.
    Fixture f;
    const OrderId id = f.submit();

    f.tracker.note_venue_refusal(id, kNow);

    const Order* order = f.tracker.find_order(id);
    REQUIRE(order != nullptr);
    REQUIRE(order->status == OrderStatus::Rejected);
    REQUIRE(f.tracker.stats().rejected == 1);
    REQUIRE(f.tracker.reconcile().ok());
}

TEST_CASE("a second venue refusal for one order is not counted twice",
          "[live][tracker][reconcile]") {
    // An order enters a terminal state once. Counting a repeat would make
    // `rejected` exceed the number of Rejected orders and fail the identity --
    // so the guard against double counting is itself checked by the identity.
    Fixture f;
    const OrderId id = f.submit();

    f.tracker.note_venue_refusal(id, kNow);
    f.tracker.note_venue_refusal(id, kNow);

    REQUIRE(f.tracker.stats().rejected == 1);
    REQUIRE(f.tracker.reconcile().ok());
}

TEST_CASE("a refusal for an unknown order changes nothing", "[live][tracker][reconcile]") {
    Fixture f;
    f.submit();

    f.tracker.note_venue_refusal(99'999, kNow);

    REQUIRE(f.tracker.stats().rejected == 0);
    REQUIRE(f.tracker.reconcile().ok());
}

TEST_CASE("a rail refusal registers an order and reconciles", "[live][tracker][reconcile]") {
    Fixture f;
    f.submit();
    const OrderId refused = f.tracker.register_refusal(
        make_request(), kNow, kNow, 0, RejectReason::InvalidQuantity, "over the share cap");

    const Order* order = f.tracker.find_order(refused);
    REQUIRE(order != nullptr);
    REQUIRE(order->status == OrderStatus::Rejected);

    const StatsReconciliation r = f.tracker.reconcile();
    REQUIRE(r.ok());
    // One submitted plus one refusal registered, against two orders.
    REQUIRE(r.rejected == 1);
    REQUIRE(f.tracker.stats().submitted == 1);
}

TEST_CASE("a replaced order is counted, not silently folded into cancels",
          "[live][tracker][reconcile]") {
    // Replaced sets the order to Cancelled and, until the reconciliation went
    // in, incremented nothing -- so the printed `cancelled` under-reported by
    // one per replacement. The identity found it; this holds it found.
    Fixture f;
    const OrderId id = f.submit();
    f.tracker.order_map().record_acknowledged(id, "venue-1");

    f.tracker.apply_update(update_for(f.tracker, id, UpdateKind::Replaced), kNow);

    const Order* order = f.tracker.find_order(id);
    REQUIRE(order != nullptr);
    REQUIRE(order->status == OrderStatus::Cancelled);
    REQUIRE(f.tracker.stats().replaced == 1);
    // Counted apart from a venue cancel, because they are different events.
    REQUIRE(f.tracker.stats().cancelled == 0);
    REQUIRE(f.tracker.reconcile().ok());
}

TEST_CASE("an order cancelled by the venue reconciles", "[live][tracker][reconcile]") {
    Fixture f;
    const OrderId id = f.submit();
    f.tracker.order_map().record_acknowledged(id, "venue-1");

    f.tracker.apply_update(update_for(f.tracker, id, UpdateKind::Cancelled), kNow);

    REQUIRE(f.tracker.stats().cancelled == 1);
    REQUIRE(f.tracker.stats().replaced == 0);
    const StatsReconciliation r = f.tracker.reconcile();
    REQUIRE(r.ok());
    REQUIRE(r.cancelled == 1);
}

TEST_CASE("every booked fill is a blotter row", "[live][tracker][reconcile]") {
    Fixture f;
    const OrderId id = f.submit(10.0);
    f.tracker.order_map().record_acknowledged(id, "venue-1");

    TradeUpdate partial = update_for(f.tracker, id, UpdateKind::PartialFill);
    partial.cumulative_filled = 4.0;
    partial.fill_quantity = 4.0;
    partial.fill_price = 100.0;
    f.tracker.apply_update(partial, kNow);

    TradeUpdate rest = update_for(f.tracker, id, UpdateKind::Fill);
    rest.cumulative_filled = 10.0;
    rest.fill_quantity = 6.0;
    rest.fill_price = 101.0;
    f.tracker.apply_update(rest, kNow);

    // Two fill messages, two blotter rows, and the order is complete.
    REQUIRE(f.ledger.blotter().size() == 2);
    REQUIRE(f.tracker.stats().partial_fills == 1);
    REQUIRE(f.tracker.stats().filled == 1);

    const StatsReconciliation r = f.tracker.reconcile();
    REQUIRE(r.ok());
    REQUIRE(r.filled == 1);
}

TEST_CASE("a fill message that books nothing is counted but adds no blotter row",
          "[live][tracker][reconcile]") {
    // The venue can send a fill event whose cumulative quantity has not moved --
    // after a reconnect re-reads an order, for instance. It counts as a message
    // and must not count as a row, and the identity is what holds those apart.
    Fixture f;
    const OrderId id = f.submit(10.0);
    f.tracker.order_map().record_acknowledged(id, "venue-1");

    TradeUpdate empty = update_for(f.tracker, id, UpdateKind::PartialFill);
    empty.cumulative_filled = 0.0;
    empty.fill_quantity = 0.0;
    empty.fill_price = 0.0;
    f.tracker.apply_update(empty, kNow);

    REQUIRE(f.ledger.blotter().empty());
    REQUIRE(f.tracker.stats().partial_fills == 1);
    REQUIRE(f.tracker.reconcile().ok());
}

TEST_CASE("an expired order reconciles", "[live][tracker][reconcile]") {
    Fixture f;
    const OrderId id = f.submit();
    f.tracker.order_map().record_acknowledged(id, "venue-1");

    f.tracker.apply_update(update_for(f.tracker, id, UpdateKind::Expired), kNow);

    REQUIRE(f.tracker.stats().expired == 1);
    const StatsReconciliation r = f.tracker.reconcile();
    REQUIRE(r.ok());
    REQUIRE(r.expired == 1);
}

TEST_CASE("an over-counted tally fails the identity too", "[live][tracker][reconcile]") {
    // The mirror of the historical bug: a counter incremented without the thing
    // it counts. reconcile() is an equality in both directions, not a lower
    // bound, so a summary that over-reports fails as loudly as one that
    // under-reports.
    Fixture f;
    const OrderId id = f.submit();
    f.tracker.note_venue_refusal(id, kNow);
    REQUIRE(f.tracker.reconcile().ok());

    // Now un-reject the order, leaving the counter where it is.
    Order* order = f.tracker.mutable_order(id);
    REQUIRE(order != nullptr);
    order->status = OrderStatus::Working;

    const StatsReconciliation broken = f.tracker.reconcile();
    REQUIRE_FALSE(broken.ok());
    REQUIRE(broken.describe().find("0 != 1") != std::string::npos);
}

// --- SummaryAudit -----------------------------------------------------------
//
// The class that turns the identities above into an exit code. Tested here
// rather than in apps/, which is why it lives in the library: a guard whose only
// output is stdout can only be tested by capturing stdout.

TEST_CASE("a summary audit that checked nothing is a failure, not a pass",
          "[live][summary][reconcile]") {
    // The whole point. An audit with no identities in it prints the same
    // reassuring nothing as one that checked everything, so it must not report
    // success -- see the vacuous-pass law in docs/AUDIT.md.
    const SummaryAudit empty;

    REQUIRE_FALSE(empty.ok());
    REQUIRE(empty.checked() == 0);
    REQUIRE(empty.exit_code() == kInconsistentSummary);
    REQUIRE(empty.report().find("nothing was checked") != std::string::npos);
}

TEST_CASE("a passing summary audit says how many identities held", "[live][summary][reconcile]") {
    SummaryAudit audit;
    audit.check("one == one", 1, 1);
    audit.check("two == two", 2, 2);
    audit.check_at_most("one <= two", 1, 2);

    REQUIRE(audit.ok());
    REQUIRE(audit.checked() == 3);
    REQUIRE(audit.failed() == 0);
    REQUIRE(audit.exit_code() == 0);
    // The count is in the text, because a pass that does not say how much it
    // compared cannot be audited by reading it.
    REQUIRE(audit.report().find("3 identities hold") != std::string::npos);
}

TEST_CASE("a failing summary audit names every identity and both sides",
          "[live][summary][reconcile]") {
    SummaryAudit audit;
    audit.check("holds", 5, 5);
    audit.check("submitted == accepted + rejected", 128, 124);
    audit.check_at_most("bounded", 9, 2);

    REQUIRE_FALSE(audit.ok());
    REQUIRE(audit.checked() == 3);
    REQUIRE(audit.failed() == 2);
    REQUIRE(audit.exit_code() == kInconsistentSummary);

    const std::string text = audit.report();
    REQUIRE(text.find("2 of 3 identities fail") != std::string::npos);
    REQUIRE(text.find("submitted == accepted + rejected") != std::string::npos);
    REQUIRE(text.find("128 != 124") != std::string::npos);
    REQUIRE(text.find("9 <= 2") != std::string::npos);
    // The one that held is not listed; only failures are.
    REQUIRE(text.find("holds") == std::string::npos);
}

TEST_CASE("an audit absorbs a tracker's identities under a name", "[live][summary][reconcile]") {
    Fixture f;
    const OrderId id = f.submit();

    SummaryAudit audit;
    audit.absorb("live", f.tracker.reconcile());
    REQUIRE(audit.ok());
    REQUIRE(audit.checked() == 6);

    // Break the tracker the way the 128/124/0 bug broke it, and the absorbed
    // identity fails with the label that says whose it was.
    Order* order = f.tracker.mutable_order(id);
    REQUIRE(order != nullptr);
    order->status = OrderStatus::Rejected;

    SummaryAudit after;
    after.absorb("live", f.tracker.reconcile());
    REQUIRE_FALSE(after.ok());
    REQUIRE(after.exit_code() == kInconsistentSummary);
    REQUIRE(after.report().find("live: orders in Rejected == rejected") != std::string::npos);
}
