#ifndef MARKETSCAN_RESEARCH_DRAWDOWN_ENTRY_HPP
#define MARKETSCAN_RESEARCH_DRAWDOWN_ENTRY_HPP

/*
 * The `drawdown_entry` template's rule, as a built-in strategy.
 *
 * The specification is contracts/research/DRAWDOWN_ENTRY.md. This is one of two
 * implementations; the other is research/drawdown_entry.py, and they are
 * cross-tested on committed fixtures for exact agreement on every order.
 *
 * Built in rather than a theory DLL because the research job instantiates it
 * hundreds of times per question with different parameters, and because a
 * strategy whose output is graded should not be swappable on disk between the
 * run and the grade.
 */

#include <cstdint>
#include <string_view>

namespace marketscan::research {

inline constexpr std::string_view kDrawdownEntryId = "drawdown_entry";

/// At most this many tranches. Each one is two parameters.
inline constexpr int kMaxTranches = 4;

// Parameter names, as the strategy binds them.
namespace drawdown_param {
inline constexpr const char* kReference = "reference";  // ReferenceKind
inline constexpr const char* kWindow = "window_days";
inline constexpr const char* kTrancheCount = "tranche_count";
inline constexpr const char* kLevel[kMaxTranches] = {
    "tranche0_level", "tranche1_level", "tranche2_level", "tranche3_level"};
inline constexpr const char* kFraction[kMaxTranches] = {
    "tranche0_fraction", "tranche1_fraction", "tranche2_fraction", "tranche3_fraction"};
inline constexpr const char* kRecoverToHigh = "recover_to_high";
inline constexpr const char* kTakeProfit = "take_profit_pct";      // 0 is off
inline constexpr const char* kStopLoss = "stop_loss_pct";          // 0 is off
inline constexpr const char* kTrailingStop = "trailing_stop_pct";  // 0 is off
inline constexpr const char* kTimeStop = "time_stop_days";         // 0 is off
inline constexpr const char* kReentry = "reentry";
inline constexpr const char* kReserve = "reserve_pct";
inline constexpr const char* kCommission = "commission_per_order";  // dollars
/// First decision day, as days since 1970-01-01 of the bar's UTC stamp.
inline constexpr const char* kStartDay = "start_day";
}  // namespace drawdown_param

enum class ReferenceKind : int {
    AllTimeHigh = 0,
    RollingHigh = 1,
    SinceLastExit = 2,
};

/// Why a position was sold. The values are the spec's reason codes.
enum class ExitReason : std::uint8_t {
    None = 0,
    RecoverToHigh = 1,
    TakeProfit = 2,
    StopLoss = 3,
    TrailingStop = 4,
    TimeStop = 5,
};

[[nodiscard]] std::string_view to_string(ExitReason reason) noexcept;

// Order tags, DRAWDOWN_ENTRY.md "Order tags".
inline constexpr std::uint64_t kBuyTag = 0x100;
inline constexpr std::uint64_t kSellTag = 0x200;
inline constexpr std::uint64_t kTagPayload = 0xFF;

[[nodiscard]] constexpr bool is_buy_tag(std::uint64_t tag) noexcept {
    return (tag & ~kTagPayload) == kBuyTag;
}

[[nodiscard]] constexpr bool is_sell_tag(std::uint64_t tag) noexcept {
    return (tag & ~kTagPayload) == kSellTag;
}

/// True once the strategy is in the registry.
///
/// Calling this is also what keeps it there. The registration is a static
/// initialiser in a static library, and a linker drops an object file nothing
/// refers to -- registration and all -- without a word. Every caller that
/// needs the strategy calls this first.
[[nodiscard]] bool drawdown_entry_registered();

}  // namespace marketscan::research

#endif  // MARKETSCAN_RESEARCH_DRAWDOWN_ENTRY_HPP
