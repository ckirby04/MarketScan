// The drawdown_entry rule, from the engine's side.
//
// The research layer's half is research/tests/test_drawdown_entry.py. Both are
// held to data/samples/research/drawdown_expected.json, which the research
// layer's implementation produced and which this side must reproduce fill for
// fill -- same bar, side, quantity, price and tag, with no tolerance on any of
// them. The specification both follow is contracts/research/DRAWDOWN_ENTRY.md.
//
// The rest of this file is what only the engine can show: that every fill is
// honest by the engine's own lookahead triple, that the ledger balanced, and
// that a run is reproducible.

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include "marketscan/core/session_calendar.hpp"
#include "marketscan/data/bar_store.hpp"
#include "marketscan/research/rule_run.hpp"

using namespace marketscan;
using namespace marketscan::research;
using nlohmann::json;

namespace {

const std::filesystem::path kFixtures =
    std::filesystem::path(MARKETSCAN_SOURCE_DIR) / "data" / "samples" / "research";

json read_json(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    REQUIRE(in.good());
    return json::parse(in);
}

std::int64_t day_number(const std::string& iso) {
    return calendar::days_from_civil(std::stoll(iso.substr(0, 4)),
                                     static_cast<unsigned>(std::stoul(iso.substr(5, 2))),
                                     static_cast<unsigned>(std::stoul(iso.substr(8, 2))));
}

/// The committed price path, stamped at each day's close as a daily file is.
BarSeries load_prices() {
    std::ifstream in(kFixtures / "drawdown_prices.csv");
    REQUIRE(in.good());
    BarSeries series;
    series.ticker = "FIXTURE";
    std::string line;
    std::getline(in, line);  // header
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty()) {
            continue;
        }
        const auto comma = line.find(',');
        StoredBar bar;
        bar.ts = bar_store::daily_close_stamp(day_number(line.substr(0, comma)));
        // strtod, like Python's float(): both read the decimal string to the
        // nearest double, so the two layers see the same closes.
        bar.close = std::strtod(line.c_str() + comma + 1, nullptr);
        bar.open = bar.high = bar.low = bar.close;
        bar.volume = 1'000'000.0;
        series.bars.push_back(bar);
    }
    REQUIRE(series.size() > 500);
    return series;
}

RuleRunConfig config_for(const json& c, const BarSeries& prices) {
    RuleRunConfig config;
    config.series = prices;
    config.start_day = day_number(c.at("start").get<std::string>());
    config.starting_cash = Money::from_dollars(c.at("starting_cash").get<std::int64_t>());
    config.spread_bps = c.at("spread_bps").get<double>();
    config.commission_per_order = c.at("commission").get<double>();
    config.reserve_pct = c.at("reserve").get<double>();

    RuleParams& rule = config.rule;
    const std::string reference = c.value("reference", "all_time_high");
    rule.reference = reference == "rolling_high"      ? ReferenceKind::RollingHigh
                     : reference == "since_last_exit" ? ReferenceKind::SinceLastExit
                                                      : ReferenceKind::AllTimeHigh;
    rule.window_days = c.value("window", 252);
    rule.tranches.clear();
    for (const json& t : c.at("tranches")) {
        rule.tranches.push_back(Tranche{t[0].get<double>(), t[1].get<double>()});
    }
    rule.recover_to_high = c.value("recover_to_high", false);
    rule.take_profit_pct = c.value("take_profit", 0.0);
    rule.stop_loss_pct = c.value("stop_loss", 0.0);
    rule.trailing_stop_pct = c.value("trailing_stop", 0.0);
    rule.time_stop_days = c.value("time_stop", 0);
    rule.reentry = c.value("reentry", true);
    return config;
}

}  // namespace

TEST_CASE("the engine's drawdown_entry reproduces the research layer's fills exactly") {
    const BarSeries prices = load_prices();
    const json cases = read_json(kFixtures / "drawdown_cases.json").at("cases");
    const json expected = read_json(kFixtures / "drawdown_expected.json");
    REQUIRE(cases.size() == expected.size());

    std::size_t fills_compared = 0;
    for (const json& c : cases) {
        const std::string name = c.at("name").get<std::string>();
        INFO(name);
        const RuleRun run = run_rule(config_for(c, prices));
        REQUIRE(run.ok());

        const json& want = expected.at(name);
        CHECK(run.rejected == want.at("rejected").get<std::uint64_t>());
        // CHECK, not REQUIRE, and compare the common prefix: a REQUIRE here ends
        // the whole test case, and every case after the first disagreement goes
        // unchecked.
        CHECK(run.fills.size() == want.at("fills").size());
        const std::size_t common = std::min(run.fills.size(), want.at("fills").size());
        for (std::size_t i = 0; i < common; ++i) {
            const RuleFill& got = run.fills[i];
            const json& w = want.at("fills")[i];
            INFO("fill " << i << " on bar " << got.bar);
            CHECK(got.bar == w.at("bar").get<std::size_t>());
            CHECK((got.side == MS_SIDE_BUY ? "buy" : "sell") == w.at("side").get<std::string>());
            CHECK(got.quantity == w.at("quantity").get<double>());
            CHECK(got.price == w.at("price").get<double>());
            CHECK(got.tag == w.at("tag").get<std::uint64_t>());
            ++fills_compared;
        }
    }
    // Not vacuous: the corpus has to have produced something to agree about.
    CHECK(fills_compared > 200);
}

