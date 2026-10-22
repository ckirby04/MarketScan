// Synthetic history (contracts/research/JOB.md section 1a), from the engine's side.
//
// The cross-test: extend_history must reproduce, double for double, what
// research/synthetic.py computes for data/samples/research/synthetic_cases.json
// (written by scripts/make_synthetic_cases.py from seeded random walks, no
// market data). research/tests/test_synthetic.py holds the Python side to the
// same file, so neither implementation can drift alone.
//
// Then what only the job can show: a synthetic extension is honoured only for
// the recipe it was copied from, and a run on synthetic history says so in its
// data block, its trades and its warnings.

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include "marketscan/core/session_calendar.hpp"
#include "marketscan/data/bar_store.hpp"
#include "marketscan/research/job.hpp"
#include "marketscan/research/json_schema.hpp"
#include "marketscan/research/rule_run.hpp"

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

std::int64_t day_number(const std::string& iso) {
    return calendar::days_from_civil(std::stoll(iso.substr(0, 4)),
                                     static_cast<unsigned>(std::stoul(iso.substr(5, 2))),
                                     static_cast<unsigned>(std::stoul(iso.substr(8, 2))));
}

std::string iso_of(std::int64_t day) {
    const auto civil = calendar::civil_from_days(day);
    char buffer[16];
    std::snprintf(buffer,
                  sizeof(buffer),
                  "%04lld-%02u-%02u",
                  static_cast<long long>(civil.year),
                  civil.month,
                  civil.day);
    return buffer;
}

BarSeries daily_series(const std::string& ticker, const json& days, const json& closes) {
    BarSeries series;
    series.ticker = ticker;
    for (std::size_t i = 0; i < days.size(); ++i) {
        StoredBar bar;
        bar.ts = bar_store::daily_close_stamp(day_number(days[i].get<std::string>()));
        bar.open = bar.high = bar.low = bar.close = closes[i].get<double>();
        bar.volume = 1'000'000.0;
        series.bars.push_back(bar);
    }
    return series;
}

std::set<std::string> error_paths(const JobOutcome& outcome) {
    std::set<std::string> out;
    for (const JobError& e : outcome.errors) {
        out.insert(e.path);
    }
    return out;
}

/// A leveraged LEV whose real history is the last `real_share` of the
/// fixture's days, a recipe that extends it from the fixture (as BASE), and a
/// flat 2% rate: everything section 1a needs, with no market data.
struct SyntheticWorld {
    MemoryBarSource data;
    json recipes;
    std::string first_real;
};

std::unique_ptr<SyntheticWorld> synthetic_world(double real_share) {
    auto world = std::make_unique<SyntheticWorld>();
    BarSeries base = load_close_csv(kFixtures / "drawdown_prices.csv", "BASE");
    BarSeries rate = base;
    rate.ticker = "RATE";
    for (StoredBar& bar : rate.bars) {
        bar.open = bar.high = bar.low = bar.close = 2.0;
    }
    BarSeries lev;
    lev.ticker = "LEV";
    const auto from =
        static_cast<std::ptrdiff_t>(static_cast<double>(base.bars.size()) * (1.0 - real_share));
    lev.bars.assign(base.bars.begin() + from, base.bars.end());
    for (StoredBar& bar : lev.bars) {
        bar.open = bar.high = bar.low = bar.close = bar.close * 0.5;
    }
    world->first_real = iso_of(day_of(lev.bars.front().ts));
    world->data.add(base);
    world->data.add(rate);
    world->data.add(lev);
    world->recipes = json::array({json{{"record", "synthetic_recipe"},
                                       {"recipe_id", "lev_from_base_2x"},
                                       {"target_security_id", "lev"},
                                       {"underlying_security_id", "base"},
                                       {"method", "daily_leverage"},
                                       {"params",
                                        {{"leverage", 2},
                                         {"annual_fee_pct", 0.9},
                                         {"borrow_spread_pct", 0.5},
                                         {"cash_rate_series_id", "rate"}}},
                                       {"available_from", "2000-01-03"},
                                       {"validation",
                                        {{"overlap_from", world->first_real},
                                         {"overlap_to", "2003-08-29"},
                                         {"daily_return_correlation", 0.99},
                                         {"annualized_tracking_difference_pct", 0.25},
                                         {"max_cumulative_gap_pct", 3.0},
                                         {"computed_by", "the test"}}}}});
    return world;
}

