#include <cstdint>
#include <limits>

#include <catch2/catch_test_macros.hpp>

#include "marketscan/core/money.hpp"

using marketscan::Money;

TEST_CASE("whole dollars are exact", "[money]") {
    REQUIRE(Money::from_dollars(1).micros() == 1'000'000);
    REQUIRE(Money::from_dollars(0).micros() == 0);
    REQUIRE(Money::from_dollars(-250).micros() == -250'000'000);
    REQUIRE(Money::from_dollars(1) == Money::from_micros(1'000'000));
}

TEST_CASE("micro-dollar resolution survives a decimal round trip", "[money]") {
    REQUIRE(Money::from_double(12.34).micros() == 12'340'000);
    REQUIRE(Money::from_double(-12.34).micros() == -12'340'000);
    REQUIRE(Money::from_double(0.000001).micros() == 1);
    REQUIRE(Money::from_double(0.0).micros() == 0);
}

// The reason this type exists. A double accumulator drifts away from the right
// answer here by an amount that looks exactly like a small trading edge.
TEST_CASE("repeated addition does not drift", "[money]") {
    Money exact;
    double naive = 0.0;

    const Money cent = Money::from_double(0.01);
    for (int i = 0; i < 1'000'000; ++i) {
        exact += cent;
        naive += 0.01;
    }

    REQUIRE(exact == Money::from_dollars(10'000));
    REQUIRE(exact.micros() == 10'000'000'000);

    // Demonstrates the failure being prevented, rather than just asserting the
    // fix works: the naive sum is not the right number.
    REQUIRE(naive != 10'000.0);
}

// Half away from zero, not half to even: a long and a short of the same size
// must cost the same, and "round" should mean what a person means by it.
//
// The inputs are chosen so every intermediate double is exact (3, 0.5 and 1.5
// are all representable), which keeps this a test of the rounding rule rather
// than of floating point.
TEST_CASE("rounding is symmetric about zero", "[money]") {
    REQUIRE(Money::from_micros(3).scaled(0.5).micros() == 2);    // 1.5 -> 2
    REQUIRE(Money::from_micros(-3).scaled(0.5).micros() == -2);  // -1.5 -> -2
    REQUIRE(Money::from_micros(1).scaled(0.5).micros() == 1);    // 0.5 -> 1
    REQUIRE(Money::from_micros(-1).scaled(0.5).micros() == -1);  // -0.5 -> -1
    REQUIRE(Money::from_micros(5).scaled(0.5).micros() == 3);    // 2.5 -> 3
    REQUIRE(Money::from_micros(-5).scaled(0.5).micros() == -3);  // -2.5 -> -3
}

TEST_CASE("notional turns a price and a quantity into exact cash", "[money]") {
    REQUIRE(Money::notional(100.0, 10.0) == Money::from_dollars(1'000));
    REQUIRE(Money::notional(19.99, 3.0).micros() == 59'970'000);
    REQUIRE(Money::notional(100.0, -10.0) == Money::from_dollars(-1'000));
    REQUIRE(Money::notional(0.0, 10.0).is_zero());
    REQUIRE(Money::notional(100.0, 0.0).is_zero());

    // A price with more precision than a cent is not silently truncated.
    REQUIRE(Money::notional(1.234567, 1.0).micros() == 1'234'567);
}

TEST_CASE("arithmetic", "[money]") {
    const Money a = Money::from_dollars(100);
    const Money b = Money::from_double(0.25);

    REQUIRE((a + b).micros() == 100'250'000);
    REQUIRE((a - b).micros() == 99'750'000);
    REQUIRE((-a).micros() == -100'000'000);
    REQUIRE((a * 3).micros() == 300'000'000);
    REQUIRE((3 * a).micros() == 300'000'000);
    REQUIRE(a.abs() == a);
    REQUIRE((-a).abs() == a);
    REQUIRE(Money{}.is_zero());

    Money acc = a;
    acc -= a;
    REQUIRE(acc.is_zero());
}

TEST_CASE("ordering", "[money]") {
    const Money low = Money::from_dollars(-5);
    const Money mid = Money{};
    const Money high = Money::from_dollars(5);

    REQUIRE(low < mid);
    REQUIRE(mid < high);
    REQUIRE(low < high);
    REQUIRE(high > low);
    REQUIRE(low <= low);
    REQUIRE(low == Money::from_dollars(-5));
    REQUIRE(low != high);
}

TEST_CASE("per_unit refuses to produce a NaN", "[money]") {
    REQUIRE(Money::from_dollars(1'000).per_unit(10.0) == 100.0);
    REQUIRE(Money::from_dollars(1'000).per_unit(0.0) == 0.0);
}

TEST_CASE("crosses the plugin boundary unchanged", "[money][abi]") {
    const Money m = Money::from_double(-1234.567891);
    const MS_Money wire = m.to_abi();
    REQUIRE(Money::from_abi(wire) == m);
    REQUIRE(wire == -1'234'567'891);
}

TEST_CASE("to_string", "[money]") {
    REQUIRE(Money::from_dollars(0).to_string() == "0.00");
    REQUIRE(Money::from_dollars(1'234).to_string() == "1234.00");
    REQUIRE(Money::from_double(-12.34).to_string() == "-12.34");
    REQUIRE(Money::from_double(0.5).to_string() == "0.50");

    // Sub-cent residue is shown rather than hidden, because that is exactly
    // where a commission or a slippage calculation goes wrong.
    REQUIRE(Money::from_micros(1'234'567).to_string() == "1.234567");
    REQUIRE(Money::from_micros(-1).to_string() == "-0.000001");
}

TEST_CASE("INT64_MIN formats without overflowing on negation", "[money]") {
    const Money m = Money::from_micros(std::numeric_limits<std::int64_t>::min());
    REQUIRE(m.to_string() == "-9223372036854.775808");
}
