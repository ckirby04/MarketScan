#ifndef MARKETSCAN_CORE_MONEY_HPP
#define MARKETSCAN_CORE_MONEY_HPP

#include <cassert>
#include <cmath>
#include <compare>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <string>

#include "marketscan/abi/plugin_abi.h"

namespace marketscan {

/// Exact cash, in micro-dollars.
///
/// Binary floating point cannot represent 0.01, so a `double` ledger drifts:
/// accumulate a cent a million times and the answer is wrong in a way that
/// looks like P&L. Every cash quantity in MarketScan -- balances, realized
/// P&L, commission, notional -- is an integer count of micro-dollars.
///
/// Prices stay `double`, because a price is a measurement rather than a
/// balance, and exchanges quote plenty of instruments in fractions no decimal
/// scale captures. Money enters the system at the point a price meets a
/// quantity, in `notional()`, and is exact from there on.
///
/// Trivially copyable and the same size as MS_Money, so it crosses the plugin
/// boundary as-is.
class Money {
public:
    using rep = std::int64_t;

    /// Micro-dollars per dollar.
    static constexpr rep kScale = 1'000'000;

    constexpr Money() noexcept = default;

    /// Construct from a raw micro-dollar count. Explicit by name so that no
    /// integer silently becomes money.
    static constexpr Money from_micros(rep micros) noexcept { return Money{micros}; }

    /// Exact: whole dollars carry no rounding.
    static constexpr Money from_dollars(rep dollars) noexcept {
        assert(dollars <= std::numeric_limits<rep>::max() / kScale &&
               dollars >= std::numeric_limits<rep>::min() / kScale &&
               "dollar amount overflows Money");
        return Money{dollars * kScale};
    }

    /// Rounds half away from zero, which is what a person means by "round",
    /// and -- unlike the default rounding mode -- is symmetric about zero so a
    /// long and a short of the same size cost the same.
    static Money from_double(double dollars) noexcept {
        return Money{round_to_micros(dollars * static_cast<double>(kScale))};
    }

    /// The cash value of `quantity` units at `price`. This is the single place
    /// a floating point measurement becomes an exact balance.
    static Money notional(double price, double quantity) noexcept {
        return Money{round_to_micros(price * quantity * static_cast<double>(kScale))};
    }

    [[nodiscard]] constexpr rep micros() const noexcept { return micros_; }

    /// Lossy by construction. For display and for metrics that are ratios
    /// anyway -- never feed the result back into a balance.
    [[nodiscard]] constexpr double to_double() const noexcept {
        return static_cast<double>(micros_) / static_cast<double>(kScale);
    }

    /// The MS_Money this crosses the plugin boundary as.
    [[nodiscard]] constexpr MS_Money to_abi() const noexcept {
        return static_cast<MS_Money>(micros_);
    }

    static constexpr Money from_abi(MS_Money m) noexcept { return Money{static_cast<rep>(m)}; }

    [[nodiscard]] constexpr bool is_zero() const noexcept { return micros_ == 0; }

    [[nodiscard]] constexpr Money abs() const noexcept {
        return Money{micros_ < 0 ? -micros_ : micros_};
    }

    /// Price implied by spreading this amount over `quantity` units.
    /// Returns 0.0 for a zero quantity rather than a NaN that would poison
    /// every statistic downstream.
    [[nodiscard]] double per_unit(double quantity) const noexcept {
        if (quantity == 0.0) {
            return 0.0;
        }
        return to_double() / quantity;
    }

    constexpr Money& operator+=(Money rhs) noexcept {
        assert(!add_overflows(micros_, rhs.micros_) && "Money addition overflows");
        micros_ += rhs.micros_;
        return *this;
    }

    constexpr Money& operator-=(Money rhs) noexcept {
        assert(!add_overflows(micros_, -rhs.micros_) && "Money subtraction overflows");
        micros_ -= rhs.micros_;
        return *this;
    }

    constexpr Money& operator*=(rep k) noexcept {
        assert((k == 0 || (micros_ <= std::numeric_limits<rep>::max() / (k < 0 ? -k : k) &&
                           micros_ >= std::numeric_limits<rep>::min() / (k < 0 ? -k : k))) &&
               "Money multiplication overflows");
        micros_ *= k;
        return *this;
    }

    friend constexpr Money operator+(Money a, Money b) noexcept { return a += b; }

    friend constexpr Money operator-(Money a, Money b) noexcept { return a -= b; }

    friend constexpr Money operator*(Money a, rep k) noexcept { return a *= k; }

    friend constexpr Money operator*(rep k, Money a) noexcept { return a *= k; }

    constexpr Money operator-() const noexcept { return Money{-micros_}; }

    /// Scale by a dimensionless factor -- a commission rate, a fraction of
    /// capital. Rounds half away from zero like every other conversion here.
    [[nodiscard]] Money scaled(double factor) const noexcept {
        return Money{round_to_micros(static_cast<double>(micros_) * factor)};
    }

    friend constexpr bool operator==(Money, Money) noexcept = default;
    friend constexpr auto operator<=>(Money, Money) noexcept = default;

    /// Decimal string: two places when the amount lands exactly on a cent,
    /// six when there is sub-cent residue worth seeing. For logs, blotters
    /// and assertion messages.
    [[nodiscard]] std::string to_string() const;

private:
    constexpr explicit Money(rep micros) noexcept : micros_(micros) {}

    static constexpr bool add_overflows(rep a, rep b) noexcept {
        return (b > 0 && a > std::numeric_limits<rep>::max() - b) ||
               (b < 0 && a < std::numeric_limits<rep>::min() - b);
    }

    /// Round-half-away-from-zero, guarding the range where a double stops
    /// being able to name every integer.
    static rep round_to_micros(double micros) noexcept {
        assert(std::isfinite(micros) && "non-finite value converted to Money");
        // 2^53 is the last integer a double represents exactly; beyond it the
        // rounding below is meaningless.
        assert(std::fabs(micros) < 9007199254740992.0 &&
               "value too large to convert to Money exactly");
        return static_cast<rep>(std::llround(micros));
    }

    rep micros_{0};
};

static_assert(sizeof(Money) == sizeof(MS_Money));
static_assert(alignof(Money) == alignof(MS_Money));

inline constexpr Money kZeroMoney{};

}  // namespace marketscan

#endif  // MARKETSCAN_CORE_MONEY_HPP
