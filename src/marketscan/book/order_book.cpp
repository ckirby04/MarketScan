#include "marketscan/book/order_book.hpp"

#include <utility>

namespace marketscan {

void OrderBook::apply(const MS_Quote& quote) {
    // A quote in this build replaces the touch rather than amending a level,
    // because that is all an L1 feed conveys. An L2 feed would amend instead;
    // nothing downstream of here would need to change.
    bids_.clear();
    asks_.clear();
    if (quote.bid_px > 0.0 && quote.bid_sz > 0.0) {
        bids_.push_back(BookLevel{quote.bid_px, quote.bid_sz});
    }
    if (quote.ask_px > 0.0 && quote.ask_sz > 0.0) {
        asks_.push_back(BookLevel{quote.ask_px, quote.ask_sz});
    }
    last_update_ = quote.ts;
    initialised_ = true;
}

void OrderBook::set_levels(std::vector<BookLevel> bids, std::vector<BookLevel> asks, Timestamp ts) {
    bids_ = std::move(bids);
    asks_ = std::move(asks);
    last_update_ = ts;
    initialised_ = true;
}

void OrderBook::apply(const MS_Trade& trade) {
    if (trade.price > 0.0) {
        last_trade_ = trade.price;
    }
    last_update_ = trade.ts;
    initialised_ = true;
}

void OrderBook::apply(const MS_Bar& bar) {
    if (bar.close > 0.0) {
        last_close_ = bar.close;
    }
    last_update_ = bar.ts;
    initialised_ = true;
}

Price OrderBook::mid() const noexcept {
    const Price bid = best_bid();
    const Price ask = best_ask();
    if (bid > 0.0 && ask > 0.0) {
        return 0.5 * (bid + ask);
    }
    if (bid > 0.0) {
        return bid;
    }
    if (ask > 0.0) {
        return ask;
    }
    if (last_trade_ > 0.0) {
        return last_trade_;
    }
    return last_close_;
}

Price OrderBook::mark_price() const noexcept {
    const Price m = mid();
    if (m > 0.0) {
        return m;
    }
    return last_close_ > 0.0 ? last_close_ : last_trade_;
}

void OrderBook::clear() noexcept {
    bids_.clear();
    asks_.clear();
    last_trade_ = 0.0;
    last_close_ = 0.0;
    last_update_ = 0;
    initialised_ = false;
}

// ---------------------------------------------------------------------------

void MarketView::apply(const MarketEvent& event) {
    const SymbolId symbol = event.symbol();
    if (symbol == kInvalidSymbol) {
        return;
    }
    OrderBook& book = book_for_update(symbol);
    switch (event.type) {
        case EventType::Quote:
            book.apply(event.quote);
            break;
        case EventType::Trade:
            book.apply(event.trade);
            break;
        case EventType::Bar:
            book.apply(event.bar);
            break;
    }
}

const OrderBook& MarketView::book(SymbolId symbol) const noexcept {
    if (symbol == kInvalidSymbol || symbol > books_.size()) {
        return empty_;
    }
    return books_[symbol - 1];
}

OrderBook& MarketView::book_for_update(SymbolId symbol) {
    if (symbol > books_.size()) {
        books_.resize(symbol);
    }
    return books_[symbol - 1];
}

void MarketView::reserve(std::size_t symbol_count) {
    if (symbol_count > books_.size()) {
        books_.resize(symbol_count);
    }
}

void MarketView::clear() noexcept {
    for (OrderBook& book : books_) {
        book.clear();
    }
}

}  // namespace marketscan
