// The research job, from the engine's side.
//
// The golden test: a fresh run of data/samples/research/job_form.json must
// reproduce job_results.json and job_trace.json exactly. Those two files are
// what research/tests/test_grading.py regrades from contracts/research/JOB.md
// alone, so between the two tests the engine's grading is held to an
// independent implementation of the same specification.
//
// Then what only this side can show: that the Results satisfy their schema, a
// run is reproducible, the binder refuses what it cannot honour by name, and
// the small pieces -- SHA-256, the generator, the eigenvalues -- are right.

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <nlohmann/json.hpp>

#include "marketscan/research/job.hpp"
#include "marketscan/research/json_schema.hpp"
#include "marketscan/util/sha256.hpp"

using namespace marketscan;
using namespace marketscan::research;
using nlohmann::json;

namespace {

const std::filesystem::path kRepo = MARKETSCAN_SOURCE_DIR;
const std::filesystem::path kFixtures = kRepo / "data" / "samples" / "research";

json read_json(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    REQUIRE(in.good());
    return json::parse(in);
}

/// What `ms_research --fixed-time --null-draws 1000 --paths 50 --data-snapshot
/// fixture` uses to write the committed fixture.
JobOptions fixture_options() {
    JobOptions options;
    options.null_draws = 1000;
    options.bootstrap_paths = 50;
    options.data_snapshot = "fixture";
    return options;
}

const MemoryBarSource& fixture_data() {
    static MemoryBarSource data;
    static const bool loaded = [] {
        data.add(load_close_csv(kFixtures / "drawdown_prices.csv", "FIXTURE"));
        return true;
    }();
    (void)loaded;
    return data;
}

std::set<std::string> error_paths(const JobOutcome& outcome) {
    std::set<std::string> out;
    for (const JobError& e : outcome.errors) {
        out.insert(e.path);
    }
    return out;
}

}  // namespace

TEST_CASE("the research job reproduces its committed Results and trace exactly") {
    const json form = read_json(kFixtures / "job_form.json");
    const JobOutcome outcome = run_job(form, fixture_data(), fixture_options());
    REQUIRE(outcome.ok());
    // json == json compares every double exactly. A difference means the engine
    // changed what it computes; regenerate with ms_research only on purpose,
    // and test_grading.py will then say whether the research layer agrees.
    CHECK(outcome.results == read_json(kFixtures / "job_results.json"));
    CHECK(outcome.trace == read_json(kFixtures / "job_trace.json"));
}

TEST_CASE("the research job's Results satisfy results.schema.json") {
    const JsonSchema schema =
        JsonSchema::load(kRepo / "contracts" / "research" / "results.schema.json");
    const JobOutcome outcome =
        run_job(read_json(kFixtures / "job_form.json"), fixture_data(), fixture_options());
    REQUIRE(outcome.ok());
    const auto errors = schema.validate(outcome.results);
    for (const auto& e : errors) {
        UNSCOPED_INFO(e.instance_path << ": " << e.message);
    }
    CHECK(errors.empty());
}

TEST_CASE("the same form over the same bars gives byte-identical Results") {
    const json form = read_json(kFixtures / "job_form.json");
    JobOptions options = fixture_options();
    options.bootstrap_paths = 10;
    const JobOutcome a = run_job(form, fixture_data(), options);
    const JobOutcome b = run_job(form, fixture_data(), options);
    REQUIRE(a.ok());
    CHECK(a.results.dump() == b.results.dump());
    CHECK(a.trace.dump() == b.trace.dump());
    CHECK(a.equity_csv == b.equity_csv);
}

TEST_CASE("the prompt and provenance do not change the job") {
    json form = read_json(kFixtures / "job_form.json");
    JobOptions options = fixture_options();
    options.bootstrap_paths = 5;
    const JobOutcome a = run_job(form, fixture_data(), options);
    form["prompt"] = "a different wording of the same question";
    form["provenance"] = json::object();
    const JobOutcome b = run_job(form, fixture_data(), options);
    REQUIRE(a.ok());
    CHECK(a.results.dump() == b.results.dump());
}

