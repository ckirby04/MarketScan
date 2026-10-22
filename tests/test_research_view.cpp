// The Research view, from the half of the application that has no window.
//
// SPEC_RESEARCH_VIEW.md's rules for the view, each asserted rather than left to
// a reviewer comparing screenshots:
//
//   1. The view computes no number. Everything it draws comes from
//      `ResearchView`, and every field of that is read straight off the
//      Results document -- checked here field by field.
//   2. There is still one equity chart. The research builder and the session
//      builder produce the same model for the same curves: same geometry, same
//      colours, same presentation.
//   3. The form editor's decisions about the schema round-trip: the default
//      for every oneOf branch is recognised as that branch.
//   4. The session runs a job off the GUI thread, writes the run to disk, and
//      reopening it shows the same Results.

#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include "chart_model.hpp"
#include "marketscan/research/job.hpp"
#include "marketscan/research/json_schema.hpp"
#include "plain_words.hpp"
#include "research_session.hpp"
#include "research_view.hpp"
#include "schema_form.hpp"
#include "series_colors.hpp"
#include "ui_state.hpp"

using namespace marketscan;
using namespace marketscan::app;
using nlohmann::json;

namespace {

const std::filesystem::path kRepo = MARKETSCAN_SOURCE_DIR;
const std::filesystem::path kFixtures = kRepo / "data" / "samples" / "research";

json read_json(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    REQUIRE(in.good());
    return json::parse(in);
}

/// NaN for null, the number otherwise: the only reading research_view does.
bool same(double field, const json& value) {
    if (value.is_null()) {
        return std::isnan(field);
    }
    return field == value.get<double>();
}

std::vector<research::JobCurve> fixture_curves() {
    research::MemoryBarSource data;
    data.add(research::load_close_csv(kFixtures / "drawdown_prices.csv", "FIXTURE"));
    research::JobOptions options;
    options.null_draws = 20;
    options.bootstrap_paths = 2;
    auto outcome = research::run_job(read_json(kFixtures / "job_form.json"), data, options);
    REQUIRE(outcome.ok());
    return outcome.curves;
}

}  // namespace

