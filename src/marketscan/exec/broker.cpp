#include "marketscan/exec/broker.hpp"

#include <algorithm>
#include <cmath>

namespace marketscan {
namespace {

/// Quantities are floating point, so "fully filled" cannot be an equality
/// test. This tolerance is far below any tradeable size and far above the
/// rounding error of summing a few fills.
constexpr Quantity kQuantityEpsilon = 1e-9;

[[nodiscard]] bool is_sane_quantity(Quantity q) noexcept {
    return std::isfinite(q) && q > 0.0;
}

[[nodiscard]] bool is_sane_price(Price p) noexcept {
    return std::isfinite(p) && p > 0.0;
}

}  // namespace

Broker::Broker(Money starting_cash,
               const IFillModel* fill_model,
               const MarketView* market,
               OrderId order_id_base)
    : ledger_(starting_cash),
      fill_model_(fill_model),
      market_(market),
      order_id_base_(order_id_base) {}

OrderId Broker::submit(const MS_OrderRequest& request, Timestamp now) {
    Order order;
    order.id = order_id_base_ + next_sequence_++;
    order.request = request;
    order.submitted_ts = now;
    order.last_update_ts = now;
    order.submitted_day = time::day_number(now);
    order.status = OrderStatus::Working;

    const auto reject = [&](RejectReason reason) {
        order.status = OrderStatus::Rejected;
        order.reject_reason = reason;
        ++rejected_count_;
        orders_.push_back(order);
        return kInvalidOrderId;
    };

    if (!is_sane_quantity(request.quantity)) {
        return reject(RejectReason::InvalidQuantity);
    }
    if (request.type == MS_ORD_LIMIT && !is_sane_price(request.limit_price)) {
        return reject(RejectReason::InvalidPrice);
    }
    if (request.symbol == kInvalidSymbol) {
        return reject(RejectReason::UnknownSymbol);
    }

    const OrderBook& book = market_->book(request.symbol);
    if (!book.initialised()) {
        // Nothing has ever been seen for this instrument, so there is no price
        // to trade against. Common on the very first event of a run, and a
        // strategy that acts before the market has spoken should be told.
        return reject(RejectReason::NoMarketData);
    }

    // What the strategy could see when it decided. The blotter measures
    // slippage against this.
    order.reference_price = book.mid();

    orders_.push_back(order);
    Order& stored = orders_.back();

    const RejectReason outcome = try_fill(stored, now);

    if (stored.request.type == MS_ORD_MARKET || stored.request.tif == MS_TIF_IOC) {
        finalise_immediate(stored, now, outcome);
    } else if (stored.is_working()) {
        working_.push_back(orders_.size() - 1);
    }

    if (stored.status == OrderStatus::Rejected) {
        ++rejected_count_;
        return kInvalidOrderId;
    }
    return stored.id;
}

RejectReason Broker::try_fill(Order& order, Timestamp now) {
    const OrderBook& book = market_->book(order.request.symbol);

    FillRequest request;
    request.order = &order;
    request.book = &book;
    request.ts = now;

    const FillResult result = fill_model_->evaluate(request);
    order.last_update_ts = now;

    if (result.reason == RejectReason::InvalidQuantity ||
        result.reason == RejectReason::InvalidPrice) {
        order.status = OrderStatus::Rejected;
        order.reject_reason = result.reason;
        return result.reason;
    }

    if (!result.filled()) {
        return result.reason;  // not now; a resting order will be asked again
    }

    // Cash account: the whole order is refused rather than part filled. A buy
    // costs cash; a sell credits it, and can still fail if the commission is
    // larger than the proceeds.
    const Quantity signed_qty = side_sign(order.request.side) * result.quantity;
    const Money cost = Money::notional(result.price, signed_qty) + result.commission;
    if (!ledger_.can_afford(cost)) {
        order.status = OrderStatus::Rejected;
        order.reject_reason = RejectReason::InsufficientCash;
        return RejectReason::InsufficientCash;
    }

    ledger_.apply_fill(now,
                       order.id,
                       order.request.client_order_id,
                       order.request.symbol,
                       order.request.side,
                       result.quantity,
                       result.price,
                       order.reference_price,
                       result.commission);

    // Quantity-weighted, so a limit order filled across several events reports
    // the average it actually achieved.
    const Quantity previous = order.filled_quantity;
    const Quantity total = previous + result.quantity;
    order.average_fill_price =
        (order.average_fill_price * previous + result.price * result.quantity) / total;
    order.filled_quantity = total;

    order.status =
        order.remaining() <= kQuantityEpsilon ? OrderStatus::Filled : OrderStatus::PartiallyFilled;

    if (listener_ != nullptr) {
        MS_Fill fill{};
        fill.ts = now;
        fill.symbol = order.request.symbol;
        fill.order_id = order.id;
        fill.client_order_id = order.request.client_order_id;
        fill.side = order.request.side;
        fill.price = result.price;
        fill.quantity = result.quantity;
        fill.commission = result.commission.to_abi();
        listener_->on_fill(fill);
    }
    return RejectReason::None;
}

void Broker::finalise_immediate(Order& order, Timestamp now, RejectReason model_reason) {
    if (order.status == OrderStatus::Rejected || order.status == OrderStatus::Filled) {
        return;
    }
    order.last_update_ts = now;
    if (order.filled_quantity <= 0.0) {
        // Nothing filled and no second chance: a market order does not rest,
        // and an IOC is cancel-the-rest by definition.
        order.status = OrderStatus::Rejected;
        order.reject_reason =
            model_reason != RejectReason::None ? model_reason : RejectReason::NotMarketable;
    } else {
        // Part filled with no way to finish. Only reachable with the impact
        // model disabled, which is a test configuration rather than a
        // realistic one.
        order.status = OrderStatus::Cancelled;
    }
}

bool Broker::cancel(OrderId id, Timestamp now) {
    for (const std::size_t index : working_) {
        Order& order = orders_[index];
        if (order.id != id) {
            continue;
        }
        if (!order.is_working()) {
            return false;
        }
        order.status = OrderStatus::Cancelled;
        order.last_update_ts = now;
        drop_from_working(id);
        return true;
    }
    return false;
}

void Broker::on_market_event(SymbolId symbol, Timestamp now) {
    if (!seen_any_event_) {
        last_day_ = time::day_number(now);
        seen_any_event_ = true;
    } else {
        const std::int64_t today = time::day_number(now);
        if (today != last_day_) {
            last_day_ = today;
            expire_day_orders(now);
        }
    }

    if (working_.empty()) {
        return;
    }

    // Submission order, which is stable across runs. Indices are copied first
    // because try_fill can mark an order terminal and mutate working_.
    candidates_.clear();
    for (const std::size_t index : working_) {
        if (orders_[index].request.symbol == symbol && orders_[index].is_working()) {
            candidates_.push_back(index);
        }
    }

    for (const std::size_t index : candidates_) {
        Order& order = orders_[index];
        if (!order.is_working()) {
            continue;
        }
        (void)try_fill(order, now);
    }

    // Retire anything that finished.
    working_.erase(
        std::remove_if(working_.begin(),
                       working_.end(),
                       [this](std::size_t index) { return !orders_[index].is_working(); }),
        working_.end());
}

void Broker::expire_day_orders(Timestamp now) {
    for (const std::size_t index : working_) {
        Order& order = orders_[index];
        if (order.is_working() && order.request.tif == MS_TIF_DAY &&
            order.submitted_day != time::day_number(now)) {
            order.status = OrderStatus::Expired;
            order.last_update_ts = now;
        }
    }
    working_.erase(
        std::remove_if(working_.begin(),
                       working_.end(),
                       [this](std::size_t index) { return !orders_[index].is_working(); }),
        working_.end());
}

void Broker::drop_from_working(OrderId id) {
    working_.erase(std::remove_if(working_.begin(),
                                  working_.end(),
                                  [&](std::size_t index) { return orders_[index].id == id; }),
                   working_.end());
}

const Order* Broker::find_order(OrderId id) const noexcept {
    for (const Order& order : orders_) {
        if (order.id == id) {
            return &order;
        }
    }
    return nullptr;
}

std::vector<const Order*> Broker::working_orders() const {
    std::vector<const Order*> result;
    result.reserve(working_.size());
    for (const std::size_t index : working_) {
        result.push_back(&orders_[index]);
    }
    return result;
}

void Broker::reset() {
    ledger_.reset();
    orders_.clear();
    working_.clear();
    candidates_.clear();
    next_sequence_ = 1;
    rejected_count_ = 0;
    last_day_ = 0;
    seen_any_event_ = false;
}

}  // namespace marketscan
