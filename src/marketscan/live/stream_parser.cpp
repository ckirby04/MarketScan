#include "marketscan/live/stream_parser.hpp"

#include <nlohmann/json.hpp>

#include "marketscan/core/types.hpp"

namespace marketscan::live {
namespace {

/// Reads a JSON number that the vendor may send as a number or as a string.
///
/// Prices arrive as numbers on this feed, but sizes have arrived as both
/// across vendors and a parser that throws on the other spelling turns a
/// cosmetic difference into a dropped event. Returns false when the field is
/// absent or is neither.
[[nodiscard]] bool read_number(const nlohmann::json& obj, const char* key, double& out) {
    const auto it = obj.find(key);
    if (it == obj.end() || it->is_null()) {
        return false;
    }
    if (it->is_number()) {
        out = it->get<double>();
        return true;
    }
    if (it->is_string()) {
        try {
            out = std::stod(it->get<std::string>());
            return true;
        } catch (const std::exception&) {
            return false;
        }
    }
    return false;
}

[[nodiscard]] bool read_timestamp(const nlohmann::json& obj, Timestamp& out) {
    const auto it = obj.find("t");
    if (it == obj.end() || !it->is_string()) {
        return false;
    }
    return time::from_iso8601(it->get<std::string>(), out);
}

/// The symbol field, resolved against the table. kInvalidSymbol when the
/// message names a symbol nobody subscribed to.
[[nodiscard]] SymbolId read_symbol(const nlohmann::json& obj, const SymbolTable& symbols) {
    const auto it = obj.find("S");
    if (it == obj.end() || !it->is_string()) {
        return kInvalidSymbol;
    }
    return symbols.find(it->get<std::string_view>());
}

}  // namespace

bool MonotonicGate::apply(MarketEvent& event) noexcept {
    const Timestamp ts = event.ts();
    if (ts >= last_) {
        last_ = ts;
        return false;
    }

    const std::int64_t backwards = last_ - ts;
    if (event.type == EventType::Bar) {
        // Interval-stamped, not late. See the class comment.
        ++interval_stamped_;
        if (backwards > max_interval_lag_) {
            max_interval_lag_ = backwards;
        }
    } else {
        ++reordered_;
        if (backwards > max_backwards_) {
            max_backwards_ = backwards;
        }
    }

    switch (event.type) {
        case EventType::Bar:
            event.bar.ts = last_;
            break;
        case EventType::Quote:
            event.quote.ts = last_;
            break;
        case EventType::Trade:
            event.trade.ts = last_;
            break;
    }
    return true;
}

void parse_stream_message_into(std::string_view text,
                               const SymbolTable& symbols,
                               ParseResult& out) {
    const nlohmann::json doc = nlohmann::json::parse(text, nullptr, false);
    if (doc.is_discarded()) {
        out.error = "stream message was not valid JSON";
        return;
    }
    if (!doc.is_array()) {
        out.error = "stream message was not a JSON array";
        return;
    }

    for (const nlohmann::json& element : doc) {
        if (!element.is_object()) {
            ++out.stats.malformed;
            continue;
        }
        const auto kind_it = element.find("T");
        if (kind_it == element.end() || !kind_it->is_string()) {
            ++out.stats.malformed;
            continue;
        }
        const std::string kind = kind_it->get<std::string>();

        if (kind == "t" || kind == "q" || kind == "b") {
            const SymbolId symbol = read_symbol(element, symbols);
            if (symbol == kInvalidSymbol) {
                ++out.stats.unknown_symbol;
                continue;
            }
            Timestamp ts = 0;
            if (!read_timestamp(element, ts)) {
                ++out.stats.malformed;
                continue;
            }

            if (kind == "t") {
                MS_Trade trade{};
                trade.ts = ts;
                trade.symbol = symbol;
                if (!read_number(element, "p", trade.price) ||
                    !read_number(element, "s", trade.size)) {
                    ++out.stats.malformed;
                    continue;
                }
                // This feed does not say who crossed the spread. Defaulting to
                // buy matches CsvFeed's behaviour for a file with no side
                // column, and is a placeholder rather than an inference --
                // docs/EXECUTION.md says so where it says what this feed is
                // and is not good for.
                trade.aggressor = MS_SIDE_BUY;
                out.events.push_back(MarketEvent::from(trade));
                ++out.stats.events;
                continue;
            }

            if (kind == "q") {
                MS_Quote quote{};
                quote.ts = ts;
                quote.symbol = symbol;
                if (!read_number(element, "bp", quote.bid_px) ||
                    !read_number(element, "ap", quote.ask_px) ||
                    !read_number(element, "bs", quote.bid_sz) ||
                    !read_number(element, "as", quote.ask_sz)) {
                    ++out.stats.malformed;
                    continue;
                }
                quote.bid_sz *= kQuoteLotSize;
                quote.ask_sz *= kQuoteLotSize;
                out.events.push_back(MarketEvent::from(quote));
                ++out.stats.events;
                continue;
            }

            MS_Bar bar{};
            bar.ts = ts;
            bar.symbol = symbol;
            if (!read_number(element, "o", bar.open) || !read_number(element, "h", bar.high) ||
                !read_number(element, "l", bar.low) || !read_number(element, "c", bar.close) ||
                !read_number(element, "v", bar.volume)) {
                ++out.stats.malformed;
                continue;
            }
            out.events.push_back(MarketEvent::from(bar));
            ++out.stats.events;
            continue;
        }

        if (kind == "success" || kind == "error" || kind == "subscription") {
            StreamControl control;
            control.type = kind;
            const auto msg = element.find("msg");
            if (msg != element.end() && msg->is_string()) {
                control.message = msg->get<std::string>();
            }
            const auto code = element.find("code");
            if (code != element.end() && code->is_number_integer()) {
                control.code = code->get<int>();
            }
            out.controls.push_back(std::move(control));
            ++out.stats.controls;
            continue;
        }

        // Corrections, cancels, trading status, LULD, updated bars. Real
        // messages this build has no event for. Counted so that a replay
        // shorter than its recording has a number attached to the difference.
        ++out.stats.unhandled_kind;
    }
}

ParseResult parse_stream_message(std::string_view text, const SymbolTable& symbols) {
    ParseResult out;
    parse_stream_message_into(text, symbols, out);
    return out;
}

}  // namespace marketscan::live
