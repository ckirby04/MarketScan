#ifndef MARKETSCAN_EXEC_LEDGER_HPP
#define MARKETSCAN_EXEC_LEDGER_HPP

#include <cstdint>
#include <vector>

#include "marketscan/book/order_book.hpp"
#include "marketscan/exec/order.hpp"

namespace marketscan {

/// One instrument's holding.
///
/// `cost_basis` is the exact cash that went into the position -- positive for
/// a long, negative for a short, where the proceeds were received rather than
/// paid. Keeping it as Money rather than deriving it from a floating point
/// average cost is what makes the ledger's P&L decomposition hold exactly
/// instead of to within a few micro-dollars of rounding.
struct Position {
    SymbolId symbol = kInvalidSymbol;

    /// Signed. Negative is short.
    Quantity quantity = 0.0;

    Money cost_basis;

    /// Last price this position was marked at.
    Price mark = 0.0;

    /// Trading P&L booked by closes in this instrument, gross of commission.
    Money realized_pnl;

    [[nodiscard]] bool is_open() const noexcept { return quantity != 0.0; }

    [[nodiscard]] Money market_value() const noexcept { return Money::notional(mark, quantity); }

    [[nodiscard]] Money unrealized_pnl() const noexcept { return market_value() - cost_basis; }

    /// Per-unit cost, for display. Zero for a flat position.
    [[nodiscard]] double average_cost() const noexcept { return cost_basis.per_unit(quantity); }
};

struct EquityPoint {
    Timestamp ts = 0;
    Money equity;
    Money cash;
};

/// One strategy's virtual account.
///
/// Every strategy in a run owns one of these and can see no other. Cash is
/// exact integer micro-dollars; only prices are floating point.
///
/// **Semantics fixed for this build, and deliberately not invented silently:**
///
/// - *Short selling* is allowed. A sell beyond the current position opens a
///   short, and the proceeds are credited to cash immediately. Borrow cost and
///   locate availability are **not modelled** -- shorting here is free, which
///   it is not in reality.
/// - *This is a cash account.* Cash never goes negative. An order that would
///   overdraw is refused whole by the broker before it reaches the ledger,
///   rather than filled part way or silently levered.
/// - *No corporate actions.* Dividends, splits and distributions do not exist;
///   the datasets this build reads do not carry them.
/// - *Positions are marked at the mid*, not the bid, so opening a position
///   does not immediately book a loss equal to half the spread. The spread is
///   charged where it is actually paid, in the fill.
class Ledger {
public:
    explicit Ledger(Money starting_cash);

    /// Books a fill. Returns the blotter row it produced.
    ///
    /// `reference_price` is the mid at the moment the order was submitted; the
    /// difference between it and `fill_price` is the slippage column.
    const TradeRecord& apply_fill(Timestamp ts,
                                  OrderId order_id,
                                  std::uint64_t client_order_id,
                                  SymbolId symbol,
                                  Side side,
                                  Quantity quantity,
                                  Price fill_price,
                                  Price reference_price,
                                  Money commission);

    /// Revalues one position. Prices that are not positive and finite are
    /// ignored rather than allowed to zero out a holding.
    void mark(SymbolId symbol, Price price);

    /// Revalues every open position from the current market.
    void mark_all(const MarketView& market);

    [[nodiscard]] Money starting_cash() const noexcept { return starting_cash_; }

    [[nodiscard]] Money cash() const noexcept { return cash_; }

    [[nodiscard]] Money market_value() const noexcept;

    /// cash + the marked value of every position. This is the definition; see
    /// check_invariant() for the independent cross-check.
    [[nodiscard]] Money equity() const noexcept { return cash_ + market_value(); }

    [[nodiscard]] Money realized_pnl() const noexcept { return realized_pnl_; }

    [[nodiscard]] Money unrealized_pnl() const noexcept;

    [[nodiscard]] Money commission_paid() const noexcept { return commission_paid_; }

    /// Realized net of commission, plus unrealized. What the run has made.
    [[nodiscard]] Money total_pnl() const noexcept { return equity() - starting_cash_; }

    [[nodiscard]] Quantity position(SymbolId symbol) const noexcept;

    /// Null when nothing has ever been held in `symbol`.
    [[nodiscard]] const Position* find_position(SymbolId symbol) const noexcept;

    /// Open positions in ascending symbol id. Ascending, not hash order, so
    /// anything that walks this list behaves the same on every run.
    [[nodiscard]] std::vector<const Position*> open_positions() const;

    [[nodiscard]] const std::vector<TradeRecord>& blotter() const noexcept { return blotter_; }

    /// True when the account could pay `cost` without going negative.
    [[nodiscard]] bool can_afford(Money cost) const noexcept { return cost <= cash_; }

    // --- Equity curve ---------------------------------------------------

    void set_equity_sample_interval(Duration interval) noexcept { sample_interval_ = interval; }

    /// Appends a point if the sampling interval has elapsed since the last.
    void maybe_sample_equity(Timestamp ts);

    /// Appends a point unconditionally.
    void sample_equity(Timestamp ts);

    [[nodiscard]] const std::vector<EquityPoint>& equity_curve() const noexcept {
        return equity_curve_;
    }

    // --- Invariant ------------------------------------------------------

    /// equity == starting_cash + realized - commission + unrealized.
    ///
    /// This holds **exactly**, not to within rounding, because cost basis is
    /// tracked as exact Money and every path that moves value between cash,
    /// basis and realized P&L moves the identical integer out of one and into
    /// another. A failure here means value was created or destroyed.
    [[nodiscard]] bool check_invariant() const noexcept;

    /// The invariant's two sides, for a test that wants to print them.
    [[nodiscard]] Money invariant_lhs() const noexcept { return equity(); }

    [[nodiscard]] Money invariant_rhs() const noexcept {
        return starting_cash_ + realized_pnl_ - commission_paid_ + unrealized_pnl();
    }

    void reset();

private:
    [[nodiscard]] Position& position_for(SymbolId symbol);

    Money starting_cash_;
    Money cash_;
    Money realized_pnl_;
    Money commission_paid_;

    // Index is symbol id - 1; dense, and iterated in ascending id order.
    std::vector<Position> positions_;

    std::vector<TradeRecord> blotter_;
    std::vector<EquityPoint> equity_curve_;

    Duration sample_interval_ = time::kMinute;
    Timestamp last_sample_ts_ = kNever;
};

}  // namespace marketscan

#endif  // MARKETSCAN_EXEC_LEDGER_HPP
