#ifndef MARKETSCAN_BOOK_ORDER_BOOK_HPP
#define MARKETSCAN_BOOK_ORDER_BOOK_HPP

#include <cstddef>
#include <vector>

#include "marketscan/core/event.hpp"
#include "marketscan/core/types.hpp"

namespace marketscan {

struct BookLevel {
    Price price = 0.0;
    Quantity size = 0.0;
};

/// The state of one instrument's market.
///
/// Stores price levels best-first on each side. The feeds in this build carry
/// top of book only, so in practice there is one level per side -- but the fill
/// model walks whatever levels exist, so an L2 or MBO feed slots in later
/// without the execution path changing.
///
/// Also remembers the last traded price and the last bar close, because a
/// position has to be marked even when no quote has arrived yet.
class OrderBook {
public:
    void apply(const MS_Quote& quote);
    void apply(const MS_Trade& trade);
    void apply(const MS_Bar& bar);

    /// Replaces both sides wholesale, best level first.
    ///
    /// This is the entry point an L2 or MBO feed would use. The L1 feeds in
    /// this build go through apply(MS_Quote), which can only ever describe the
    /// touch -- but the fill model walks whatever depth is here, so the
    /// execution path is already the one that handles a real book.
    void set_levels(std::vector<BookLevel> bids, std::vector<BookLevel> asks, Timestamp ts);

    [[nodiscard]] const std::vector<BookLevel>& bids() const noexcept { return bids_; }

    [[nodiscard]] const std::vector<BookLevel>& asks() const noexcept { return asks_; }

    /// Levels on `side` as an aggressor would consume them: a buy takes asks.
    [[nodiscard]] const std::vector<BookLevel>& levels_for(Side aggressor) const noexcept {
        return aggressor == MS_SIDE_BUY ? asks_ : bids_;
    }

    [[nodiscard]] bool has_quote() const noexcept { return !bids_.empty() || !asks_.empty(); }

    [[nodiscard]] Price best_bid() const noexcept {
        return bids_.empty() ? 0.0 : bids_.front().price;
    }

    [[nodiscard]] Price best_ask() const noexcept {
        return asks_.empty() ? 0.0 : asks_.front().price;
    }

    [[nodiscard]] Quantity bid_size() const noexcept {
        return bids_.empty() ? 0.0 : bids_.front().size;
    }

    [[nodiscard]] Quantity ask_size() const noexcept {
        return asks_.empty() ? 0.0 : asks_.front().size;
    }

    /// Mid price, or the one side that exists, or the last trade. Zero only if
    /// nothing has ever been seen for this symbol.
    [[nodiscard]] Price mid() const noexcept;

    /// The price a position marks at: the mid when there is a two-sided quote,
    /// otherwise the last trade or bar close.
    ///
    /// Marking at the mid rather than at the bid is a deliberate choice. The
    /// bid is what a long position would realise, but using it would book an
    /// unrealised loss the instant a position opens, which makes every equity
    /// curve start with a step down that has nothing to do with the strategy.
    /// The spread is charged where it is actually paid: in the fill.
    [[nodiscard]] Price mark_price() const noexcept;

    [[nodiscard]] Price last_trade() const noexcept { return last_trade_; }

    [[nodiscard]] Timestamp last_update() const noexcept { return last_update_; }

    [[nodiscard]] bool initialised() const noexcept { return initialised_; }

    void clear() noexcept;

private:
    // Best first: bids descending by price, asks ascending.
    std::vector<BookLevel> bids_;
    std::vector<BookLevel> asks_;

    Price last_trade_ = 0.0;
    Price last_close_ = 0.0;
    Timestamp last_update_ = 0;
    bool initialised_ = false;
};

/// Every instrument's book, indexed by SymbolId.
///
/// The engine updates this from each event before any strategy sees it, so a
/// strategy asking about the market inside a callback is told the state as of
/// that event -- and the fill model prices against the same state, which is
/// what makes "the quote in effect at the signal timestamp" a real guarantee
/// rather than a comment.
class MarketView {
public:
    void apply(const MarketEvent& event);

    /// Grows to fit. Never returns null.
    [[nodiscard]] const OrderBook& book(SymbolId symbol) const noexcept;

    [[nodiscard]] std::size_t size() const noexcept { return books_.size(); }

    void reserve(std::size_t symbol_count);

    /// Mutable access, growing to fit. For the engine and for feeds that
    /// populate depth directly; everything else should read through book().
    [[nodiscard]] OrderBook& book_for_update(SymbolId symbol);

    void clear() noexcept;

private:
    // Index is symbol id - 1. A vector rather than a map: ids are dense by
    // construction, and nothing here is ever iterated into a decision.
    std::vector<OrderBook> books_;
    OrderBook empty_;
};

}  // namespace marketscan

#endif  // MARKETSCAN_BOOK_ORDER_BOOK_HPP
