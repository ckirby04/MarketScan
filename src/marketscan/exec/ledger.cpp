#include "marketscan/exec/ledger.hpp"

#include "marketscan/exec/delisting.hpp"

#include <cmath>

namespace marketscan {
namespace {

[[nodiscard]] bool is_usable_price(Price p) noexcept {
    return std::isfinite(p) && p > 0.0;
}

}  // namespace

Ledger::Ledger(Money starting_cash) : starting_cash_(starting_cash), cash_(starting_cash) {}

Position& Ledger::position_for(SymbolId symbol) {
    if (symbol > positions_.size()) {
        positions_.resize(symbol);
    }
    Position& position = positions_[symbol - 1];
    position.symbol = symbol;
    return position;
}

const TradeRecord& Ledger::apply_fill(Timestamp ts,
                                      OrderId order_id,
                                      std::uint64_t client_order_id,
                                      SymbolId symbol,
                                      Side side,
                                      Quantity quantity,
                                      Price fill_price,
                                      Price reference_price,
                                      Money commission,
                                      Timestamp decision_ts,
                                      Timestamp price_ts) {
    Position& position = position_for(symbol);

    // Positive when units come in, negative when they go out.
    const Quantity signed_qty = side_sign(side) * quantity;

    // Cash leaving the account for this trade. A sell makes this negative, so
    // the same line credits the proceeds -- including for a short sale, where
    // there was no position to deliver from.
    const Money trade_notional = Money::notional(fill_price, signed_qty);

    const Quantity old_qty = position.quantity;
    const Quantity new_qty = old_qty + signed_qty;

    Money realized_delta;

    const bool reduces = old_qty != 0.0 && ((old_qty > 0.0) != (signed_qty > 0.0));
    if (reduces) {
        const Quantity closed = std::min(std::fabs(signed_qty), std::fabs(old_qty));
        const bool full_close = closed >= std::fabs(old_qty);

        // The share of the basis this close retires. Rounded once, and the
        // identical value is both removed from the basis and used to compute
        // the realized P&L, so nothing is created or destroyed between them.
        // A full close releases the whole basis rather than a scaled copy of
        // it, which would leave a rounding crumb behind to reappear later as
        // phantom unrealized P&L on a flat position.
        const Money released = full_close ? position.cost_basis
                                          : position.cost_basis.scaled(closed / std::fabs(old_qty));

        // The part of this fill that did the closing, signed opposite to the
        // position it closed.
        const Money close_notional = Money::notional(fill_price, old_qty > 0.0 ? -closed : closed);

        // Whatever is left flips the position and opens fresh. Derived by
        // subtraction rather than recomputed from the quantity, so the two
        // pieces sum to exactly the whole: rounding each independently would
        // leave the ledger a micro-dollar out of balance on every flip.
        const Money open_notional = trade_notional - close_notional;

        realized_delta = -close_notional - released;
        position.cost_basis -= released;
        position.cost_basis += open_notional;
        position.realized_pnl += realized_delta;
        realized_pnl_ += realized_delta;
    } else {
        // Opening, or adding in the direction already held.
        position.cost_basis += trade_notional;
    }

    position.quantity = new_qty;

    cash_ -= trade_notional;
    cash_ -= commission;
    commission_paid_ += commission;

    if (is_usable_price(fill_price)) {
        position.mark = fill_price;
    }

    TradeRecord record;
    record.ts = ts;
    record.order_id = order_id;
    record.client_order_id = client_order_id;
    record.symbol = symbol;
    record.side = side;
    record.quantity = quantity;
    record.reference_price = reference_price;
    record.fill_price = fill_price;
    // Positive means the fill was worse than what the strategy could see.
    record.slippage =
        is_usable_price(reference_price)
            ? Money::notional((fill_price - reference_price) * side_sign(side), quantity)
            : Money{};
    record.commission = commission;
    record.cash_delta = -trade_notional - commission;
    record.realized_pnl = realized_delta;
    record.decision_ts = decision_ts;
    record.price_ts = price_ts;

    blotter_.push_back(record);
    return blotter_.back();
}

const DelistingAdjustment& Ledger::apply_delisting_return(
    Timestamp ts, SymbolId symbol, Quantity quantity, Price mark, double delisting_return) {
    // Computed here rather than taken as a Money, so that the one expression
    // `delisting_adjustment` documents is the only one in the build. Both layers
    // form `mark * delisting_return` first and round once.
    const Money amount = delisting_adjustment(mark, quantity, delisting_return);

    // The identical integer out of one account and into another, which is what
    // keeps check_invariant() exact across this path.
    cash_ += amount;
    realized_pnl_ += amount;
    delisting_pnl_ += amount;

    delisting_adjustments_.push_back(
        DelistingAdjustment{ts, symbol, quantity, mark, delisting_return, amount});
    return delisting_adjustments_.back();
}

void Ledger::mark(SymbolId symbol, Price price) {
    if (!is_usable_price(price) || symbol == kInvalidSymbol || symbol > positions_.size()) {
        return;
    }
    positions_[symbol - 1].mark = price;
}

void Ledger::mark_all(const MarketView& market) {
    for (std::size_t i = 0; i < positions_.size(); ++i) {
        Position& position = positions_[i];
        if (!position.is_open()) {
            continue;
        }
        const Price price = market.book(static_cast<SymbolId>(i + 1)).mark_price();
        if (is_usable_price(price)) {
            position.mark = price;
        }
    }
}

Money Ledger::market_value() const noexcept {
    Money total;
    for (const Position& position : positions_) {
        if (position.is_open()) {
            total += position.market_value();
        }
    }
    return total;
}

Money Ledger::unrealized_pnl() const noexcept {
    Money total;
    for (const Position& position : positions_) {
        if (position.is_open()) {
            total += position.unrealized_pnl();
        }
    }
    return total;
}

Quantity Ledger::position(SymbolId symbol) const noexcept {
    const Position* found = find_position(symbol);
    return found == nullptr ? 0.0 : found->quantity;
}

const Position* Ledger::find_position(SymbolId symbol) const noexcept {
    if (symbol == kInvalidSymbol || symbol > positions_.size()) {
        return nullptr;
    }
    return &positions_[symbol - 1];
}

std::vector<const Position*> Ledger::open_positions() const {
    std::vector<const Position*> result;
    for (const Position& position : positions_) {
        if (position.is_open()) {
            result.push_back(&position);
        }
    }
    return result;
}

void Ledger::maybe_sample_equity(Timestamp ts) {
    if (last_sample_ts_ != kNever && sample_interval_ > 0 &&
        ts - last_sample_ts_ < sample_interval_) {
        return;
    }
    sample_equity(ts);
}

void Ledger::sample_equity(Timestamp ts) {
    equity_curve_.push_back(EquityPoint{ts, equity(), cash_});
    last_sample_ts_ = ts;
}

bool Ledger::check_invariant() const noexcept {
    return invariant_lhs() == invariant_rhs();
}

void Ledger::reset() {
    cash_ = starting_cash_;
    realized_pnl_ = Money{};
    commission_paid_ = Money{};
    positions_.clear();
    blotter_.clear();
    delisting_adjustments_.clear();
    delisting_pnl_ = Money{};
    equity_curve_.clear();
    last_sample_ts_ = kNever;
}

}  // namespace marketscan
