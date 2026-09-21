/*
 * The shared stream parser, the monotonic gate, and the recorder round trip.
 *
 * NO NETWORK. Every message below is hand-written to the shape the vendor
 * documents, with invented prices. None of it is recorded market data, which
 * could not be committed even if it were convenient -- docs/DATA_POLICY.md §7.
 *
 * WHAT THESE TESTS ARE FOR
 *
 * The parser is the one piece of code the live path and the replay path both
 * run. If it were wrong, the equivalence test would still pass -- both sides
 * would be wrong identically -- and the error would surface as a strategy that
 * behaves oddly against real data and fine against a fixture. So the parser is
 * tested against its inputs directly rather than only through the equivalence
 * it makes possible.
 */

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <filesystem>
#include <fstream>
#include <string>

#include "marketscan/core/symbol_table.hpp"
#include "marketscan/live/recorded_feed.hpp"
#include "marketscan/live/recorder.hpp"
#include "marketscan/live/stream_parser.hpp"

using namespace marketscan;
using namespace marketscan::live;
using Catch::Matchers::WithinAbs;

namespace {

/// A table holding exactly the symbols a test subscribed to. The parser looks
/// up and never interns, so anything else in a message is dropped and counted.
SymbolTable table_with(std::initializer_list<const char*> tickers) {
    SymbolTable table;
    for (const char* t : tickers) {
        (void)table.intern(t);
    }
    return table;
}

/// A scratch path that does not collide between tests.
std::filesystem::path scratch(const std::string& name) {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "marketscan_tests";
    std::filesystem::create_directories(dir);
    return dir / name;
}

}  // namespace

TEST_CASE("a trade message becomes a trade event", "[live][parser]") {
    SymbolTable table = table_with({"AAPL"});
    const ParseResult parsed = parse_stream_message(
        R"([{"T":"t","S":"AAPL","i":42,"x":"V","p":123.45,"s":300,"c":["@"],"z":"C",)"
        R"("t":"2026-09-21T18:36:33.355534795Z"}])",
        table);

    REQUIRE(parsed.error.empty());
    REQUIRE(parsed.events.size() == 1);
    REQUIRE(parsed.stats.events == 1);

    const MarketEvent& event = parsed.events.front();
    REQUIRE(event.type == EventType::Trade);
    REQUIRE(event.trade.symbol == table.find("AAPL"));
    REQUIRE_THAT(event.trade.price, WithinAbs(123.45, 1e-12));
    REQUIRE_THAT(event.trade.size, WithinAbs(300.0, 1e-12));
}

TEST_CASE("nanosecond timestamps survive the parse exactly", "[live][parser]") {
    // The whole point of a nanosecond Timestamp. A parser that truncated to
    // milliseconds would reorder events that the exchange distinguished, and
    // the loss would be invisible in every summary statistic.
    SymbolTable table = table_with({"AAPL"});
    const ParseResult parsed = parse_stream_message(
        R"([{"T":"t","S":"AAPL","p":1.0,"s":1,"t":"2026-09-21T18:36:33.355534795Z"}])", table);

    REQUIRE(parsed.events.size() == 1);

    Timestamp expected = 0;
    REQUIRE(time::from_iso8601("2026-09-21T18:36:33.355534795Z", expected));
    REQUIRE(parsed.events.front().trade.ts == expected);
    REQUIRE(parsed.events.front().trade.ts % 1000 == 795 % 1000);
}

TEST_CASE("a quote message becomes a quote event with sizes in shares", "[live][parser]") {
    SymbolTable table = table_with({"SPY"});
    const ParseResult parsed = parse_stream_message(
        R"([{"T":"q","S":"SPY","bx":"V","bp":500.10,"bs":3,"ax":"V","ap":500.12,"as":7,)"
        R"("t":"2026-09-21T18:36:33Z"}])",
        table);

    REQUIRE(parsed.events.size() == 1);
    const MS_Quote& quote = parsed.events.front().quote;
    REQUIRE_THAT(quote.bid_px, WithinAbs(500.10, 1e-12));
    REQUIRE_THAT(quote.ask_px, WithinAbs(500.12, 1e-12));

    // Shares, not lots. An earlier draft multiplied by a hundred on the
    // assumption that the feed quoted round lots; a real capture said
    // otherwise, and kQuoteLotSize carries the measurement that settled it.
    // The test is written against the constant so the two cannot drift apart,
    // and against the raw numbers so the constant cannot quietly change.
    REQUIRE(kQuoteLotSize == 1.0);
    REQUIRE_THAT(quote.bid_sz, WithinAbs(3.0 * kQuoteLotSize, 1e-12));
    REQUIRE_THAT(quote.ask_sz, WithinAbs(7.0 * kQuoteLotSize, 1e-12));
    REQUIRE_THAT(quote.bid_sz, WithinAbs(3.0, 1e-12));
    REQUIRE_THAT(quote.ask_sz, WithinAbs(7.0, 1e-12));
}