TEST_CASE("every field of the research view is read straight off the Results") {
    const json r = read_json(kFixtures / "job_results.json");
    const ResearchView v = read_results(r);
    const json& verdict = r["verdict"];

    CHECK(v.grade == verdict["grade"].get<std::string>());
    CHECK(v.data_grade == verdict["data_grade"].get<std::string>());
    CHECK(v.deciding_gate == (verdict["deciding_gate"].is_null()
                                  ? std::string()
                                  : verdict["deciding_gate"].get<std::string>()));
    CHECK(same(v.strategy_oos_cagr_pct, verdict["headline"]["strategy_oos_cagr_pct"]));
    CHECK(same(v.benchmark_oos_cagr_pct, verdict["headline"]["benchmark_oos_cagr_pct"]));
    CHECK(same(v.strategy_oos_max_drawdown_pct,
               verdict["headline"]["strategy_oos_max_drawdown_pct"]));
    CHECK(same(v.benchmark_oos_max_drawdown_pct,
               verdict["headline"]["benchmark_oos_max_drawdown_pct"]));
    CHECK(same(v.range_p5, verdict["headline"]["outcome_range"]["cagr_p5_pct"]));
    CHECK(same(v.range_p50, verdict["headline"]["outcome_range"]["cagr_p50_pct"]));
    CHECK(same(v.range_p95, verdict["headline"]["outcome_range"]["cagr_p95_pct"]));

    REQUIRE(v.gates.size() == verdict["gates"].size());
    for (std::size_t i = 0; i < v.gates.size(); ++i) {
        CHECK(v.gates[i].id == verdict["gates"][i]["id"].get<std::string>());
        CHECK(v.gates[i].status == verdict["gates"][i]["status"].get<std::string>());
        CHECK(v.gates[i].summary == verdict["gates"][i]["summary"].get<std::string>());
    }
    REQUIRE(v.warnings.size() == r["warnings"].size());

    const json& stats = r["stats"];
    for (const WindowView& w : v.windows) {
        const json& m = stats["selected"]["windows"][w.name]["metrics"];
        INFO(w.name);
        CHECK(same(w.metrics.total_return_pct, m["total_return_pct"]));
        CHECK(same(w.metrics.cagr_pct, m["cagr_pct"]));
        CHECK(same(w.metrics.volatility_pct, m["volatility_pct"]));
        CHECK(same(w.metrics.sharpe, m["sharpe"]));
        CHECK(same(w.metrics.sortino, m["sortino"]));
        CHECK(same(w.metrics.max_drawdown_pct, m["max_drawdown_pct"]));
        CHECK(same(w.metrics.max_drawdown_days, m["max_drawdown_days"]));
        CHECK(same(w.metrics.calmar, m["calmar"]));
        CHECK(same(w.metrics.ulcer_index, m["ulcer_index"]));
        CHECK(same(w.metrics.time_in_market_pct, m["time_in_market_pct"]));
        CHECK(w.metrics.n_entries == m["n_entries"].get<long long>());
        CHECK(same(w.metrics.win_rate_pct, m["win_rate_pct"]));
        CHECK(same(w.metrics.avg_trade_return_pct, m["avg_trade_return_pct"]));
        CHECK(same(w.metrics.worst_trade_pct, m["worst_trade_pct"]));
    }
    CHECK(v.windows.size() == stats["selected"]["windows"].size());

    REQUIRE(v.trades.size() == stats["trades"].size());
    for (std::size_t i = 0; i < v.trades.size(); ++i) {
        CHECK(v.trades[i].return_pct == stats["trades"][i]["return_pct"].get<double>());
        CHECK(v.trades[i].entry_price == stats["trades"][i]["entry_price"].get<double>());
    }

    REQUIRE(v.has_surface);
    const json& surface = stats["parameter_surface"];
    CHECK(v.surface.selected_index == surface["selected_index"].get<std::size_t>());
    CHECK(same(v.surface.plateau_ratio, surface["plateau_ratio"]));
    CHECK(same(v.surface.effective_n, surface["effective_n"]));
    REQUIRE(v.surface.points.size() == surface["points"].size());
    for (std::size_t i = 0; i < v.surface.points.size(); ++i) {
        CHECK(same(v.surface.points[i].in_sample_metric, surface["points"][i]["in_sample_metric"]));
        CHECK(v.surface.points[i].excluded == surface["points"][i]["excluded"].get<bool>());
    }

    const json& timing = stats["random_timing"];
    CHECK(same(v.timing_percentile, timing["strategy_percentile"]));
    CHECK(same(v.timing_p_value, timing["p_value"]));
    CHECK(same(v.timing_p_corrected, timing["p_value_corrected"]));
    CHECK(same(v.timing_p50, timing["null_p50"]));

    const json& boot = stats["bootstrap"];
    CHECK(same(v.bootstrap_strategy[1], boot["strategy"]["cagr_p50_pct"]));
    CHECK(same(v.bootstrap_benchmark[2], boot["benchmark"]["cagr_p95_pct"]));
    CHECK(same(v.bootstrap_prob_beats, boot["prob_beats_benchmark"]));
    CHECK(same(v.bootstrap_share_ruined, boot["share_paths_below_minus_90_pct"]));

    REQUIRE(v.has_scorecard);
    const json& card = r["scorecard"];
    CHECK(v.scorecard.window == card["window"].get<std::string>());
    CHECK(same(v.scorecard.overall_score, card["overall"]["score"]));
    CHECK(v.scorecard.overall_letter == card["overall"]["letter"].get<std::string>());
    REQUIRE(v.scorecard.subgrades.size() == card["subgrades"].size());
    for (std::size_t i = 0; i < v.scorecard.subgrades.size(); ++i) {
        const SubgradeView& sub = v.scorecard.subgrades[i];
        const json& doc = card["subgrades"][i];
        INFO(sub.id);
        CHECK(sub.id == doc["id"].get<std::string>());
        CHECK(sub.status == doc["status"].get<std::string>());
        CHECK(same(sub.score, doc["score"]));
        CHECK(sub.letter == doc["letter"].get<std::string>());
        CHECK(sub.numbers.size() + sub.words.size() == doc["measured"].size());
        for (const auto& [key, value] : sub.numbers) {
            CHECK(same(value, doc["measured"][key]));
        }
        for (const auto& [key, value] : sub.words) {
            CHECK(value == doc["measured"][key].get<std::string>());
        }
    }
}

TEST_CASE("Results from before the scorecard still read, without one") {
    json r = read_json(kFixtures / "job_results.json");
    r.erase("scorecard");
    CHECK_FALSE(read_results(r).has_scorecard);
}

