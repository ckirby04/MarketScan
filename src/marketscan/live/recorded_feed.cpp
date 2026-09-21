#include "marketscan/live/recorded_feed.hpp"

#include <algorithm>

namespace marketscan::live {

RecordedFeed::RecordedFeed(const std::filesystem::path& path, SymbolTable& symbols) {
    const std::vector<RecordedMessage> messages = read_recording(path, header_);

    // In header order, so a replay reproduces the live session's interning
    // order when it starts from a fresh table. Symbol ids reach the strategy,
    // so an id that differs is a difference the strategy can see.
    symbol_ids_.reserve(header_.symbols.size());
    for (const std::string& ticker : header_.symbols) {
        symbol_ids_.push_back(symbols.intern(ticker));
    }
    std::sort(symbol_ids_.begin(), symbol_ids_.end());
    symbol_ids_.erase(std::unique(symbol_ids_.begin(), symbol_ids_.end()), symbol_ids_.end());

    MonotonicGate gate;
    std::int64_t skew_total = 0;
    for (const RecordedMessage& message : messages) {
        ++stats_.messages;
        if (message.reconnect) {
            ++stats_.reconnects;
            continue;
        }

        ParseResult parsed;
        parse_stream_message_into(message.raw, symbols, parsed);
        stats_.controls += parsed.stats.controls;
        stats_.unknown_symbol += parsed.stats.unknown_symbol;
        stats_.malformed += parsed.stats.malformed;
        stats_.unhandled_kind += parsed.stats.unhandled_kind;

        for (MarketEvent event : parsed.events) {
            // Before the gate, exactly where the live feed measures it.
            if (event.type != EventType::Bar) {
                const std::int64_t skew = message.received - event.ts();
                if (stats_.skew_samples == 0) {
                    stats_.skew_min_ns = skew;
                    stats_.skew_max_ns = skew;
                } else {
                    stats_.skew_min_ns = std::min(stats_.skew_min_ns, skew);
                    stats_.skew_max_ns = std::max(stats_.skew_max_ns, skew);
                }
                skew_total += skew;
                ++stats_.skew_samples;
            }

            gate.apply(event);
            events_.push_back(event);
            received_.push_back(message.received);
            ++stats_.events;
        }
    }

    if (stats_.skew_samples > 0) {
        stats_.skew_mean_ns = skew_total / static_cast<std::int64_t>(stats_.skew_samples);
    }

    stats_.reordered = gate.reordered();
    stats_.max_backwards_ns = gate.max_backwards();
    stats_.bars_interval_stamped = gate.interval_stamped();
    stats_.max_bar_lag_ns = gate.max_interval_lag();

    span_.event_count = events_.size();
    span_.known = true;
    if (!events_.empty()) {
        span_.first = events_.front().ts();
        span_.last = events_.back().ts();
    }
}

bool RecordedFeed::next(MarketEvent& out) {
    if (cursor_ >= events_.size()) {
        return false;
    }
    out = events_[cursor_];
    ++cursor_;
    return true;
}

void RecordedFeed::reset() {
    cursor_ = 0;
}

}  // namespace marketscan::live
