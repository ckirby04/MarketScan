#ifndef MARKETSCAN_EXEC_ORDER_HPP
#define MARKETSCAN_EXEC_ORDER_HPP

#include <cstdint>
#include <string_view>

#include "marketscan/core/money.hpp"
#include "marketscan/core/types.hpp"

namespace marketscan {

using OrderId = std::uint64_t;

/// Zero is never a valid order id; it is what submit_order returns on refusal.
inline constexpr OrderId kInvalidOrderId = 0;

/// Client order id stamped on a liquidation the engine performed on a
/// strategy's behalf, because the security left the universe.
///
/// A distinct marker rather than an ordinary sale: a forced exit is not a
/// decision the strategy made, and a blotter that showed it as one would credit
/// or blame a strategy for a trade it never chose. The value is deliberately
/// absurd so it cannot collide with a client id anyone assigned on purpose.
inline constexpr std::uint64_t kForcedExitClientId = 0xDEAD'0E17ULL;

enum class OrderStatus : std::uint8_t {
    /// Accepted and resting, waiting for a price that makes it marketable.
    Working = 0,
    PartiallyFilled = 1,
    Filled = 2,
    Cancelled = 3,
    /// A DAY order that outlived its session.
    Expired = 4,
    Rejected = 5,
};

[[nodiscard]] constexpr std::string_view to_string(OrderStatus s) noexcept {
    switch (s) {
        case OrderStatus::Working:
            return "working";
        case OrderStatus::PartiallyFilled:
            return "partial";
        case OrderStatus::Filled:
            return "filled";
        case OrderStatus::Cancelled:
            return "cancelled";
        case OrderStatus::Expired:
            return "expired";
        case OrderStatus::Rejected:
            return "rejected";
    }
    return "?";
}

/// Why an order did not do what was asked.
///
/// Every refusal carries one of these and it is surfaced in the blotter and
/// the log. An order that vanishes without a reason is indistinguishable from
/// a strategy bug.
enum class RejectReason : std::uint8_t {
    None = 0,
    /// Quantity was zero, negative, or not finite.
    InvalidQuantity,
    /// A limit order with a non-positive or non-finite limit price.
    InvalidPrice,
    /// The symbol is not in this run's symbol table.
    UnknownSymbol,
    /// Nothing has ever been seen for this symbol, so there is no price to
    /// trade against. Common on the very first event of a run.
    NoMarketData,
    /// This is a cash account. The order would have taken the balance below
    /// zero, so it was refused whole rather than filled part way.
    InsufficientCash,
    /// An immediate-or-cancel order that could not be filled at all.
    NotMarketable,
};

[[nodiscard]] constexpr std::string_view to_string(RejectReason r) noexcept {
    switch (r) {
        case RejectReason::None:
            return "";
        case RejectReason::InvalidQuantity:
            return "invalid quantity";
        case RejectReason::InvalidPrice:
            return "invalid limit price";
        case RejectReason::UnknownSymbol:
            return "unknown symbol";
        case RejectReason::NoMarketData:
            return "no market data for symbol";
        case RejectReason::InsufficientCash:
            return "insufficient cash";
        case RejectReason::NotMarketable:
            return "not marketable";
    }
    return "?";
}

/// An order as the engine tracks it.
///
/// `request` is exactly what the strategy submitted, kept verbatim so the
/// blotter can show what was asked next to what happened.
struct Order {
    OrderId id = kInvalidOrderId;
    MS_OrderRequest request{};

    Timestamp submitted_ts = 0;
    Timestamp last_update_ts = 0;

    /// The first instant at which this order may fill.
    ///
    /// Equal to submitted_ts when the fill model models no latency, which is
    /// every configuration that existed before M1b. When it is later, the
    /// order is *in flight*: accepted, alive, and not yet fillable. It is
    /// still exposed to the market the whole time -- the price it eventually
    /// gets is the price when it arrives, not the price when it was sent,
    /// which is the entire point.
    Timestamp eligible_ts = 0;

    OrderStatus status = OrderStatus::Working;
    RejectReason reject_reason = RejectReason::None;

    Quantity filled_quantity = 0.0;

    /// Quantity-weighted average of every fill on this order.
    Price average_fill_price = 0.0;

    /// Mid price at the moment of submission. The blotter measures slippage
    /// against this: it is what the strategy could see when it decided.
    Price reference_price = 0.0;

    /// UTC day number at submission, for DAY expiry.
    std::int64_t submitted_day = 0;

    [[nodiscard]] Quantity remaining() const noexcept {
        const Quantity left = request.quantity - filled_quantity;
        return left > 0.0 ? left : 0.0;
    }

    [[nodiscard]] bool is_terminal() const noexcept {
        return status == OrderStatus::Filled || status == OrderStatus::Cancelled ||
               status == OrderStatus::Expired || status == OrderStatus::Rejected;
    }

    [[nodiscard]] bool is_working() const noexcept {
        return status == OrderStatus::Working || status == OrderStatus::PartiallyFilled;
    }

    /// Accepted, not yet fillable, and not yet in the market's hands.
    [[nodiscard]] bool is_in_flight(Timestamp now) const noexcept {
        return is_working() && now < eligible_ts;
    }

    /// How long this order has been alive. What a strategy asks when deciding
    /// whether to give up on it.
    [[nodiscard]] Duration age(Timestamp now) const noexcept {
        return now > submitted_ts ? now - submitted_ts : 0;
    }
};

/// A row of the trade blotter.
///
/// Slippage gets its own column because separating "the signal was wrong" from
/// "I could not get filled" is the entire point of simulating execution.
struct TradeRecord {
    Timestamp ts = 0;
    OrderId order_id = kInvalidOrderId;
    std::uint64_t client_order_id = 0;
    SymbolId symbol = kInvalidSymbol;
    Side side = MS_SIDE_BUY;

    Quantity quantity = 0.0;

    /// Mid at the moment the order was submitted.
    Price reference_price = 0.0;
    Price fill_price = 0.0;

    /// Signed cost of not trading at the reference price: positive means the
    /// fill was worse than what the strategy saw.
    Money slippage;
    Money commission;

    /// Change in cash from this fill, commission included.
    Money cash_delta;

    /// Trading P&L booked by the part of this fill that closed a position.
    Money realized_pnl;
};

}  // namespace marketscan

#endif  // MARKETSCAN_EXEC_ORDER_HPP
