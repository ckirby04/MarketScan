#ifndef MARKETSCAN_CORE_TYPES_HPP
#define MARKETSCAN_CORE_TYPES_HPP

#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

#include "marketscan/abi/plugin_abi.h"
#include "marketscan/core/money.hpp"

/// MarketScan engine types.
///
/// These are aliases of the ABI scalars rather than new types, on purpose: the
/// host and a plugin must agree on exactly one definition of "timestamp", and
/// the plugin can only see the C header. Anything that needs richer behaviour
/// -- Money -- wraps the ABI scalar without changing its size or layout.
namespace marketscan {

/// Nanoseconds since the Unix epoch, UTC. Never a local time, never a duration.
using Timestamp = MS_Nanos;

/// A span of time in nanoseconds.
using Duration = MS_Nanos;

/// Dense, run-stable index into the symbol table. Zero is never a valid
/// symbol; it is what symbol_id() returns for an unknown ticker.
using SymbolId = MS_SymbolId;

/// A quoted price. Deliberately floating point -- see money.hpp for why cash
/// is not.
using Price = double;

/// Signed size in instrument units. Negative means short.
using Quantity = double;

using Side = MS_Side;
using OrderType = MS_OrderType;
using TimeInForce = MS_TimeInForce;
using LogLevel = MS_LogLevel;

inline constexpr SymbolId kInvalidSymbol = 0;

inline constexpr Timestamp kNever = std::numeric_limits<Timestamp>::min();
inline constexpr Timestamp kForever = std::numeric_limits<Timestamp>::max();

namespace time {

inline constexpr Duration kNanosecond = 1;
inline constexpr Duration kMicrosecond = 1'000;
inline constexpr Duration kMillisecond = 1'000'000;
inline constexpr Duration kSecond = 1'000'000'000;
inline constexpr Duration kMinute = 60 * kSecond;
inline constexpr Duration kHour = 60 * kMinute;
inline constexpr Duration kDay = 24 * kHour;

/// ISO-8601 in UTC, e.g. "2024-03-15T13:30:00.000000000Z".
///
/// Formatting only. Nothing in the engine parses a timestamp back out of a
/// string, and nothing decides anything from one.
[[nodiscard]] std::string to_iso8601(Timestamp ts);

/// Parses "YYYY-MM-DDTHH:MM:SS[.fraction][Z]" and the same with a space
/// separator, which is what CSV exports tend to contain. Returns false on
/// anything it does not fully understand rather than guessing, because a
/// silently misparsed timestamp reorders an entire replay.
[[nodiscard]] bool from_iso8601(std::string_view text, Timestamp& out);

}  // namespace time

[[nodiscard]] constexpr Side opposite(Side s) noexcept {
    return s == MS_SIDE_BUY ? MS_SIDE_SELL : MS_SIDE_BUY;
}

/// +1 for a buy, -1 for a sell. Turns a side into arithmetic.
[[nodiscard]] constexpr double side_sign(Side s) noexcept {
    return s == MS_SIDE_BUY ? 1.0 : -1.0;
}

[[nodiscard]] constexpr std::string_view to_string(Side s) noexcept {
    return s == MS_SIDE_BUY ? "BUY" : "SELL";
}

[[nodiscard]] constexpr std::string_view to_string(OrderType t) noexcept {
    return t == MS_ORD_MARKET ? "MARKET" : "LIMIT";
}

[[nodiscard]] constexpr std::string_view to_string(TimeInForce t) noexcept {
    switch (t) {
        case MS_TIF_DAY:
            return "DAY";
        case MS_TIF_IOC:
            return "IOC";
        case MS_TIF_GTC:
            return "GTC";
    }
    return "?";
}

}  // namespace marketscan

#endif  // MARKETSCAN_CORE_TYPES_HPP
