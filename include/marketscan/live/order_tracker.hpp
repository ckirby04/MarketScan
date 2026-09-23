#ifndef MARKETSCAN_LIVE_ORDER_TRACKER_HPP
#define MARKETSCAN_LIVE_ORDER_TRACKER_HPP

/*
 * The order state machine: what a venue's answers do to this process's orders
 * and its ledger.
 *
 * WHY THIS IS ITS OWN CLASS
 *
 * Because two brokers need it and only one of them may own it. `LiveBroker`
 * gets its answers from a socket; `RecordedBroker` gets the same answers from a
 * recording. If each applied them itself, a replay would be comparing two
 * implementations of the state machine rather than comparing live against
 * replay -- and a difference in how a partial is booked, or in which kinds are
 * terminal, would surface as a strategy disagreement and be debugged in the
 * wrong place.
 *
 * That is the same argument that put the market data parser in its own file,
 * and it was learned the same way: the first regular-session run diverged
 * because live and replay were fed *different information*, not because they
 * computed differently. Nothing about this file should make that possible
 * again.
 *
 * So there is one state machine. A broker supplies the transport and the
 * policy; this owns the orders, the ledger and what a TradeUpdate means.
 */

#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "marketscan/core/symbol_table.hpp"
#include "marketscan/exec/broker.hpp"
#include "marketscan/exec/ledger.hpp"
#include "marketscan/exec/order.hpp"
#include "marketscan/live/order_map.hpp"
#include "marketscan/live/trade_updates.hpp"

namespace marketscan::live {

/// Tallies of what the venue's answers did. Shared by every broker that has a
/// venue, because these are facts about the venue rather than about a transport.
struct OrderTrackerStats {
    std::uint64_t submitted = 0;
    std::uint64_t accepted = 0;
    std::uint64_t filled = 0;
    std::uint64_t partial_fills = 0;
    std::uint64_t cancelled = 0;
    std::uint64_t expired = 0;
    std::uint64_t rejected = 0;

    /// Cancels the venue answered after the order had already finished. Not an
    /// error: a race that was lost is a fact about how fast this process is.
    std::uint64_t lost_cancel_races = 0;

    /// Orders refused here rather than sent, by the local rails.
    std::uint64_t refused_by_rail = 0;

    /// Updates naming an order this run did not submit. Expected when another
    /// run shares the account; counted so it is never silent.
    std::uint64_t foreign_updates = 0;

    /// Updates about an order that had already finished here. Expected after a
    /// reconnect, when the reclaim re-reads orders this run already saw end.
    std::uint64_t stale_updates = 0;

    /// Update kinds this build has never seen. Logged individually.
    std::uint64_t unknown_updates = 0;

    /// Orders the venue replaced. A replacement ends the original order, so the
    /// order's state becomes Cancelled -- but this is counted apart from
    /// `cancelled`, which means "the venue cancelled it".
    ///
    /// It was not counted at all until the reconciliation below was written,
    /// which is how the gap was found: the state census said one more order was
    /// Cancelled than the counter did, and the identity named the difference.
    std::uint64_t replaced = 0;
};

/// One arithmetic identity the tallies must satisfy, with both sides kept so a
/// failure can say which arithmetic broke rather than that something did.
struct StatsIdentity {
    /// The identity in words, e.g. "orders in Rejected == rejected".
    const char* name = "";
    std::uint64_t left = 0;
    std::uint64_t right = 0;

    /// When set, the identity is `left <= right` rather than `left == right`.
    /// Two paths register refusals and only one of them is a rail refusal, so
    /// that relation is genuinely an inequality and is not dressed up as an
    /// equation.
    bool at_most = false;

    [[nodiscard]] bool holds() const noexcept { return at_most ? left <= right : left == right; }
};

/// Every identity, checked. `ok()` is false when any of them fails.
///
/// WHY THIS EXISTS
///
/// A run once reported "128 submitted, 124 accepted, 0 rejected" while four
/// orders had plainly been refused. Nothing was lost -- every refusal was in the
/// log with its reason -- but the summary stated something untrue, and the only
/// reason anyone knew was that a person did the subtraction and it did not work.
/// Attention is a fine detector and a useless control: see the vacuous-pass law
/// in docs/AUDIT.md.
///
/// So the counters are checked against the thing they claim to count: the orders
/// themselves and the ledger's blotter. A counter is a second source of truth,
/// and the bug was exactly that the second source stopped being updated when the
/// code around it moved.
struct StatsReconciliation {
    std::vector<StatsIdentity> identities;

    /// The order-state census the identities were derived from, reported so a
    /// caller can print what was actually counted.
    std::uint64_t working = 0;
    std::uint64_t partially_filled = 0;
    std::uint64_t filled = 0;
    std::uint64_t cancelled = 0;
    std::uint64_t expired = 0;
    std::uint64_t rejected = 0;

    [[nodiscard]] bool ok() const noexcept;

    /// The failing identities, one per line, with both sides. Empty when ok().
    [[nodiscard]] std::string describe() const;
};

/// Orders, their ledger, and what a venue's answers do to both.
class OrderTracker {
public:
    /// `ledger` is borrowed and must outlive this. `run_token` distinguishes
    /// this run's client order ids from another run's against the same account.
    OrderTracker(Ledger* ledger, std::string run_token);

