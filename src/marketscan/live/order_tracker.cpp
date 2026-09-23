#include "marketscan/live/order_tracker.hpp"

#include <cstdio>
#include <optional>
#include <utility>

#include "marketscan/live/detail.hpp"
#include "marketscan/util/logging.hpp"

namespace marketscan::live {
namespace {

using detail::number;

}  // namespace

OrderTracker::OrderTracker(Ledger* ledger, std::string run_token)
    : ledger_(ledger), order_map_(std::move(run_token)) {}

const Order* OrderTracker::find_order(OrderId id) const noexcept {
    const auto it = index_.find(id);
    return it == index_.end() ? nullptr : &orders_[it->second];
}

Order* OrderTracker::mutable_order(OrderId id) noexcept {
    const auto it = index_.find(id);
    return it == index_.end() ? nullptr : &orders_[it->second];
}

std::vector<const Order*> OrderTracker::working_orders() const {
    std::vector<const Order*> out;
    for (const Order& order : orders_) {
        if (order.is_working()) {
            out.push_back(&order);
        }
    }
    return out;
}

OrderTrackerStats OrderTracker::stats() const {
    const std::lock_guard<std::mutex> lock(stats_mutex_);
    return stats_;
}

void OrderTracker::note_lost_cancel_race() {
    const std::lock_guard<std::mutex> lock(stats_mutex_);
    ++stats_.lost_cancel_races;
}

void OrderTracker::note_rail_refusal() {
    const std::lock_guard<std::mutex> lock(stats_mutex_);
    ++stats_.refused_by_rail;
}

void OrderTracker::note_venue_refusal(OrderId id, Timestamp now) {
    Order* order = mutable_order(id);
    if (order == nullptr || order->is_terminal()) {
        // Unknown, or already over. Counting it would make `rejected` exceed the
        // number of orders in Rejected and fail the very identity below.
        return;
    }

    order->status = OrderStatus::Rejected;
    order->reject_reason = RejectReason::NotMarketable;
    order->last_update_ts = now;

    const std::lock_guard<std::mutex> lock(stats_mutex_);
    ++stats_.rejected;
}

bool StatsReconciliation::ok() const noexcept {
    for (const StatsIdentity& identity : identities) {
        if (!identity.holds()) {
            return false;
        }
    }
    return true;
}

std::string StatsReconciliation::describe() const {
    std::string out;
    for (const StatsIdentity& identity : identities) {
        if (identity.holds()) {
            continue;
        }
        out += "  ";
        out += identity.name;
        out += "  ->  ";
        out += std::to_string(identity.left);
        out += identity.at_most ? " <= " : " != ";
        out += std::to_string(identity.right);
        out += "\n";
    }
    return out;
}

StatsReconciliation OrderTracker::reconcile() const {
    StatsReconciliation out;

    // The census: what the orders themselves say. This is the source of truth
    // the counters are checked against, and it is derived here rather than
    // maintained anywhere.
    for (const Order& order : orders_) {
        switch (order.status) {
            case OrderStatus::Working:
                ++out.working;
                break;
            case OrderStatus::PartiallyFilled:
                ++out.partially_filled;
                break;
            case OrderStatus::Filled:
                ++out.filled;
                break;
            case OrderStatus::Cancelled:
                ++out.cancelled;
                break;
            case OrderStatus::Expired:
                ++out.expired;
                break;
            case OrderStatus::Rejected:
                ++out.rejected;
                break;
        }
    }

    const OrderTrackerStats s = stats();
    const std::uint64_t registered = static_cast<std::uint64_t>(orders_.size());

    // Exactly three functions push to orders_: two count a submission, one
    // counts a registered refusal. Nothing else may create an order.
    out.identities.push_back({"orders registered == submitted + refusals registered",
                              registered,
                              s.submitted + registered_refusals_,
                              false});

    // The three identities that catch a dropped terminal counter. An order
    // enters a terminal state at most once -- apply_update short-circuits on
    // is_terminal() -- so each of these is one-to-one rather than approximate.
    //
    // The second of them is the one that would have failed the 128/124/0 run.
    out.identities.push_back({"orders in Rejected == rejected", out.rejected, s.rejected, false});
    out.identities.push_back({"orders in Expired == expired", out.expired, s.expired, false});
    out.identities.push_back({"orders in Cancelled == cancelled + replaced",
                              out.cancelled,
                              s.cancelled + s.replaced,
                              false});

    // Every blotter row came from book_fill and every book_fill wrote one row.
    // The ledger belongs to the broker that owns this tracker and nothing else
    // books into it, so this is an equality and not a bound.
    out.identities.push_back({"blotter rows == fills booked",
                              static_cast<std::uint64_t>(ledger_->blotter().size()),
                              fills_booked_,
                              false});

    // A rail refusal always registers a refusal; a replay divergence registers
    // one without being a rail refusal. Hence <=, stated as such.
    out.identities.push_back(
        {"rail refusals <= refusals registered", s.refused_by_rail, registered_refusals_, true});

    return out;
}

OrderId OrderTracker::register_order(const MS_OrderRequest& request,
                                     Timestamp now,
                                     Timestamp decision_ts,
                                     OrderId base) {
    Order order;
    order.id = base + next_sequence_++;
    order.request = request;
    order.submitted_ts = now;
    order.last_update_ts = now;
    order.eligible_ts = now;
    order.decision_ts = decision_ts;
    order.status = OrderStatus::Working;

    index_[order.id] = orders_.size();
    orders_.push_back(order);
    order_map_.record_submitted(order.id);

    {
        const std::lock_guard<std::mutex> lock(stats_mutex_);
        ++stats_.submitted;
    }
    return order.id;
}

OrderId OrderTracker::register_order_with_id(const MS_OrderRequest& request,
                                             Timestamp now,
                                             Timestamp decision_ts,
                                             OrderId id) {
    Order order;
    order.id = id;
    order.request = request;
    order.submitted_ts = now;
    order.last_update_ts = now;
    order.eligible_ts = now;
    order.decision_ts = decision_ts;
    order.status = OrderStatus::Working;

    index_[order.id] = orders_.size();
    orders_.push_back(order);
    order_map_.record_submitted(order.id);

    // Keep the running sequence ahead of any id handed in, so a later
    // register_order cannot collide with one of these.
    if (id >= next_sequence_) {
        next_sequence_ = id + 1;
    }

    {
        const std::lock_guard<std::mutex> lock(stats_mutex_);
        ++stats_.submitted;
    }
    return order.id;
}

OrderId OrderTracker::register_refusal(const MS_OrderRequest& request,
                                       Timestamp now,
                                       Timestamp decision_ts,
                                       OrderId base,
                                       RejectReason reason,
                                       const char* message) {
    Order order;
    order.id = base + next_sequence_++;
    order.request = request;
    order.submitted_ts = now;
    order.last_update_ts = now;
    order.eligible_ts = now;
    order.decision_ts = decision_ts;
    order.status = OrderStatus::Rejected;
    order.reject_reason = reason;

    index_[order.id] = orders_.size();
    orders_.push_back(order);
    ++registered_refusals_;

    {
        const std::lock_guard<std::mutex> lock(stats_mutex_);
        ++stats_.rejected;
    }

    // Refused, and visible: the order is in the log with its reason rather than
    // simply absent.
    emit_status(orders_.back(), MS_ORDER_REJECTED, true, message);
    return order.id;
}

void OrderTracker::emit_status(const Order& order,
                               MS_OrderState state,
                               bool terminal,
                               const char* reason) {
    if (listener_ == nullptr) {
        return;
    }
    MS_OrderStatus status{};
    status.ts = order.last_update_ts;
    status.order_id = order.id;
    status.client_order_id = order.request.client_order_id;
    status.state = state;
    status.terminal = terminal ? 1u : 0u;
    status.filled_quantity = order.filled_quantity;
    status.replaced_by = 0;
    status.reason = reason;
    listener_->on_order_status(status);
}

void OrderTracker::book_fill(Order& order, Quantity quantity, Price price, Timestamp ts) {
    if (!(quantity > 0.0) || !(price > 0.0)) {
        return;
    }

    const Quantity before = order.filled_quantity;
    order.filled_quantity = before + quantity;
    order.average_fill_price =
        before > 0.0
            ? (order.average_fill_price * before + price * quantity) / order.filled_quantity
            : price;
    order.last_update_ts = ts;
    order.status = order.filled_quantity + 1e-9 >= order.request.quantity
                       ? OrderStatus::Filled
                       : OrderStatus::PartiallyFilled;

    const TradeRecord& record = ledger_->apply_fill(ts,
                                                    order.id,
                                                    order.request.client_order_id,
                                                    order.request.symbol,
                                                    order.request.side,
                                                    quantity,
                                                    price,
                                                    order.reference_price,
                                                    Money{},
                                                    order.decision_ts,
                                                    ts);
    (void)record;
    // Next to the write it counts, so the two cannot come apart.
    ++fills_booked_;

    if (listener_ != nullptr) {
        MS_Fill fill{};
        fill.ts = ts;
        fill.symbol = order.request.symbol;
        fill.order_id = order.id;
        fill.client_order_id = order.request.client_order_id;
        fill.side = order.request.side;
        fill.price = price;
        fill.quantity = quantity;
        listener_->on_fill(fill);
    }
}

void OrderTracker::apply_update(const TradeUpdate& update, Timestamp now) {
    const std::optional<OrderId> engine_id = order_map_.engine_id_for(update.client_order_id);
    if (!engine_id.has_value()) {
        // Another run's order on the same account, or a manual one. Counted so
        // that "the venue reported a fill nobody booked" is never silent.
        const std::lock_guard<std::mutex> lock(stats_mutex_);
        ++stats_.foreign_updates;
        return;
    }

    Order* order = mutable_order(*engine_id);
    if (order == nullptr) {
        const std::lock_guard<std::mutex> lock(stats_mutex_);
        ++stats_.foreign_updates;
        return;
    }

    if (order->is_terminal()) {
        // Already over here. The reclaim after a reconnect re-reads every order
        // the venue knows about, so this is the ordinary case rather than an
        // odd one -- and applying it again would emit a second terminal status
        // for one order.
        const std::lock_guard<std::mutex> lock(stats_mutex_);
        ++stats_.stale_updates;
        return;
    }

    if (!update.broker_order_id.empty()) {
        order_map_.record_acknowledged(order->id, update.broker_order_id);
    }

    const Timestamp ts = update.ts != 0 ? update.ts : now;
    order->last_update_ts = ts;

    switch (update.kind) {
        case UpdateKind::Accepted: {
            const std::lock_guard<std::mutex> lock(stats_mutex_);
            ++stats_.accepted;
            break;
        }

        case UpdateKind::PartialFill:
        case UpdateKind::Fill: {
            // The venue's cumulative number, not a running sum of our own. A
            // sum that missed one message would be quietly wrong forever;
            // reading the cumulative figure means a missed message costs one
            // blotter row's granularity and nothing else.
            const Quantity already = order->filled_quantity;
            Quantity increment = update.cumulative_filled - already;
            if (!(increment > 0.0)) {
                increment = update.fill_quantity;
            }
            const Price price =
                update.fill_price > 0.0 ? update.fill_price : update.average_fill_price;

            if (increment > 0.0 && price > 0.0) {
                book_fill(*order, increment, price, ts);
            }

            const std::lock_guard<std::mutex> lock(stats_mutex_);
            if (update.kind == UpdateKind::Fill) {
                ++stats_.filled;
            } else {
                ++stats_.partial_fills;
            }
            break;
        }

        case UpdateKind::Cancelled:
            order->status = OrderStatus::Cancelled;
            {
                const std::lock_guard<std::mutex> lock(stats_mutex_);
                ++stats_.cancelled;
            }
            break;

        case UpdateKind::Expired:
            order->status = OrderStatus::Expired;
            {
                const std::lock_guard<std::mutex> lock(stats_mutex_);
                ++stats_.expired;
            }
            break;

        case UpdateKind::Rejected:
            order->status = OrderStatus::Rejected;
            order->reject_reason = RejectReason::NotMarketable;
            {
                const std::lock_guard<std::mutex> lock(stats_mutex_);
                ++stats_.rejected;
            }
            break;

        case UpdateKind::Replaced:
            // A replacement ends the original order. Counted separately from a
            // cancel, because the venue did two different things -- and counted
            // at all only since the reconciliation went in, which is what found
            // that it was not.
            order->status = OrderStatus::Cancelled;
            {
                const std::lock_guard<std::mutex> lock(stats_mutex_);
                ++stats_.replaced;
            }
            break;

        case UpdateKind::Transient:
            // Alive, nothing to do, and deliberately not silence: the order
            // stays working and the next message decides.
            return;

        case UpdateKind::Unknown: {
            {
                const std::lock_guard<std::mutex> lock(stats_mutex_);
                ++stats_.unknown_updates;
            }
            log::write(MS_LOG_ERROR,
                       "broker",
                       "unknown trade update '" + update.raw_event + "' for order " +
                           std::to_string(order->id) +
                           "; the order is left working rather than assumed finished");
            return;
        }
    }

    if (update.kind == UpdateKind::Fill && order->status != OrderStatus::Filled) {
        // The venue says the order is done and our arithmetic says it is not.
        // Believe the venue about the state, say so about the difference.
        log::write(MS_LOG_ERROR,
                   "broker",
                   "order " + std::to_string(order->id) + " filled " +
                       number(order->filled_quantity, 4) + " of " +
                       number(order->request.quantity, 4) +
                       " but the venue calls it complete. Booking it complete and reporting the "
                       "difference rather than correcting it silently.");
        order->status = OrderStatus::Filled;
    }

    if (order->is_terminal()) {
        MS_OrderState state = MS_ORDER_CANCELLED;
        const char* reason = "cancelled";
        switch (order->status) {
            case OrderStatus::Filled:
                // A complete fill is reported through on_fill, and a terminal
                // status as well, so a strategy that only implements one of
                // the two callbacks still learns the order is over.
                state = MS_ORDER_ACCEPTED;
                reason = "filled";
                break;
            case OrderStatus::Expired:
                state = MS_ORDER_EXPIRED;
                reason = "expired at the venue";
                break;
            case OrderStatus::Rejected:
                state = MS_ORDER_REJECTED;
                reason = update.reason.empty() ? "rejected by the venue" : update.reason.c_str();
                break;
            default:
                break;
        }
        emit_status(*order, state, true, reason);
        order_map_.retire(order->id);
    }
}

}  // namespace marketscan::live