TEST_CASE("the binder refuses what it cannot honour, every reason at once, by path") {
    json form = read_json(kFixtures / "job_form.json");
    form["universe"]["price_adjustment"] = "total_return";
    form["cash"] = {{"yield", "tbill_3m"}};
    form["execution"]["fill"] = "next_open";
    form["robustness"]["walk_forward"] = {{"train_years", 5}, {"test_years", 1}};
    form["universe"]["assets"][0]["synthetic_extension"] = {{"recipe_id", "r"},
                                                            {"underlying", "QQQ"},
                                                            {"leverage", 3},
                                                            {"annual_fee_pct", 0.84},
                                                            {"borrow_spread_pct", 0.5}};
    const JobOutcome outcome = run_job(form, fixture_data(), fixture_options());
    CHECK_FALSE(outcome.ok());
    CHECK(outcome.results.is_null());
    const std::set<std::string> paths = error_paths(outcome);
    CHECK(paths.contains("/universe/price_adjustment"));
    CHECK(paths.contains("/cash/yield"));
    CHECK(paths.contains("/execution/fill"));
    CHECK(paths.contains("/robustness/walk_forward"));
    CHECK(paths.contains("/universe/assets/0/synthetic_extension"));
}

TEST_CASE("the binder refuses missing data and a duplicated exit by path") {
    json form = read_json(kFixtures / "job_form.json");
    form["universe"]["assets"][0]["symbol"] = "NOSUCH";
    form["strategy"]["exit"]["rules"].push_back({{"type", "trailing_stop"}, {"pct", 5}});
    const JobOutcome outcome = run_job(form, fixture_data(), fixture_options());
    const std::set<std::string> paths = error_paths(outcome);
    CHECK(paths.contains("/universe/assets/0/symbol"));
    CHECK(paths.contains("/strategy/exit/rules/2"));
}

TEST_CASE("parameter_surface false hides the surface but the gates still measure it") {
    json form = read_json(kFixtures / "job_form.json");
    form["robustness"]["parameter_surface"] = false;
    JobOptions options = fixture_options();
    options.bootstrap_paths = 3;
    const JobOutcome hidden = run_job(form, fixture_data(), options);
    REQUIRE(hidden.ok());
    CHECK(hidden.results["stats"]["parameter_surface"].is_null());
    bool plateau_measured = false;
    for (const json& gate : hidden.results["verdict"]["gates"]) {
        plateau_measured =
            plateau_measured || (gate["id"] == "plateau" && gate["status"] != "not_applicable");
    }
    CHECK(plateau_measured);
}

TEST_CASE("a sweep step too small to count is refused, not cast") {
    json form = read_json(kFixtures / "job_form.json");
    form["strategy"]["entry"]["threshold_pct"] = {
        {"sweep", {{"min", 0.0}, {"max", 50.0}, {"step", 1e-300}}}};
    const JobOutcome outcome = run_job(form, fixture_data(), fixture_options());
    CHECK_FALSE(outcome.ok());
}

TEST_CASE("a run directory written by write_run reads back the same curves") {
    JobOptions options = fixture_options();
    options.bootstrap_paths = 2;
    const JobOutcome outcome =
        run_job(read_json(kFixtures / "job_form.json"), fixture_data(), options);
    REQUIRE(outcome.ok());
    const auto dir = std::filesystem::temp_directory_path() / "ms_write_run_test";
    std::filesystem::remove_all(dir);
    write_run(dir, read_json(kFixtures / "job_form.json"), outcome, true);
    for (const char* file :
         {"form.json", "results.json", "equity.csv", "curves.json", "trace.json"}) {
        CHECK(std::filesystem::exists(dir / file));
    }
    const auto curves = read_curves(dir / "curves.json");
    REQUIRE(curves.size() == outcome.curves.size());
    CHECK(curves.front().equity == outcome.curves.front().equity);
    CHECK(curves.back().drawdown == outcome.curves.back().drawdown);
    std::filesystem::remove_all(dir);
}

TEST_CASE("a sweep over five hundred points is refused") {
    json form = read_json(kFixtures / "job_form.json");
    form["strategy"]["entry"]["threshold_pct"] = {
        {"sweep", {{"min", 0.0}, {"max", 99.0}, {"step", 0.1}}}};
    const JobOutcome outcome = run_job(form, fixture_data(), fixture_options());
    CHECK_FALSE(outcome.ok());
}