json synthetic_form() {
    json form = read_json(kFixtures / "job_form.json");
    form["universe"]["assets"][0] = {{"symbol", "LEV"},
                                     {"security_id", "lev"},
                                     {"synthetic_extension",
                                      {{"recipe_id", "lev_from_base_2x"},
                                       {"underlying", "BASE"},
                                       {"leverage", 2},
                                       {"annual_fee_pct", 0.9},
                                       {"borrow_spread_pct", 0.5}}}};
    form["benchmarks"] = json::array({"LEV", "BASE"});
    return form;
}

JobOptions synthetic_options(const json& recipes) {
    JobOptions options;
    options.null_draws = 100;
    options.bootstrap_paths = 20;
    options.data_snapshot = "fixture";
    options.recipes = recipes;
    return options;
}

bool has_warning(const json& results, const std::string& code) {
    const json& warnings = results.at("warnings");
    return std::any_of(
        warnings.begin(), warnings.end(), [&](const json& w) { return w.at("code") == code; });
}

}  // namespace

TEST_CASE("synthetic history matches research/synthetic.py exactly") {
    const json cases = read_json(kFixtures / "synthetic_cases.json").at("cases");
    REQUIRE(cases.size() >= 6);
    int built = 0;
    int refused = 0;
    for (const json& c : cases) {
        INFO(c.at("name").get<std::string>());
        const json& in = c.at("inputs");
        const BarSeries underlying =
            daily_series("U", in.at("underlying_days"), in.at("underlying"));
        const BarSeries rate = daily_series("R", in.at("rate_days"), in.at("rates"));
        const json& p = in.at("params");
        const SyntheticParams params{p.at("leverage").get<double>(),
                                     p.at("annual_fee_pct").get<double>(),
                                     p.at("borrow_spread_pct").get<double>()};
        const json& expected = c.at("expected");
        try {
            const SyntheticBars out =
                extend_history(underlying,
                               rate,
                               day_number(in.at("first_real_day").get<std::string>()),
                               in.at("first_real_close").get<double>(),
                               params);
            REQUIRE_FALSE(expected.contains("error"));
            json days = json::array();
            for (const std::int64_t day : out.days) {
                days.push_back(iso_of(day));
            }
            CHECK(days == expected.at("days"));
            // Exactly: the same operations in the same order give the same doubles.
            CHECK(json(out.closes) == expected.at("closes"));
            ++built;
        } catch (const std::runtime_error& error) {
            REQUIRE(expected.contains("error"));
            CHECK(std::string(error.what()) == expected.at("error").get<std::string>());
            ++refused;
        }
    }
    // Both kinds of case were exercised, so neither branch passed vacuously.
    CHECK(built >= 4);
    CHECK(refused >= 2);
}

TEST_CASE("a run on synthetic history marks every part of it that used the simulation") {
    const auto world = synthetic_world(0.3);
    const JobOutcome outcome =
        run_job(synthetic_form(), world->data, synthetic_options(world->recipes));
    REQUIRE(outcome.ok());
    const json& results = outcome.results;
    const auto errors = JsonSchema::load(kRepo / "contracts" / "research" / "results.schema.json")
                            .validate(results);
    for (const auto& e : errors) {
        UNSCOPED_INFO(e.instance_path << ": " << e.message);
    }
    CHECK(errors.empty());

    const json& asset = results.at("stats").at("data").at(0);
    CHECK(asset.at("series_id") == "LEV+synthetic:lev_from_base_2x");
    CHECK(asset.at("used_from") == "2000-01-03");
    const json& synthetic = asset.at("synthetic");
    CHECK(synthetic.at("recipe_id") == "lev_from_base_2x");
    CHECK(synthetic.at("from") == "2000-01-03");
    CHECK(day_number(synthetic.at("to").get<std::string>()) < day_number(world->first_real));
    CHECK(synthetic.at("daily_return_correlation") == 0.99);
    CHECK(synthetic.at("annualized_tracking_difference_pct") == 0.25);
    // BASE is loaded as itself, never as synthetic.
    CHECK(results.at("stats").at("data").at(1).at("synthetic").is_null());

    // A trade is synthetic exactly when it was entered before the first real bar.
    const json& trades = results.at("stats").at("trades");
    REQUIRE_FALSE(trades.empty());
    for (const json& trade : trades) {
        CHECK(trade.at("synthetic").get<bool>() ==
              (day_number(trade.at("entry_date").get<std::string>()) <
               day_number(world->first_real)));
    }
    // Seventy per cent simulated: more than half.
    CHECK(has_warning(results, "MOSTLY_SYNTHETIC"));
    // The LEV benchmark is the asset's own extended series, from the same day.
    const json& bench = results.at("stats").at("benchmarks").at(0);
    CHECK(bench.at("symbol") == "LEV");
    CHECK(bench.at("windows").at("full").at("from") ==
          results.at("stats").at("selected").at("windows").at("full").at("from"));
}

