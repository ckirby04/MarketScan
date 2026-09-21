#ifndef MARKETSCAN_APP_PANELS_EQUITY_CHART_HPP
#define MARKETSCAN_APP_PANELS_EQUITY_CHART_HPP

/*
 * The one equity chart, drawn.
 *
 * Deliberately thin. Everything that could differ between the two view modes --
 * which series, which colour, which geometry, which axis -- was decided in
 * `chart_model.hpp`, on the other side of the ImGui boundary, where it can be
 * compared by a test. What is left here is stroking lines, and a bug that can
 * only live in stroking lines is a bug you can see.
 *
 * There is no second implementation of this for Overview. That is the point.
 */

#include <string>

#include "../chart_model.hpp"

struct ImVec2;

namespace marketscan::app::panels {

/// Draws the model into the current ImGui window.
///
/// `plot_id` distinguishes the two instantiations for ImPlot's internal state;
/// it does not change what is drawn.
///
/// Returns the id of a series whose direct label or legend entry was clicked, or
/// empty. Selection is the caller's business: Operator points it at the
/// leaderboard, Overview at its own selector, and the chart does not know which.
std::string draw_equity_chart(const ChartModel& model, const char* plot_id, ImVec2 size);

/// The underwater strip: the emphasised series against the benchmark, sharing
/// the chart's x-axis.
///
/// Its own function rather than a density of the chart, because it plots a
/// different quantity. It reads `StrategySnapshot::drawdown` -- it does not
/// derive one.
void draw_drawdown_strip(const ChartModel& model,
                         const SessionSnapshot& snapshot,
                         const char* plot_id,
                         ImVec2 size);

}  // namespace marketscan::app::panels

#endif  // MARKETSCAN_APP_PANELS_EQUITY_CHART_HPP
