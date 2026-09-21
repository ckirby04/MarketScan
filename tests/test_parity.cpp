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
#include "marketscan/data/universe.hpp"
#include "marketscan/exec/delisting.hpp"
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

[[nodiscard]] Expected read_expected(const char* name = "parity_expected.csv") {
    std::ifstream file(fixture(name));
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

// --- The churn case -------------------------------------------------------
//
// Everything above runs on a panel nothing leaves. This part runs on one that
// names leave, which is the case Session D exists to make testable.
//
// The reason it could not be tested before is worth stating, because it is a
// trap the obvious fixture walks straight into. When the departure return IS the
// final bar -- which is what the generator used to emit -- "liquidate at the last
// price the book has" captures the loss by construction. Both layers then agree
// perfectly while being wrong in the same direction about the size of the exit
// they assumed, and the test measures the *timing* of a liquidation and nothing
// about its *magnitude*.
//
// The generator now emits the delisting return separately from the price series,
// so the two halves are separable and both are asserted:
//
//   - under `last_price` the adjustment is zero, and agreement is agreement about
//     WHEN each layer noticed the departure and at what price it liquidated;
//   - under `apply_true_return` both layers apply the same known return, and
//     agreement is additionally agreement about HOW MUCH the exit was worth;
//   - and the two runs are required to differ, by exactly the sum of the
//     adjustments, so neither passes by the switch having done nothing.

/// One membership period, as `parity_churn_membership.csv` spells it.
struct MembershipRow {
    std::string symbol;
    std::string first_session;
    std::string last_session;
};

[[nodiscard]] std::vector<MembershipRow> read_membership() {
    std::ifstream file(fixture("parity_churn_membership.csv"));
    REQUIRE(file);

    std::vector<MembershipRow> rows;
    std::string line;
    std::getline(file, line);  // header
    while (std::getline(file, line)) {
        if (line.empty()) {
            continue;
        }
        std::istringstream row(line);
        MembershipRow out;
        REQUIRE(std::getline(row, out.symbol, ','));
        REQUIRE(std::getline(row, out.first_session, ','));
        REQUIRE(std::getline(row, out.last_session, ','));
        rows.push_back(out);
    }
    return rows;
}

/// A delisting return the Python reference booked, from `parity_churn_applied.csv`.
struct AppliedRow {
    std::string date;
    std::string symbol;
    double quantity = 0.0;
    double mark = 0.0;
    double delisting_return = 0.0;
    std::int64_t micros = 0;
};

[[nodiscard]] std::vector<AppliedRow> read_applied() {
    std::ifstream file(fixture("parity_churn_applied.csv"));
    REQUIRE(file);

    std::vector<AppliedRow> rows;
    std::string line;
    std::getline(file, line);  // header
    while (std::getline(file, line)) {
        if (line.empty()) {
            continue;
        }
        std::istringstream row(line);
        std::string field;
        AppliedRow out;
        REQUIRE(std::getline(row, out.date, ','));
        REQUIRE(std::getline(row, out.symbol, ','));
        REQUIRE(std::getline(row, field, ','));
        out.quantity = std::strtod(field.c_str(), nullptr);
        REQUIRE(std::getline(row, field, ','));
        out.mark = std::strtod(field.c_str(), nullptr);
        REQUIRE(std::getline(row, field, ','));
        out.delisting_return = std::strtod(field.c_str(), nullptr);
        REQUIRE(std::getline(row, field, ','));
        out.micros = std::strtoll(field.c_str(), nullptr, 10);
        rows.push_back(out);
    }
    return rows;
}

struct ChurnReplay {
    std::vector<Sample> curve;
    Money final_equity;
    std::uint64_t forced_exits = 0;
    std::uint64_t delistings_applied = 0;
    Money delisting_pnl;
    std::vector<DelistingAdjustment> adjustments;
    std::vector<std::string> adjustment_symbols;

    std::size_t membership_periods = 0;
    std::size_t delisting_rows = 0;
    bool invariant_holds = false;

    /// The lookahead triple, tallied over the blotter.
    StrategyInstance::InstantAudit audit;
    std::vector<TradeRecord> blotter;

    /// The target book's counters, read back out of the published metrics --
    /// they are protected state on `CrossSectionalStrategy`, which is the right
    /// place for them and the wrong side of the plugin boundary.
    double targets_released = 0.0;
    double targets_abandoned = 0.0;
    double targets_outstanding = 0.0;
};

/// A metric the strategy published, or `missing` if it never did.
[[nodiscard]] double
published(const StrategyInstance& theory, std::string_view name, double missing = -1.0) {
    for (const PublishedMetric& metric : theory.metrics()) {
        if (metric.name == name) {
            return metric.value;
        }
    }
    return missing;
}

/// Replays the churn fixture with a universe installed and a delisting model
/// selected.
///
/// The universe is built from the committed membership file rather than from the
/// panel's own coverage, and the two are required to agree by
/// `research/parity.py`'s `churn_membership` -- one source, two readers. A daily
/// bar of date D is timestamped midnight UTC of D, so a period of [midnight of
/// the first session, midnight after the last) covers every bar a name printed
/// and nothing after it. The first instant it does not cover is the first event
/// of the next session, which is when `sweep_removals` fires.
[[nodiscard]] ChurnReplay replay_churn(const IFillModel& fill_model, DelistingReturnModel model) {
    SymbolTable symbols;
    Engine engine;

    Universe universe;
    const std::vector<MembershipRow> rows = read_membership();
    REQUIRE(rows.size() > 30);
    for (const MembershipRow& row : rows) {
        Timestamp from = kNever;
        Timestamp last = kNever;
        REQUIRE(time::from_iso8601(row.first_session, from));
        REQUIRE(time::from_iso8601(row.last_session, last));
        universe.add(symbols.intern(row.symbol), from, last + time::kDay);
    }
    universe.build();

    const DelistingReturnsFile table =
        load_delisting_returns(fixture("parity_churn_delistings.csv"), symbols);
    INFO("delisting table: " << table.returns.size() << " rows, " << table.rejected.size()
                             << " rejected");
    REQUIRE(table.rejected.empty());
    REQUIRE(table.returns.size() > 0);

    CsvFeedOptions options;
    options.synthesize_quotes = false;
    engine.add_source(
        std::make_unique<CsvFeed>(fixture("parity_churn_panel.csv"), symbols, options));

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
    // No SecurityMaster: this fixture's tickers are its securities, and the
    // master exists to resolve a ticker that meant two companies.
    theory.set_universe(&universe, nullptr);
    theory.set_delisting_model(model, &table.returns);
    engine.add_sink(&theory);
    theory.start();

    ChurnReplay out;
    out.membership_periods = universe.size();
    out.delisting_rows = table.returns.size();

    Sample pending;
    while (engine.step()) {
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
    out.forced_exits = theory.forced_exits();
    out.delistings_applied = theory.delistings_applied();
    out.delisting_pnl = theory.ledger().delisting_pnl();
    out.adjustments = theory.ledger().delisting_adjustments();
    out.invariant_holds = theory.ledger().check_invariant();
    out.audit = theory.audit_instants();
    out.blotter = theory.ledger().blotter();
    out.targets_released = published(theory, "targets_released");
    out.targets_abandoned = published(theory, "targets_abandoned");
    out.targets_outstanding = published(theory, "targets_outstanding");
    for (const DelistingAdjustment& adjustment : out.adjustments) {
        out.adjustment_symbols.emplace_back(symbols.c_name(adjustment.symbol));
    }
    return out;
}

/// The shared diagnostic for a curve comparison. Built whole and reported through
/// FAIL, because an INFO scope ends with the block it was declared in.
void require_identical(const std::vector<Sample>& curve,
                       const Expected& expected,
                       const char* what) {
    REQUIRE(curve.size() == expected.equity.size());

    std::size_t first = curve.size();
    std::size_t differing = 0;
    std::int64_t worst = 0;
    for (std::size_t i = 0; i < curve.size(); ++i) {
        const std::int64_t delta = curve[i].equity.micros() - expected.equity[i].micros();
        if (delta != 0) {
            ++differing;
            if (first == curve.size()) {
                first = i;
            }
            worst = std::max(worst, std::abs(delta));
        }
    }
    if (differing == 0) {
        return;
    }

    std::ostringstream message;
    message << what << ": " << differing << " of " << curve.size() << " sessions differ.\n"
            << "  first at session " << first << " (" << expected.dates[first] << ")\n"
            << "    engine    equity " << curve[first].equity.micros() << "  cash "
            << curve[first].cash.micros() << "\n"
            << "    reference equity " << expected.equity[first].micros() << "  cash "
            << expected.cash[first].micros() << "\n"
            << "    equity delta "
            << (curve[first].equity.micros() - expected.equity[first].micros()) << "  cash delta "
            << (curve[first].cash.micros() - expected.cash[first].micros()) << "\n"
            << "  worst " << worst << " micro-dollars\n"
            << "  final equity engine " << curve.back().equity.micros() << " reference "
            << expected.equity.back().micros();
    FAIL(message.str());
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

    // Reported through FAIL rather than INFO + CHECK, because an INFO scope ends
    // with the block it was declared in: the first version of this put the
    // diagnostics inside an `if` and the CHECK outside it, so CI printed
    // "1 == 0" and nothing else, and the one thing worth knowing -- which
    // session, and by how much -- was discarded before the assertion ran.
    if (differing != 0) {
        std::ostringstream message;
        message << differing << " of " << run.curve.size() << " sessions differ.\n"
                << "  first at session " << first << " (" << expected.dates[first] << ")\n"
                << "    engine    equity " << run.curve[first].equity.micros() << "  cash "
                << run.curve[first].cash.micros() << "\n"
                << "    reference equity " << expected.equity[first].micros() << "  cash "
                << expected.cash[first].micros() << "\n"
                << "    equity delta "
                << (run.curve[first].equity.micros() - expected.equity[first].micros())
                << "  cash delta "
                << (run.curve[first].cash.micros() - expected.cash[first].micros()) << "\n"
                << "  worst " << worst << " micro-dollars\n"
                << "  final equity engine " << run.final_equity.micros() << " reference "
                << expected.equity.back().micros();
        FAIL(message.str());
    }

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
    constexpr double kSpreadOnly = 84.74;
    constexpr double kSpreadPlusImpact = 1114.27;

    INFO("gap $" << gap << " against $" << kSpreadOnly << " of spread and $" << kSpreadPlusImpact
                 << " of spread-plus-impact");

    // Far above the spread alone, and within a few percent of the full model.
    CHECK(gap > kSpreadOnly * 5.0);
    CHECK(gap > kSpreadPlusImpact * 0.9);
    CHECK(gap < kSpreadPlusImpact * 1.1);
}

TEST_CASE("the churn fixture is a fixture with churn in it", "[parity][churn]") {
    // Before any parity claim: the thing being tested has to be present. A
    // membership file nobody leaves, or a delisting table whose tickers do not
    // match the feed's, would make every assertion below pass while measuring
    // nothing -- and that is the specific failure this part of Session D was
    // written to stop being reported as a result.
    const OptimisticFillModel optimistic{free_of_charge()};
    const ChurnReplay run = replay_churn(optimistic, DelistingReturnModel::ApplyTrueReturn);

    INFO("membership periods " << run.membership_periods << ", delisting rows "
                               << run.delisting_rows << ", forced exits " << run.forced_exits
                               << ", applied " << run.delistings_applied);

    // More periods than a thirty-name panel has members, which is what says names
    // were replaced rather than merely present.
    CHECK(run.membership_periods > 30);
    CHECK(run.delisting_rows > 10);

    // Positions really were held into departures, and the table really did cover
    // them. Counted separately on purpose: a run configured for
    // `apply_true_return` against a table covering none of its departures would
    // otherwise look like a universe whose delistings happened to cost nothing.
    CHECK(run.forced_exits >= 5);
    CHECK(run.delistings_applied == run.forced_exits);

    // Both signs, so neither the credit nor the debit path is untested.
    bool sold_at_a_loss = false;
    bool sold_at_a_gain = false;
    bool total_loss = false;
    for (const DelistingAdjustment& adjustment : run.adjustments) {
        sold_at_a_loss = sold_at_a_loss || adjustment.amount.micros() < 0;
        sold_at_a_gain = sold_at_a_gain || adjustment.amount.micros() > 0;
        total_loss = total_loss || adjustment.delisting_return == -1.0;
    }
    CHECK(sold_at_a_loss);
    CHECK(sold_at_a_gain);
    CHECK(total_loss);

    // And value was neither created nor destroyed getting there. The delisting
    // path moves the identical integer out of cash and into realized P&L, which is
    // the only reason this holds without the invariant being told about it.
    CHECK(run.invariant_holds);
}

TEST_CASE("under last_price the two layers agree about timing, exactly", "[parity][churn]") {
    // What this asserts, precisely: that both layers noticed each departure on the
    // same session and liquidated at the same price. The delisting return is not
    // applied by either side, so the magnitude of the assumed exit is not under
    // test here -- the next case is where that is asserted.
    //
    // This is the default model, so this curve is also the one that says no
    // committed result moved when the switch appeared.
    const Expected expected = read_expected("parity_churn_expected_last_price.csv");
    const OptimisticFillModel optimistic{free_of_charge()};
    const ChurnReplay run = replay_churn(optimistic, DelistingReturnModel::LastPrice);

    CHECK(run.forced_exits >= 5);
    CHECK(run.delistings_applied == 0);
    CHECK(run.delisting_pnl == Money{});

    require_identical(run.curve, expected, "last_price");
    CHECK(run.final_equity == expected.equity.back());
}

TEST_CASE("under apply_true_return the two layers agree about magnitude too",
          "[parity][churn][m9]") {
    // Session D Part 2's definition of done for the parity half. Both layers apply
    // the same known delisting return to the same position at the same mark, so
    // the curves have to be identical -- and there is no tolerance parameter in
    // this assertion either.
    const Expected expected = read_expected("parity_churn_expected_apply_true_return.csv");
    const OptimisticFillModel optimistic{free_of_charge()};
    const ChurnReplay run = replay_churn(optimistic, DelistingReturnModel::ApplyTrueReturn);

    require_identical(run.curve, expected, "apply_true_return");
    CHECK(run.final_equity == expected.equity.back());
}

TEST_CASE("every booked delisting return matches the reference row for row", "[parity][churn]") {
    // The curves agreeing is the assertion; this is what makes a disagreement
    // legible. A curve that parts company at session 90 says only that something
    // is wrong by then. This says which name, on which date, against what
    // position, and by how much.
    const std::vector<AppliedRow> expected = read_applied();
    const OptimisticFillModel optimistic{free_of_charge()};
    const ChurnReplay run = replay_churn(optimistic, DelistingReturnModel::ApplyTrueReturn);

    REQUIRE(!expected.empty());
    if (run.adjustments.size() != expected.size()) {
        std::ostringstream message;
        message << "the engine booked " << run.adjustments.size() << " delisting returns and the "
                << "reference booked " << expected.size() << ".\n  engine:";
        for (std::size_t i = 0; i < run.adjustment_symbols.size(); ++i) {
            message << " " << run.adjustment_symbols[i];
        }
        message << "\n  reference:";
        for (const AppliedRow& row : expected) {
            message << " " << row.symbol;
        }
        FAIL(message.str());
    }

    Money total;
    for (std::size_t i = 0; i < expected.size(); ++i) {
        const AppliedRow& want = expected[i];
        const DelistingAdjustment& got = run.adjustments[i];
        total += got.amount;

        if (run.adjustment_symbols[i] == want.symbol && got.amount.micros() == want.micros &&
            got.quantity == want.quantity && got.mark == want.mark &&
            got.delisting_return == want.delisting_return) {
            continue;
        }

        std::ostringstream message;
        message << "delisting adjustment " << i << " differs.\n"
                << "  engine    " << run.adjustment_symbols[i] << "  quantity " << got.quantity
                << "  mark " << got.mark << "  return " << got.delisting_return << "  micros "
                << got.amount.micros() << "\n"
                << "  reference " << want.symbol << " (" << want.date << ")  quantity "
                << want.quantity << "  mark " << want.mark << "  return " << want.delisting_return
                << "  micros " << want.micros;
        FAIL(message.str());
    }

    CHECK(total == run.delisting_pnl);
}

TEST_CASE("the assumed exit is worth what the difference between the models says",
          "[parity][churn]") {
    // The measurement Session D Part 2.3 asks for, as a gate rather than a number
    // in a document: the same strategy, the same panel, one assumption changed.
    //
    // This is also what stops the two parity cases above from passing by the
    // switch having done nothing at all. Two identical curves prove agreement; a
    // third fact is needed to prove agreement about something.
    const OptimisticFillModel optimistic{free_of_charge()};
    const ChurnReplay assumed = replay_churn(optimistic, DelistingReturnModel::LastPrice);
    const ChurnReplay honest = replay_churn(optimistic, DelistingReturnModel::ApplyTrueReturn);

    const Money gap = honest.final_equity - assumed.final_equity;
    INFO("last_price ends at " << assumed.final_equity.micros() << " micro-dollars, "
                               << "apply_true_return at " << honest.final_equity.micros()
                               << "; gap " << gap.micros() << ", booked "
                               << honest.delisting_pnl.micros());

    // The same departures, on the same sessions, at the same prices. Only the
    // adjustment differs.
    CHECK(assumed.forced_exits == honest.forced_exits);
    CHECK(assumed.delistings_applied == 0);
    CHECK(honest.delistings_applied == honest.forced_exits);

    // The gap is NOT the sum of the adjustments, and the difference is worth
    // stating because the first version of this test asserted that it was.
    //
    // A delisting return moves cash, decision-time equity is cash plus the marked
    // book, and per-name notional is a fraction of that. So the loss compounds:
    // every rebalance after the first booked adjustment sizes a smaller book, and
    // the two runs hold genuinely different positions from there on. The gap is the
    // adjustments plus everything the smaller book then failed to earn.
    //
    // On this fixture that second term is $95.51 against $30,008.25 of adjustments,
    // because most of the loss lands in the last month and has little time left to
    // compound. Small here, and the wrong thing to assert as small: over a longer
    // panel it is the larger of the two. Assuming a zero delisting return does not
    // merely hide the losses, it leaves a backtest compounding capital it never
    // had -- so the assertion is the inequality, not a figure.
    CHECK(honest.delisting_pnl.micros() < 0);
    CHECK(gap.micros() < 0);
    CHECK(std::abs(gap.micros()) > std::abs(honest.delisting_pnl.micros()));

    // And it is not second-order. On this fixture the assumption is worth about
    // thirty points of return on a hundred thousand dollars of starting cash,
    // which is the figure docs/PATHOLOGIES.md reports.
    CHECK(std::abs(gap.micros()) > 10'000'000'000LL);
}

TEST_CASE("the symbol that rolls each session is never in a traded leg",
          "[parity][churn][lookahead]") {
    // The property BOTH parity fixtures depend on, and which neither used to
    // state. It is not decoration: without it, exact parity is impossible for a
    // reason that has nothing to do with what either fixture is measuring.
    //
    // The engine applies a bar to its book before showing it to anybody, so that a
    // strategy and the fill model pricing its order see the same market. That is
    // right for a strategy reacting to a print. It is wrong for a cross-sectional
    // one: `advance_session` fires `on_session_close(N)` from inside the dispatch
    // of the FIRST BAR of session N+1, so at the instant a session-N decision is
    // priced, exactly one symbol's book is already a session ahead. An order in
    // that symbol is sized against session N's close and filled at N+1's.
    //
    // Measured, not argued. The churn fixture before it carried a roll symbol put
    // the short leg into SY0000, which sorts first on every session: sized against
    // a close of 148.6207 and filled at 143.216, which is $606.07 at session 1 of
    // 126 and $942 by the end. That is the one-symbol lookahead Session D Part 3
    // resolves; this asserts that neither fixture is currently exposed to it, so
    // that the delisting question and the fill-timing question stay separable.
    //
    // The churn-free fixture satisfies this **by accident** -- SY0000 sorts first
    // and never once lands in a decile, which nobody chose. The churn fixture
    // satisfies it deliberately: `ROLL` sorts before every SY.... ticker and its
    // exposure is pinned mid-rank. Either way the property is now checked.
    const OptimisticFillModel optimistic{free_of_charge()};

    SECTION("the churn-free fixture") {
        const Replay run = replay(optimistic, false);
        // The first symbol interned is the first symbol of the first session,
        // and the file is sorted by date then symbol.
        REQUIRE(!run.curve.empty());
    }

    SECTION("neither fixture trades its rolling symbol") {
        // Asserted from the blotter: the rolling symbol is the one with the
        // lowest id, because both fixtures are sorted by date then symbol and the
        // feed interns in file order.
        for (const char* panel : {"parity_panel.csv", "parity_churn_panel.csv"}) {
            SymbolTable symbols;
            Engine engine;
            CsvFeedOptions options;
            options.synthesize_quotes = false;
            engine.add_source(std::make_unique<CsvFeed>(fixture(panel), symbols, options));

            PluginLoader loader({std::filesystem::path(MARKETSCAN_PARITY_THEORY_DIR)});
            loader.rescan();

            StrategyRuntimeConfig config;
            config.starting_cash = Money::from_dollars(100'000);
            StrategyInstance theory("decile_parity",
                                    "Decile Parity",
                                    loader.create("decile_parity"),
                                    {},
                                    config,
                                    &optimistic,
                                    &engine.market(),
                                    &symbols,
                                    &engine.clock());
            engine.add_sink(&theory);
            theory.start();
            while (engine.step()) {
            }
            theory.stop();

            // Id 1 is the first ticker interned, which is the first row of the
            // file, which is the first symbol of every session.
            constexpr SymbolId kRolling = 1;
            INFO(panel << ": rolling symbol is " << symbols.c_name(kRolling) << ", "
                       << theory.ledger().blotter().size() << " fills");
            REQUIRE(!theory.ledger().blotter().empty());
            for (const TradeRecord& row : theory.ledger().blotter()) {
                if (row.symbol == kRolling) {
                    std::ostringstream message;
                    message << panel << " trades " << symbols.c_name(kRolling)
                            << ", which sorts first on every session. The engine prices its "
                            << "orders a session ahead of the decision that sized them, so "
                            << "exact parity is impossible. See research/parity.py ROLL_SYMBOL "
                            << "and Session D Part 3.";
                    FAIL(message.str());
                }
            }
        }
    }
}

TEST_CASE("the delisting model refuses a name it does not recognise", "[parity][churn]") {
    // A misspelled model silently meaning `last_price` is how a run gets measured
    // under an assumption nobody chose.
    DelistingReturnModel model = DelistingReturnModel::ApplyTrueReturn;
    CHECK(parse_delisting_return_model("last_price", model));
    CHECK(model == DelistingReturnModel::LastPrice);
    CHECK(parse_delisting_return_model("apply_true_return", model));
    CHECK(model == DelistingReturnModel::ApplyTrueReturn);

    CHECK_FALSE(parse_delisting_return_model("last-price", model));
    CHECK_FALSE(parse_delisting_return_model("", model));
    CHECK_FALSE(parse_delisting_return_model("apply_true_returns", model));
    // Unchanged by a failed parse, so a caller that ignores the result is left
    // with whatever it had rather than with a default it did not ask for.
    CHECK(model == DelistingReturnModel::ApplyTrueReturn);

    // The names are the strings research/delisting.py uses. Both layers'
    // reports have to be comparable.
    CHECK(name_of(DelistingReturnModel::LastPrice) == "last_price");
    CHECK(name_of(DelistingReturnModel::ApplyTrueReturn) == "apply_true_return");
}

TEST_CASE("a security the table says nothing about is not a delisting return of zero",
          "[parity][churn]") {
    // The distinction `DelistingReturns::has` exists to keep. A survivor has no
    // row, and writing one of zero for it would turn "nobody measured this" into
    // "this cost nothing" -- which is the same optimistic assumption this switch
    // was added to stop making silently.
    SymbolTable symbols;
    const DelistingReturnsFile table =
        load_delisting_returns(fixture("parity_churn_delistings.csv"), symbols);
    REQUIRE(table.rejected.empty());

    const SymbolId measured = table.returns.entries().front().symbol;
    CHECK(table.returns.has(measured));

    const SymbolId unmeasured = symbols.intern("NOT_IN_THE_TABLE");
    CHECK_FALSE(table.returns.has(unmeasured));
    CHECK(table.returns.find(unmeasured) == 0.0);

    // Every row in the committed table is a name that actually left, so a table
    // row of exactly zero would be suspicious rather than merely uninformative.
    for (const DelistingReturns::Entry& entry : table.returns.entries()) {
        INFO(symbols.c_name(entry.symbol) << " " << entry.delisting_return << " " << entry.cause);
        CHECK(entry.delisting_return >= -1.0);
        CHECK((entry.cause == "distress" || entry.cause == "voluntary"));
    }
}

TEST_CASE("a delisting table row that cannot be used is reported, not dropped", "[parity][churn]") {
    // A table whose tickers do not match the feed's looks exactly like a universe
    // nobody ever left, which is why the loader hands back what it could not use.
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "marketscan_bad_delistings.csv";
    {
        std::ofstream out(path);
        REQUIRE(out);
        out << "symbol,delisting_return,cause\n"
            << "GOOD,-0.5,distress\n"
            << ",-0.5,no ticker\n"
            << "NAN_ROW,not a number,bad\n"
            << "TOO_LOW,-1.5,below total loss\n"
            << "TOO_HIGH,11,implausible\n"
            << "TRAILING,-0.5x,trailing characters\n"
            << "SHORT_ROW\n";
    }

    SymbolTable symbols;
    const DelistingReturnsFile table = load_delisting_returns(path, symbols);
    std::filesystem::remove(path);

    CHECK(table.returns.size() == 1);
    CHECK(table.returns.find(symbols.find("GOOD")) == -0.5);
    CHECK(table.rejected.size() == 6);

    // A missing file is a rejection with a reason, not an empty table.
    const DelistingReturnsFile missing =
        load_delisting_returns(std::filesystem::path("no_such_delistings.csv"), symbols);
    CHECK(missing.returns.empty());
    CHECK(missing.rejected.size() == 1);
}

// --- The lookahead invariant ----------------------------------------------
//
// Session D Part 3, and the whole of it is "record, do not infer".
//
// Every fill carries three timestamps now: the information horizon of the
// decision that produced it, the market state its price came out of, and the
// fill itself. The invariant is that the price is stamped **later** than the
// horizon -- a decision taken from session N's closes cannot trade at session N's
// closes, because that is trading on information simultaneously with acting on
// it, which is free money that does not exist.
//
// WHAT IT MEASURED BEFORE THE FIX
//
// On both committed fixtures, with the triple recorded and the target book not
// yet written: **every single strategy fill was simultaneous** -- 56 of 56 on the
// churn-free panel, 55 of 55 on the churn panel, `decision_ts == price_ts` in
// every case. Not one honest fill in either run.
//
// This file's own churn-parity case is what forced the question. STATUS.md had
// the effect down as "predicted and not observed", and the reasoning was right
// but the evidence was a curve that happened to agree.

TEST_CASE("no fill uses a price its decision had already seen", "[parity][lookahead][m9]") {
    // The invariant, on both fixtures, with no tolerance and no exemption beyond
    // the one named below.
    const OptimisticFillModel optimistic{free_of_charge()};

    SECTION("the churn-free fixture") {
        SymbolTable symbols;
        Engine engine;
        CsvFeedOptions options;
        options.synthesize_quotes = false;
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
                                &optimistic,
                                &engine.market(),
                                &symbols,
                                &engine.clock());
        engine.add_sink(&theory);
        theory.start();
        while (engine.step()) {
        }
        theory.stop();

        const StrategyInstance::InstantAudit audit = theory.audit_instants();
        INFO("total " << audit.total() << " honest " << audit.honest << " simultaneous "
                      << audit.simultaneous << " stale " << audit.stale << " unrecorded "
                      << audit.unrecorded << " forced " << audit.forced_exits);

        // The run has to have traded, or the invariant holds vacuously.
        REQUIRE(audit.total() > 20);
        CHECK(audit.honest == audit.total());
        CHECK(audit.simultaneous == 0);
        CHECK(audit.stale == 0);
        CHECK(audit.unrecorded == 0);
        CHECK(audit.holds());
    }

    SECTION("the churn fixture, with forced exits in it") {
        const ChurnReplay run = replay_churn(optimistic, DelistingReturnModel::ApplyTrueReturn);
        INFO("total " << run.audit.total() << " honest " << run.audit.honest << " simultaneous "
                      << run.audit.simultaneous << " stale " << run.audit.stale << " unrecorded "
                      << run.audit.unrecorded << " forced " << run.audit.forced_exits);

        REQUIRE(run.audit.total() > 20);
        CHECK(run.audit.holds());

        // Forced exits are excluded from the invariant and are not zero here, so
        // the exclusion is load-bearing rather than theoretical. A forced exit is
        // not a decision, and the security has stopped printing -- there is no
        // later price for it to fill against. That is **the assumed exit**, named
        // and measured in its own right; see exec/delisting.hpp.
        CHECK(run.audit.forced_exits >= 5);
        CHECK(run.audit.honest + run.audit.forced_exits == run.audit.total());
    }
}

TEST_CASE("every fill is stamped with the triple", "[parity][lookahead]") {
    // Recorded, not inferred. Each row carries the three instants, so the question
    // is answered by reading the blotter rather than by reading the engine.
    const OptimisticFillModel optimistic{free_of_charge()};
    const ChurnReplay run = replay_churn(optimistic, DelistingReturnModel::LastPrice);

    REQUIRE(!run.blotter.empty());
    for (const TradeRecord& row : run.blotter) {
        INFO("order " << row.order_id << " client " << row.client_order_id << " decision "
                      << row.decision_ts << " price " << row.price_ts << " fill " << row.ts);

        // The price a fill came out of is a market state that really existed.
        CHECK(row.price_ts != kNever);

        if (row.client_order_id == kForcedExitClientId) {
            // The host's own liquidation. It prices off the last book the name
            // ever had, which is by construction not later than anything.
            CHECK(row.price_ts <= row.ts);
            continue;
        }

        CHECK(row.decision_ts != kNever);
        CHECK(row.honest_instant());

        // And the fill is never *before* the price it used. The engine fills a
        // target on the bar that released it, so these are the same instant.
        CHECK(row.ts >= row.price_ts);
        CHECK(row.price_ts == row.ts);
    }
}

TEST_CASE("a target waits for a price and is abandoned if none comes", "[parity][lookahead]") {
    // The mechanism behind the invariant, asserted where it is observable.
    //
    // A target set from session N's cross-section becomes an order when its symbol
    // next prints. A name that leaves the universe never prints again, so its
    // target expires rather than filling at a stale price -- which is the
    // behaviour that makes "abandoned" a number worth having: a strategy whose
    // targets routinely expire is one whose universe is leaving under it.
    const OptimisticFillModel optimistic{free_of_charge()};
    const ChurnReplay churning = replay_churn(optimistic, DelistingReturnModel::ApplyTrueReturn);

    // The churn fixture has 29 departures, and a departing name is in the
    // cross-section of its final session -- so it gets a target it can never
    // reach whenever that session is a rebalance one.
    INFO("released " << churning.targets_released << ", abandoned " << churning.targets_abandoned);
    CHECK(churning.targets_released > 0);
    CHECK(churning.targets_abandoned > 0);

    // Every target either released or was abandoned. Nothing is still outstanding
    // at the end of a run, because the final session close clears them.
    CHECK(churning.targets_outstanding == 0);
}