TEST_CASE("on a flat price every random-timing alternative ties with the rule") {
    BarSeries flat = load_close_csv(kFixtures / "drawdown_prices.csv", "FLAT");
    flat.bars.resize(120);
    for (StoredBar& bar : flat.bars) {
        bar.open = bar.high = bar.low = bar.close = 50.0;
    }
    MemoryBarSource data;
    data.add(flat);

    json form = read_json(kFixtures / "job_form.json");
    form["universe"]["assets"][0]["symbol"] = "FLAT";
    form["benchmarks"] = {"FLAT"};
    form["strategy"]["entry"]["threshold_pct"] = 0;
    form["strategy"]["exit"]["rules"] = {{{"type", "time_stop"}, {"days", 10}}};
    form["robustness"]["oos_split"] = nullptr;
    form["objective"]["metric"] = "total_return";
    JobOptions options = fixture_options();
    options.null_draws = 50;
    options.bootstrap_paths = 3;
    const JobOutcome outcome = run_job(form, data, options);
    REQUIRE(outcome.ok());
    const json& timing = outcome.results["stats"]["random_timing"];
    CHECK(timing["strategy_percentile"] == 50.0);
    CHECK(timing["p_value"] == 1.0);
}

// --- The pieces ---------------------------------------------------------------

TEST_CASE("SHA-256 against the FIPS 180 example vectors") {
    CHECK(sha256_hex("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(sha256_hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(sha256_hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    // Exactly one block of padding boundary: 55 and 56 bytes straddle it.
    CHECK(sha256_hex(std::string(55, 'a')) ==
          "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318");
    CHECK(sha256_hex(std::string(56, 'a')) ==
          "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a");
}

TEST_CASE("SplitMix64 against its reference outputs") {
    SplitMix64 rng(0);
    CHECK(rng.next() == 0xE220A8397B1DCDAFULL);
    CHECK(rng.next() == 0x6E789E6AA1B965F4ULL);
}

TEST_CASE("percentile interpolates and drops nulls") {
    const double nan = std::numeric_limits<double>::quiet_NaN();
    CHECK(percentile({3.0, 1.0, nan, 2.0}, 0.5) == 2.0);
    CHECK(percentile({1.0, 2.0}, 0.25) == 1.25);
    CHECK(percentile({5.0}, 0.95) == 5.0);
    CHECK(std::isnan(percentile({}, 0.5)));
}

TEST_CASE("symmetric eigenvalues of small known matrices") {
    std::vector<double> two = symmetric_eigenvalues({{2.0, 1.0}, {1.0, 2.0}});
    std::sort(two.begin(), two.end());
    CHECK_THAT(two[0], Catch::Matchers::WithinAbs(1.0, 1e-12));
    CHECK_THAT(two[1], Catch::Matchers::WithinAbs(3.0, 1e-12));

    // All-ones 3x3: eigenvalues 3, 0, 0 -- the shape of a sweep whose points
    // are all the same strategy, which is what N_eff = 1 has to come from.
    std::vector<double> ones =
        symmetric_eigenvalues({{1.0, 1.0, 1.0}, {1.0, 1.0, 1.0}, {1.0, 1.0, 1.0}});
    std::sort(ones.begin(), ones.end());
    CHECK_THAT(ones[0], Catch::Matchers::WithinAbs(0.0, 1e-12));
    CHECK_THAT(ones[1], Catch::Matchers::WithinAbs(0.0, 1e-12));
    CHECK_THAT(ones[2], Catch::Matchers::WithinAbs(3.0, 1e-12));
}

TEST_CASE("the canonical form drops prompt and provenance and sorts keys") {
    const json form = {{"zeta", 1},
                       {"alpha", {{"b", 2}, {"a", 1}}},
                       {"prompt", "p"},
                       {"provenance", json::object()}};
    CHECK(canonical_form(form) == R"({"alpha":{"a":1,"b":2},"zeta":1})");
}

TEST_CASE("a malformed close CSV is refused with its file and line") {
    const auto path = std::filesystem::temp_directory_path() / "ms_research_bad.csv";
    {
        std::ofstream out(path);
        out << "date,close\n2020-01-02,10\n2020-02-30,11\n";
    }
    try {
        (void)load_close_csv(path, "BAD");
        FAIL("expected a refusal");
    } catch (const std::runtime_error& error) {
        CHECK(std::string(error.what()).find(":3:") != std::string::npos);
    }
    std::filesystem::remove(path);
}
