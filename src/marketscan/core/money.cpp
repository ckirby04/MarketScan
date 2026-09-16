#include "marketscan/core/money.hpp"

#include <cstdio>

namespace marketscan {

std::string Money::to_string() const {
    const bool negative = micros_ < 0;

    // Via unsigned, so that INT64_MIN does not overflow on negation.
    const auto magnitude = negative ? (~static_cast<std::uint64_t>(micros_) + 1U)
                                    : static_cast<std::uint64_t>(micros_);

    constexpr auto scale = static_cast<std::uint64_t>(kScale);
    const std::uint64_t whole = magnitude / scale;
    const std::uint64_t frac = magnitude % scale;

    char buf[48];
    int n = 0;
    if (frac % 10'000U == 0U) {
        n = std::snprintf(buf,
                          sizeof buf,
                          "%s%llu.%02llu",
                          negative ? "-" : "",
                          static_cast<unsigned long long>(whole),
                          static_cast<unsigned long long>(frac / 10'000U));
    } else {
        n = std::snprintf(buf,
                          sizeof buf,
                          "%s%llu.%06llu",
                          negative ? "-" : "",
                          static_cast<unsigned long long>(whole),
                          static_cast<unsigned long long>(frac));
    }
    return std::string(buf, static_cast<std::size_t>(n < 0 ? 0 : n));
}

}  // namespace marketscan
