#ifndef MARKETSCAN_APP_PANELS_HPP
#define MARKETSCAN_APP_PANELS_HPP

#include <string>

#include "../chart_model.hpp"
#include "../series_colors.hpp"
#include "../session.hpp"
#include "../ui_state.hpp"

namespace marketscan::app::panels {

/// What every panel is handed.
///
/// The snapshot is const and complete: it cannot change while a panel draws
/// it, which is what lets a panel read a strategy's equity curve and its
/// blotter and be sure they describe the same instant. Anything a panel wants
/// to *change* goes through the session as a command.
struct Context {
    Session& session;
    const SessionSnapshot& snapshot;
    UiState& ui;
};

// Each of these draws one dockable window, including its own Begin/End, and
// does nothing if its UiState flag is false.

/// Overview. Draws into the caller's window rather than owning one: it is a
/// whole view, not a dockable panel, and nothing in it is dockable either.
void overview(Context& context);

void leaderboard(Context& context);
void equity_curves(Context& context);
void drawdown(Context& context);
void relative_strength(Context& context);
void correlation_matrix(Context& context);
void trade_blotter(Context& context);
void positions(Context& context);
void theory_detail(Context& context);
void universe_inspector(Context& context);
void plugin_manager(Context& context);
void transport(Context& context);
void latency(Context& context);
void log_console(Context& context);

/// Draws every panel, in a fixed order.
void draw_all(Context& context);

// --- Shared formatting ---------------------------------------------------
//
// In one place so that a P&L is coloured and signed the same way in every
// panel. A number that means "loss" in one table and "small" in another is
// how a dashboard misleads without ever being wrong.

[[nodiscard]] std::string format_money(Money value, bool with_sign = false);
[[nodiscard]] std::string format_percent(double fraction, int precision = 2);
[[nodiscard]] std::string format_metric(double value, int precision = 2);
[[nodiscard]] std::string format_quantity(double value);
[[nodiscard]] std::string format_price(double value);
[[nodiscard]] std::string format_timestamp(Timestamp ts);
[[nodiscard]] std::string format_time_of_day(Timestamp ts);
[[nodiscard]] std::string format_duration(Duration nanos);
[[nodiscard]] std::string format_count(std::uint64_t value);

/// Draws text in the monospaced face. For anything in a column.
void mono_text(const std::string& text);

/// Draws a money value coloured by sign, right-aligned in the current column.
void money_cell(Money value, bool with_sign = true);

/// Draws a metric, or a dim dash when it is not defined.
void metric_cell(double value, int precision = 2);

/// A right-aligned monospaced cell.
void right_text(const std::string& text, unsigned int color = 0);

/// `(?)` with a tooltip. For explaining a column rather than guessing what a
/// reader already knows.
void help_marker(const char* description);

}  // namespace marketscan::app::panels

#endif  // MARKETSCAN_APP_PANELS_HPP
