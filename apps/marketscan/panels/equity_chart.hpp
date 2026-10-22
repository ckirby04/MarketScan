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
/// different quantity. It reads each series' `drawdown` from the model -- it
/// does not derive one, and it does not care whether a session or a research
/// job supplied it.
void draw_drawdown_strip(const ChartModel& model, const char* plot_id, ImVec2 size);

/// Pins a plot's own surface and chrome to the token table. Every plot in the
/// application wears it, so the Research view's sweep and range plots sit on the
/// same surface as the equity chart.
///
/// ImPlot's default style paints its own background, and contrast and
/// colour-vision results are only meaningful against the surface actually
/// rendered on -- so the surface is set rather than inherited.
void push_chart_style(ChartDensity density);
void pop_chart_style();

}  // namespace marketscan::app::panels

#endif  // MARKETSCAN_APP_PANELS_EQUITY_CHART_HPP
