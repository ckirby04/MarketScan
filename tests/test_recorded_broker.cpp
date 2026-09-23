/*
 * The venue side of a recording, and the correspondence assertion.
 *
 * WHY THIS FILE EXISTS
 *
 * The first regular-session equivalence run diverged, and the strategy was not
 * at fault. Live, orders were refused; in replay the fill simulator accepted
 * them. The strategy reacted correctly to both and therefore made different
 * decisions, because it was given different information.
 *
 * A recording now holds both directions, and replay hands a strategy exactly
 * what the venue handed it. These tests hold that: no network, and every message
 * hand-written to the vendor's documented shape with invented prices.
 */

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "marketscan/core/symbol_table.hpp"
#include "marketscan/live/recorded_broker.hpp"
#include "marketscan/live/recorded_feed.hpp"
#include "marketscan/live/recorder.hpp"

using namespace marketscan;
using namespace marketscan::live;
using Catch::Matchers::WithinAbs;

namespace {

std::filesystem::path scratch(const std::string& name) {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "marketscan_tests";
    std::filesystem::create_directories(dir);
    return dir / name;
}

/// Records what a strategy would have been told.
class Listener final : public IFillListener {
public:
    void on_fill(const MS_Fill& fill) override { fills.push_back(fill); }

    void on_order_status(const MS_OrderStatus& status) override {
        states.push_back(status.state);
        terminal.push_back(status.terminal != 0u);
        reasons.emplace_back(status.reason != nullptr ? status.reason : "");
    }

    std::vector<MS_Fill> fills;
    std::vector<std::uint32_t> states;
    std::vector<bool> terminal;
    std::vector<std::string> reasons;
};

MS_OrderRequest an_order(SymbolId symbol, Quantity quantity = 10.0, Price limit = 100.0) {
    MS_OrderRequest request{};
    request.symbol = symbol;
    request.side = MS_SIDE_BUY;
    request.type = MS_ORD_LIMIT;
    request.tif = MS_TIF_DAY;
    request.quantity = quantity;
    request.limit_price = limit;
    request.client_order_id = 2;
    return request;
}

constexpr OrderId kBase = 1'000'000;
constexpr const char* kRun = "testrun";

/// The client order id OrderMap derives for the Nth order of this run.
std::string client_id(OrderId sequence) {
    return std::string("ms-") + kRun + "-" + std::to_string(kBase + sequence);
}

}  // namespace

TEST_CASE("a recording carries submissions and responses", "[live][recorded][venue]") {
    const std::filesystem::path path = scratch("venue_side.ndjson");
    {
        RecordingHeader header;
        header.symbols = {"AAPL"};
        StreamRecorder recorder(path, header);

        recorder.record(R"([{"T":"q","S":"AAPL","bp":99,"bs":1,"ap":101,"as":1,)"
                        R"("t":"2026-09-23T14:00:00Z"}])",
                        1000);
        recorder.record_submission(client_id(1),
                                   kBase + 1,
                                   "AAPL",
                                   MS_SIDE_BUY,
                                   MS_ORD_LIMIT,
                                   MS_TIF_DAY,
                                   10.0,
                                   100.0,
                                   2,
                                   true,
                                   {},
                                   1100);
        recorder.record_response(R"({"stream":"trade_updates","data":{"event":"fill",)"
                                 R"("price":"100.00","qty":"10","order":{"id":"v1",)"
                                 R"("client_order_id":")" +
                                     client_id(1) +
                                     R"(","symbol":"AAPL","qty":"10","filled_qty":"10"}}})",
                                 1200);
    }

    const RecordingContents contents = read_recording_contents(path);
    REQUIRE(contents.header.version == 2);
    REQUIRE(contents.has_venue_side());
    REQUIRE(contents.messages.size() == 1);
    REQUIRE(contents.submissions.size() == 1);
    REQUIRE(contents.responses.size() == 1);

    const RecordedSubmission& submission = contents.submissions.front();
    REQUIRE(submission.client_order_id == client_id(1));
    REQUIRE(submission.engine_id == kBase + 1);
    REQUIRE(submission.symbol == "AAPL");
    REQUIRE(submission.side == MS_SIDE_BUY);
    REQUIRE(submission.type == MS_ORD_LIMIT);
    REQUIRE(submission.tif == MS_TIF_DAY);
    REQUIRE_THAT(submission.quantity, WithinAbs(10.0, 1e-12));
    REQUIRE_THAT(submission.limit_price, WithinAbs(100.0, 1e-12));
    REQUIRE(submission.strategy_client_id == 2);
    REQUIRE(submission.accepted);

    // The three kinds are interleaved in one arrival order, which is the point.
    REQUIRE(contents.messages.front().sequence == 0);
    REQUIRE(contents.submissions.front().sequence == 1);
    REQUIRE(contents.responses.front().sequence == 2);

    std::filesystem::remove(path);
}

