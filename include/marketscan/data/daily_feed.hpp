#ifndef MARKETSCAN_DATA_DAILY_FEED_HPP
#define MARKETSCAN_DATA_DAILY_FEED_HPP

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "marketscan/data/bar_store.hpp"
#include "marketscan/data/feed.hpp"

namespace marketscan {

struct DailyFeedOptions {
    /// Derive a quote from each bar's close and emit it immediately before the
    /// bar, at the same timestamp.
    ///
    /// On by default here, unlike CsvFeed. A daily file has no quotes at all
    /// and never will, so a run over one without this has nothing for a market
    /// order to cross and fills at the close -- which is the optimistic model
    /// wearing a disguise. The intraday path can default it off because an
    /// intraday file might carry real quotes; a daily one cannot.
    bool synthesize_quotes = true;

    /// Full spread, in basis points of the close, for synthesized quotes.
    ///
    /// Two basis points on a liquid large-cap is roughly right and is wrong for
    /// everything else. It is a single number standing in for a distribution
    /// that varies by name, by size and by minute, and it is here to make the
    /// spread non-zero rather than to be accurate.
    double spread_bps = 2.0;

    /// Size shown on each side of a synthesized quote.
    double quote_size = 10'000.0;

    /// Skip files whose ticker is not in this list. Empty means take them all.
    std::vector<std::string> only;
};

/// Replays daily bars from a directory of cached .msb files.
///
/// Everything is loaded and merged into one timestamp order up front, and the
/// feed is a single source rather than one source per symbol. That is a
/// deliberate trade. Five hundred symbols as five hundred sources would make
/// the engine's k-way merge scan five hundred slots per event, which for two
/// and a half million events is a billion comparisons spent deciding what to
/// dispatch. Merging once costs memory -- about 50 bytes a bar, so a hundred
/// megabytes for twenty years of five hundred names -- and buys a replay whose
/// per-event cost is a pointer increment.
///
/// If that memory ever becomes the constraint, the answer is a packed
/// already-sorted bundle streamed from disk, not five hundred sources.
///
/// Ties break by symbol id, which is assignment order, which is sorted filename
/// order. Nothing here depends on how a filesystem chooses to enumerate a
/// directory.
class DailyBarFeed final : public IMarketDataFeed {
public:
    /// Reads every .msb file in `directory`. Throws std::runtime_error if the
    /// directory does not exist or a file in it is malformed.
    ///
    /// A malformed file is fatal rather than skipped. A cache with one corrupt
    /// symbol in it produces a backtest over a universe that quietly lost a
    /// name, and nothing downstream can tell that from a name that was never
    /// in the index.
    DailyBarFeed(const std::filesystem::path& directory,
                 SymbolTable& symbols,
                 DailyFeedOptions options = {});

    /// Replays series already in memory. For tests and for callers that
    /// assembled bars some other way.
    DailyBarFeed(std::vector<BarSeries> series,
                 SymbolTable& symbols,
                 DailyFeedOptions options = {});

    bool next(MarketEvent& out) override;

    [[nodiscard]] bool can_reset() const noexcept override { return true; }

    void reset() override { cursor_ = 0; }

    [[nodiscard]] std::string_view name() const noexcept override { return name_; }

    [[nodiscard]] const std::vector<SymbolId>& symbols() const noexcept override {
        return symbol_ids_;
    }

    [[nodiscard]] FeedSpan span() const noexcept override;

    [[nodiscard]] std::size_t size() const noexcept { return events_.size(); }

    /// Bars loaded, before quote synthesis doubled the event count.
    [[nodiscard]] std::size_t bar_count() const noexcept { return bar_count_; }

private:
    void
    build(std::vector<BarSeries> series, SymbolTable& symbols, const DailyFeedOptions& options);

    std::string name_;
    std::vector<MarketEvent> events_;
    std::vector<SymbolId> symbol_ids_;
    std::size_t bar_count_ = 0;
    std::size_t cursor_ = 0;
};

}  // namespace marketscan

#endif  // MARKETSCAN_DATA_DAILY_FEED_HPP
