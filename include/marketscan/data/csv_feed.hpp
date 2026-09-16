#ifndef MARKETSCAN_DATA_CSV_FEED_HPP
#define MARKETSCAN_DATA_CSV_FEED_HPP

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "marketscan/data/feed.hpp"

namespace marketscan {

struct CsvFeedOptions {
    char delimiter = ',';

    /// Ticker for every row, when the file has no symbol column. Required in
    /// that case: a feed is not allowed to guess what instrument it is reading.
    std::string default_symbol;

    /// Derive a quote from each bar's close and emit it immediately before the
    /// bar, at the same timestamp.
    ///
    /// Bar data carries no spread, and the spread-crossing fill model needs
    /// one. Without this, a market order against a bars-only file has nothing
    /// to cross and would fill at the close -- which is the optimistic model
    /// wearing a disguise. Off by default so that turning it on is a decision
    /// someone made rather than one they inherited.
    bool synthesize_quotes = false;

    /// Full spread, in basis points of the close, for synthesized quotes.
    double spread_bps = 2.0;

    /// Size shown on each side of a synthesized quote.
    double quote_size = 100.0;
};

/// Reads bars, quotes or trades from a delimited text file.
///
/// The header names the columns and decides what kind of rows the file holds:
/// bid/ask columns mean quotes, open/high/low/close mean bars, a bare price
/// means trades. One kind per file.
///
/// Everything is loaded and stably sorted by timestamp up front. Stably,
/// because rows sharing a timestamp must keep their file order -- that order
/// is an input to the run, and a sort that scrambled it would make the same
/// file replay differently on a different standard library.
///
/// A malformed row is an error naming the file and line, never a skipped row.
/// Quietly dropping input is how a backtest ends up describing a dataset that
/// does not exist.
class CsvFeed final : public IMarketDataFeed {
public:
    /// Throws std::runtime_error on a missing file, an unrecognisable header
    /// or a malformed row.
    CsvFeed(const std::filesystem::path& path, SymbolTable& symbols, CsvFeedOptions options = {});

    bool next(MarketEvent& out) override;

    [[nodiscard]] bool can_reset() const noexcept override { return true; }

    void reset() override { cursor_ = 0; }

    [[nodiscard]] std::string_view name() const noexcept override { return name_; }

    [[nodiscard]] const std::vector<SymbolId>& symbols() const noexcept override {
        return symbol_ids_;
    }

    [[nodiscard]] FeedSpan span() const noexcept override;

    [[nodiscard]] std::size_t size() const noexcept { return events_.size(); }

private:
    std::string name_;
    std::vector<MarketEvent> events_;
    std::vector<SymbolId> symbol_ids_;
    std::size_t cursor_ = 0;
};

}  // namespace marketscan

#endif  // MARKETSCAN_DATA_CSV_FEED_HPP
