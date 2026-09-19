// M9 -- the same strategy, both ways, made to agree.
//
// `theories/decile_parity` and `research/parity.py` implement one rule: long the
// top decile of the synthetic exposure, short the bottom, rebalanced every 21
// sessions. This replays the committed fixture through the engine and compares
// the equity curve to the one the Python reference produced from the same file.
//
// The fixture has **no churn**, and that ordering is the point of the milestone.
// The two layers model delistings differently -- the engine force-liquidates at
// the last price, the research layer compounds the departure return -- so a
// panel with churn has an expected gap, and an expected gap is exactly what a
// real bug hides behind. On a panel where nothing leaves the universe there is
// no legitimate source of disagreement, so the assertion is exact.
//
// Exact meaning exact: both sides compute equity as cash plus the marked value
// of every position, in integer micro-dollars, rounding half away from zero.
// Agreement to the micro-dollar is two implementations doing the same
// arithmetic, not a tolerance chosen to make them agree.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "marketscan/core/engine.hpp"
#include "marketscan/data/csv_feed.hpp"
#include "marketscan/exec/fill_model.hpp"
#include "marketscan/strategy/plugin_loader.hpp"
#include "marketscan/strategy/strategy_instance.hpp"

using namespace marketscan;

namespace {

[[nodiscard]] std::filesystem::path fixture(const char* name) {
    return std::filesystem::path(MARKETSCAN_TEST_DATA_DIR) / name;
}

/// The curve `research/parity.py` wrote, keyed by date.
struct Expected {
    std::vector<std::string> dates;
    std::vector<Money> equity;
    std::vector<Money> cash;
};

[[nodiscard]] Expected read_expected() {
    std::ifstream file(fixture("parity_expected.csv"));
    REQUIRE(file);

    Expected out;
    std::string line;
    std::getline(file, line);  // header
    while (std::getline(file, line)) {
        if (line.empty()) {
            continue;
        }
        std::istringstream row(line);
        std::string date;
        std::string equity;
        std::string cash;
        REQUIRE(std::getline(row, date, ','));
        REQUIRE(std::getline(row, equity, ','));
        REQUIRE(std::getline(row, cash, ','));
        out.dates.push_back(date);
        out.equity.push_back(Money::from_micros(std::strtoll(equity.c_str(), nullptr, 10)));
        out.cash.push_back(Money::from_micros(std::strtoll(cash.c_str(), nullptr, 10)));
    }
    return out;
}

/// Zero commission. The engine charges half a cent a share by default; the
/// parity assertion is about the ledger and the fill model agreeing, and a
/// commission schedule modelled twice would be a second place for them to
/// disagree about something that is not the point.
[[nodiscard]] FillModelConfig free_of_charge() {
    FillModelConfig config;
    config.commission.per_share = Money{};
    config.commission.per_order = Money{};
    config.commission.minimum = Money{};
    return config;
}

struct Sample {
    Timestamp ts = kNever;
    Money equity;
    Money cash;
};

struct Replay {
    std::vector<Sample> curve;
    Money final_equity;
    std::uint64_t events = 0;
};

/// Replays the fixture and samples equity at the end of every session.
///
/// Sampled by hand rather than through the ledger's own equity curve, because
/// that samples on an interval and what is wanted here is one point per session
/// -- the last state of the ledger before the clock moves on. Daily bars for one
/// date share a timestamp, so a change of timestamp is a change of session.
[[nodiscard]] Replay replay(const IFillModel& fill_model, bool synthesize_quotes) {
    SymbolTable symbols;
    Engine engine;

    CsvFeedOptions options;
    options.synthesize_quotes = synthesize_quotes;
    engine.add_source(std::make_unique<CsvFeed>(fixture("parity_panel.csv"), symbols, options));

    PluginLoader loader({std::filesystem::path(MARKETSCAN_PARITY_THEORY_DIR)});
    loader.rescan();

    StrategyRuntimeConfig config;
    config.starting_cash = Money::from_dollars(100'000);

    StrategyInstance theory("decile_parity",
                            "Decile Parity",
                            loader.create("decile_parity"),
                            {},
                            config,
                            &fill_model,
                            &engine.market(),
                            &symbols,
                            &engine.clock());
    engine.add_sink(&theory);
    theory.start();

    Replay out;
    Sample pending;
    while (engine.step()) {
        ++out.events;
        const Timestamp now = engine.clock().now();
        if (pending.ts != kNever && now != pending.ts) {
            out.curve.push_back(pending);
        }
        pending = Sample{now, theory.equity(), theory.cash()};
    }
    if (pending.ts != kNever) {
        out.curve.push_back(pending);
    }

    theory.stop();
    out.final_equity = theory.equity();
    return out;
}

}  // namespace

TEST_CASE("the fixture and the reference curve are the same length", "[parity]") {
    const Expected expected = read_expected();
    REQUIRE(expected.dates.size() > 100);

    const OptimisticFillModel optimistic{free_of_charge()};
    const Replay run = replay(optimistic, false);

    INFO("engine sampled " << run.curve.size() << " sessions, reference has "
                           << expected.dates.size());
    REQUIRE(run.curve.size() == expected.dates.size());
}

TEST_CASE("the engine and the research pipeline agree, exactly", "[parity][m9]") {
    // The milestone, and there is no tolerance parameter in this assertion.
    //
    // It used to allow sixteen micro-dollars. That was wrong in a way worth
    // recording: the two layers agreed exactly for 84 sessions and then differed
    // by one micro-dollar, which looked like float noise and was not. They were
    // computing decision-time equity differently -- the C++ side called the
    // engine's equity(), which marks each position at the last price its book
    // holds, and on_session_close fires while the *next* session's first bar is
    // already applied. One symbol was marked a day ahead.
    //
    // A different equity means a different share count, which means genuinely
    // different positions from that rebalance onward. Tolerating a micro-dollar
    // today is tolerating a real divergence later. docs/SIZING.md now specifies
    // the rule and both layers implement it; this asserts they landed in the
    // same place.
    const Expected expected = read_expected();
    const OptimisticFillModel optimistic{free_of_charge()};
    const Replay run = replay(optimistic, false);

    REQUIRE(run.curve.size() == expected.equity.size());

    std::size_t first = run.curve.size();
    std::size_t differing = 0;
    std::int64_t worst = 0;
    for (std::size_t i = 0; i < run.curve.size(); ++i) {
        const std::int64_t delta = run.curve[i].equity.micros() - expected.equity[i].micros();
        if (delta != 0) {
            ++differing;
            if (first == run.curve.size()) {
                first = i;
            }
            worst = std::max(worst, std::abs(delta));
        }
    }

    if (first < run.curve.size()) {
        INFO("first divergence at session " << first << " (" << expected.dates[first] << ")");
        INFO("  engine    equity " << run.curve[first].equity.micros() << "  cash "
                                   << run.curve[first].cash.micros());
        INFO("  reference equity " << expected.equity[first].micros() << "  cash "
                                   << expected.cash[first].micros());
        INFO(differing << " of " << run.curve.size() << " sessions differ, worst " << worst
                       << " micro-dollars");
    }

    CHECK(differing == 0);
    CHECK(run.final_equity == expected.equity.back());
}

TEST_CASE("crossing costs what the fill model says it costs", "[parity][m9]") {
    // Spec 2 section 8: the spread-crossing result should be worse by an amount
    // consistent with the modelled costs. It is -- but "the modelled costs" are
    // not the spread, and the first version of this test assumed they were.
    //
    // The spread is two basis points, so crossing it costs a basis point of what
    // trades. Over this fixture that is $85.84, and the research layer -- which
    // models the spread and nothing else -- measures $85.25. The engine came out
    // $1,122.29 worse, thirteen times as much, which looked like a defect.
    //
    // It is not. The engine also models **market impact**: a synthesized quote
    // shows 100 shares at the touch, and everything beyond that is charged
    //
    //     impact_bps = impact_coefficient_bps * sqrt(excess / displayed)
    //
    // at a coefficient of 10bps. The orders here have a median size of 306
    // shares, so two thirds of every one of them pays about 14bps rather than 1.
    // Working that through the fill model's own arithmetic predicts $1,136
    // against $1,122 measured, and the 1.2% residual is the two equity paths
    // diverging slightly once costs are charged and the trade sizes with them.
    //
    // So the assertion is against the derived figure, not a loose bound. The
    // research layer is frictionless by design and the engine owns execution;
    // this is the number that difference is worth on this fixture.
    const OptimisticFillModel optimistic{free_of_charge()};
    const SpreadCrossingFillModel crossing{free_of_charge()};

    // Quotes synthesized in both runs, so the only difference is the fill model.
    const Replay free_run = replay(optimistic, true);
    const Replay costed = replay(crossing, true);

    CHECK(costed.final_equity < free_run.final_equity);

    const double gap =
        static_cast<double>((free_run.final_equity - costed.final_equity).micros()) / 1'000'000.0;

    // Derived from the fixture's own trades through the fill model's formula;
    // see research/parity.py, which computes both figures.
    constexpr double kSpreadOnly = 85.84;
    constexpr double kSpreadPlusImpact = 1136.02;

    INFO("gap $" << gap << " against $" << kSpreadOnly << " of spread and $" << kSpreadPlusImpact
                 << " of spread-plus-impact");

    // Far above the spread alone, and within a few percent of the full model.
    CHECK(gap > kSpreadOnly * 5.0);
    CHECK(gap > kSpreadPlusImpact * 0.9);
    CHECK(gap < kSpreadPlusImpact * 1.1);
}
