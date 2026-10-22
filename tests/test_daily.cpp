// Daily bars: the cache format and the feed that replays it.
//
// Everything here runs offline. The fetcher is the only thing in this project
// that touches a network and it is a script, deliberately, so that no test can
// depend on someone else's service being up or on a machine having internet.
// The fixtures are synthetic and committed; real market data never enters the
// repository.

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "temp_paths.hpp"
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "marketscan/core/engine.hpp"
#include "marketscan/core/session_calendar.hpp"
#include "marketscan/data/bar_store.hpp"
#include "marketscan/data/csv_feed.hpp"
#include "marketscan/data/daily_feed.hpp"

using namespace marketscan;
using Catch::Matchers::ContainsSubstring;

namespace {

/// A directory of its own, cleaned up on the way out.
class ScratchDir {
public:
    ScratchDir() {
        path_ = test::unique_temp_path("marketscan_daily_");
        std::filesystem::create_directories(path_);
    }

    ScratchDir(const ScratchDir&) = delete;
    ScratchDir& operator=(const ScratchDir&) = delete;
    ScratchDir(ScratchDir&&) = delete;
    ScratchDir& operator=(ScratchDir&&) = delete;

    ~ScratchDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

    [[nodiscard]] std::filesystem::path file(const std::string& name) const { return path_ / name; }

private:
    std::filesystem::path path_;
};

/// A few days of plausible bars for one name, stamped at the closing bell.
[[nodiscard]] BarSeries series(const std::string& ticker, int days, double start = 100.0) {
    BarSeries out;
    out.ticker = ticker;
    out.interval = bar_store::kDailyInterval;

    // 2024-01-02 is the first trading day of that year.
    const std::int64_t first_day = calendar::days_from_civil(2024, 1, 2);
    double price = start;
    for (int i = 0; i < days; ++i) {
        StoredBar bar;
        bar.ts = bar_store::daily_close_stamp(first_day + i);
        bar.open = price;
        bar.close = price * 1.001;
        bar.high = bar.close * 1.002;
        bar.low = bar.open * 0.998;
        bar.volume = 1'000'000.0;
        out.bars.push_back(bar);
        price = bar.close;
    }
    return out;
}

}  // namespace

// --------------------------------------------------------------------------
// The timestamp convention
// --------------------------------------------------------------------------

TEST_CASE("a daily bar is stamped at the closing bell", "[daily][store]") {
    // 2024-01-02 is standard time: 16:00 EST is 21:00 UTC.
    const std::int64_t january = calendar::days_from_civil(2024, 1, 2);
    Timestamp expected = 0;
    REQUIRE(time::from_iso8601("2024-01-02T21:00:00Z", expected));
    REQUIRE(bar_store::daily_close_stamp(january) == expected);

    // 2024-07-01 is daylight time: 16:00 EDT is 20:00 UTC. A stamp built by
    // adding a fixed offset to a date would have both of these wrong by an
    // hour for half the year, and nothing downstream would notice.
    const std::int64_t july = calendar::days_from_civil(2024, 7, 1);
    REQUIRE(time::from_iso8601("2024-07-01T20:00:00Z", expected));
    REQUIRE(bar_store::daily_close_stamp(july) == expected);
}

TEST_CASE("a daily bar belongs to the session it closes", "[daily][store]") {
    // The stamp must fall inside its own exchange day, not the next one. At
    // 21:00 UTC in January it is 16:00 in New York, still the same date.
    struct Date {
        int year;
        unsigned month;
        unsigned day;
    };

    for (const Date& date : {Date{2024, 1, 2},
                             Date{2024, 3, 11},
                             Date{2024, 7, 1},
                             Date{2024, 11, 4},
                             Date{2024, 12, 31}}) {
        const auto [year, month, day] = date;
        const std::int64_t number = calendar::days_from_civil(year, month, day);
        const Timestamp stamp = bar_store::daily_close_stamp(number);
        INFO(year << "-" << month << "-" << day << " -> " << time::to_iso8601(stamp));
        REQUIRE(session_day(ExchangeZone::UsEastern, stamp) == number);
    }
}

