#include "marketscan/data/daily_feed.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace marketscan {
namespace {

[[nodiscard]] bool wanted(const std::vector<std::string>& only, const std::string& ticker) {
    return only.empty() || std::find(only.begin(), only.end(), ticker) != only.end();
}

[[nodiscard]] bool plausible(const StoredBar& bar) noexcept {
    const bool finite = std::isfinite(bar.open) && std::isfinite(bar.high) &&
                        std::isfinite(bar.low) && std::isfinite(bar.close) &&
                        std::isfinite(bar.volume);
    if (!finite || bar.low <= 0.0) {
        return false;
    }
    // The high and the low have to bracket the open and the close. A bar that
    // fails this lets a fill simulator trade at a price the day never saw,
    // which is a free lunch nothing downstream would flag.
    return bar.high >= bar.low && bar.high >= bar.open && bar.high >= bar.close &&
           bar.low <= bar.open && bar.low <= bar.close;
}

}  // namespace

DailyBarFeed::DailyBarFeed(const std::filesystem::path& directory,
                           SymbolTable& symbols,
                           DailyFeedOptions options) {
    std::error_code ec;
    if (!std::filesystem::is_directory(directory, ec)) {
        throw std::runtime_error("DailyBarFeed: '" + directory.string() +
                                 "' is not a directory. Populate a cache first: "
                                 "python scripts/fetch_stooq.py --symbols ...");
    }

    const std::vector<std::filesystem::path> files = bar_store::list(directory);
    if (files.empty()) {
        throw std::runtime_error("DailyBarFeed: no " + std::string(bar_store::kExtension) +
                                 " files in '" + directory.string() + "'");
    }

    std::vector<BarSeries> series;
    series.reserve(files.size());
    for (const std::filesystem::path& file : files) {
        BarSeries loaded = bar_store::read(file);
        if (wanted(options.only, loaded.ticker)) {
            series.push_back(std::move(loaded));
        }
    }

    name_ = "daily:" + directory.filename().string();
    build(std::move(series), symbols, options);
}

DailyBarFeed::DailyBarFeed(std::vector<BarSeries> series,
                           SymbolTable& symbols,
                           DailyFeedOptions options)
    : name_("daily:memory") {
    build(std::move(series), symbols, options);
}

void DailyBarFeed::build(std::vector<BarSeries> series,
                         SymbolTable& symbols,
                         const DailyFeedOptions& options) {
    // Interned in the order the files were listed, which bar_store::list
    // sorted. Symbol ids therefore depend on the set of files and nothing else
    // -- not on the order a filesystem chose to walk a directory, which is not
    // a promise any filesystem makes.
    std::size_t total_bars = 0;
    for (const BarSeries& one : series) {
        total_bars += one.bars.size();
    }

    events_.reserve(total_bars * (options.synthesize_quotes ? 2 : 1));
    symbol_ids_.reserve(series.size());

    const double half_spread_fraction = options.spread_bps * 1e-4 * 0.5;

    for (const BarSeries& one : series) {
        if (one.ticker.empty()) {
            throw std::runtime_error("DailyBarFeed: a series has no ticker");
        }
        const SymbolId id = symbols.intern(one.ticker);
        symbol_ids_.push_back(id);

        for (const StoredBar& stored : one.bars) {
            if (!plausible(stored)) {
                throw std::runtime_error("DailyBarFeed: " + one.ticker +
                                         " has an impossible bar at " +
                                         time::to_iso8601(stored.ts) +
                                         " (high/low do not bracket open/close, or a price is not "
                                         "positive and finite)");
            }

            MS_Bar bar{};
            bar.ts = stored.ts;
            bar.symbol = id;
            bar.open = stored.open;
            bar.high = stored.high;
            bar.low = stored.low;
            bar.close = stored.close;
            bar.volume = stored.volume;

            if (options.synthesize_quotes) {
                const double half_spread = stored.close * half_spread_fraction;
                MS_Quote quote{};
                quote.ts = stored.ts;
                quote.symbol = id;
                quote.bid_px = stored.close - half_spread;
                quote.ask_px = stored.close + half_spread;
                quote.bid_sz = options.quote_size;
                quote.ask_sz = options.quote_size;
                events_.push_back(MarketEvent::from(quote));
            }
            events_.push_back(MarketEvent::from(bar));
            ++bar_count_;
        }
    }

    // Stable, so that the quote a bar was given stays immediately in front of
    // it. The spread-crossing model prices an order from the quote in effect at
    // the signal timestamp, and a sort that put the bar first would leave a
    // strategy acting inside on_bar with yesterday's quote.
    std::stable_sort(events_.begin(),
                     events_.end(),
                     [](const MarketEvent& a, const MarketEvent& b) { return a.ts() < b.ts(); });

    std::sort(symbol_ids_.begin(), symbol_ids_.end());
    symbol_ids_.erase(std::unique(symbol_ids_.begin(), symbol_ids_.end()), symbol_ids_.end());
}

bool DailyBarFeed::next(MarketEvent& out) {
    if (cursor_ >= events_.size()) {
        return false;
    }
    out = events_[cursor_++];
    return true;
}

FeedSpan DailyBarFeed::span() const noexcept {
    FeedSpan span;
    if (events_.empty()) {
        span.known = true;
        return span;
    }
    span.first = events_.front().ts();
    span.last = events_.back().ts();
    span.event_count = events_.size();
    span.known = true;
    return span;
}

}  // namespace marketscan