TEST_CASE("every drawdown_entry fill is later than everything its decision could see") {
    const BarSeries prices = load_prices();
    const json cases = read_json(kFixtures / "drawdown_cases.json").at("cases");
    for (const json& c : cases) {
        INFO(c.at("name").get<std::string>());
        const RuleRun run = run_rule(config_for(c, prices));
        REQUIRE(run.ok());
        for (const RuleFill& fill : run.fills) {
            CHECK(fill.honest);
            CHECK(fill.decision_ts < fill.price_ts);
            // And the fill priced off the bar it landed on, decided on the one
            // before.
            CHECK(fill.price_ts == prices.bars[fill.bar].ts);
            CHECK(fill.decision_ts == prices.bars[fill.bar - 1].ts);
        }
        CHECK(run.invariant_held);
    }
}

TEST_CASE("a drawdown_entry run has one equity point per bar, at that bar's close") {
    const BarSeries prices = load_prices();
    const json cases = read_json(kFixtures / "drawdown_cases.json").at("cases");
    const RuleRun run = run_rule(config_for(cases.at(0), prices));
    REQUIRE(run.ok());
    REQUIRE(run.times.size() == prices.size());
    for (std::size_t i = 0; i < prices.size(); ++i) {
        CHECK(run.times[i] == prices.bars[i].ts);
    }
    CHECK(run.equity.front() == 10'000.0);
}

TEST_CASE("the same drawdown_entry run twice gives the same fills and the same curve") {
    const BarSeries prices = load_prices();
    const json cases = read_json(kFixtures / "drawdown_cases.json").at("cases");
    const RuleRunConfig config = config_for(cases.at(4), prices);
    const RuleRun a = run_rule(config);
    const RuleRun b = run_rule(config);
    REQUIRE(a.ok());
    REQUIRE(b.ok());
    CHECK(a.equity == b.equity);
    REQUIRE(a.fills.size() == b.fills.size());
    for (std::size_t i = 0; i < a.fills.size(); ++i) {
        CHECK(a.fills[i].price == b.fills[i].price);
        CHECK(a.fills[i].quantity == b.fills[i].quantity);
    }
}

TEST_CASE("buy_and_hold is the rule with one tranche at zero and no exits") {
    const RuleParams rule = RuleParams::buy_and_hold();
    REQUIRE(rule.tranches.size() == 1);
    CHECK(rule.tranches[0].level_pct == 0.0);
    CHECK(rule.tranches[0].fraction_pct == 100.0);
    CHECK_FALSE(rule.recover_to_high);
    CHECK(rule.take_profit_pct == 0.0);
    CHECK(rule.stop_loss_pct == 0.0);
    CHECK(rule.trailing_stop_pct == 0.0);
    CHECK(rule.time_stop_days == 0);
}

TEST_CASE("a rule run refuses what it cannot run, by name") {
    RuleRunConfig empty;
    CHECK_FALSE(run_rule(empty).ok());

    RuleRunConfig too_many;
    too_many.series = load_prices();
    too_many.rule.tranches.assign(kMaxTranches + 1, Tranche{10.0, 10.0});
    const RuleRun run = run_rule(too_many);
    CHECK_FALSE(run.ok());
    CHECK(run.error.find("tranches") != std::string::npos);
}

TEST_CASE("day_of floors, so the day before the epoch is day -1") {
    constexpr Timestamp kDay = 86'400LL * 1'000'000'000LL;
    CHECK(day_of(0) == 0);
    CHECK(day_of(kDay - 1) == 0);
    CHECK(day_of(kDay) == 1);
    // Truncation would call both of these day 0.
    CHECK(day_of(-1) == -1);
    CHECK(day_of(-kDay) == -1);
    CHECK(day_of(-kDay - 1) == -2);
}

TEST_CASE("the drawdown_entry strategy is registered once the runner is linked") {
    CHECK(drawdown_entry_registered());
}