// --------------------------------------------------------------------------
// The cache format
// --------------------------------------------------------------------------

TEST_CASE("a series survives a round trip", "[daily][store]") {
    const ScratchDir dir;
    const BarSeries written = series("AAPL", 500);
    bar_store::write(dir.file("AAPL.msb"), written);

    const BarSeries read = bar_store::read(dir.file("AAPL.msb"));

    REQUIRE(read.ticker == "AAPL");
    REQUIRE(read.interval == bar_store::kDailyInterval);
    REQUIRE(read.bars.size() == written.bars.size());

    // Bit for bit, not to within a tolerance. These are raw doubles on disk;
    // if they came back merely close, something is converting them.
    for (std::size_t i = 0; i < read.bars.size(); ++i) {
        INFO("bar " << i);
        REQUIRE(read.bars[i].ts == written.bars[i].ts);
        REQUIRE(read.bars[i].open == written.bars[i].open);
        REQUIRE(read.bars[i].high == written.bars[i].high);
        REQUIRE(read.bars[i].low == written.bars[i].low);
        REQUIRE(read.bars[i].close == written.bars[i].close);
        REQUIRE(read.bars[i].volume == written.bars[i].volume);
    }
}

TEST_CASE("an empty series is legal", "[daily][store]") {
    const ScratchDir dir;
    BarSeries empty;
    empty.ticker = "NONE";
    bar_store::write(dir.file("NONE.msb"), empty);

    const BarSeries read = bar_store::read(dir.file("NONE.msb"));
    REQUIRE(read.ticker == "NONE");
    REQUIRE(read.bars.empty());
}

TEST_CASE("the file size is exactly the format", "[daily][store]") {
    const ScratchDir dir;
    bar_store::write(dir.file("X.msb"), series("X", 100));

    // 64-byte header plus 48 bytes a bar. Asserted because the format is a
    // promise to every file already written: a field quietly added to the
    // header would make every cached file unreadable, and this is what turns
    // that into a failing test rather than a support question.
    REQUIRE(std::filesystem::file_size(dir.file("X.msb")) == 64 + 100 * 48);
}

TEST_CASE("a corrupt file is refused, and says why", "[daily][store]") {
    const ScratchDir dir;

    SECTION("not a bar file at all") {
        // Long enough to hold a header, so the magic check is what rejects it
        // rather than the length check in front of it.
        std::ofstream(dir.file("junk.msb"), std::ios::binary) << std::string(200, 'x');
        REQUIRE_THROWS_WITH(bar_store::read(dir.file("junk.msb")), ContainsSubstring("bad magic"));
    }

    SECTION("too short for a header") {
        std::ofstream(dir.file("tiny.msb"), std::ios::binary) << "MSB1";
        REQUIRE_THROWS_WITH(bar_store::read(dir.file("tiny.msb")), ContainsSubstring("too short"));
    }

    SECTION("truncated body") {
        bar_store::write(dir.file("cut.msb"), series("CUT", 50));
        std::filesystem::resize_file(dir.file("cut.msb"), 64 + 20 * 48);
        REQUIRE_THROWS_WITH(bar_store::read(dir.file("cut.msb")), ContainsSubstring("truncated"));
    }

    SECTION("missing file") {
        REQUIRE_THROWS_WITH(bar_store::read(dir.file("absent.msb")),
                            ContainsSubstring("cannot open"));
    }
}

TEST_CASE("out-of-order bars are refused rather than sorted", "[daily][store]") {
    const ScratchDir dir;
    BarSeries backwards = series("BAD", 5);
    std::swap(backwards.bars[1], backwards.bars[3]);

    // Refused on the way in, so a writer that misunderstood its input finds
    // out. Sorting silently would hide that and leave whatever else it
    // misunderstood in place.
    REQUIRE_THROWS_WITH(bar_store::write(dir.file("BAD.msb"), backwards),
                        ContainsSubstring("not strictly increasing"));
}

TEST_CASE("a ticker that does not fit is refused", "[daily][store]") {
    const ScratchDir dir;
    BarSeries long_name = series("X", 2);
    long_name.ticker = std::string(40, 'A');
    REQUIRE_THROWS_WITH(bar_store::write(dir.file("long.msb"), long_name),
                        ContainsSubstring("does not fit"));
}

