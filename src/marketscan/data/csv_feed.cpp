#include "marketscan/data/csv_feed.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <fstream>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace marketscan {
namespace {

constexpr std::size_t kNoColumn = static_cast<std::size_t>(-1);

[[nodiscard]] std::string_view trim(std::string_view text) noexcept {
    const auto is_space = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    while (!text.empty() && is_space(text.front())) {
        text.remove_prefix(1);
    }
    while (!text.empty() && is_space(text.back())) {
        text.remove_suffix(1);
    }
    return text;
}

[[nodiscard]] std::string to_lower(std::string_view text) {
    std::string result(text);
    for (char& c : result) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return result;
}

void split(std::string_view line, char delimiter, std::vector<std::string_view>& out) {
    out.clear();
    std::size_t start = 0;
    while (true) {
        const std::size_t pos = line.find(delimiter, start);
        if (pos == std::string_view::npos) {
            out.push_back(trim(line.substr(start)));
            return;
        }
        out.push_back(trim(line.substr(start, pos - start)));
        start = pos + 1;
    }
}

/// Column layout, resolved once from the header.
struct Columns {
    std::size_t timestamp = kNoColumn;
    std::size_t symbol = kNoColumn;
    std::size_t open = kNoColumn;
    std::size_t high = kNoColumn;
    std::size_t low = kNoColumn;
    std::size_t close = kNoColumn;
    std::size_t volume = kNoColumn;
    std::size_t bid_px = kNoColumn;
    std::size_t ask_px = kNoColumn;
    std::size_t bid_sz = kNoColumn;
    std::size_t ask_sz = kNoColumn;
    std::size_t price = kNoColumn;
    std::size_t size = kNoColumn;
    std::size_t side = kNoColumn;
};

/// Accepts the spellings that real exports actually use, and nothing else.
/// An unrecognised column is ignored; a missing required one is an error.
void assign_column(Columns& columns, const std::string& header, std::size_t index) {
    static const std::unordered_map<std::string, std::size_t Columns::*> kAliases = {
        {"timestamp", &Columns::timestamp},
        {"ts", &Columns::timestamp},
        {"time", &Columns::timestamp},
        {"datetime", &Columns::timestamp},
        {"date", &Columns::timestamp},
        {"symbol", &Columns::symbol},
        {"ticker", &Columns::symbol},
        {"open", &Columns::open},
        {"o", &Columns::open},
        {"high", &Columns::high},
        {"h", &Columns::high},
        {"low", &Columns::low},
        {"l", &Columns::low},
        {"close", &Columns::close},
        {"c", &Columns::close},
        {"volume", &Columns::volume},
        {"v", &Columns::volume},
        {"bid_px", &Columns::bid_px},
        {"bid", &Columns::bid_px},
        {"bid_price", &Columns::bid_px},
        {"ask_px", &Columns::ask_px},
        {"ask", &Columns::ask_px},
        {"ask_price", &Columns::ask_px},
        {"bid_sz", &Columns::bid_sz},
        {"bid_size", &Columns::bid_sz},
        {"ask_sz", &Columns::ask_sz},
        {"ask_size", &Columns::ask_sz},
        {"price", &Columns::price},
        {"px", &Columns::price},
        {"size", &Columns::size},
        {"qty", &Columns::size},
        {"quantity", &Columns::size},
        {"side", &Columns::side},
        {"aggressor", &Columns::side},
    };

    // Lookup only; this map is never iterated, so its order cannot reach a
    // decision or an emitted order.
    if (const auto it = kAliases.find(header); it != kAliases.end()) {
        columns.*(it->second) = index;
    }
}

enum class RowKind { Bar, Quote, Trade };

[[nodiscard]] std::string location(const std::filesystem::path& path, std::size_t line) {
    return path.string() + ":" + std::to_string(line);
}

[[noreturn]] void fail(const std::filesystem::path& path, std::size_t line, std::string_view what) {
    throw std::runtime_error(location(path, line) + ": " + std::string(what));
}

[[nodiscard]] double parse_double(std::string_view text,
                                  const std::filesystem::path& path,
                                  std::size_t line,
                                  std::string_view column) {
    // from_chars, not strtod: it is locale independent, so a machine whose
    // locale uses a decimal comma reads the same file the same way.
    double value = 0.0;
    const char* first = text.data();
    const char* last = text.data() + text.size();
    const auto [ptr, ec] = std::from_chars(first, last, value);
    if (ec != std::errc{} || ptr != last) {
        fail(path,
             line,
             "column '" + std::string(column) + "' is not a number: '" + std::string(text) + "'");
    }
    return value;
}

[[nodiscard]] std::string_view field(const std::vector<std::string_view>& fields,
                                     std::size_t index) noexcept {
    return index == kNoColumn || index >= fields.size() ? std::string_view{} : fields[index];
}

}  // namespace

