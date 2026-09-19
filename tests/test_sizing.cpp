// The sizing rule, cross-tested against the other implementation.
//
// docs/SIZING.md is the specification. research/sizing.py is the other
// implementation of it, and it wrote data/samples/sizing_cases.csv: a grid of
// inputs with the share counts the rule produces, printed at %.17g so a
// binary64 round-trips exactly.
//
// This reads the same file and asserts **bit equality**, not closeness. Two
// implementations of one written rule, evaluated in the order the rule gives,
// on identical inputs, produce identical doubles -- and if they ever do not,
// the specification is ambiguous somewhere and that is the thing to fix.
//
// The grid is adversarial on purpose: prices near zero, equity on rounding
// boundaries, deciles that do not divide cleanly, cross-sections too small to
// form two legs, and the configuration that produced the 84-session divergence
// in the M9 fixture.

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "marketscan/strategy/sizing.hpp"

namespace {

struct Case {
    std::string name;
    std::int64_t equity_micros = 0;
    double gross = 0.0;
    std::uint32_t count = 0;
    double decile = 0.0;
    double close = 0.0;
    double position = 0.0;
    bool long_side = false;

    std::uint32_t side = 0;
    double per_name = 0.0;
    double target = 0.0;
    double delta = 0.0;
    int submit = 0;
};

[[nodiscard]] std::vector<std::string> split(const std::string& line) {
    std::vector<std::string> fields;
    std::istringstream stream(line);
    std::string field;
    while (std::getline(stream, field, ',')) {
        fields.push_back(field);
    }
    return fields;
}

[[nodiscard]] std::vector<Case> read_cases() {
    const std::filesystem::path path =
        std::filesystem::path(MARKETSCAN_TEST_DATA_DIR) / "sizing_cases.csv";
    std::ifstream file(path);
    REQUIRE(file);

    std::vector<Case> cases;
    std::string line;
    std::getline(file, line);  // header
    while (std::getline(file, line)) {
        if (line.empty()) {
            continue;
        }
        const std::vector<std::string> f = split(line);
        REQUIRE(f.size() == 13);
        Case one;
        one.name = f[0];
        one.equity_micros = std::strtoll(f[1].c_str(), nullptr, 10);
        one.gross = std::strtod(f[2].c_str(), nullptr);
        one.count = static_cast<std::uint32_t>(std::strtoul(f[3].c_str(), nullptr, 10));
        one.decile = std::strtod(f[4].c_str(), nullptr);
        one.close = std::strtod(f[5].c_str(), nullptr);
        one.position = std::strtod(f[6].c_str(), nullptr);
        one.long_side = f[7] == "1";
        one.side = static_cast<std::uint32_t>(std::strtoul(f[8].c_str(), nullptr, 10));
        one.per_name = std::strtod(f[9].c_str(), nullptr);
        one.target = std::strtod(f[10].c_str(), nullptr);
        one.delta = std::strtod(f[11].c_str(), nullptr);
        one.submit = std::atoi(f[12].c_str());
        cases.push_back(std::move(one));
    }
    return cases;
}

}  // namespace

TEST_CASE("the grid covers the cases the specification calls adversarial", "[sizing]") {
    const std::vector<Case> cases = read_cases();
    REQUIRE(cases.size() >= 40);

    // Not a count for its own sake: if the generator ever stops emitting the
    // awkward rows, this suite would still pass while testing only the middle
    // of the range.
    bool has_stand_down = false;
    bool has_tiny_price = false;
    bool has_boundary_equity = false;
    bool has_short = false;
    for (const Case& one : cases) {
        has_stand_down = has_stand_down || one.side == 0;
        has_tiny_price = has_tiny_price || (one.close > 0.0 && one.close <= 1e-4);
        has_boundary_equity = has_boundary_equity || one.equity_micros < 10;
        has_short = has_short || !one.long_side;
    }
    CHECK(has_stand_down);
    CHECK(has_tiny_price);
    CHECK(has_boundary_equity);
    CHECK(has_short);
}

TEST_CASE("both implementations of the sizing rule produce identical shares", "[sizing][parity]") {
    // Bit equality, with no tolerance parameter anywhere in this assertion.
    for (const Case& one : read_cases()) {
        INFO("case " << one.name);

        const std::uint32_t side = marketscan::sizing::side_count(one.count, one.decile);
        CHECK(side == one.side);

        const double per_name =
            marketscan::sizing::per_name_notional(one.equity_micros, one.gross, side);
        if (side == 0) {
            // Stand down: the rule produces nothing and the expected row says so.
            CHECK(one.per_name == 0.0);
            CHECK(one.submit == 0);
            continue;
        }
        CHECK(per_name == one.per_name);

        const double target = marketscan::sizing::target_shares(per_name, one.close, one.long_side);
        CHECK(target == one.target);

        const double delta = marketscan::sizing::trade_delta(target, one.position);
        CHECK(delta == one.delta);

        CHECK(static_cast<int>(marketscan::sizing::should_submit(delta)) == one.submit);
    }
}

TEST_CASE("notional rounds half away from zero, as money.hpp specifies", "[sizing]") {
    // The one place the default rounding mode would disagree with the other
    // implementation, and it disagrees by exactly one micro-dollar.
    CHECK(marketscan::sizing::notional_micros(0.5e-6, 1.0) == 1);
    CHECK(marketscan::sizing::notional_micros(-0.5e-6, 1.0) == -1);
    CHECK(marketscan::sizing::notional_micros(2.5e-6, 1.0) == 3);
    CHECK(marketscan::sizing::notional_micros(-2.5e-6, 1.0) == -3);
    CHECK(marketscan::sizing::notional_micros(10.0, 3.0) == 30'000'000);
}

TEST_CASE("a cross-section too small for two legs sizes nothing", "[sizing]") {
    CHECK(marketscan::sizing::side_count(1, 0.1) == 0);
    CHECK(marketscan::sizing::side_count(0, 0.1) == 0);
    CHECK(marketscan::sizing::side_count(2, 0.5) == 1);
    CHECK(marketscan::sizing::side_count(1, 0.5) == 0);  // 1*2 > 1
    CHECK(marketscan::sizing::side_count(30, 0.0) == 0);
}