TEST_CASE("a market-data-only recording has no venue side", "[live][recorded][venue]") {
    // This is the signal a replay uses to fall back to the fill simulator. Not a
    // flag anyone sets: a property of the file.
    const std::filesystem::path path = scratch("data_only.ndjson");
    {
        RecordingHeader header;
        header.symbols = {"AAPL"};
        StreamRecorder recorder(path, header);
        recorder.record(R"([{"T":"t","S":"AAPL","p":1,"s":1,"t":"2026-09-23T14:00:00Z"}])", 1);
    }

    const RecordingContents contents = read_recording_contents(path);
    REQUIRE_FALSE(contents.has_venue_side());
    REQUIRE(contents.messages.size() == 1);
    REQUIRE(contents.submissions.empty());
    REQUIRE(contents.responses.empty());

    std::filesystem::remove(path);
}

TEST_CASE("a version 1 recording still reads", "[live][recorded][venue]") {
    // Written before Session F2. It must not become unreadable, and it must
    // report no venue side rather than an error.
    const std::filesystem::path path = scratch("v1.ndjson");
    {
        std::ofstream out(path, std::ios::binary);
        out << R"({"marketscan_recording":1,"symbols":["AAPL"],"feed":"iex"})" << '\n';
        out << R"({"seq":0,"recv":1,"raw":"[{\"T\":\"t\",\"S\":\"AAPL\",\"p\":1,\"s\":1,)"
            << R"(\"t\":\"2026-09-23T14:00:00Z\"}]"})" << '\n';
    }

    const RecordingContents contents = read_recording_contents(path);
    REQUIRE(contents.header.version == 1);
    REQUIRE_FALSE(contents.has_venue_side());
    REQUIRE(contents.messages.size() == 1);

    std::filesystem::remove(path);
}

