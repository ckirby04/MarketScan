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

namespace {

/// Builds and delivers one status event.
///
/// Every terminal transition in this file goes through here, so a state that
/// forgets to notify is a state that does not compile rather than one a strategy
/// silently never hears about.
void notify_status(IFillListener* listener,
                   const Order& order,
                   Timestamp ts,
                   MS_OrderState state,
                   const char* reason) {
    if (listener == nullptr) {
        return;
    }
    MS_OrderStatus status{};
    status.ts = ts;
    status.order_id = order.id;
    status.client_order_id = order.request.client_order_id;
    status.state = static_cast<std::uint32_t>(state);
    status.terminal = state == MS_ORDER_ACCEPTED ? 0U : 1U;
    status.filled_quantity = order.filled_quantity;
    status.replaced_by = 0;
    // Never null: a plugin should not have to check, and an empty string is
    // what "no further detail" looks like.
    status.reason = reason != nullptr ? reason : "";
    listener->on_order_status(status);
}

}  // namespace

OrderId Broker::submit(const MS_OrderRequest& request, Timestamp now, Timestamp decision_ts) {
    Order order;
    order.id = order_id_base_ + next_sequence_++;
    order.request = request;
    order.decision_ts = decision_ts;
    order.submitted_ts = now;
    order.last_update_ts = now;
    order.eligible_ts = now;
    order.submitted_day = session_day(zone_, now);
    order.status = OrderStatus::Working;

    const auto reject = [&](RejectReason reason) {
        order.status = OrderStatus::Rejected;
        order.reject_reason = reason;
        ++rejected_count_;
        orders_.push_back(order);
        // Before returning: a strategy that submitted and got kInvalidOrderId
        // back knows the call failed, but a strategy that stores order ids and
        // waits needs the event too, and live it is the only notification there
        // is.
        notify_status(listener_, orders_.back(), now, MS_ORDER_REJECTED, to_string(reason).data());
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

    // A rejection the configuration asked for.
    //
    // After the validity checks and before anything is charged for, which is
    // where a venue refuses: the order was well formed and the venue said no
    // anyway. Drawn from the strategy's seeded stream, so this is reproducible.
    //
    // The draw happens only when the feature is on. A draw taken unconditionally
    // would consume a number from the stream and shift every later draw, which
    // would make turning the feature on change results that have nothing to do
    // with rejections.
    if (fill_model_->config().simulated_reject_probability > 0.0 && jitter_ != nullptr &&
        jitter_->uniform() < fill_model_->config().simulated_reject_probability) {
        return reject(RejectReason::SimulatedRejection);
    }

    // What the strategy could see when it decided. The blotter measures
    // slippage against this.
    order.reference_price = book.mid();

    // Everything above is validation, and validation is instantaneous even
    // live: a venue refuses a malformed order without the market hearing about
    // it. Flight time is charged from here, once the order is real.
    order.eligible_ts = now + flight_time();

    orders_.push_back(order);
    Order& stored = orders_.back();

    if (stored.eligible_ts > now) {
        // In flight. Nothing is priced and the book has not been asked;
        // whatever the market does between here and arrival happens to this
        // order too. A market order waits like anything else.
        working_.push_back(orders_.size() - 1);
        return stored.id;
    }

    const RejectReason outcome = try_fill(stored, now);
    if (resolves_on_arrival(stored)) {
        finalise_immediate(stored, now, outcome);
    }
    if (stored.is_working()) {
        working_.push_back(orders_.size() - 1);
    }

    if (stored.status == OrderStatus::Rejected) {
        ++rejected_count_;
        return kInvalidOrderId;
    }
    return stored.id;
}

Duration Broker::flight_time() {
    const FillModelConfig& config = fill_model_->config();
    if (!config.models_latency()) {
        return 0;
    }
    Duration total = config.submit_latency;
    if (config.latency_jitter > 0 && jitter_ != nullptr) {
        // Uniform on [0, jitter), from the run's seeded stream, so a replay
        // draws the same numbers in the same order and lands on the same
        // fills. Jitter taken from the machine would make the result depend on
        // how busy the machine was, which is the one thing a backtest must
        // never do.
        const double scale = static_cast<double>(config.latency_jitter);
        total += static_cast<Duration>(jitter_->uniform() * scale);
    }
    return total;
}

std::size_t Broker::in_flight_count(Timestamp now) const noexcept {
    std::size_t count = 0;
    for (const std::size_t index : working_) {
        count += orders_[index].is_in_flight(now) ? 1 : 0;
    }
    return count;
}

RejectReason Broker::try_fill(Order& order, Timestamp now) {
    const OrderBook& book = market_->book(order.request.symbol);

    // The market state this fill comes out of, read before the fill model runs
    // so that it is the state the price was actually computed from rather than
    // whatever the book holds by the time the row is written.
    const Timestamp price_ts = book.last_update();

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
                       result.commission,
                       order.decision_ts,
                       price_ts);

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
        // Nothing filled and no second chance: a market order does not rest
        // waiting for a price, and an IOC is cancel-the-rest by definition.
        order.status = OrderStatus::Rejected;
        order.reject_reason =
            model_reason != RejectReason::None ? model_reason : RejectReason::NotMarketable;
        notify_status(
            listener_, order, now, MS_ORDER_REJECTED, to_string(order.reject_reason).data());
        return;
    }
    if (order.request.type == MS_ORD_MARKET && fill_model_->config().models_partial_fills()) {
        // Part filled because the touch was not deep enough, not because the
        // price was wrong. A real market order does not give up here: it keeps
        // working through the book across the next prints, so the remainder
        // stays alive and is offered the next event for this symbol.
        return;
    }
    // Part filled with no way to finish: an IOC, by definition, or a market
    // order in a configuration modelling neither impact nor participation,
    // which is a test setup rather than a realistic one.
    order.status = OrderStatus::Cancelled;
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
        // Whatever has already filled stands. A cancel reaches the remainder
        // and nothing else: a fill is a trade that happened, and no
        // instruction sent afterwards makes it not have happened.
        order.status = OrderStatus::Cancelled;
        order.last_update_ts = now;
        notify_status(listener_,
                      order,
                      now,
                      MS_ORDER_CANCELLED,
                      order.filled_quantity > 0.0 ? "cancelled after a partial fill" : "cancelled");
        drop_from_working(id);
        return true;
    }

    // Not working. Either the id was never issued, or -- the case worth
    // counting -- the order finished before the cancel reached it. Live that
    // is a race with a real winner, and a strategy whose cancels are routinely
    // too late is one whose logic assumes a speed it does not have.
    const Order* known = find_order(id);
    if (known != nullptr && known->is_terminal()) {
        ++late_cancels_;
    }
    return false;
}

