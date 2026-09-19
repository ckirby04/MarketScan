#ifndef MARKETSCAN_STRATEGY_SIZING_HPP
#define MARKETSCAN_STRATEGY_SIZING_HPP

/*
 * The position sizing rule. The specification is docs/SIZING.md.
 *
 * This is one of two implementations. The other is research/sizing.py, and
 * neither calls the other: the research layer is deliberately independent of
 * the engine, which is what makes the two agreeing evidence rather than a
 * tautology. They are cross-tested against one committed grid of cases.
 *
 * Every function here is one step of the rule as written, in the order written,
 * with the types the specification names. Nothing is folded into a single
 * expression even where that would be shorter, because the reason the two
 * implementations drifted apart in the first place is that the intermediate
 * steps had no names.
 *
 * Header-only and free of engine dependencies beyond the ABI types, so a theory
 * compiled outside this tree can use it.
 */

#include <cmath>
#include <cstdint>

#include "marketscan/abi/plugin_abi.h"

namespace marketscan::sizing {

/// Below this many shares a delta is not submitted. SIZING.md step 7.
inline constexpr double kMinimumTrade = 1e-9;

/// Money's scale: micro-dollars to the dollar.
inline constexpr double kMicrosPerDollar = 1'000'000.0;

/// `Money::notional`: micro-dollars, rounded half away from zero.
///
/// std::llround is half away from zero, which is what money.hpp specifies and
/// what research/sizing.py reproduces. The default floating-point rounding mode
/// is half to even and would disagree on every exact half.
[[nodiscard]] inline std::int64_t notional_micros(double price, double quantity) noexcept {
    return static_cast<std::int64_t>(std::llround(price * quantity * kMicrosPerDollar));
}

/// Step 1: how many names each leg takes. Zero means stand down this session.
[[nodiscard]] inline std::uint32_t side_count(std::uint32_t count, double decile) noexcept {
    if (count == 0 || decile <= 0.0) {
        return 0;
    }
    const auto side = static_cast<std::uint32_t>(std::floor(static_cast<double>(count) * decile));
    if (side == 0 || side * 2 > count) {
        return 0;
    }
    return side;
}

/// Steps 3 and 4: the dollar target for one name on one leg.
[[nodiscard]] inline double
per_name_notional(std::int64_t equity_micros, double gross, std::uint32_t side) noexcept {
    if (side == 0) {
        return 0.0;
    }
    const double book = (static_cast<double>(equity_micros) / kMicrosPerDollar) * gross;
    return book / static_cast<double>(2 * side);
}

/// Step 5. Fractional by design -- SIZING.md says why.
[[nodiscard]] inline double target_shares(double per_name, double close, bool long_side) noexcept {
    if (!(close > 0.0)) {
        return 0.0;
    }
    return (long_side ? per_name : -per_name) / close;
}

/// Step 6.
[[nodiscard]] inline double trade_delta(double target, double position) noexcept {
    return target - position;
}

/// Step 7.
[[nodiscard]] inline bool should_submit(double delta) noexcept {
    return std::abs(delta) >= kMinimumTrade;
}

}  // namespace marketscan::sizing

#endif  // MARKETSCAN_STRATEGY_SIZING_HPP