TEST_CASE("synthetic history under half the range is marked but not warned about") {
    const auto world = synthetic_world(0.7);
    const JobOutcome outcome =
        run_job(synthetic_form(), world->data, synthetic_options(world->recipes));
    REQUIRE(outcome.ok());
    CHECK_FALSE(outcome.results.at("stats").at("data").at(0).at("synthetic").is_null());
    CHECK_FALSE(has_warning(outcome.results, "MOSTLY_SYNTHETIC"));
}

TEST_CASE("a synthetic extension is honoured only for the recipe it was copied from") {
    const auto world = synthetic_world(0.3);
    const JobOptions options = synthetic_options(world->recipes);
    const std::string at = "/universe/assets/0/synthetic_extension";

    json fee = synthetic_form();
    fee["universe"]["assets"][0]["synthetic_extension"]["annual_fee_pct"] = 0.5;
    CHECK(error_paths(run_job(fee, world->data, options)) ==
          std::set<std::string>{at + "/annual_fee_pct"});

    json underlying = synthetic_form();
    underlying["universe"]["assets"][0]["synthetic_extension"]["underlying"] = "QQQ";
    CHECK(error_paths(run_job(underlying, world->data, options)) ==
          std::set<std::string>{at + "/underlying"});

    json target = synthetic_form();
    target["universe"]["assets"][0]["security_id"] = "other";
    CHECK(error_paths(run_job(target, world->data, options)) ==
          std::set<std::string>{at + "/recipe_id"});

    // A recipe with no validation cannot be used (CATALOG.md).
    json unvalidated = world->recipes;
    unvalidated[0].erase("validation");
    CHECK(error_paths(run_job(synthetic_form(), world->data, synthetic_options(unvalidated))) ==
          std::set<std::string>{at + "/recipe_id"});

    // No rate series: refused, not financed at zero.
    MemoryBarSource no_rate;
    no_rate.add(*world->data.load("BASE"));
    no_rate.add(*world->data.load("LEV"));
    CHECK(error_paths(run_job(synthetic_form(), no_rate, options)) ==
          std::set<std::string>{at + "/recipe_id"});

    // And with everything matching, it runs.
    CHECK(run_job(synthetic_form(), world->data, options).ok());
}

TEST_CASE("several daily directories answer in order") {
    const auto root = std::filesystem::temp_directory_path() / "ms_research_dirs";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "a");
    std::filesystem::create_directories(root / "b");
    BarSeries one = load_close_csv(kFixtures / "drawdown_prices.csv", "ONE");
    one.interval = bar_store::kDailyInterval;
    bar_store::write(root / "b" / "ONE.msb", one);
    BarSeries shadow = one;
    shadow.bars.resize(10);
    bar_store::write(root / "a" / "ONE.msb", shadow);
    const DirectoryBarSource source(std::vector<std::filesystem::path>{root / "a", root / "b"});
    REQUIRE(source.load("ONE").has_value());
    CHECK(source.load("ONE")->bars.size() == 10);  // the first directory answers
    const DirectoryBarSource reversed(std::vector<std::filesystem::path>{root / "b", root / "a"});
    CHECK(reversed.load("ONE")->bars.size() == one.bars.size());
    CHECK_FALSE(source.load("TWO").has_value());
    std::filesystem::remove_all(root);
}
