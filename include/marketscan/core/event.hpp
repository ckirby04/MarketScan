#ifndef MARKETSCAN_CORE_EVENT_HPP
#define MARKETSCAN_CORE_EVENT_HPP

#include <cstdint>
#include <type_traits>

#include "marketscan/core/types.hpp"

namespace marketscan {

enum class EventType : std::uint8_t {
    Bar = 0,
    Quote = 1,
    Trade = 2,
};

[[nodiscard]] constexpr std::string_view to_string(EventType t) noexcept {
    switch (t) {
        case EventType::Bar:
            return "bar";
        case EventType::Quote:
            return "quote";
        case EventType::Trade:
            return "trade";
    }
    return "?";
}

/// One thing that happened in the market.
///
/// A tagged union rather than std::variant, for two reasons. It holds the ABI
/// structs themselves, so dispatching to a plugin passes a pointer straight
/// through with no translation step that could round or reorder anything. And
/// it is trivially copyable and allocation-free, so it moves through the SPSC
/// ring buffer by memcpy on the live path where the hot loop may not allocate.
struct MarketEvent {
    EventType type{EventType::Bar};

    union {
        MS_Bar bar;
        MS_Quote quote;
        MS_Trade trade;
    };

    constexpr MarketEvent() noexcept : bar{} {}

    [[nodiscard]] constexpr Timestamp ts() const noexcept {
        switch (type) {
            case EventType::Bar:
                return bar.ts;
            case EventType::Quote:
                return quote.ts;
            case EventType::Trade:
                return trade.ts;
        }
        return 0;
    }

    [[nodiscard]] constexpr SymbolId symbol() const noexcept {
        switch (type) {
            case EventType::Bar:
                return bar.symbol;
            case EventType::Quote:
                return quote.symbol;
            case EventType::Trade:
                return trade.symbol;
        }
        return kInvalidSymbol;
    }

    /// The price this event implies for marking a position. A bar marks at its
    /// close, a trade at its price, a quote at its mid. A one-sided quote marks
    /// at whichever side exists rather than at a mid computed from a zero.
    [[nodiscard]] constexpr Price mark_price() const noexcept {
        switch (type) {
            case EventType::Bar:
                return bar.close;
            case EventType::Trade:
                return trade.price;
            case EventType::Quote: {
                const bool has_bid = quote.bid_px > 0.0;
                const bool has_ask = quote.ask_px > 0.0;
                if (has_bid && has_ask) {
                    return 0.5 * (quote.bid_px + quote.ask_px);
                }
                if (has_bid) {
                    return quote.bid_px;
                }
                if (has_ask) {
                    return quote.ask_px;
                }
                return 0.0;
            }
        }
        return 0.0;
    }

    [[nodiscard]] static constexpr MarketEvent from(const MS_Bar& b) noexcept {
        MarketEvent e;
        e.type = EventType::Bar;
        e.bar = b;
        return e;
    }

    [[nodiscard]] static constexpr MarketEvent from(const MS_Quote& q) noexcept {
        MarketEvent e;
        e.type = EventType::Quote;
        e.quote = q;
        return e;
    }

    [[nodiscard]] static constexpr MarketEvent from(const MS_Trade& t) noexcept {
        MarketEvent e;
        e.type = EventType::Trade;
        e.trade = t;
        return e;
    }
};

static_assert(std::is_trivially_copyable_v<MarketEvent>,
              "MarketEvent must be memcpy-able to travel through the ring buffer");
static_assert(std::is_trivially_destructible_v<MarketEvent>);

/// Calls `fn` with the concrete ABI struct.
template<typename Fn>
constexpr decltype(auto) visit(const MarketEvent& e, Fn&& fn) {
    switch (e.type) {
        case EventType::Bar:
            return fn(e.bar);
        case EventType::Quote:
            return fn(e.quote);
        case EventType::Trade:
            return fn(e.trade);
    }
    return fn(e.bar);
}

}  // namespace marketscan

#endif  // MARKETSCAN_CORE_EVENT_HPP