TEST_CASE("a recorded refusal is replayed as a refusal", "[live][recorded][venue]") {
    // The bug this whole session is about. Live, the venue refused; replay must
    // refuse too, so a strategy that reacts to a refusal reacts the same way.
    SymbolTable symbols;
    const SymbolId aapl = symbols.intern("AAPL");

    std::vector<RecordedSubmission> submissions;
    RecordedSubmission refused;
    refused.sequence = 1;
    refused.client_order_id = client_id(1);
    refused.engine_id = kBase + 1;
    refused.symbol = "AAPL";
    refused.side = MS_SIDE_BUY;
    refused.type = MS_ORD_LIMIT;
    refused.tif = MS_TIF_DAY;
    refused.quantity = 10.0;
    refused.limit_price = 100.0;
    refused.accepted = false;
    refused.reason = "insufficient buying power";
    submissions.push_back(refused);

    Listener listener;
    RecordedBroker broker(submissions, {}, &symbols, Money::from_dollars(100'000), kBase, kRun);
    broker.set_fill_listener(&listener);

    const OrderId id = broker.submit(an_order(aapl), 2000);

    REQUIRE(id == kInvalidOrderId);
    REQUIRE(broker.submissions_matched() == 1);
    REQUIRE_FALSE(broker.failure().happened);

    // Visible in the log with the venue's own words, not silently absent.
    REQUIRE(broker.orders().size() == 1);
    REQUIRE(broker.orders().front().status == OrderStatus::Rejected);
    REQUIRE(listener.reasons.size() == 1);
    REQUIRE(listener.reasons.front() == "insufficient buying power");
    REQUIRE(listener.terminal.front());
}

TEST_CASE("a recorded fill is replayed onto the right order", "[live][recorded][venue]") {
    SymbolTable symbols;
    const SymbolId aapl = symbols.intern("AAPL");

    RecordedSubmission accepted;
    accepted.sequence = 1;
    accepted.client_order_id = client_id(1);
    accepted.engine_id = kBase + 1;
    accepted.symbol = "AAPL";
    accepted.side = MS_SIDE_BUY;
    accepted.type = MS_ORD_LIMIT;
    accepted.tif = MS_TIF_DAY;
    accepted.quantity = 10.0;
    accepted.limit_price = 100.0;
    accepted.accepted = true;

    // A partial, then the rest. The path that had never run against a venue.
    RecordedResponse partial;
    partial.sequence = 2;
    partial.raw = R"({"stream":"trade_updates","data":{"event":"partial_fill","price":"100.00",)"
                  R"("qty":"4","order":{"id":"v1","client_order_id":")" +
                  client_id(1) + R"(","symbol":"AAPL","qty":"10","filled_qty":"4"}}})";

    RecordedResponse rest;
    rest.sequence = 3;
    rest.raw = R"({"stream":"trade_updates","data":{"event":"fill","price":"100.00","qty":"6",)"
               R"("order":{"id":"v1","client_order_id":")" +
               client_id(1) + R"(","symbol":"AAPL","qty":"10","filled_qty":"10"}}})";

    Listener listener;
    ReplayPosition position;
    RecordedBroker broker(
        {accepted}, {partial, rest}, &symbols, Money::from_dollars(100'000), kBase, kRun);
    broker.set_fill_listener(&listener);
    broker.set_position_source(&position);

    const OrderId id = broker.submit(an_order(aapl), 2000);
    REQUIRE(id == kBase + 1);

    // Nothing is due yet: the feed has not reached those lines.
    broker.on_market_event(aapl, 2100);
    REQUIRE(listener.fills.empty());

    position.sequence = 2;
    broker.on_market_event(aapl, 2200);
    REQUIRE(listener.fills.size() == 1);
    REQUIRE_THAT(listener.fills.front().quantity, WithinAbs(4.0, 1e-12));
    REQUIRE(broker.find_order(id)->status == OrderStatus::PartiallyFilled);

    position.sequence = 3;
    broker.on_market_event(aapl, 2300);
    REQUIRE(listener.fills.size() == 2);
    REQUIRE_THAT(listener.fills.back().quantity, WithinAbs(6.0, 1e-12));
    REQUIRE(broker.find_order(id)->status == OrderStatus::Filled);
    REQUIRE_THAT(broker.ledger().position(aapl), WithinAbs(10.0, 1e-12));
    REQUIRE(broker.responses_delivered() == 2);
}

TEST_CASE("a submission that does not correspond fails with a diff",
          "[live][recorded][correspondence]") {
    // The self-checking half. As long as the decisions agree the responses
    // correspond by construction; the moment they do not, the failure names the
    // order and the field rather than feeding the wrong answer forward.
    SymbolTable symbols;
    const SymbolId aapl = symbols.intern("AAPL");

    RecordedSubmission expected;
    expected.sequence = 1;
    expected.client_order_id = client_id(1);
    expected.engine_id = kBase + 1;
    expected.symbol = "AAPL";
    expected.side = MS_SIDE_BUY;
    expected.type = MS_ORD_LIMIT;
    expected.tif = MS_TIF_DAY;
    expected.quantity = 10.0;
    expected.limit_price = 100.0;
    expected.accepted = true;

    RecordedBroker broker({expected}, {}, &symbols, Money::from_dollars(100'000), kBase, kRun);

    // Same order, different size.
    const OrderId id = broker.submit(an_order(aapl, 45.0, 100.0), 2000);

    REQUIRE(id == kInvalidOrderId);
    REQUIRE(broker.failure().happened);
    REQUIRE(broker.failure().index == 0);
    REQUIRE(broker.failure().field == "quantity");

    const std::string diff = broker.failure().describe();
    REQUIRE(diff.find("10.0000") != std::string::npos);
    REQUIRE(diff.find("45.0000") != std::string::npos);
    REQUIRE(diff.find("recorded") != std::string::npos);
    REQUIRE(diff.find("replayed") != std::string::npos);
}

TEST_CASE("each compared field is reported by name", "[live][recorded][correspondence]") {
    SymbolTable symbols;
    const SymbolId aapl = symbols.intern("AAPL");
    const SymbolId spy = symbols.intern("SPY");

    const auto recorded = [] {
        RecordedSubmission s;
        s.client_order_id = client_id(1);
        s.engine_id = kBase + 1;
        s.symbol = "AAPL";
        s.side = MS_SIDE_BUY;
        s.type = MS_ORD_LIMIT;
        s.tif = MS_TIF_DAY;
        s.quantity = 10.0;
        s.limit_price = 100.0;
        s.accepted = true;
        return s;
    };

    struct Case {
        const char* field;
        MS_OrderRequest request;
    };

    MS_OrderRequest wrong_symbol = an_order(spy);
    MS_OrderRequest wrong_side = an_order(aapl);
    wrong_side.side = MS_SIDE_SELL;
    MS_OrderRequest wrong_type = an_order(aapl);
    wrong_type.type = MS_ORD_MARKET;
    MS_OrderRequest wrong_tif = an_order(aapl);
    wrong_tif.tif = MS_TIF_IOC;
    MS_OrderRequest wrong_price = an_order(aapl, 10.0, 101.0);

    for (const Case& c : {Case{"symbol", wrong_symbol},
                          Case{"side", wrong_side},
                          Case{"type", wrong_type},
                          Case{"time in force", wrong_tif},
                          Case{"limit price", wrong_price}}) {
        INFO(c.field);
        RecordedBroker broker(
            {recorded()}, {}, &symbols, Money::from_dollars(100'000), kBase, kRun);
        REQUIRE(broker.submit(c.request, 2000) == kInvalidOrderId);
        REQUIRE(broker.failure().happened);
        REQUIRE(broker.failure().field == c.field);
    }
}

TEST_CASE("more submissions than the recording has is a divergence",
          "[live][recorded][correspondence]") {
    // The shape the real run took: live was refused and resubmitted, replay was
    // accepted and did not. One side runs out of recorded answers, and inventing
    // one would be the worst possible response.
    SymbolTable symbols;
    const SymbolId aapl = symbols.intern("AAPL");

    RecordedSubmission only;
    only.client_order_id = client_id(1);
    only.engine_id = kBase + 1;
    only.symbol = "AAPL";
    only.side = MS_SIDE_BUY;
    only.type = MS_ORD_LIMIT;
    only.tif = MS_TIF_DAY;
    only.quantity = 10.0;
    only.limit_price = 100.0;
    only.accepted = true;

    RecordedBroker broker({only}, {}, &symbols, Money::from_dollars(100'000), kBase, kRun);

    REQUIRE(broker.submit(an_order(aapl), 2000) == kBase + 1);
    REQUIRE_FALSE(broker.failure().happened);

    REQUIRE(broker.submit(an_order(aapl), 2100) == kInvalidOrderId);
    REQUIRE(broker.failure().happened);
    REQUIRE(broker.failure().field == "count");
    REQUIRE(broker.submissions_matched() == 2);
    REQUIRE(broker.submissions_recorded() == 1);
}

TEST_CASE("a recorded replay cannot be rewound", "[live][recorded][venue]") {
    SymbolTable symbols;
    (void)symbols.intern("AAPL");
    RecordedBroker broker({}, {}, &symbols, Money::from_dollars(100'000), kBase, kRun);
    REQUIRE_THROWS_AS(broker.reset(), std::logic_error);
}

TEST_CASE("draining delivers whatever the feed never reached", "[live][recorded][venue]") {
    // A session ends by cancelling everything, and those answers arrive after
    // the last market event. Without a drain they would never be applied and the
    // replay's ledger would differ from the live one for no good reason.
    SymbolTable symbols;
    const SymbolId aapl = symbols.intern("AAPL");

    RecordedSubmission accepted;
    accepted.client_order_id = client_id(1);
    accepted.engine_id = kBase + 1;
    accepted.symbol = "AAPL";
    accepted.side = MS_SIDE_BUY;
    accepted.type = MS_ORD_LIMIT;
    accepted.tif = MS_TIF_DAY;
    accepted.quantity = 10.0;
    accepted.limit_price = 100.0;
    accepted.accepted = true;

    RecordedResponse late;
    late.sequence = 9'999;
    late.raw = R"({"stream":"trade_updates","data":{"event":"canceled","order":{"id":"v1",)"
               R"("client_order_id":")" +
               client_id(1) + R"(","symbol":"AAPL","qty":"10","filled_qty":"0"}}})";

    Listener listener;
    ReplayPosition position;
    RecordedBroker broker({accepted}, {late}, &symbols, Money::from_dollars(100'000), kBase, kRun);
    broker.set_fill_listener(&listener);
    broker.set_position_source(&position);

    const OrderId id = broker.submit(an_order(aapl), 2000);
    position.sequence = 5;
    broker.on_market_event(aapl, 2100);
    REQUIRE(broker.responses_delivered() == 0);
    REQUIRE(broker.find_order(id)->is_working());

    broker.drain_remaining(2200);
    REQUIRE(broker.responses_delivered() == 1);
    REQUIRE(broker.find_order(id)->status == OrderStatus::Cancelled);
}