TEST_CASE("plain words cover every identifier the Results can carry") {
    // A gate, warning or sub-grade added to the contract without words would
    // reach the screen as a schema name, in both apps. This keeps them in step.
    const json schema = read_json(kRepo / "contracts" / "research" / "results.schema.json");
    const json words = read_json(kRepo / "contracts" / "research" / "plain_words.json");
    const PlainWords plain(words);  // and the file has the shape the reader expects

    for (const json& id : schema["$defs"]["gate_id"]["enum"]) {
        INFO(id);
        CHECK(words["gates"].contains(id.get<std::string>()));
    }
    const json& codes = schema["properties"]["warnings"]["items"]["properties"]["code"]["enum"];
    REQUIRE_FALSE(codes.empty());
    for (const json& code : codes) {
        INFO(code);
        CHECK(words["warnings"].contains(code.get<std::string>()));
    }
    const json& subgrades = schema["properties"]["scorecard"]["properties"]["subgrades"]["items"]
                                  ["properties"]["id"]["enum"];
    REQUIRE_FALSE(subgrades.empty());
    for (const json& id : subgrades) {
        INFO(id);
        CHECK(words["subgrades"].contains(id.get<std::string>()));
    }
}

TEST_CASE("plain words: lookups, fallbacks, and values with their units") {
    const PlainWords plain =
        PlainWords::load(kRepo / "contracts" / "research" / "plain_words.json");
    CHECK(plain.gate_name("oos_benchmark") == "Held up against holding");
    CHECK_FALSE(plain.gate_asks("evidence").empty());
    CHECK(plain.param_label("/strategy/entry/threshold_pct") == "Drop from the high");
    CHECK(plain.param_value("/strategy/entry/threshold_pct", 20.0) == "20%");
    CHECK(plain.param_value("/strategy/entry/threshold_pct", 2.5) == "2.5%");
    CHECK(plain.param_value("/strategy/reference/window_days", 252.0) == "252 days");
    CHECK(plain.objective("calmar") == "best annual return for its worst drop");
    CHECK(plain.subgrade_name("risk_adjusted") == "Risk-adjusted");

    // Unknown identifiers fall back exactly as the web app's do.
    CHECK(plain.gate_name("new_gate") == "new gate");
    CHECK(plain.warning("NEW_CODE", "the engine's words") == "the engine's words");
    CHECK(plain.param_value("/x/size_pct", 10.0) == "10%");
    CHECK(plain.param_value("/x/count", 3.0) == "3");
    const PlainWords empty;
    CHECK(empty.subgrade_name("versus_holding") == "versus holding");

    CHECK(plain_number(20.0) == "20");
    CHECK(plain_number(0.12345) == "0.1235");
    CHECK(plain_number(-0.00001) == "0");
}

TEST_CASE("the view never calls a grade a good idea") {
    for (const char* grade : {"supported", "fragile", "not_supported", "insufficient_evidence"}) {
        const std::string words = grade_words(grade);
        CHECK(words.find("GOOD") == std::string::npos);
        CHECK(words.find("BUY") == std::string::npos);
    }
}

TEST_CASE("the research chart and the session chart are one chart") {
    const std::vector<research::JobCurve> curves = fixture_curves();
    REQUIRE(curves.size() == 2);

    // The same curves as a session would publish them.
    SessionSnapshot snapshot;
    for (const research::JobCurve& curve : curves) {
        StrategySnapshot row;
        row.id = curve.id;
        row.display_name = curve.label;
        row.is_benchmark = curve.is_benchmark;
        row.times = curve.times;
        row.equity_curve = curve.equity;
        row.normalised = curve.normalised;
        row.drawdown = curve.drawdown;
        snapshot.strategies.push_back(std::move(row));
    }

    for (const ChartDensity density : {ChartDensity::Overview, ChartDensity::Operator}) {
        ChartRequest request;
        request.density = density;
        const ChartModel from_session =
            build_chart_model(snapshot, SeriesColors(snapshot), request);
        const ChartModel from_research = build_chart_model(curves, colors_for(curves), request);
        REQUIRE(from_session.series.size() == from_research.series.size());
        for (std::size_t i = 0; i < from_session.series.size(); ++i) {
            const ChartSeries& a = from_session.series[i];
            const ChartSeries& b = from_research.series[i];
            INFO(a.id);
            CHECK(a.id == b.id);
            CHECK(a.label == b.label);
            CHECK(a.identity_color == b.identity_color);
            CHECK(a.drawn_color == b.drawn_color);
            CHECK(a.line_weight == b.line_weight);
            CHECK(a.emphasized == b.emphasized);
            CHECK(a.is_benchmark == b.is_benchmark);
            CHECK(a.direct_label == b.direct_label);
            CHECK(*a.times == *b.times);
            CHECK(*a.values == *b.values);
            CHECK(*a.drawdown == *b.drawdown);
        }
        CHECK(from_session.y_label == from_research.y_label);
    }
}