void Broker::on_market_event(SymbolId symbol, Timestamp now) {
    if (!seen_any_event_) {
        last_day_ = session_day(zone_, now);
        seen_any_event_ = true;
    } else {
        const std::int64_t today = session_day(zone_, now);
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
        if (now < order.eligible_ts) {
            continue;  // still crossing the wire
        }
        const RejectReason outcome = try_fill(order, now);
        if (order.status == OrderStatus::Rejected) {
            // Refused on arrival -- by the cash check, typically, after the
            // price moved while the order was in flight. Reported the way a
            // refusal at submission is: counted, and told to the strategy.
            // finalise_immediate returns early on a status try_fill already
            // set, and before this an order refused here simply vanished: the
            // strategy was never told, kept waiting for a fill, and the count
            // said nothing had been refused.
            ++rejected_count_;
            notify_status(
                listener_, order, now, MS_ORDER_REJECTED, to_string(order.reject_reason).data());
            continue;
        }
        if (resolves_on_arrival(order)) {
            // An order with one chance takes it on the event it arrives, and
            // that event is this one. Reaching here at all means it was in
            // flight until now, because a zero-latency one resolved in
            // submit().
            finalise_immediate(order, now, outcome);
        }
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
            order.submitted_day != session_day(zone_, now)) {
            order.status = OrderStatus::Expired;
            order.last_update_ts = now;
            notify_status(
                listener_, order, now, MS_ORDER_EXPIRED, "a DAY order outlived its session");
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
    late_cancels_ = 0;
    last_day_ = 0;
    seen_any_event_ = false;
}

}  // namespace marketscan
