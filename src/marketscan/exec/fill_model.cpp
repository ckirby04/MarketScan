#include "marketscan/exec/fill_model.hpp"

#include <algorithm>
#include <cmath>

namespace marketscan {
namespace {

/// Worsens `price` by `bps`, in whichever direction hurts the aggressor.
[[nodiscard]] Price worsen(Price price, Side side, double bps) noexcept {
    if (bps == 0.0) {
        return price;
    }
    const double factor = bps * 1e-4;
    return side == MS_SIDE_BUY ? price * (1.0 + factor) : price * (1.0 - factor);
}

/// True when `price` is outside what a limit order is willing to pay or accept.
[[nodiscard]] bool violates_limit(Price price, Side side, Price limit) noexcept {
    return side == MS_SIDE_BUY ? price > limit : price < limit;
}

[[nodiscard]] bool is_sane_quantity(Quantity q) noexcept {
    return std::isfinite(q) && q > 0.0;
}

[[nodiscard]] bool is_sane_price(Price p) noexcept {
    return std::isfinite(p) && p > 0.0;
}

}  // namespace

Money CommissionSchedule::charge(Quantity quantity) const noexcept {
    Money total = per_share.scaled(quantity) + per_order;
    return total < minimum ? minimum : total;
}

FillResult SpreadCrossingFillModel::evaluate(const FillRequest& request) const {
    FillResult result;
    if (request.order == nullptr || request.book == nullptr) {
        result.reason = RejectReason::NoMarketData;
        return result;
    }

    const Order& order = *request.order;
    const OrderBook& book = *request.book;
    const Side side = order.request.side;

    const Quantity wanted = order.remaining();
    if (!is_sane_quantity(wanted)) {
        result.reason = RejectReason::InvalidQuantity;
        return result;
    }

    const bool is_limit = order.request.type == MS_ORD_LIMIT;
    const Price limit = order.request.limit_price;
    if (is_limit && !is_sane_price(limit)) {
        result.reason = RejectReason::InvalidPrice;
        return result;
    }

    // Asks for a buy, bids for a sell: the side the aggressor consumes.
    const std::vector<BookLevel>& levels = book.levels_for(side);
    if (levels.empty()) {
        result.reason = RejectReason::NoMarketData;
        return result;
    }

    // --- Participation cap ----------------------------------------------
    //
    // A real order for four times the displayed size does not clear in one
    // print. Capping what one event may take is what turns a single fill into
    // a sequence of them: the remainder stays working and is offered the next
    // event, at whatever the book says then.
    //
    // Measured against the size at the touch rather than the whole book,
    // because the touch is what is actually available to an aggressor now.
    Quantity remaining = wanted;
    if (config_.models_partial_fills()) {
        const Quantity slice = config_.max_participation * levels.front().size;
        if (slice > 0.0 && slice < remaining) {
            remaining = slice;
        }
    }

    Quantity filled = 0.0;
    double gross = 0.0;  // running sum of price * quantity
    Quantity displayed = 0.0;

    // --- Walk the real book, best level first ---------------------------
    //
    // With an L1 feed there is one level here. With L2 or MBO there are many,
    // and this loop is already the code that handles them.
    for (const BookLevel& level : levels) {
        displayed += level.size;
        if (remaining <= 0.0) {
            continue;  // keep totalling displayed size for the impact base
        }
        const Price price = worsen(level.price, side, config_.slippage_bps);
        if (is_limit && violates_limit(price, side, limit)) {
            break;  // and every level past this one is worse still
        }
        const Quantity take = std::min(remaining, level.size);
        filled += take;
        gross += price * take;
        remaining -= take;
    }

    // --- Past the end of the book ---------------------------------------
    //
    // Nothing is displayed here, so nothing about this is observed. The
    // square-root curve is a modelling assumption whose only job is to make
    // the cost of size show up in the slippage column instead of being free.
    if (remaining > 0.0 && displayed > 0.0 && config_.models_impact()) {
        const Price base = worsen(levels.back().price, side, config_.slippage_bps);

        Quantity excess = remaining;
        if (is_limit) {
            // Solve for the largest excess whose price stays within the limit.
            // impact_bps(e) = coefficient * sqrt(e / displayed), so
            //   e_max = displayed * (allowed_bps / coefficient)^2.
            const double allowed_bps =
                side == MS_SIDE_BUY ? (limit / base - 1.0) * 1e4 : (1.0 - limit / base) * 1e4;
            if (allowed_bps <= 0.0) {
                excess = 0.0;
            } else {
                const double ratio = allowed_bps / config_.impact_coefficient_bps;
                excess = std::min(excess, displayed * ratio * ratio);
            }
        }

        if (excess > 0.0) {
            const double impact_bps =
                config_.impact_coefficient_bps * std::sqrt(excess / displayed);
            const Price price = worsen(base, side, impact_bps);
            filled += excess;
            gross += price * excess;
            remaining -= excess;
        }
    }

    if (filled <= 0.0) {
        // Structurally unfillable right now. Whether that is fatal depends on
        // the order type, which is the broker's decision, not this model's.
        return result;
    }

    // The average price is a computed quantity: summing price * size and
    // dividing by the total can land a few ulps on the better side of the
    // touch. Clamping here costs nothing and turns "a market order never fills
    // better than the touch" from a property that is almost always true into
    // one that is exactly true, which is the kind of guarantee worth having.
    const Price touch = worsen(levels.front().price, side, config_.slippage_bps);
    const Price average = gross / filled;

    result.quantity = filled;
    result.price = side == MS_SIDE_BUY ? std::max(average, touch) : std::min(average, touch);
    result.commission = config_.commission.charge(filled);
    return result;
}

FillResult OptimisticFillModel::evaluate(const FillRequest& request) const {
    FillResult result;
    if (request.order == nullptr || request.book == nullptr) {
        result.reason = RejectReason::NoMarketData;
        return result;
    }

    const Order& order = *request.order;
    const OrderBook& book = *request.book;

    const Quantity wanted = order.remaining();
    if (!is_sane_quantity(wanted)) {
        result.reason = RejectReason::InvalidQuantity;
        return result;
    }

    // The last print, with no spread, no depth and no size penalty. This is
    // the assumption the realistic model exists to argue with.
    Price price = book.last_trade();
    if (!is_sane_price(price)) {
        price = book.mark_price();
    }
    if (!is_sane_price(price)) {
        result.reason = RejectReason::NoMarketData;
        return result;
    }

    // Even a fantasy fills within its limit; a limit order trading through its
    // own price would not be optimistic, it would be incoherent.
    if (order.request.type == MS_ORD_LIMIT) {
        if (!is_sane_price(order.request.limit_price)) {
            result.reason = RejectReason::InvalidPrice;
            return result;
        }
        if (violates_limit(price, order.request.side, order.request.limit_price)) {
            return result;
        }
    }

    result.quantity = wanted;
    result.price = price;
    // Commission is still charged. This model is a claim about execution
    // price, not an argument that trading is free -- and holding everything
    // else equal is what makes the comparison with the realistic model mean
    // something.
    result.commission = config_.commission.charge(wanted);
    return result;
}

}  // namespace marketscan
