#include "marketscan/data/synthetic_feed.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace marketscan {

SyntheticFeed::SyntheticFeed(SyntheticFeedConfig config, SymbolTable& symbols)
    : config_(std::move(config)), rng_(config_.seed) {
    if (config_.instruments.empty()) {
        throw std::invalid_argument("SyntheticFeed: no instruments configured");
    }
    if (config_.bars_per_session == 0 || config_.sessions == 0) {
        throw std::invalid_argument("SyntheticFeed: needs at least one bar in one session");
    }
    if (config_.bar_interval <= 0) {
        throw std::invalid_argument("SyntheticFeed: bar_interval must be positive");
    }

    symbol_ids_.reserve(config_.instruments.size());
    prices_.reserve(config_.instruments.size());
    for (const SyntheticInstrument& instrument : config_.instruments) {
        if (instrument.initial_price <= 0.0) {
            throw std::invalid_argument("SyntheticFeed: initial_price must be positive for '" +
                                        instrument.ticker + "'");
        }
        symbol_ids_.push_back(symbols.intern(instrument.ticker));
        prices_.push_back(instrument.initial_price);
    }

    total_bars_ = static_cast<std::uint64_t>(config_.bars_per_session) * config_.sessions;

    // One session is one trading day, so a bar is that fraction of a day.
    dt_ = 1.0 / (config_.trading_days_per_year * static_cast<double>(config_.bars_per_session));

    // Two events per instrument per bar at most, so the buffer never grows
    // after construction and next() does not allocate.
    pending_.reserve(config_.instruments.size() * 2);
}

Timestamp SyntheticFeed::timestamp_for(std::uint64_t bar_index) const noexcept {
    const auto per_session = static_cast<std::uint64_t>(config_.bars_per_session);
    const std::uint64_t session = bar_index / per_session;
    const std::uint64_t within = bar_index % per_session;
    return config_.start + static_cast<Timestamp>(session) * config_.session_gap +
           static_cast<Timestamp>(within) * config_.bar_interval;
}

void SyntheticFeed::generate_next_timestamp() {
    pending_.clear();
    pending_cursor_ = 0;

    const Timestamp ts = timestamp_for(bar_index_);
    const double sqrt_dt = std::sqrt(dt_);

    for (std::size_t i = 0; i < config_.instruments.size(); ++i) {
        const SyntheticInstrument& instrument = config_.instruments[i];
        const double open = prices_[i];

        // Geometric Brownian motion in log space, so the price cannot go
        // negative no matter how large a draw turns up.
        const double drift = (instrument.annual_drift -
                              0.5 * instrument.annual_volatility * instrument.annual_volatility) *
                             dt_;
        const double shock = instrument.annual_volatility * sqrt_dt * rng_.normal();
        const double close = open * std::exp(drift + shock);

        // The intrabar excursion is drawn separately, then applied outside the
        // open-close range, so the high is never below either and the low is
        // never above either. Bars that violate that are the single most
        // common way synthetic data produces impossible fills.
        const double body_high = std::max(open, close);
        const double body_low = std::min(open, close);
        const double excursion =
            instrument.annual_volatility * sqrt_dt * config_.intrabar_range_factor;
        const double high = body_high * (1.0 + excursion * std::fabs(rng_.normal()));
        const double low = body_low * (1.0 - excursion * std::fabs(rng_.normal()));

        const double volume = instrument.mean_volume * std::exp(rng_.normal(0.0, 0.35));

        prices_[i] = close;

        if (config_.emit_quotes) {
            const double half_spread = close * config_.spread_bps * 1e-4 * 0.5;
            MS_Quote quote{};
            quote.ts = ts;
            quote.symbol = symbol_ids_[i];
            quote.bid_px = close - half_spread;
            quote.ask_px = close + half_spread;
            quote.bid_sz = config_.quote_size;
            quote.ask_sz = config_.quote_size;
            pending_.push_back(MarketEvent::from(quote));
        }

        MS_Bar bar{};
        bar.ts = ts;
        bar.symbol = symbol_ids_[i];
        bar.open = open;
        bar.high = high;
        bar.low = std::max(low, 0.0);
        bar.close = close;
        bar.volume = std::round(volume);
        pending_.push_back(MarketEvent::from(bar));
    }

    ++bar_index_;
}

bool SyntheticFeed::next(MarketEvent& out) {
    if (pending_cursor_ >= pending_.size()) {
        if (bar_index_ >= total_bars_) {
            return false;
        }
        generate_next_timestamp();
    }
    out = pending_[pending_cursor_++];
    return true;
}

void SyntheticFeed::reset() {
    rng_.reseed(config_.seed);
    for (std::size_t i = 0; i < config_.instruments.size(); ++i) {
        prices_[i] = config_.instruments[i].initial_price;
    }
    bar_index_ = 0;
    pending_.clear();
    pending_cursor_ = 0;
}

FeedSpan SyntheticFeed::span() const noexcept {
    FeedSpan result;
    result.known = true;
    result.first = timestamp_for(0);
    result.last = timestamp_for(total_bars_ - 1);
    const std::uint64_t per_bar =
        static_cast<std::uint64_t>(config_.instruments.size()) * (config_.emit_quotes ? 2 : 1);
    result.event_count = total_bars_ * per_bar;
    return result;
}

}  // namespace marketscan