TEST_CASE("research curves carry their own drawdown, not one the view derives") {
    for (const research::JobCurve& curve : fixture_curves()) {
        REQUIRE(curve.drawdown.size() == curve.equity.size());
        CHECK(curve.normalised.front() == 1.0);
        CHECK(curve.drawdown.front() == 0.0);
        for (const double d : curve.drawdown) {
            CHECK(d <= 0.0);
        }
    }
}

TEST_CASE("a colour assignment from a plain entity list matches one from a snapshot") {
    SessionSnapshot snapshot;
    for (const char* id : {"zeta", "alpha", "bench", "mid"}) {
        StrategySnapshot row;
        row.id = id;
        row.is_benchmark = std::string(id) == "bench";
        snapshot.strategies.push_back(row);
    }
    const SeriesColors a(snapshot);
    const SeriesColors b(std::vector<std::pair<std::string, bool>>{
        {"zeta", false}, {"alpha", false}, {"bench", true}, {"mid", false}});
    for (const char* id : {"zeta", "alpha", "bench", "mid"}) {
        CHECK(a.color(id) == b.color(id));
        CHECK(a.slot(id) == b.slot(id));
    }
}

TEST_CASE("the form editor recognises every oneOf branch's default as that branch") {
    const json root = read_json(kRepo / "contracts" / "research" / "strategy_form.schema.json");
    std::size_t checked = 0;
    std::function<void(const json&)> walk = [&](const json& node) {
        if (node.is_object()) {
            if (node.contains("oneOf")) {
                const json& branches = node.at("oneOf");
                for (std::size_t i = 0; i < branches.size(); ++i) {
                    const json value = schema_form::default_for(root, branches[i]);
                    INFO(branches[i].dump());
                    CHECK(schema_form::branch_of(root, branches, value) == i);
                    ++checked;
                }
            }
            for (const auto& [key, child] : node.items()) {
                (void)key;
                walk(child);
            }
        } else if (node.is_array()) {
            for (const json& child : node) {
                walk(child);
            }
        }
    };
    walk(root);
    CHECK(checked > 10);
}

TEST_CASE("the form editor's defaults satisfy the schema where the schema can be satisfied") {
    const json root = read_json(kRepo / "contracts" / "research" / "strategy_form.schema.json");
    const research::JsonSchema param =
        research::JsonSchema::compile(json{{"$defs", root["$defs"]}, {"$ref", "#/$defs/param"}});
    for (const json& branch : root["$defs"]["param"]["oneOf"]) {
        CHECK(param.is_valid(schema_form::default_for(root, branch)));
    }
}

TEST_CASE("form labels and property order") {
    CHECK(schema_form::label_for("threshold_pct") == "threshold %");
    CHECK(schema_form::label_for("window_days") == "window days");
    CHECK(schema_form::pointer_token("/strategy/x") == "~1strategy~1x");

    const json root = read_json(kRepo / "contracts" / "research" / "strategy_form.schema.json");
    const auto order = schema_form::property_order(root["properties"]["date_range"]);
    REQUIRE(order.size() == 3);
    CHECK(order[0] == "start");
    CHECK(order[1] == "end");
    CHECK(order[2] == "warmup_days");

    const json& reference = root["$defs"]["drawdown_reference"]["oneOf"];
    CHECK(schema_form::branch_label(root, reference[0]) == "all_time_high");
    CHECK(schema_form::branch_label(root, reference[1]) == "rolling_high");
}