TEST_CASE("a summary reads the ends without reading the middle", "[daily][store]") {
    const ScratchDir dir;
    const BarSeries written = series("SPY", 1'000);
    bar_store::write(dir.file("SPY.msb"), written);

    const bar_store::Summary summary = bar_store::summarise(dir.file("SPY.msb"));
    REQUIRE(summary.ticker == "SPY");
    REQUIRE(summary.bars == 1'000);
    REQUIRE(summary.first == written.bars.front().ts);
    REQUIRE(summary.last == written.bars.back().ts);
}

TEST_CASE("a cache is listed in sorted order", "[daily][store][determinism]") {
    const ScratchDir dir;
    // Written in an order that is not the sorted one.
    for (const char* ticker : {"ZZZ", "AAA", "MMM", "BBB"}) {
        bar_store::write(dir.file(std::string(ticker) + ".msb"), series(ticker, 3));
    }
    std::ofstream(dir.file("notes.txt")) << "ignored";

    const std::vector<std::filesystem::path> files = bar_store::list(dir.path());

    // The order files are listed in is the order their symbols are interned,
    // which is the order ids are assigned, which reaches the tie-break between
    // two bars at the same timestamp. A directory listing is not a promise.
    REQUIRE(files.size() == 4);
    REQUIRE(files[0].stem() == "AAA");
    REQUIRE(files[1].stem() == "BBB");
    REQUIRE(files[2].stem() == "MMM");
    REQUIRE(files[3].stem() == "ZZZ");
}

// --------------------------------------------------------------------------
// The feed
// --------------------------------------------------------------------------

TEST_CASE("the daily feed merges symbols into one timestamp order", "[daily][feed]") {
    const ScratchDir dir;
    for (const char* ticker : {"AAA", "BBB", "CCC"}) {
        bar_store::write(dir.file(std::string(ticker) + ".msb"), series(ticker, 20));
    }

    SymbolTable symbols;
    DailyFeedOptions options;
    options.synthesize_quotes = false;
    DailyBarFeed feed(dir.path(), symbols, options);

    REQUIRE(feed.symbols().size() == 3);
    REQUIRE(feed.bar_count() == 60);

    MarketEvent event;
    Timestamp previous = kNever;
    std::size_t seen = 0;
    while (feed.next(event)) {
        REQUIRE(event.ts() >= previous);
        previous = event.ts();
        ++seen;
    }
    REQUIRE(seen == 60);
}

TEST_CASE("the daily feed puts a quote in front of every bar", "[daily][feed][fills]") {
    const ScratchDir dir;
    bar_store::write(dir.file("AAA.msb"), series("AAA", 10));

    SymbolTable symbols;
    DailyBarFeed feed(dir.path(), symbols);  // quotes on by default

    // On by default here and off by default for CsvFeed, because a daily file
    // has no quotes and never will. Without one a market order has nothing to
    // cross and fills at the close, which is the optimistic model in disguise.
    REQUIRE(feed.size() == 20);

    MarketEvent event;
    bool expecting_quote = true;
    Timestamp quote_ts = 0;
    while (feed.next(event)) {
        if (expecting_quote) {
            REQUIRE(event.type == EventType::Quote);
            REQUIRE(event.quote.bid_px < event.quote.ask_px);
            quote_ts = event.ts();
        } else {
            REQUIRE(event.type == EventType::Bar);
            // Same timestamp, quote first: the spread-crossing model prices an
            // order from the quote in effect at the signal timestamp, and a
            // strategy acting inside on_bar must already have had one.
            REQUIRE(event.ts() == quote_ts);
            REQUIRE(event.bar.close > event.quote.bid_px * 0.9);
        }
        expecting_quote = !expecting_quote;
    }
}

TEST_CASE("the synthesized spread straddles the close", "[daily][feed][fills]") {
    const ScratchDir dir;
    BarSeries one;
    one.ticker = "AAA";
    StoredBar bar;
    bar.ts = bar_store::daily_close_stamp(calendar::days_from_civil(2024, 6, 3));
    bar.open = bar.low = 99.0;
    bar.close = 100.0;
    bar.high = 101.0;
    bar.volume = 1.0;
    one.bars.push_back(bar);
    bar_store::write(dir.file("AAA.msb"), one);

    SymbolTable symbols;
    DailyFeedOptions options;
    options.spread_bps = 10.0;
    DailyBarFeed feed(dir.path(), symbols, options);

    MarketEvent event;
    REQUIRE(feed.next(event));
    REQUIRE(event.type == EventType::Quote);
    REQUIRE(event.quote.bid_px == Catch::Approx(99.95));
    REQUIRE(event.quote.ask_px == Catch::Approx(100.05));
}

TEST_CASE("the daily feed replays identically", "[daily][feed][determinism]") {
    const ScratchDir dir;
    for (const char* ticker : {"DDD", "AAA", "CCC", "BBB"}) {
        bar_store::write(dir.file(std::string(ticker) + ".msb"), series(ticker, 30, 50.0));
    }

    const auto once = [&dir] {
        SymbolTable symbols;
        DailyBarFeed feed(dir.path(), symbols);
        std::vector<std::pair<Timestamp, SymbolId>> order;
        MarketEvent event;
        while (feed.next(event)) {
            order.emplace_back(event.ts(), event.symbol());
        }
        return order;
    };

    const auto first = once();
    const auto second = once();

    REQUIRE(first.size() == 240);
    REQUIRE(first == second);

    // And the tie-break really is symbol id, which is sorted-filename order --
    // so AAA precedes BBB at the same timestamp regardless of the order the
    // files were written in.
    REQUIRE(first[0].second < first[2].second);
}

TEST_CASE("a feed can be reset and replayed", "[daily][feed][determinism]") {
    const ScratchDir dir;
    bar_store::write(dir.file("AAA.msb"), series("AAA", 15));

    SymbolTable symbols;
    DailyBarFeed feed(dir.path(), symbols);
    REQUIRE(feed.can_reset());

    MarketEvent event;
    std::size_t first_pass = 0;
    while (feed.next(event)) {
        ++first_pass;
    }
    feed.reset();
    std::size_t second_pass = 0;
    while (feed.next(event)) {
        ++second_pass;
    }
    REQUIRE(first_pass == second_pass);
    REQUIRE(first_pass == 30);
}

TEST_CASE("an empty or missing cache is an error, not an empty run", "[daily][feed]") {
    const ScratchDir dir;

    SECTION("directory does not exist") {
        SymbolTable symbols;
        REQUIRE_THROWS_WITH(DailyBarFeed(dir.file("nope"), symbols),
                            ContainsSubstring("not a directory"));
    }

    SECTION("directory holds no bar files") {
        SymbolTable symbols;
        std::ofstream(dir.file("readme.txt")) << "nothing here";
        REQUIRE_THROWS_WITH(DailyBarFeed(dir.path(), symbols), ContainsSubstring("no .msb files"));
    }
}

TEST_CASE("an impossible bar is refused at load", "[daily][feed]") {
    const ScratchDir dir;
    BarSeries bad;
    bad.ticker = "BAD";
    StoredBar bar;
    bar.ts = bar_store::daily_close_stamp(calendar::days_from_civil(2024, 6, 3));
    bar.open = 100.0;
    bar.high = 101.0;
    bar.low = 99.0;
    bar.close = 150.0;  // above the high
    bar.volume = 1.0;
    bad.bars.push_back(bar);
    bar_store::write(dir.file("BAD.msb"), bad);

    // A bar whose high and low do not bracket its open and close lets a fill
    // simulator trade at a price the day never saw. Refused at load rather
    // than tolerated, because nothing downstream can detect it.
    SymbolTable symbols;
    REQUIRE_THROWS_WITH(DailyBarFeed(dir.path(), symbols), ContainsSubstring("impossible bar"));
}

TEST_CASE("a corrupt file in the cache fails the run", "[daily][feed]") {
    const ScratchDir dir;
    bar_store::write(dir.file("GOOD.msb"), series("GOOD", 5));
    std::ofstream(dir.file("BROKEN.msb"), std::ios::binary) << std::string(200, 'x');

    // Fatal rather than skipped. A cache with one corrupt symbol produces a
    // backtest over a universe that quietly lost a name, and nothing
    // downstream can tell that from a name that was never in the index.
    SymbolTable symbols;
    REQUIRE_THROWS_WITH(DailyBarFeed(dir.path(), symbols), ContainsSubstring("bad magic"));
}

// --------------------------------------------------------------------------
// Against the committed fixture, and at scale
// --------------------------------------------------------------------------

TEST_CASE("the committed daily sample loads and converts", "[daily][feed]") {
    // The CSV is the committed artefact and the binary is the cache format.
    // They are produced independently -- one by a Python script, one by the
    // C++ writer -- so converting between them tests something.
    SymbolTable csv_symbols;
    CsvFeedOptions csv_options;
    csv_options.synthesize_quotes = false;
    CsvFeed csv(std::filesystem::path(MARKETSCAN_TEST_DATA_DIR) / "synthetic_daily_5.csv",
                csv_symbols,
                csv_options);

    REQUIRE(csv.symbols().size() == 5);
    REQUIRE(csv.size() == 3'780);

    // Group the CSV's bars by ticker and write them out as a cache.
    const ScratchDir dir;
    std::vector<BarSeries> grouped(csv_symbols.count() + 1);
    MarketEvent event;
    while (csv.next(event)) {
        if (event.type != EventType::Bar) {
            continue;
        }
        BarSeries& target = grouped[event.bar.symbol];
        target.ticker = std::string(csv_symbols.name(event.bar.symbol));
        target.bars.push_back(StoredBar{event.bar.ts,
                                        event.bar.open,
                                        event.bar.high,
                                        event.bar.low,
                                        event.bar.close,
                                        event.bar.volume});
    }
    for (const BarSeries& one : grouped) {
        if (!one.ticker.empty()) {
            bar_store::write(dir.file(one.ticker + ".msb"), one);
        }
    }

    SymbolTable daily_symbols;
    DailyFeedOptions options;
    options.synthesize_quotes = false;
    DailyBarFeed feed(dir.path(), daily_symbols, options);

    REQUIRE(feed.bar_count() == 3'780);
    REQUIRE(feed.symbols().size() == 5);
    REQUIRE(daily_symbols.find("ALFA") != kInvalidSymbol);
}

TEST_CASE("a multi-year cache across hundreds of names replays", "[daily][feed][scale]") {
    // The Definition of Done asks for several hundred symbols over multiple
    // years. Synthetic, because the real thing cannot be committed -- but the
    // shape of the work is identical, and what is being checked is that the
    // merge holds up rather than that the prices mean anything.
    const ScratchDir dir;
    constexpr int kSymbols = 300;
    constexpr int kDays = 1'260;  // five years

    for (int i = 0; i < kSymbols; ++i) {
        char ticker[8];
        std::snprintf(ticker, sizeof(ticker), "S%03d", i);
        bar_store::write(dir.file(std::string(ticker) + ".msb"),
                         series(ticker, kDays, 20.0 + i * 0.5));
    }

    SymbolTable symbols;
    DailyFeedOptions options;
    options.synthesize_quotes = false;

    const auto started = std::chrono::steady_clock::now();
    DailyBarFeed feed(dir.path(), symbols, options);

    Engine engine;
    engine.add_source(std::make_unique<DailyBarFeed>(dir.path(), symbols, options));
    const std::uint64_t dispatched = engine.run();
    const auto elapsed = std::chrono::steady_clock::now() - started;

    REQUIRE(feed.symbols().size() == kSymbols);
    REQUIRE(dispatched == kSymbols * kDays);

    INFO("dispatched " << dispatched << " events in "
                       << std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count()
                       << "ms");

    // Every day is present for every name, and the run is in order.
    REQUIRE(engine.stats().bars_dispatched == kSymbols * kDays);
    REQUIRE(engine.stats().first_event_ts < engine.stats().last_event_ts);
}