TEST_CASE("a bar message becomes a bar event", "[live][parser]") {
    SymbolTable table = table_with({"AAPL"});
    const ParseResult parsed = parse_stream_message(
        R"([{"T":"b","S":"AAPL","o":1.0,"h":4.0,"l":0.5,"c":3.0,"v":900,"n":12,"vw":2.5,)"
        R"("t":"2026-09-21T18:37:00Z"}])",
        table);

    REQUIRE(parsed.events.size() == 1);
    const MS_Bar& bar = parsed.events.front().bar;
    REQUIRE_THAT(bar.open, WithinAbs(1.0, 1e-12));
    REQUIRE_THAT(bar.high, WithinAbs(4.0, 1e-12));
    REQUIRE_THAT(bar.low, WithinAbs(0.5, 1e-12));
    REQUIRE_THAT(bar.close, WithinAbs(3.0, 1e-12));
    REQUIRE_THAT(bar.volume, WithinAbs(900.0, 1e-12));
}

TEST_CASE("one message keeps the order of the events inside it", "[live][parser]") {
    SymbolTable table = table_with({"AAPL", "SPY"});
    const ParseResult parsed = parse_stream_message(
        R"([{"T":"q","S":"AAPL","bp":1,"bs":1,"ap":2,"as":1,"t":"2026-09-21T18:00:00Z"},)"
        R"({"T":"t","S":"AAPL","p":1.5,"s":1,"t":"2026-09-21T18:00:00Z"},)"
        R"({"T":"q","S":"SPY","bp":3,"bs":1,"ap":4,"as":1,"t":"2026-09-21T18:00:00Z"}])",
        table);

    REQUIRE(parsed.events.size() == 3);
    REQUIRE(parsed.events[0].type == EventType::Quote);
    REQUIRE(parsed.events[0].symbol() == table.find("AAPL"));
    REQUIRE(parsed.events[1].type == EventType::Trade);
    REQUIRE(parsed.events[2].type == EventType::Quote);
    REQUIRE(parsed.events[2].symbol() == table.find("SPY"));
}

TEST_CASE("control messages are separated from market events", "[live][parser]") {
    SymbolTable table = table_with({"AAPL"});
    const ParseResult parsed = parse_stream_message(
        R"([{"T":"success","msg":"authenticated"},{"T":"error","code":402,"msg":"auth failed"}])",
        table);

    REQUIRE(parsed.events.empty());
    REQUIRE(parsed.controls.size() == 2);
    REQUIRE(parsed.controls[0].type == "success");
    REQUIRE(parsed.controls[0].message == "authenticated");
    REQUIRE(parsed.controls[1].type == "error");
    REQUIRE(parsed.controls[1].code == 402);
}

TEST_CASE("a symbol nobody subscribed to is counted, not interned", "[live][parser]") {
    // The parser runs on the socket thread while the engine thread reads the
    // table. Interning there would be a data race; a new id appearing
    // mid-run would also be a symbol no strategy was told about.
    SymbolTable table = table_with({"AAPL"});
    const std::size_t before = table.ids().size();

    const ParseResult parsed = parse_stream_message(
        R"([{"T":"t","S":"TSLA","p":1,"s":1,"t":"2026-09-21T18:00:00Z"}])", table);

    REQUIRE(parsed.events.empty());
    REQUIRE(parsed.stats.unknown_symbol == 1);
    REQUIRE(table.ids().size() == before);
    REQUIRE(table.find("TSLA") == kInvalidSymbol);
}

TEST_CASE("message kinds this build has no event for are counted", "[live][parser]") {
    SymbolTable table = table_with({"AAPL"});
    const ParseResult parsed = parse_stream_message(
        R"([{"T":"c","S":"AAPL"},{"T":"x","S":"AAPL"},{"T":"s","S":"AAPL"}])", table);

    REQUIRE(parsed.events.empty());
    REQUIRE(parsed.stats.unhandled_kind == 3);
}