TEST_CASE("the research session runs a form off the GUI thread and can reopen the run") {
    const auto runs = std::filesystem::temp_directory_path() / "ms_research_view_runs";
    std::filesystem::remove_all(runs);

    ResearchConfig config;
    config.contracts_dir = kRepo / "contracts" / "research";
    config.runs_dir = runs;
    config.csv_sources = {{"FIXTURE", kFixtures / "drawdown_prices.csv"}};
    config.null_draws = 20;
    config.bootstrap_paths = 3;
    ResearchSession session(config);

    const auto first = session.snapshot();
    CHECK(first->setup_error.empty());
    CHECK_FALSE(first->assistant_available);
    CHECK_FALSE(first->assistant_reason.empty());
    CHECK(first->form_errors.empty());  // the starter form is valid

    session.set_form(read_json(kFixtures / "job_form.json"));
    session.run();
    session.wait_idle();

    const auto done = session.snapshot();
    REQUIRE(done->state == ResearchState::Done);
    REQUIRE(done->run);
    CHECK(done->run->results["verdict"]["grade"].is_string());
    REQUIRE(done->recent.size() == 1);
    REQUIRE(std::filesystem::exists(done->run->directory / "results.json"));

    session.open(done->run->directory);
    const auto reopened = session.snapshot();
    REQUIRE(reopened->run);
    CHECK(reopened->run->results == done->run->results);
    CHECK(reopened->run->curves.size() == done->run->curves.size());
    CHECK(reopened->run->curves.front().equity == done->run->curves.front().equity);

    std::filesystem::remove_all(runs);
}

TEST_CASE("a damaged run directory is not opened, and the snapshot says why") {
    const auto runs = std::filesystem::temp_directory_path() / "ms_research_view_damaged";
    std::filesystem::remove_all(runs);
    const auto broken = runs / "broken";
    std::filesystem::create_directories(broken);
    std::ofstream(broken / "form.json") << read_json(kFixtures / "job_form.json").dump();
    std::ofstream(broken / "results.json") << R"({"results_version": "0.1"})";
    std::ofstream(broken / "curves.json") << "[]";

    ResearchConfig config;
    config.contracts_dir = kRepo / "contracts" / "research";
    config.runs_dir = runs;
    config.csv_sources = {{"FIXTURE", kFixtures / "drawdown_prices.csv"}};
    ResearchSession session(config);
    session.open(broken);
    const auto snap = session.snapshot();
    CHECK(snap->state == ResearchState::Editing);
    CHECK_FALSE(snap->run);
    CHECK(snap->open_error.find("results.schema.json") != std::string::npos);
    std::filesystem::remove_all(runs);
}

TEST_CASE("an invalid form does not run, and a refused one says why by path") {
    ResearchConfig config;
    config.contracts_dir = kRepo / "contracts" / "research";
    config.runs_dir = std::filesystem::temp_directory_path() / "ms_research_view_refused";
    config.csv_sources = {{"FIXTURE", kFixtures / "drawdown_prices.csv"}};
    config.null_draws = 5;
    config.bootstrap_paths = 1;
    ResearchSession session(config);

    json form = read_json(kFixtures / "job_form.json");
    form["execution"]["fill"] = "same_close";  // not in the schema
    session.set_form(form);
    CHECK_FALSE(session.snapshot()->form_errors.empty());
    session.run();
    session.wait_idle();
    CHECK(session.snapshot()->state == ResearchState::Editing);

    form["execution"]["fill"] = "next_open";  // in the schema, refused by the binder
    session.set_form(form);
    CHECK(session.snapshot()->form_errors.empty());
    session.run();
    session.wait_idle();
    const auto refused = session.snapshot();
    REQUIRE(refused->state == ResearchState::Refused);
    bool named = false;
    for (const auto& e : refused->job_errors) {
        named = named || e.path == "/execution/fill";
    }
    CHECK(named);
    std::filesystem::remove_all(config.runs_dir);
}

TEST_CASE("a job whose progress callback says stop returns no Results") {
    research::MemoryBarSource data;
    data.add(research::load_close_csv(kFixtures / "drawdown_prices.csv", "FIXTURE"));
    research::JobOptions options;
    options.null_draws = 5;
    options.bootstrap_paths = 5;
    options.progress = [](std::string_view stage, int, int) { return stage != "bootstrap"; };
    const auto outcome = research::run_job(read_json(kFixtures / "job_form.json"), data, options);
    CHECK(outcome.cancelled);
    CHECK_FALSE(outcome.ok());
    CHECK(outcome.results.is_null());
}

TEST_CASE("the Research view mode is remembered like the other two") {
    const auto path = std::filesystem::temp_directory_path() / "ms_ui_state_research.state";
    UiState saved;
    saved.view_mode = ViewMode::Research;
    saved.save(path);
    UiState loaded;
    loaded.load(path);
    CHECK(loaded.view_mode == ViewMode::Research);
    CHECK(std::string(to_string(ViewMode::Research)) == "research");
    std::filesystem::remove(path);
}
