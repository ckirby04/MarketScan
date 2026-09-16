#ifndef MARKETSCAN_DATA_SYNTHETIC_FEED_HPP
#define MARKETSCAN_DATA_SYNTHETIC_FEED_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "marketscan/core/random.hpp"
#include "marketscan/data/feed.hpp"

namespace marketscan {

/// One instrument for the synthetic generator to invent.
struct SyntheticInstrument {
    std::string ticker = "SYN";
    double initial_price = 100.0;

    /// Annualised drift and volatility of the log price.
    double annual_drift = 0.05;
    double annual_volatility = 0.20;

    /// Mean bar volume. Actual volume is lognormal around this.
    double mean_volume = 100'000.0;
};

struct SyntheticFeedConfig {
    std::vector<SyntheticInstrument> instruments;

    /// Timestamp of the first bar of the first session.
    Timestamp start = 0;

    Duration bar_interval = time::kMinute;

    /// A US equity session is 390 one-minute bars.
    std::uint32_t bars_per_session = 390;
    std::uint32_t sessions = 1;

    /// Sessions begin on consecutive days at the same time of day. The gap
    /// between a session's close and the next open carries no price move: this
    /// generator does not model overnight risk, and pretending otherwise would
    /// put a gap into the data that nothing downstream is equipped to explain.
    Duration session_gap = time::kDay;

    std::uint64_t seed = 0xC0FF'EE00'1234'5678ULL;

    /// Emit a quote immediately before each bar, at the same timestamp.
    ///
    /// Order matters and is not incidental: the spread-crossing fill model
    /// prices a market order from the quote in effect at the signal timestamp,
    /// so a strategy acting inside on_bar must already have had that quote.
    bool emit_quotes = true;

    /// Half-spread, in basis points of price, around the bar close.
    double spread_bps = 2.0;

    /// Size shown at the touch.
    double quote_size = 500.0;

    /// Used to convert a bar into a fraction of a year.
    double trading_days_per_year = 252.0;

    /// Scales the intrabar excursion that produces the high and the low.
    double intrabar_range_factor = 0.6;
};

/// Geometric Brownian motion, seeded and reproducible.
///
/// Exists so that the engine, the ledger and the GUI can be exercised without
/// a data file, and so that tests have an input that is the same on every
/// machine. It is not a market model and nothing about a strategy's
/// performance against it means anything.
///
/// Streams rather than precomputing, so a long run costs no memory, and draws
/// from DeterministicRng in a fixed order so two runs of the same config
/// produce byte-identical events.
class SyntheticFeed final : public IMarketDataFeed {
public:
    /// Interns every instrument's ticker into `symbols`, which must outlive
    /// the feed.
    SyntheticFeed(SyntheticFeedConfig config, SymbolTable& symbols);

    bool next(MarketEvent& out) override;

    [[nodiscard]] bool can_reset() const noexcept override { return true; }

    void reset() override;

    [[nodiscard]] std::string_view name() const noexcept override { return "synthetic"; }

    [[nodiscard]] const std::vector<SymbolId>& symbols() const noexcept override {
        return symbol_ids_;
    }

    [[nodiscard]] FeedSpan span() const noexcept override;

    [[nodiscard]] const SyntheticFeedConfig& config() const noexcept { return config_; }

private:
    /// Generates every event at the next timestamp into `pending_`.
    void generate_next_timestamp();

    [[nodiscard]] Timestamp timestamp_for(std::uint64_t bar_index) const noexcept;

    SyntheticFeedConfig config_;
    std::vector<SymbolId> symbol_ids_;

    /// Current price per instrument, parallel to config_.instruments.
    std::vector<double> prices_;

    DeterministicRng rng_;
    std::uint64_t bar_index_ = 0;
    std::uint64_t total_bars_ = 0;

    /// Events for one timestamp, drained before the next is generated.
    std::vector<MarketEvent> pending_;
    std::size_t pending_cursor_ = 0;

    /// Fraction of a year represented by one bar.
    double dt_ = 0.0;
};

}  // namespace marketscan

#endif  // MARKETSCAN_DATA_SYNTHETIC_FEED_HPP
