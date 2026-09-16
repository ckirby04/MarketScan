#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "marketscan/data/csv_feed.hpp"
#include "marketscan/data/synthetic_feed.hpp"

using namespace marketscan;

namespace {

std::vector<MarketEvent> drain(IEventSource& source) {
    std::vector<MarketEvent> events;
    MarketEvent event;
    while (source.next(event)) {
        events.push_back(event);
    }
    return events;
}

SyntheticFeedConfig small_config(std::uint64_t seed = 12345) {
    SyntheticFeedConfig config;
    config.instruments = {
        SyntheticInstrument{"AAA", 100.0, 0.05, 0.20, 50'000.0},
        SyntheticInstrument{"BBB", 50.0, -0.02, 0.35, 20'000.0},
    };
    Timestamp start = 0;
    REQUIRE(time::from_iso8601("2024-03-18T13:30:00Z", start));
    config.start = start;
    config.bars_per_session = 10;
    config.sessions = 3;
    config.seed = seed;
    return config;
}

/// Writes `content` to a uniquely named file that is removed when the guard
/// goes out of scope.
class TempFile {
public:
    explicit TempFile(std::string_view content, std::string_view suffix = ".csv") {
        static int counter = 0;
        path_ = std::filesystem::temp_directory_path() /
                ("marketscan_test_" + std::to_string(++counter) + std::string(suffix));
        std::ofstream out(path_, std::ios::binary);
        out << content;
    }

    TempFile(const TempFile&) = delete;
    TempFile& operator=(const TempFile&) = delete;

    ~TempFile() {
        std::error_code ec;
        std::filesystem::remove(path_, ec);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

std::filesystem::path sample_csv() {
    return std::filesystem::path(MARKETSCAN_TEST_DATA_DIR) / "synthetic_spy_1min.csv";
}

}  // namespace

// --------------------------------------------------------------------------
// SymbolTable
// --------------------------------------------------------------------------

TEST_CASE("symbols get dense ids in first-seen order", "[symbols]") {
    SymbolTable table;
    REQUIRE(table.empty());

    const SymbolId spy = table.intern("SPY");
    const SymbolId qqq = table.intern("QQQ");

    REQUIRE(spy == 1);
    REQUIRE(qqq == 2);
    REQUIRE(table.intern("SPY") == spy);
    REQUIRE(table.count() == 2);
    REQUIRE(table.name(spy) == "SPY");
    REQUIRE(std::string(table.c_name(qqq)) == "QQQ");
    REQUIRE(table.ids() == std::vector<SymbolId>{1, 2});
}

TEST_CASE("an unknown ticker is not a symbol", "[symbols]") {
    SymbolTable table;
    table.intern("SPY");

    REQUIRE(table.find("NOPE") == kInvalidSymbol);
    REQUIRE(table.find("spy") == kInvalidSymbol);  // exact match, never folded
    REQUIRE(table.intern("") == kInvalidSymbol);
    REQUIRE(table.name(kInvalidSymbol).empty());
    REQUIRE(table.name(99).empty());

    // Never nullptr, so a plugin that skips the check cannot crash on strlen.
    REQUIRE(table.c_name(99) != nullptr);
    REQUIRE(std::string(table.c_name(99)).empty());
}

// The ABI promises a plugin that a symbol name stays valid for the run. A
// vector<string> would break that promise the first time it reallocated.
TEST_CASE("symbol name pointers survive later interning", "[symbols][abi]") {
    SymbolTable table;
    const SymbolId first = table.intern("AAA");
    const char* held = table.c_name(first);

    for (int i = 0; i < 1'000; ++i) {
        table.intern("SYM" + std::to_string(i));
    }

    REQUIRE(std::string(held) == "AAA");
    REQUIRE(held == table.c_name(first));
}

// --------------------------------------------------------------------------
// SyntheticFeed
// --------------------------------------------------------------------------

TEST_CASE("the synthetic feed produces the configured extent", "[feed][synthetic]") {
    SymbolTable symbols;
    SyntheticFeed feed(small_config(), symbols);

    REQUIRE(feed.symbols().size() == 2);
    REQUIRE(symbols.name(feed.symbols()[0]) == "AAA");

    const FeedSpan span = feed.span();
    REQUIRE(span.known);
    REQUIRE(span.event_count == 3 * 10 * 2 * 2);  // sessions * bars * symbols * (quote+bar)

    const auto events = drain(feed);
    REQUIRE(events.size() == span.event_count);
    REQUIRE(events.front().ts() == span.first);
    REQUIRE(events.back().ts() == span.last);
}

TEST_CASE("synthetic events are in non-decreasing timestamp order", "[feed][synthetic]") {
    SymbolTable symbols;
    SyntheticFeed feed(small_config(), symbols);
    const auto events = drain(feed);

    bool ordered = true;
    for (std::size_t i = 1; i < events.size(); ++i) {
        if (events[i].ts() < events[i - 1].ts()) {
            ordered = false;
            break;
        }
    }
    REQUIRE(ordered);
}

// The spread-crossing fill model prices an order from the quote in effect at
// the signal timestamp. If the bar arrived first, a strategy acting inside
// on_bar would have no spread to cross on the very first bar.
TEST_CASE("each quote precedes its bar at the same timestamp", "[feed][synthetic][fills]") {
    SymbolTable symbols;
    SyntheticFeed feed(small_config(), symbols);
    const auto events = drain(feed);

    for (std::size_t i = 0; i + 1 < events.size(); i += 2) {
        REQUIRE(events[i].type == EventType::Quote);
        REQUIRE(events[i + 1].type == EventType::Bar);
        REQUIRE(events[i].ts() == events[i + 1].ts());
        REQUIRE(events[i].symbol() == events[i + 1].symbol());
        REQUIRE(events[i].quote.bid_px < events[i + 1].bar.close);
        REQUIRE(events[i].quote.ask_px > events[i + 1].bar.close);
    }
}

// A bar whose high is below its close lets a fill simulator trade at a price
// that never existed, which is the most common way synthetic data invents
// profit out of nothing.
TEST_CASE("synthetic bars are internally consistent", "[feed][synthetic]") {
    SymbolTable symbols;
    SyntheticFeedConfig config = small_config();
    config.bars_per_session = 200;
    config.sessions = 5;
    SyntheticFeed feed(config, symbols);

    bool consistent = true;
    for (const MarketEvent& event : drain(feed)) {
        if (event.type != EventType::Bar) {
            continue;
        }
        const MS_Bar& bar = event.bar;
        if (bar.low <= 0.0 || bar.high < bar.low || bar.high < bar.open || bar.high < bar.close ||
            bar.low > bar.open || bar.low > bar.close || bar.volume < 0.0) {
            consistent = false;
            break;
        }
    }
    REQUIRE(consistent);
}

TEST_CASE("sessions are separated by a gap", "[feed][synthetic]") {
    SymbolTable symbols;
    SyntheticFeedConfig config = small_config();
    config.instruments.resize(1);
    SyntheticFeed feed(config, symbols);
    const auto events = drain(feed);

    // Two events per bar, ten bars per session.
    const Timestamp last_of_session = events[2 * 10 - 1].ts();
    const Timestamp first_of_next = events[2 * 10].ts();
    REQUIRE(first_of_next - last_of_session == time::kDay - 9 * time::kMinute);
}

TEST_CASE("the same seed replays exactly", "[feed][synthetic][determinism]") {
    SymbolTable a_symbols;
    SymbolTable b_symbols;
    SyntheticFeed a(small_config(777), a_symbols);
    SyntheticFeed b(small_config(777), b_symbols);

    const auto first = drain(a);
    const auto second = drain(b);

    REQUIRE(first.size() == second.size());
    REQUIRE(first == second);
}

TEST_CASE("a different seed produces different prices", "[feed][synthetic][determinism]") {
    SymbolTable a_symbols;
    SymbolTable b_symbols;
    SyntheticFeed a(small_config(1), a_symbols);
    SyntheticFeed b(small_config(2), b_symbols);

    const auto first = drain(a);
    const auto second = drain(b);

    REQUIRE(first.size() == second.size());
    REQUIRE(first.back().ts() == second.back().ts());
    REQUIRE(first.back().mark_price() != second.back().mark_price());
}

TEST_CASE("reset rewinds the synthetic feed exactly", "[feed][synthetic][determinism]") {
    SymbolTable symbols;
    SyntheticFeed feed(small_config(99), symbols);

    const auto first = drain(feed);
    REQUIRE(feed.can_reset());
    feed.reset();
    const auto second = drain(feed);

    REQUIRE(first.size() == second.size());
    REQUIRE(first == second);
}

TEST_CASE("the synthetic feed rejects nonsense configuration", "[feed][synthetic]") {
    SymbolTable symbols;

    SyntheticFeedConfig empty;
    REQUIRE_THROWS_AS(SyntheticFeed(empty, symbols), std::invalid_argument);

    SyntheticFeedConfig zero_bars = small_config();
    zero_bars.bars_per_session = 0;
    REQUIRE_THROWS_AS(SyntheticFeed(zero_bars, symbols), std::invalid_argument);

    SyntheticFeedConfig bad_price = small_config();
    bad_price.instruments[0].initial_price = 0.0;
    REQUIRE_THROWS_AS(SyntheticFeed(bad_price, symbols), std::invalid_argument);
}

// --------------------------------------------------------------------------
// CsvFeed
// --------------------------------------------------------------------------

TEST_CASE("the committed sample loads", "[feed][csv]") {
    SymbolTable symbols;
    CsvFeed feed(sample_csv(), symbols);

    REQUIRE(feed.size() == 780);
    REQUIRE(feed.symbols().size() == 1);
    REQUIRE(symbols.name(feed.symbols()[0]) == "SPY");

    const FeedSpan span = feed.span();
    REQUIRE(span.known);
    REQUIRE(time::to_iso8601(span.first) == "2024-03-18T13:30:00.000000000Z");
    REQUIRE(time::to_iso8601(span.last) == "2024-03-19T19:59:00.000000000Z");

    const auto events = drain(feed);
    REQUIRE(events.size() == 780);
    REQUIRE(events.front().type == EventType::Bar);
    REQUIRE(events.front().bar.open == 510.0);
}

TEST_CASE("the sample replays identically", "[feed][csv][determinism]") {
    SymbolTable symbols;
    CsvFeed feed(sample_csv(), symbols);

    const auto first = drain(feed);
    feed.reset();
    const auto second = drain(feed);

    REQUIRE(first.size() == second.size());
    REQUIRE(first == second);
}

TEST_CASE("rows are sorted by timestamp, stably", "[feed][csv][determinism]") {
    // Two rows share a timestamp; the file's order between them must survive.
    const TempFile file("timestamp,symbol,close\n"
                        "2024-01-03T00:00:00Z,BBB,30\n"
                        "2024-01-01T00:00:00Z,AAA,10\n"
                        "2024-01-02T00:00:00Z,FIRST,20\n"
                        "2024-01-02T00:00:00Z,SECOND,21\n");

    SymbolTable symbols;
    CsvFeed feed(file.path(), symbols);
    const auto events = drain(feed);

    REQUIRE(events.size() == 4);
    REQUIRE(events[0].bar.close == 10);
    REQUIRE(symbols.name(events[1].symbol()) == "FIRST");
    REQUIRE(symbols.name(events[2].symbol()) == "SECOND");
    REQUIRE(events[3].bar.close == 30);
}

TEST_CASE("a close-only file is a valid price series", "[feed][csv]") {
    const TempFile file("timestamp,close\n"
                        "2024-01-01T00:00:00Z,100.5\n"
                        "2024-01-01T00:01:00Z,101.0\n");

    SymbolTable symbols;
    CsvFeedOptions options;
    options.default_symbol = "ONLY";
    CsvFeed feed(file.path(), symbols, options);

    const auto events = drain(feed);
    REQUIRE(events.size() == 2);
    REQUIRE(events[0].bar.open == 100.5);
    REQUIRE(events[0].bar.high == 100.5);
    REQUIRE(events[0].bar.low == 100.5);
    REQUIRE(events[0].bar.close == 100.5);
    REQUIRE(symbols.name(events[0].symbol()) == "ONLY");
}

TEST_CASE("quote rows are recognised from the header", "[feed][csv]") {
    const TempFile file("ts,ticker,bid,ask,bid_size,ask_size\n"
                        "2024-01-01T00:00:00Z,SPY,99.99,100.01,300,400\n");

    SymbolTable symbols;
    CsvFeed feed(file.path(), symbols);
    const auto events = drain(feed);

    REQUIRE(events.size() == 1);
    REQUIRE(events[0].type == EventType::Quote);
    REQUIRE(events[0].quote.bid_px == 99.99);
    REQUIRE(events[0].quote.ask_sz == 400);
}

TEST_CASE("trade rows are recognised from the header", "[feed][csv]") {
    const TempFile file("timestamp,symbol,price,size,side\n"
                        "2024-01-01T00:00:00Z,SPY,100.0,50,buy\n"
                        "2024-01-01T00:00:01Z,SPY,100.5,25,SELL\n");

    SymbolTable symbols;
    CsvFeed feed(file.path(), symbols);
    const auto events = drain(feed);

    REQUIRE(events.size() == 2);
    REQUIRE(events[0].type == EventType::Trade);
    REQUIRE(events[0].trade.aggressor == MS_SIDE_BUY);
    REQUIRE(events[1].trade.aggressor == MS_SIDE_SELL);
}

TEST_CASE("synthesized quotes straddle the close and precede the bar", "[feed][csv][fills]") {
    const TempFile file("timestamp,symbol,close\n"
                        "2024-01-01T00:00:00Z,SPY,100.0\n");

    SymbolTable symbols;
    CsvFeedOptions options;
    options.synthesize_quotes = true;
    options.spread_bps = 10.0;  // 10bp full spread -> +/- 5bp
    CsvFeed feed(file.path(), symbols, options);
    const auto events = drain(feed);

    REQUIRE(events.size() == 2);
    REQUIRE(events[0].type == EventType::Quote);
    REQUIRE(events[1].type == EventType::Bar);
    REQUIRE(events[0].quote.bid_px == 100.0 - 0.05);
    REQUIRE(events[0].quote.ask_px == 100.0 + 0.05);
}

TEST_CASE("comments and blank lines are skipped", "[feed][csv]") {
    const TempFile file("# a comment before the header\n"
                        "\n"
                        "timestamp,symbol,close\n"
                        "# and one in the body\n"
                        "2024-01-01T00:00:00Z,SPY,100\n"
                        "\n");

    SymbolTable symbols;
    CsvFeed feed(file.path(), symbols);
    REQUIRE(feed.size() == 1);
}

// Silently dropping input is how a backtest ends up describing a dataset that
// does not exist. Every one of these is an error that names the line.
TEST_CASE("malformed input is refused, not skipped", "[feed][csv]") {
    SymbolTable symbols;

    SECTION("missing file") {
        REQUIRE_THROWS_AS(CsvFeed("does/not/exist.csv", symbols), std::runtime_error);
    }

    SECTION("no header") {
        const TempFile file("# only a comment\n");
        REQUIRE_THROWS_AS(CsvFeed(file.path(), symbols), std::runtime_error);
    }

    SECTION("no timestamp column") {
        const TempFile file("symbol,close\nSPY,100\n");
        REQUIRE_THROWS_WITH(CsvFeed(file.path(), symbols),
                            Catch::Matchers::ContainsSubstring("timestamp column"));
    }

    SECTION("unrecognisable payload") {
        const TempFile file("timestamp,symbol\n2024-01-01T00:00:00Z,SPY\n");
        REQUIRE_THROWS_AS(CsvFeed(file.path(), symbols), std::runtime_error);
    }

    SECTION("no symbol column and no default") {
        const TempFile file("timestamp,close\n2024-01-01T00:00:00Z,100\n");
        REQUIRE_THROWS_WITH(CsvFeed(file.path(), symbols),
                            Catch::Matchers::ContainsSubstring("refusing to"));
    }

    SECTION("unparseable timestamp") {
        const TempFile file("timestamp,symbol,close\nyesterday,SPY,100\n");
        REQUIRE_THROWS_WITH(CsvFeed(file.path(), symbols),
                            Catch::Matchers::ContainsSubstring("unparseable timestamp"));
    }

    SECTION("non-numeric price") {
        const TempFile file("timestamp,symbol,close\n2024-01-01T00:00:00Z,SPY,par\n");
        REQUIRE_THROWS_WITH(CsvFeed(file.path(), symbols),
                            Catch::Matchers::ContainsSubstring("is not a number"));
    }

    SECTION("impossible bar") {
        const TempFile file("timestamp,symbol,open,high,low,close\n"
                            "2024-01-01T00:00:00Z,SPY,100,101,99,105\n");  // close above high
        REQUIRE_THROWS_WITH(CsvFeed(file.path(), symbols),
                            Catch::Matchers::ContainsSubstring("impossible bar"));
    }

    SECTION("crossed quote") {
        const TempFile file("timestamp,symbol,bid,ask\n"
                            "2024-01-01T00:00:00Z,SPY,101,100\n");
        REQUIRE_THROWS_WITH(CsvFeed(file.path(), symbols),
                            Catch::Matchers::ContainsSubstring("crossed quote"));
    }

    SECTION("no data rows") {
        const TempFile file("timestamp,symbol,close\n");
        REQUIRE_THROWS_WITH(CsvFeed(file.path(), symbols),
                            Catch::Matchers::ContainsSubstring("no data rows"));
    }
}

TEST_CASE("an error names the file and line", "[feed][csv]") {
    const TempFile file("timestamp,symbol,close\n"
                        "2024-01-01T00:00:00Z,SPY,100\n"
                        "2024-01-01T00:01:00Z,SPY,oops\n");

    SymbolTable symbols;
    REQUIRE_THROWS_WITH(CsvFeed(file.path(), symbols), Catch::Matchers::ContainsSubstring(":3:"));
}

// --------------------------------------------------------------------------
// The committed sample and the script that made it
// --------------------------------------------------------------------------
//
// data/samples/synthetic_spy_1min.csv is checked in, and scripts/
// make_sample_data.py claims to reproduce it byte for byte by reimplementing
// the same generator in Python. That claim was true when it was written and
// nothing was defending it: the script is not run by the build, so a change to
// SyntheticFeed would leave the two silently disagreeing, and the committed
// file -- which several tests read -- would become a fossil of a generator
// that no longer exists.
//
// This test closes that gap from the C++ side, which is the side that matters
// and the side that needs no Python on the machine. It regenerates the sample
// with the configuration the script documents and compares bar for bar
// against the committed file. If it fails, either the generator changed or
// the script and the feed have drifted apart, and both are worth knowing.

TEST_CASE("the committed sample is what the generator produces", "[feed][determinism]") {
    // Exactly the configuration in scripts/make_sample_data.py. Kept in sync
    // by this test failing if it ever is not.
    SyntheticFeedConfig config;
    config.instruments = {SyntheticInstrument{"SPY", 510.00, 0.08, 0.15, 90'000.0}};
    REQUIRE(time::from_iso8601("2024-03-18T13:30:00Z", config.start));
    config.bars_per_session = 390;
    config.sessions = 2;
    config.seed = 0xC0FF'EE00'1234'5678ULL;
    config.emit_quotes = false;

    SymbolTable generated_symbols;
    SyntheticFeed generated(config, generated_symbols);

    SymbolTable committed_symbols;
    CsvFeed committed(std::filesystem::path(MARKETSCAN_TEST_DATA_DIR) / "synthetic_spy_1min.csv",
                      committed_symbols);

    std::size_t compared = 0;
    MarketEvent from_generator;
    MarketEvent from_file;

    while (committed.next(from_file)) {
        if (from_file.type != EventType::Bar) {
            continue;
        }
        REQUIRE(generated.next(from_generator));
        REQUIRE(from_generator.type == EventType::Bar);

        INFO("bar " << compared);
        REQUIRE(from_generator.bar.ts == from_file.bar.ts);

        // The committed file carries two decimal places, so the comparison is
        // to the precision the file has rather than to the precision the
        // generator has. A drift large enough to matter moves the cent.
        REQUIRE(from_generator.bar.open == Catch::Approx(from_file.bar.open).margin(0.005));
        REQUIRE(from_generator.bar.high == Catch::Approx(from_file.bar.high).margin(0.005));
        REQUIRE(from_generator.bar.low == Catch::Approx(from_file.bar.low).margin(0.005));
        REQUIRE(from_generator.bar.close == Catch::Approx(from_file.bar.close).margin(0.005));
        REQUIRE(from_generator.bar.volume == Catch::Approx(from_file.bar.volume).margin(0.5));
        ++compared;
    }

    REQUIRE(compared == 780);
    REQUIRE_FALSE(generated.next(from_generator));
}
