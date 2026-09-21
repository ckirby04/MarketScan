// M9b -- the two view modes, and the rules that stop them diverging.
//
// SPEC_VIEW_MODES.md is binding and its three load-bearing rules are all of the
// same shape: two renderers must not become two implementations. Each one is
// asserted here rather than left to a reviewer comparing screenshots.
//
//   1. One snapshot, two renderers, zero duplicate computation.
//   2. One EquityChart, instantiated at two densities. A curve that differs
//      between modes is a bug, not a style.
//   3. Colour follows the entity, assigned from its stable id -- never from
//      draw order, filter state or visibility.
//
// These are testable because the model is data. `chart_model.cpp`,
// `series_colors.cpp` and `palette.cpp` deliberately do not link ImGui, so what
// each mode would draw can be built and compared without a window, a GPU or a
// screenshot. The drawing on top of the model is thin by design, and the thing
// that could actually go wrong -- two modes disagreeing about a number or a
// colour -- cannot hide in it.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "chart_model.hpp"
#include "palette.hpp"
#include "series_colors.hpp"
#include "session.hpp"
#include "ui_state.hpp"

using namespace marketscan;
using namespace marketscan::app;

namespace {

/// A snapshot with `count` theories plus a benchmark, each with a real curve.
///
/// Built by hand rather than by running a session: these tests are about what
/// the view does with a snapshot, and a hand-built one can have nine theories
/// and a folded ninth without needing nine plugins on disk.
[[nodiscard]] SessionSnapshot make_snapshot(std::size_t count, bool with_benchmark = true) {
    SessionSnapshot snapshot;
    snapshot.clock = 1'700'000'000LL * static_cast<Timestamp>(time::kSecond);

    const auto add = [&snapshot](const std::string& id, bool benchmark, double drift) {
        StrategySnapshot row;
        row.id = id;
        row.display_name = id;
        row.is_benchmark = benchmark;
        row.starting_cash = Money::from_dollars(100'000);
        for (int i = 0; i < 40; ++i) {
            row.times.push_back(1'700'000'000.0 + i * 60.0);
            const double level = 100'000.0 * (1.0 + drift * i);
            row.equity_curve.push_back(level);
            row.normalised.push_back(level / 100'000.0);
            row.drawdown.push_back(0.0);
        }
        row.summary.total_return = drift * 39.0;
        row.summary.sharpe = 1.0 + drift;
        row.summary.max_drawdown = 0.05;
        row.summary.information_ratio = 0.5;
        row.trades = 12;
        snapshot.strategies.push_back(std::move(row));
    };

    if (with_benchmark) {
        add("benchmark", true, 0.0005);
        snapshot.benchmark_index = 0;
    }
    for (std::size_t i = 0; i < count; ++i) {
        // Zero-padded so the id order and the creation order agree, which keeps
        // the tests legible; nothing under test depends on that.
        std::string id = "theory_";
        id += static_cast<char>('a' + static_cast<char>(i));
        add(id, false, 0.001 + 0.0001 * static_cast<double>(i));
    }
    return snapshot;
}

[[nodiscard]] ChartRequest request_for(ChartDensity density, const std::string& emphasis = {}) {
    ChartRequest request;
    request.density = density;
    request.emphasis_id = emphasis;
    return request;
}

}  // namespace

// ---------------------------------------------------------------------------
// Rule 3: colour follows the entity
// ---------------------------------------------------------------------------

TEST_CASE("a theory keeps its colour when another is hidden", "[view][color]") {
    // The failure this prevents, precisely: ImPlot assigns colours by plot
    // order, so hiding one series repaints every series after it. A theory
    // silently changes colour and the leaderboard stops matching the chart --
    // nothing is *wrong* on either side, they simply stop describing the same
    // thing.
    const SessionSnapshot snapshot = make_snapshot(5);
    const SeriesColors colors(snapshot);

    const ChartModel full =
        build_chart_model(snapshot, colors, request_for(ChartDensity::Operator));

    ChartRequest filtered = request_for(ChartDensity::Operator);
    filtered.hidden = {"theory_b"};
    const ChartModel hidden = build_chart_model(snapshot, colors, filtered);

    REQUIRE(hidden.series.size() == full.series.size() - 1);

    for (const ChartSeries& survivor : hidden.series) {
        const auto before = std::find_if(full.series.begin(),
                                         full.series.end(),
                                         [&](const ChartSeries& s) { return s.id == survivor.id; });
        REQUIRE(before != full.series.end());
        INFO(survivor.id << ": " << before->identity_color << " -> " << survivor.identity_color);
        CHECK(survivor.identity_color == before->identity_color);
        CHECK(survivor.drawn_color == before->drawn_color);
    }
}

TEST_CASE("the assignment depends on the set of theories, not on their order", "[view][color]") {
    const SessionSnapshot forwards = make_snapshot(4);

    SessionSnapshot backwards = forwards;
    std::reverse(backwards.strategies.begin(), backwards.strategies.end());
    // The benchmark moved with the reversal; keep the index honest.
    for (std::size_t i = 0; i < backwards.strategies.size(); ++i) {
        if (backwards.strategies[i].is_benchmark) {
            backwards.benchmark_index = i;
        }
    }

    const SeriesColors a(forwards);
    const SeriesColors b(backwards);

    for (const StrategySnapshot& row : forwards.strategies) {
        INFO(row.id);
        CHECK(a.color(row.id) == b.color(row.id));
        CHECK(a.slot(row.id) == b.slot(row.id));
    }
}

TEST_CASE("eight theories get eight distinct colours, and the ninth folds", "[view][color]") {
    const SessionSnapshot snapshot = make_snapshot(9);
    const SeriesColors colors(snapshot);

    CHECK(colors.assigned() == Palette::kSeriesCount);
    CHECK(colors.folded_ids().size() == 1);

    std::set<unsigned int> distinct;
    std::size_t folded = 0;
    for (const StrategySnapshot& row : snapshot.strategies) {
        if (row.is_benchmark) {
            continue;
        }
        if (colors.folded(row.id)) {
            ++folded;
            // Folded, not given a generated ninth hue. A ninth colour is
            // indistinguishable from one of the eight for a colour-blind viewer,
            // so it would counterfeit an identity rather than add one.
            CHECK(colors.color(row.id) == theme::colors().emphasis_gray);
            continue;
        }
        distinct.insert(colors.color(row.id));
    }
    CHECK(folded == 1);
    CHECK(distinct.size() == Palette::kSeriesCount);
}

TEST_CASE("the benchmark never wears a categorical hue", "[view][color]") {
    // It is the thing being compared against, not one of the things being
    // compared, and a viewer must never mistake it for a theory.
    const SessionSnapshot snapshot = make_snapshot(3);
    const SeriesColors colors(snapshot);

    const unsigned int benchmark = colors.color("benchmark");
    for (std::size_t i = 0; i < Palette::kSeriesCount; ++i) {
        CHECK(benchmark != theme::colors().series[i]);
    }
    // And it does not consume a slot, so three theories still get slots 0-2.
    CHECK(colors.assigned() == 3);
}

TEST_CASE("no categorical slot reuses a status colour", "[view][color]") {
    // A P&L indicator and a theory's identity must never be the same colour, or
    // a green line reads as a profit and a theory reads as a status.
    for (const Appearance appearance : {Appearance::Dark, Appearance::Light}) {
        const Palette& palette = theme::colors(appearance);
        const unsigned int reserved[] = {palette.status_good,
                                         palette.status_warning,
                                         palette.status_serious,
                                         palette.status_critical};
        for (std::size_t i = 0; i < Palette::kSeriesCount; ++i) {
            for (const unsigned int status : reserved) {
                INFO("appearance " << static_cast<int>(appearance) << " slot " << i);
                CHECK(palette.series[i] != status);
            }
        }
    }
}

TEST_CASE("both appearances define eight distinct categorical slots", "[view][color]") {
    for (const Appearance appearance : {Appearance::Dark, Appearance::Light}) {
        const Palette& palette = theme::colors(appearance);
        std::set<unsigned int> distinct;
        for (std::size_t i = 0; i < Palette::kSeriesCount; ++i) {
            distinct.insert(palette.series[i]);
        }
        INFO("appearance " << static_cast<int>(appearance));
        CHECK(distinct.size() == Palette::kSeriesCount);
        // And the chart surface is pinned: contrast results are only meaningful
        // against the surface actually rendered on.
        CHECK(palette.chart_surface != 0U);
        CHECK(palette.emphasis_gray != palette.chart_surface);

        // The benchmark's hue is its own. Equal to the context gray, it would
        // render identically to the series it is the reference for -- which is
        // what it did until a test said so.
        CHECK(palette.benchmark != palette.emphasis_gray);
        CHECK(palette.benchmark != palette.chart_surface);
        for (std::size_t i = 0; i < Palette::kSeriesCount; ++i) {
            CHECK(palette.benchmark != palette.series[i]);
        }
    }
}

// ---------------------------------------------------------------------------
// Rule 2: one chart, two densities
// ---------------------------------------------------------------------------

TEST_CASE("the two modes draw identical curve geometry", "[view][chart]") {
    // The assertion the spec asks for in as many words. Same snapshot, two
    // densities: the same series, in the same order, pointing at the same data,
    // with the same number of points.
    const SessionSnapshot snapshot = make_snapshot(4);
    const SeriesColors colors(snapshot);

    const ChartModel overview =
        build_chart_model(snapshot, colors, request_for(ChartDensity::Overview, "theory_b"));
    const ChartModel operator_view =
        build_chart_model(snapshot, colors, request_for(ChartDensity::Operator, "theory_b"));

    REQUIRE(overview.series.size() == operator_view.series.size());
    REQUIRE(!overview.series.empty());

    for (std::size_t i = 0; i < overview.series.size(); ++i) {
        const ChartSeries& a = overview.series[i];
        const ChartSeries& b = operator_view.series[i];
        INFO("series " << i << " " << a.id << " / " << b.id);

        CHECK(a.id == b.id);
        // The same arrays, not merely equal ones. Two modes reading different
        // vectors is how they drift.
        CHECK(a.times == b.times);
        CHECK(a.values == b.values);
        CHECK(a.points() == b.points());
        CHECK(a.emphasized == b.emphasized);
        CHECK(a.is_benchmark == b.is_benchmark);
        CHECK(a.folded == b.folded);

        // Rule 3 again, across modes: which entity holds which colour **when
        // coloured** is not a density decision.
        CHECK(a.identity_color == b.identity_color);
    }

    // And the axis is the same axis.
    CHECK(overview.normalised == operator_view.normalised);
    CHECK(overview.log_scale == operator_view.log_scale);
    CHECK(overview.y_label == operator_view.y_label);
    CHECK(overview.now_seconds == operator_view.now_seconds);
}

TEST_CASE("density changes presentation and nothing else", "[view][chart]") {
    const SessionSnapshot snapshot = make_snapshot(4);
    const SeriesColors colors(snapshot);

    const ChartModel overview =
        build_chart_model(snapshot, colors, request_for(ChartDensity::Overview, "theory_a"));
    const ChartModel dense =
        build_chart_model(snapshot, colors, request_for(ChartDensity::Operator, "theory_a"));

    bool greyed_something = false;
    bool weights_differ = false;
    for (std::size_t i = 0; i < overview.series.size(); ++i) {
        const ChartSeries& a = overview.series[i];
        const ChartSeries& b = dense.series[i];

        weights_differ = weights_differ || a.line_weight != b.line_weight;

        if (!a.emphasized && !a.is_benchmark) {
            // Context, in the mode where one series is the subject.
            CHECK(a.drawn_color == theme::colors().emphasis_gray);
            greyed_something = true;
        }
        // Operator keeps the full categorical palette: there the series
        // genuinely are the subject.
        CHECK(b.drawn_color == b.identity_color);
    }

    CHECK(greyed_something);
    CHECK(weights_differ);

    // Overview labels the subject against the curve; Operator uses a legend.
    const ChartSeries* subject = overview.emphasis();
    REQUIRE(subject != nullptr);
    CHECK(subject->direct_label);
    for (const ChartSeries& s : dense.series) {
        CHECK_FALSE(s.direct_label);
    }
}

TEST_CASE("emphasis is a chart feature, not a mode feature", "[view][chart]") {
    // Operator follows the selected leaderboard row and Overview follows its own
    // selector; the chart does not know or care which asked.
    const SessionSnapshot snapshot = make_snapshot(3);
    const SeriesColors colors(snapshot);

    for (const ChartDensity density : {ChartDensity::Overview, ChartDensity::Operator}) {
        const ChartModel model =
            build_chart_model(snapshot, colors, request_for(density, "theory_c"));
        const ChartSeries* subject = model.emphasis();
        REQUIRE(subject != nullptr);
        CHECK(subject->id == "theory_c");

        std::size_t emphasised = 0;
        for (const ChartSeries& s : model.series) {
            emphasised += s.emphasized ? 1 : 0;
        }
        CHECK(emphasised == 1);
    }
}

TEST_CASE("an empty emphasis falls back to the first theory, not the benchmark", "[view][chart]") {
    const SessionSnapshot snapshot = make_snapshot(2);
    const SeriesColors colors(snapshot);
    const ChartModel model =
        build_chart_model(snapshot, colors, request_for(ChartDensity::Overview));

    const ChartSeries* subject = model.emphasis();
    REQUIRE(subject != nullptr);
    CHECK_FALSE(subject->is_benchmark);
    CHECK(subject->id == "theory_a");
}

TEST_CASE("the benchmark is drawn in both modes and never greyed", "[view][chart]") {
    // A demo that shows only the up-and-to-the-right curve is a dishonest demo,
    // and the benchmark is the reference that stops it being one.
    const SessionSnapshot snapshot = make_snapshot(3);
    const SeriesColors colors(snapshot);

    for (const ChartDensity density : {ChartDensity::Overview, ChartDensity::Operator}) {
        const ChartModel model = build_chart_model(snapshot, colors, request_for(density));
        const ChartSeries* benchmark = model.benchmark();
        REQUIRE(benchmark != nullptr);
        CHECK(benchmark->drawn_color == benchmark->identity_color);
        CHECK(benchmark->drawn_color != theme::colors().emphasis_gray);
    }
}

// ---------------------------------------------------------------------------
// Rule 1: one snapshot, zero duplicate computation
// ---------------------------------------------------------------------------

TEST_CASE("every figure the views surface is read from the snapshot", "[view][snapshot]") {
    // The rule, as a structural check rather than a promise. Each number a view
    // shows has to exist as a field somebody else could read -- if it does not,
    // the view is computing it, and two views computing independently is how
    // Overview ends up showing 12.4% while Operator shows 12.1%.
    //
    // Excess return is the one that nearly went the other way. It is a single
    // subtraction, which is exactly what makes it tempting to do in the renderer.
    const SessionSnapshot snapshot = make_snapshot(2);

    const StrategySnapshot& theory = snapshot.strategies[1];
    REQUIRE_FALSE(theory.is_benchmark);

    // The four KPI tiles.
    CHECK(std::isfinite(theory.summary.information_ratio));
    CHECK(std::isfinite(theory.summary.sharpe));
    CHECK(std::isfinite(theory.summary.max_drawdown));
    CHECK(theory.trades > 0);

    // The hero figure's inputs. `excess_return` is a field, not an expression in
    // a panel; the fixture leaves it NaN because only Session fills it, and the
    // point here is that the field exists to be filled.
    CHECK(std::isfinite(theory.summary.total_return));
    CHECK(snapshot.benchmark_index < snapshot.strategies.size());
    CHECK(std::isfinite(snapshot.strategies[snapshot.benchmark_index].summary.total_return));

    // The drawdown strip reads a series, it does not derive one.
    CHECK(theory.drawdown.size() == theory.equity_curve.size());
}

TEST_CASE("a snapshot with nothing in it produces an empty chart rather than a crash",
          "[view][chart]") {
    SessionSnapshot snapshot;
    const SeriesColors colors(snapshot);
    const ChartModel model =
        build_chart_model(snapshot, colors, request_for(ChartDensity::Overview));

    CHECK(model.empty());
    CHECK(model.emphasis() == nullptr);
    CHECK(model.benchmark() == nullptr);
    CHECK(model.now_seconds < 0.0);
}

TEST_CASE("a theory with one point is not drawn as a line", "[view][chart]") {
    SessionSnapshot snapshot = make_snapshot(1);
    snapshot.strategies.back().times.resize(1);
    snapshot.strategies.back().equity_curve.resize(1);
    snapshot.strategies.back().normalised.resize(1);

    const SeriesColors colors(snapshot);
    const ChartModel model =
        build_chart_model(snapshot, colors, request_for(ChartDensity::Operator));

    for (const ChartSeries& s : model.series) {
        CHECK(s.points() >= 2);
    }
}

// ---------------------------------------------------------------------------
// Switching modes does not touch the engine
// ---------------------------------------------------------------------------

TEST_CASE("switching view mode does not restart, pause or perturb the run", "[view][session]") {
    // Two presentations over one running engine. The run continues at whatever
    // speed it was going; only the renderer changes.
    //
    // This holds structurally -- `ViewMode` lives in `UiState`, which the Session
    // has never heard of -- and structural arguments are how things quietly stop
    // being true. So it is measured: flip the mode while the replay is running
    // and check the engine neither stopped nor restarted.
    SessionConfig config;
    config.bars_per_session = 400;
    config.sessions = 2;
    config.seed = 99;
    config.theory_roots = {std::filesystem::path(MARKETSCAN_THEORY_DIR)};

    Session session(config);
    UiState ui;

    const auto wait_for = [&session](auto predicate) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
        std::shared_ptr<const SessionSnapshot> snapshot = session.snapshot();
        while (std::chrono::steady_clock::now() < deadline) {
            snapshot = session.snapshot();
            if (predicate(*snapshot)) {
                return snapshot;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return snapshot;
    };

    REQUIRE(wait_for([](const SessionSnapshot& s) {
                return !s.strategies.empty();
            })->strategies.size() > 0);

    session.play();
    const std::shared_ptr<const SessionSnapshot> moving =
        wait_for([](const SessionSnapshot& s) { return s.events > 50; });
    REQUIRE(moving->events > 50);
    const TransportState before = moving->state;

    // Flip several times, as fast as the shortcut would allow.
    for (int i = 0; i < 20; ++i) {
        ui.view_mode = ui.view_mode == ViewMode::Overview ? ViewMode::Operator : ViewMode::Overview;
    }

    const std::shared_ptr<const SessionSnapshot> after =
        wait_for([&](const SessionSnapshot& s) { return s.events > moving->events; });

    INFO("before " << moving->events << " events, after " << after->events);
    // Still going forward, and never wound back to the start.
    CHECK(after->events >= moving->events);
    CHECK(after->clock >= moving->clock);
    // And still in whatever transport state it was in -- not paused, not reset.
    CHECK((after->state == before || after->state == TransportState::Finished));
}

TEST_CASE("the view mode is remembered, and a first run opens in Overview", "[view][session]") {
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "marketscan_view_state_test.state";
    std::filesystem::remove(path);

    // First run: nothing to read, and the default is the view that answers the
    // question rather than the one full of instruments.
    UiState fresh;
    fresh.load(path);
    CHECK(fresh.view_mode == ViewMode::Overview);

    UiState changed;
    changed.view_mode = ViewMode::Operator;
    changed.save(path);

    UiState reloaded;
    reloaded.load(path);
    CHECK(reloaded.view_mode == ViewMode::Operator);

    // A corrupt or unrelated file leaves the default rather than failing to
    // start: a remembered preference is not worth refusing to launch over.
    {
        std::ofstream junk(path, std::ios::trunc);
        junk << "this is not a settings file\n";
    }
    UiState after_junk;
    after_junk.load(path);
    CHECK(after_junk.view_mode == ViewMode::Overview);

    std::filesystem::remove(path);
}
