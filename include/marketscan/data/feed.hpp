#ifndef MARKETSCAN_DATA_FEED_HPP
#define MARKETSCAN_DATA_FEED_HPP

#include <cstdint>
#include <vector>

#include "marketscan/core/engine.hpp"
#include "marketscan/core/symbol_table.hpp"
#include "marketscan/core/types.hpp"

namespace marketscan {

/// What a feed knows about its own extent, before it has been run.
///
/// The transport panel needs this to draw a scrub bar, and it cannot get it by
/// consuming the feed, because consuming the feed is the thing the user is
/// about to scrub through. A live feed leaves `known` false: there is no end
/// timestamp for data that has not happened yet.
struct FeedSpan {
    Timestamp first = 0;
    Timestamp last = 0;
    std::uint64_t event_count = 0;
    bool known = false;
};

/// A market data feed.
///
/// An IEventSource that additionally knows which symbols it covers and, when
/// it is a replay rather than a live transport, how far it extends.
///
/// Feeds intern their tickers into a SymbolTable supplied by the caller, so
/// every feed in a run shares one id space and a strategy asking for "SPY"
/// gets the same id whichever feed supplied it.
class IMarketDataFeed : public IEventSource {
public:
    /// Symbols this feed can produce, in ascending id order. Available before
    /// the first call to next().
    [[nodiscard]] virtual const std::vector<SymbolId>& symbols() const noexcept = 0;

    [[nodiscard]] virtual FeedSpan span() const noexcept { return {}; }
};

}  // namespace marketscan

#endif  // MARKETSCAN_DATA_FEED_HPP