TEST_CASE("a message missing a required field is malformed, not a zero event", "[live][parser]") {
    // The failure that matters: a trade with no price must not arrive as a
    // trade at 0.00, which would mark a position to zero and look like a
    // catastrophic loss rather than like the bad input it is.
    SymbolTable table = table_with({"AAPL"});
    const ParseResult parsed =
        parse_stream_message(R"([{"T":"t","S":"AAPL","s":100,"t":"2026-09-21T18:00:00Z"}])", table);

    REQUIRE(parsed.events.empty());
    REQUIRE(parsed.stats.malformed == 1);
}

TEST_CASE("text that is not a JSON array is an error, not silence", "[live][parser]") {
    SymbolTable table = table_with({"AAPL"});
    REQUIRE_FALSE(parse_stream_message("not json at all", table).error.empty());
    REQUIRE_FALSE(parse_stream_message(R"({"T":"t"})", table).error.empty());
}

TEST_CASE("the monotonic gate raises a late event and counts it", "[live][gate]") {
    SymbolTable table = table_with({"AAPL"});
    // Two trades, the second stamped 500 microseconds before the first. The
    // engine would throw on this; the gate is what stops it reaching there.
    ParseResult parsed = parse_stream_message(
        R"([{"T":"t","S":"AAPL","p":1,"s":1,"t":"2026-09-21T18:00:00.001000000Z"},)"
        R"({"T":"t","S":"AAPL","p":2,"s":1,"t":"2026-09-21T18:00:00.000500000Z"}])",
        table);
    REQUIRE(parsed.events.size() == 2);

    MonotonicGate gate;
    REQUIRE_FALSE(gate.apply(parsed.events[0]));
    REQUIRE(gate.apply(parsed.events[1]));

    REQUIRE(parsed.events[1].ts() == parsed.events[0].ts());
    REQUIRE(gate.reordered() == 1);
    REQUIRE(gate.max_backwards() == 500'000);
    // Trades, so nothing lands in the bar counters.
    REQUIRE(gate.interval_stamped() == 0);
    REQUIRE(gate.max_interval_lag() == 0);
    // Raised, not dropped: the trade is still there.
    REQUIRE_THAT(parsed.events[1].trade.price, WithinAbs(2.0, 1e-12));
}

TEST_CASE("a late bar is counted apart from tick jitter", "[live][gate]") {
    // The distinction a real capture forced. A one-minute bar is stamped at
    // the start of its interval and cannot be sent until the interval ends, so
    // it always arrives about sixty seconds behind the ticks around it. Folded
    // into the jitter statistic it read as a 60,236 ms transport failure; kept
    // apart it reads as what it is.
    SymbolTable table = table_with({"AAPL"});
    ParseResult parsed = parse_stream_message(
        R"([{"T":"t","S":"AAPL","p":1,"s":1,"t":"2026-09-21T18:56:59.900000000Z"},)"
        R"({"T":"b","S":"AAPL","o":1,"h":2,"l":1,"c":2,"v":10,"t":"2026-09-21T18:56:00Z"},)"
        R"({"T":"t","S":"AAPL","p":2,"s":1,"t":"2026-09-21T18:56:59.800000000Z"}])",
        table);
    REQUIRE(parsed.events.size() == 3);

    MonotonicGate gate;
    for (MarketEvent& event : parsed.events) {
        (void)gate.apply(event);
    }

    // One bar, lagging by the interval it covers.
    REQUIRE(gate.interval_stamped() == 1);
    REQUIRE(gate.max_interval_lag() == 59'900'000'000LL);

    // One trade, lagging by a tenth of a second. The bar does not appear in
    // this number, which is the whole point.
    REQUIRE(gate.reordered() == 1);
    REQUIRE(gate.max_backwards() == 100'000'000LL);
}

TEST_CASE("the gate leaves an already ordered stream untouched", "[live][gate]") {
    SymbolTable table = table_with({"AAPL"});
    ParseResult parsed =
        parse_stream_message(R"([{"T":"t","S":"AAPL","p":1,"s":1,"t":"2026-09-21T18:00:00Z"},)"
                             R"({"T":"t","S":"AAPL","p":2,"s":1,"t":"2026-09-21T18:00:01Z"},)"
                             R"({"T":"t","S":"AAPL","p":3,"s":1,"t":"2026-09-21T18:00:01Z"}])",
                             table);

    MonotonicGate gate;
    for (MarketEvent& event : parsed.events) {
        REQUIRE_FALSE(gate.apply(event));
    }
    REQUIRE(gate.reordered() == 0);
    REQUIRE(gate.max_backwards() == 0);
    REQUIRE(gate.interval_stamped() == 0);
    REQUIRE(gate.max_interval_lag() == 0);
}

TEST_CASE("a recording round trips to the same event sequence", "[live][recorder]") {
    const std::filesystem::path path = scratch("roundtrip.ndjson");

    const std::vector<std::string> messages = {
        R"([{"T":"success","msg":"connected"}])",
        R"([{"T":"q","S":"AAPL","bp":1.25,"bs":2,"ap":1.27,"as":4,"t":"2026-09-21T18:00:00.000000001Z"}])",
        R"([{"T":"t","S":"AAPL","p":1.26,"s":50,"t":"2026-09-21T18:00:00.000000002Z"},)"
        R"({"T":"b","S":"SPY","o":1,"h":2,"l":0.5,"c":1.5,"v":10,"t":"2026-09-21T18:00:01Z"}])",
    };

    {
        RecordingHeader header;
        header.host = "stream.data.alpaca.markets";
        header.path = "/v2/iex";
        header.feed = "iex";
        header.symbols = {"AAPL", "SPY"};
        header.started = 1'758'000'000'000'000'000LL;

        StreamRecorder recorder(path, header);
        Timestamp recv = 1'758'000'000'000'000'000LL;
        for (const std::string& message : messages) {
            recorder.record(message, recv);
            recv += 1'000'000;
        }
        REQUIRE(recorder.messages() == messages.size());
    }

    // Parse the originals directly, then through the recording. Same parser
    // both times, which is the point: the file is the only variable.
    SymbolTable direct_table;
    (void)direct_table.intern("AAPL");
    (void)direct_table.intern("SPY");
    std::vector<MarketEvent> direct;
    {
        MonotonicGate gate;
        for (const std::string& message : messages) {
            ParseResult parsed;
            parse_stream_message_into(message, direct_table, parsed);
            for (MarketEvent event : parsed.events) {
                gate.apply(event);
                direct.push_back(event);
            }
        }
    }

    SymbolTable replay_table;
    RecordedFeed feed(path, replay_table);

    REQUIRE(feed.header().version == 1);
    REQUIRE(feed.header().feed == "iex");
    REQUIRE(feed.header().symbols == std::vector<std::string>{"AAPL", "SPY"});
    REQUIRE(feed.stats().messages == messages.size());
    REQUIRE(feed.stats().controls == 1);

    std::vector<MarketEvent> replayed;
    MarketEvent event;
    while (feed.next(event)) {
        replayed.push_back(event);
    }

    REQUIRE(replayed.size() == direct.size());
    REQUIRE(replayed.size() == 3);
    for (std::size_t i = 0; i < replayed.size(); ++i) {
        REQUIRE(replayed[i] == direct[i]);
    }

    // Receive timestamps survive, one per event, and the two events from the
    // third message share its arrival time.
    REQUIRE(feed.receive_timestamps().size() == replayed.size());
    REQUIRE(feed.receive_timestamps()[1] == feed.receive_timestamps()[2]);

    std::filesystem::remove(path);
}

TEST_CASE("a recorded feed replays the same sequence twice", "[live][recorder]") {
    const std::filesystem::path path = scratch("resettable.ndjson");
    {
        RecordingHeader header;
        header.symbols = {"AAPL"};
        StreamRecorder recorder(path, header);
        recorder.record(R"([{"T":"t","S":"AAPL","p":1,"s":1,"t":"2026-09-21T18:00:00Z"}])", 1);
        recorder.record(R"([{"T":"t","S":"AAPL","p":2,"s":1,"t":"2026-09-21T18:00:01Z"}])", 2);
    }

    SymbolTable table;
    RecordedFeed feed(path, table);
    REQUIRE(feed.can_reset());

    const auto drain = [&] {
        std::vector<MarketEvent> out;
        MarketEvent event;
        while (feed.next(event)) {
            out.push_back(event);
        }
        return out;
    };

    const std::vector<MarketEvent> first = drain();
    feed.reset();
    const std::vector<MarketEvent> second = drain();

    REQUIRE(first.size() == 2);
    REQUIRE(first == second);

    std::filesystem::remove(path);
}

TEST_CASE("replay skew is measured before the gate, not after", "[live][recorder][skew]") {
    // The bug this locks down. Skew is arrival time minus the exchange
    // timestamp the message actually carried. The monotonic gate then raises
    // some of those timestamps, and measuring after it subtracts the feed's own
    // reordering from a number that is supposed to describe the network.
    //
    // It is not academic: the first version of this computed skew in the CLI
    // from gated timestamps, and a replay reported a mean 0.2 ms adrift of the
    // live session it was reproducing -- a divergence between two things that
    // are meant to be the same measurement.
    const std::filesystem::path path = scratch("skew_before_gate.ndjson");
    {
        RecordingHeader header;
        header.symbols = {"AAPL"};
        StreamRecorder recorder(path, header);
        // Arrival at t=10s exactly, for both. The second trade is stamped one
        // second before the first, so the gate will raise it by a second --
        // and its true skew is 2s while its gated skew would be 1s.
        recorder.record(R"([{"T":"t","S":"AAPL","p":1,"s":1,"t":"2026-09-21T18:00:09Z"}])",
                        10'000'000'000LL + 1'758'000'000'000'000'000LL);
        recorder.record(R"([{"T":"t","S":"AAPL","p":2,"s":1,"t":"2026-09-21T18:00:08Z"}])",
                        10'000'000'000LL + 1'758'000'000'000'000'000LL);
    }

    SymbolTable table;
    RecordedFeed feed(path, table);

    REQUIRE(feed.stats().skew_samples == 2);
    REQUIRE(feed.stats().reordered == 1);

    // Both events arrived at the same instant, one second apart on the
    // exchange clock, so the skews differ by exactly one second.
    REQUIRE(feed.stats().skew_max_ns - feed.stats().skew_min_ns == 1'000'000'000LL);

    // And the gate did raise the second one, which is what would have hidden
    // that second had the measurement been taken after it.
    MarketEvent first;
    MarketEvent second;
    REQUIRE(feed.next(first));
    REQUIRE(feed.next(second));
    REQUIRE(first.ts() == second.ts());

    std::filesystem::remove(path);
}

TEST_CASE("a reconnect marker is a visible gap, not a missing message", "[live][recorder]") {
    const std::filesystem::path path = scratch("reconnect.ndjson");
    {
        RecordingHeader header;
        header.symbols = {"AAPL"};
        StreamRecorder recorder(path, header);
        recorder.record(R"([{"T":"t","S":"AAPL","p":1,"s":1,"t":"2026-09-21T18:00:00Z"}])", 1);
        recorder.record_reconnect(2, 1, "connection closed by the server");
        recorder.record(R"([{"T":"t","S":"AAPL","p":2,"s":1,"t":"2026-09-21T18:00:05Z"}])", 3);
    }

    SymbolTable table;
    RecordedFeed feed(path, table);
    REQUIRE(feed.stats().reconnects == 1);
    REQUIRE(feed.stats().events == 2);
    REQUIRE(feed.stats().messages == 3);

    std::filesystem::remove(path);
}

TEST_CASE("a corrupt recording throws rather than replaying part of itself", "[live][recorder]") {
    // A replay that silently drops a line disagrees with the session it is
    // reproducing, and the equivalence test would then be comparing a strategy
    // against a truncated version of its own input.
    const std::filesystem::path path = scratch("corrupt.ndjson");
    {
        std::ofstream out(path, std::ios::binary);
        out << R"({"marketscan_recording":1,"symbols":["AAPL"]})" << '\n';
        out << R"({"seq":0,"recv":1,"raw":"[]"})" << '\n';
        out << "this line is not JSON" << '\n';
    }

    SymbolTable table;
    REQUIRE_THROWS_AS(RecordedFeed(path, table), std::runtime_error);

    std::filesystem::remove(path);
}

TEST_CASE("a file that is not a recording is refused", "[live][recorder]") {
    const std::filesystem::path path = scratch("not_a_recording.ndjson");
    {
        std::ofstream out(path, std::ios::binary);
        out << R"({"something":"else"})" << '\n';
    }

    SymbolTable table;
    REQUIRE_THROWS_AS(RecordedFeed(path, table), std::runtime_error);

    std::filesystem::remove(path);
}

TEST_CASE("a recording never contains the secret", "[live][recorder][secrets]") {
    // The auth frame is the one outgoing message that carries the key, and it
    // is the one thing a recording must not hold. The recorder only ever sees
    // incoming messages -- this asserts that the class offers no way to record
    // an outgoing one by holding it to the only API it has.
    const std::filesystem::path path = scratch("no_secret.ndjson");
    {
        RecordingHeader header;
        header.symbols = {"AAPL"};
        StreamRecorder recorder(path, header);
        recorder.record(R"([{"T":"success","msg":"authenticated"}])", 1);
    }

    std::string content;
    {
        std::ifstream in(path, std::ios::binary);
        content.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
    REQUIRE(content.find("secret") == std::string::npos);
    REQUIRE(content.find("APCA") == std::string::npos);
    REQUIRE(content.find("\"action\":\"auth\"") == std::string::npos);

    std::filesystem::remove(path);
}