CsvFeed::CsvFeed(const std::filesystem::path& path, SymbolTable& symbols, CsvFeedOptions options) {
    name_ = "csv:" + path.filename().string();

    std::ifstream file(path, std::ios::binary);
    if (!file) {
        throw std::runtime_error("CsvFeed: cannot open '" + path.string() + "'");
    }

    std::string line;
    std::size_t line_number = 0;

    // --- Header ---------------------------------------------------------
    Columns columns;
    std::vector<std::string_view> fields;
    bool found_header = false;
    while (std::getline(file, line)) {
        ++line_number;
        const std::string_view trimmed = trim(line);
        if (!trimmed.empty() && trimmed.front() != '#') {
            found_header = true;
            break;
        }
    }
    if (!found_header) {
        throw std::runtime_error("CsvFeed: '" + path.string() + "' contains no header row");
    }

    {
        const std::string header_line(trim(line));
        split(header_line, options.delimiter, fields);
        for (std::size_t i = 0; i < fields.size(); ++i) {
            assign_column(columns, to_lower(fields[i]), i);
        }
    }

    if (columns.timestamp == kNoColumn) {
        fail(path,
             line_number,
             "no timestamp column (expected one of: timestamp, ts, time, datetime, date)");
    }

    RowKind kind{};
    if (columns.bid_px != kNoColumn && columns.ask_px != kNoColumn) {
        kind = RowKind::Quote;
    } else if (columns.close != kNoColumn) {
        kind = RowKind::Bar;
    } else if (columns.price != kNoColumn) {
        kind = RowKind::Trade;
    } else {
        fail(path,
             line_number,
             "header names no recognisable data: expected bid/ask for quotes, "
             "open/high/low/close for bars, or price for trades");
    }

    SymbolId default_id = kInvalidSymbol;
    if (columns.symbol == kNoColumn) {
        if (options.default_symbol.empty()) {
            fail(path,
                 line_number,
                 "no symbol column and no default_symbol configured; refusing to "
                 "guess which instrument this file describes");
        }
        default_id = symbols.intern(options.default_symbol);
    }

    if (options.synthesize_quotes && kind != RowKind::Bar) {
        throw std::runtime_error("CsvFeed: synthesize_quotes only applies to bar data");
    }

    // --- Rows -----------------------------------------------------------
    std::vector<SymbolId> seen_ids;
    const double half_spread_fraction = options.spread_bps * 1e-4 * 0.5;

    while (std::getline(file, line)) {
        ++line_number;
        const std::string_view trimmed = trim(line);
        if (trimmed.empty() || trimmed.front() == '#') {
            continue;
        }

        split(trimmed, options.delimiter, fields);

        const std::string_view ts_text = field(fields, columns.timestamp);
        if (ts_text.empty()) {
            fail(path, line_number, "empty timestamp");
        }
        Timestamp ts = 0;
        if (!time::from_iso8601(ts_text, ts)) {
            fail(path, line_number, "unparseable timestamp '" + std::string(ts_text) + "'");
        }

        SymbolId id = default_id;
        if (columns.symbol != kNoColumn) {
            const std::string_view ticker = field(fields, columns.symbol);
            if (ticker.empty()) {
                fail(path, line_number, "empty symbol");
            }
            id = symbols.intern(ticker);
        }
        if (std::find(seen_ids.begin(), seen_ids.end(), id) == seen_ids.end()) {
            seen_ids.push_back(id);
        }

        switch (kind) {
            case RowKind::Bar: {
                const double close =
                    parse_double(field(fields, columns.close), path, line_number, "close");

                MS_Bar bar{};
                bar.ts = ts;
                bar.symbol = id;
                bar.close = close;
                // A file with only a close is a valid price series; the other
                // three collapse onto it rather than being invented.
                bar.open =
                    columns.open == kNoColumn
                        ? close
                        : parse_double(field(fields, columns.open), path, line_number, "open");
                bar.high =
                    columns.high == kNoColumn
                        ? close
                        : parse_double(field(fields, columns.high), path, line_number, "high");
                bar.low = columns.low == kNoColumn
                              ? close
                              : parse_double(field(fields, columns.low), path, line_number, "low");
                bar.volume =
                    columns.volume == kNoColumn
                        ? 0.0
                        : parse_double(field(fields, columns.volume), path, line_number, "volume");

                if (bar.high < bar.low || bar.high < bar.open || bar.high < bar.close ||
                    bar.low > bar.open || bar.low > bar.close) {
                    fail(path,
                         line_number,
                         "impossible bar: high/low do not bracket open/close. A bar "
                         "like this lets a fill simulator trade at a price that "
                         "never existed.");
                }

                if (options.synthesize_quotes) {
                    const double half_spread = close * half_spread_fraction;
                    MS_Quote quote{};
                    quote.ts = ts;
                    quote.symbol = id;
                    quote.bid_px = close - half_spread;
                    quote.ask_px = close + half_spread;
                    quote.bid_sz = options.quote_size;
                    quote.ask_sz = options.quote_size;
                    events_.push_back(MarketEvent::from(quote));
                }
                events_.push_back(MarketEvent::from(bar));
                break;
            }

            case RowKind::Quote: {
                MS_Quote quote{};
                quote.ts = ts;
                quote.symbol = id;
                quote.bid_px =
                    parse_double(field(fields, columns.bid_px), path, line_number, "bid_px");
                quote.ask_px =
                    parse_double(field(fields, columns.ask_px), path, line_number, "ask_px");
                quote.bid_sz =
                    columns.bid_sz == kNoColumn
                        ? 0.0
                        : parse_double(field(fields, columns.bid_sz), path, line_number, "bid_sz");
                quote.ask_sz =
                    columns.ask_sz == kNoColumn
                        ? 0.0
                        : parse_double(field(fields, columns.ask_sz), path, line_number, "ask_sz");

                if (quote.bid_px > 0.0 && quote.ask_px > 0.0 && quote.bid_px > quote.ask_px) {
                    fail(path,
                         line_number,
                         "crossed quote: bid is above ask. Replaying one would let a "
                         "strategy buy below the price it sells at, for free.");
                }
                events_.push_back(MarketEvent::from(quote));
                break;
            }

            case RowKind::Trade: {
                MS_Trade trade{};
                trade.ts = ts;
                trade.symbol = id;
                trade.price =
                    parse_double(field(fields, columns.price), path, line_number, "price");
                trade.size =
                    columns.size == kNoColumn
                        ? 0.0
                        : parse_double(field(fields, columns.size), path, line_number, "size");

                trade.aggressor = MS_SIDE_BUY;
                if (columns.side != kNoColumn) {
                    const std::string side = to_lower(field(fields, columns.side));
                    if (side == "s" || side == "sell" || side == "sold" || side == "a" ||
                        side == "ask") {
                        trade.aggressor = MS_SIDE_SELL;
                    } else if (!(side == "b" || side == "buy" || side == "bought" ||
                                 side == "bid" || side.empty())) {
                        fail(path, line_number, "unrecognised side '" + side + "'");
                    }
                }
                events_.push_back(MarketEvent::from(trade));
                break;
            }
        }
    }

    if (events_.empty()) {
        throw std::runtime_error("CsvFeed: '" + path.string() + "' contained no data rows");
    }

    // Stable, so rows sharing a timestamp keep the order the file gave them.
    std::stable_sort(events_.begin(),
                     events_.end(),
                     [](const MarketEvent& a, const MarketEvent& b) { return a.ts() < b.ts(); });

    std::sort(seen_ids.begin(), seen_ids.end());
    symbol_ids_ = std::move(seen_ids);
}

bool CsvFeed::next(MarketEvent& out) {
    if (cursor_ >= events_.size()) {
        return false;
    }
    out = events_[cursor_++];
    return true;
}

FeedSpan CsvFeed::span() const noexcept {
    FeedSpan result;
    if (events_.empty()) {
        return result;
    }
    result.known = true;
    result.first = events_.front().ts();
    result.last = events_.back().ts();
    result.event_count = events_.size();
    return result;
}

}  // namespace marketscan