    void set_fill_listener(IFillListener* listener) noexcept { listener_ = listener; }

    /// Records an order this process is about to send, or has just sent, and
    /// returns its engine id. The order starts Working.
    ///
    /// `base` is the caller's order-id base, so two brokers in one run cannot
    /// collide -- exactly as the simulator's order_id_base does.
    OrderId register_order(const MS_OrderRequest& request,
                           Timestamp now,
                           Timestamp decision_ts,
                           OrderId base);

    /// Records an order under an engine id chosen by the caller.
    ///
    /// For a replay, which must reuse the id the live run used: every client
    /// order id in a recording is `ms-<run>-<engine id>`, so an id that differs
    /// derives a different client id and routes none of the recorded responses.
    /// Deriving the id from a base instead let live and replay disagree about
    /// the base and silently misroute all 391 responses of one session -- the
    /// decisions still matched, so the run reported EQUIVALENT while the
    /// recorded fills reached nothing.
    OrderId register_order_with_id(const MS_OrderRequest& request,
                                   Timestamp now,
                                   Timestamp decision_ts,
                                   OrderId id);

    /// Records an order that was refused before it left this process, so it is
    /// visible in the log with its reason rather than simply absent.
    OrderId register_refusal(const MS_OrderRequest& request,
                             Timestamp now,
                             Timestamp decision_ts,
                             OrderId base,
                             RejectReason reason,
                             const char* message);

    /// Applies one venue answer. The single definition of what an update means.
    void apply_update(const TradeUpdate& update, Timestamp now);

    /// Notes that a cancel arrived too late to matter.
    void note_lost_cancel_race();

    void note_rail_refusal();

    /// The venue refused an order the submit call had already registered as
    /// working: ends the order *and* counts it, here, together.
    ///
    /// It takes the order id for a reason. The caller used to set
    /// `order->status = Rejected` itself and then call a no-argument version of
    /// this to do the counting, and the rewire that extracted this class kept
    /// the first half and dropped the second: a run reported "128 submitted, 124
    /// accepted, 0 rejected" while four orders sat Rejected in the log with
    /// their reasons. Two call sites for one transition is what made that
    /// possible, so there is now one.
    ///
    /// That is the guarantee. The check is `reconcile()`, which compares the
    /// counter against a census of the orders and would have failed the run. Both
    /// are kept: see the guarantee-and-check argument in docs/AUDIT.md, which is
    /// about this exact pair.
    ///
    /// Does nothing if `id` is unknown or the order has already finished.
    void note_venue_refusal(OrderId id, Timestamp now);

    [[nodiscard]] const Order* find_order(OrderId id) const noexcept;
    [[nodiscard]] Order* mutable_order(OrderId id) noexcept;
    [[nodiscard]] std::vector<const Order*> working_orders() const;

    [[nodiscard]] const std::vector<Order>& orders() const noexcept { return orders_; }

    [[nodiscard]] Ledger& ledger() noexcept { return *ledger_; }

    [[nodiscard]] const Ledger& ledger() const noexcept { return *ledger_; }

    [[nodiscard]] OrderMap& order_map() noexcept { return order_map_; }

    [[nodiscard]] const OrderMap& order_map() const noexcept { return order_map_; }

    [[nodiscard]] OrderTrackerStats stats() const;

    /// Checks every tally against the orders and the blotter it claims to
    /// describe. Call it once the run has stopped: the socket thread mutates
    /// orders while it runs, so a mid-run census is a snapshot of a moving
    /// target rather than a failing identity.
    [[nodiscard]] StatsReconciliation reconcile() const;

    /// Tells the listener about a status. Public because a broker that refuses
    /// an order locally has to report that too.
    void emit_status(const Order& order, MS_OrderState state, bool terminal, const char* reason);

private:
    void book_fill(Order& order, Quantity quantity, Price price, Timestamp ts);

    Ledger* ledger_;
    IFillListener* listener_ = nullptr;

    std::vector<Order> orders_;
    std::map<OrderId, std::size_t> index_;
    OrderMap order_map_;

    /// The socket thread writes these and anyone may read them, so they are
    /// taken under a lock rather than made individually atomic -- there are a
    /// dozen of them and they are read as a set.
    mutable std::mutex stats_mutex_;
    OrderTrackerStats stats_;

    /// Orders created already-terminal by `register_refusal`, and fills that
    /// actually reached the blotter.
    ///
    /// Not in `OrderTrackerStats` because nothing prints them; they exist so
    /// `reconcile()` has something to compare `orders_` and the blotter against.
    /// Both are incremented in the same function that does the thing they
    /// count -- next to the `push_back`, next to the `apply_fill` -- which is
    /// the difference between a counter that cannot drift and the one that did.
    std::uint64_t registered_refusals_ = 0;
    std::uint64_t fills_booked_ = 0;

    OrderId next_sequence_ = 1;
};

}  // namespace marketscan::live

#endif  // MARKETSCAN_LIVE_ORDER_TRACKER_HPP
