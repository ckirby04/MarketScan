#ifndef MARKETSCAN_APP_CHART_MODEL_HPP
#define MARKETSCAN_APP_CHART_MODEL_HPP

/*
 * What the equity chart draws, decided once for both view modes.
 *
 * THERE IS EXACTLY ONE EQUITY CHART IN THIS APPLICATION
 *
 * Not an Overview chart and an Operator chart. One model, built here, rendered
 * by one draw function at one of two densities. Two chart implementations will
 * drift, and the drift shows up in a screenshot before it shows up in a bug
 * report -- one mode annualising slightly differently from the other, or
 * repainting a theory a different colour, and nobody noticing until the picture
 * is already in somebody else's inbox.
 *
 * WHAT DENSITY MAY CHANGE, AND WHAT IT MAY NOT
 *
 * May: line weight, whether a non-emphasised series keeps its categorical hue or
 * drops to the de-emphasis gray, how many series carry a direct label, legend
 * placement, chrome.
 *
 * May not: the underlying data, **which entity holds which colour when
 * coloured**, the axis scale and normalisation, or the geometry of any curve. A
 * curve that looks different between modes is a bug, not a style.
 *
 * Both halves are asserted in tests/test_view_modes.cpp, which is the reason
 * this file is data rather than drawing and lives in the half of the application
 * that does not link ImGui.
 *
 * NOTHING HERE COMPUTES A METRIC
 *
 * Every number is read from the snapshot. If a view needs one that is not there,
 * it goes **into the snapshot** so both modes read the same object -- see
 * `StrategySnapshot::excess_return`, which exists because Overview's hero figure
 * needed it and computing it in the view would have been the first crack.
 */

#include <cstddef>
#include <string>
#include <vector>

#include "series_colors.hpp"
#include "session.hpp"

namespace marketscan::app {

/// How much the chart is asked to show at once.
enum class ChartDensity : std::uint8_t {
    /// Legible from across a room. One series is the subject, the rest are
    /// context in gray, every line heavier, few labels.
    Overview = 0,

    /// The working view. Full categorical palette, thinner lines, a legend, and
    /// every series treated as a subject -- because here they are.
    Operator = 1,
};

/// One curve.
struct ChartSeries {
    std::string id;
    std::string label;

    /// Borrowed from the snapshot the model was built against, which must
    /// outlive it. Seconds since the epoch, and the value series, in step.
    const std::vector<double>* times = nullptr;
    const std::vector<double>* values = nullptr;

    /// The colour this entity owns. **Identical in both densities**: it is a
    /// property of the theory, not of how it is being drawn.
    unsigned int identity_color = 0;

    /// What is actually stroked. Equals `identity_color`, or the de-emphasis
    /// gray when this series is context in a mode that greys its context.
    unsigned int drawn_color = 0;

    float line_weight = 2.0F;

    bool emphasized = false;
    bool is_benchmark = false;

    /// Past the eighth categorical slot: shares the folded gray and is named
    /// "Other" rather than given a ninth hue.
    bool folded = false;

    /// Whether this series carries a label drawn against the curve itself.
    bool direct_label = false;

    [[nodiscard]] std::size_t points() const noexcept;
};

/// The whole chart, as data.
struct ChartModel {
    std::vector<ChartSeries> series;

    /// Rebased to starting capital. **One y-axis, always.** A dual-axis chart is
    /// not an acceptable output of this milestone under any framing, so there is
    /// no second scale to configure and no field here that could introduce one.
    bool normalised = true;
    bool log_scale = false;

    std::string y_label;

    /// Marker at the engine's current instant, in the same units as `times`.
    /// Negative when the run has not started.
    double now_seconds = -1.0;

    ChartDensity density = ChartDensity::Operator;

    /// Theories folded into "Other", for a legend that should say so once
    /// rather than repeat a gray swatch.
    std::size_t folded_count = 0;

    [[nodiscard]] bool empty() const noexcept { return series.empty(); }

    /// The emphasised series, or null. Overview draws its labels and its
    /// drawdown strip from this.
    [[nodiscard]] const ChartSeries* emphasis() const noexcept;

    [[nodiscard]] const ChartSeries* benchmark() const noexcept;
};

/// What the chart is being asked to draw, apart from the data.
struct ChartRequest {
    ChartDensity density = ChartDensity::Operator;

    /// Which theory is the subject. Empty means the first non-benchmark one.
    ///
    /// **Emphasis is a chart feature, not a mode feature.** Operator follows the
    /// selected leaderboard row and Overview follows its own selector, and the
    /// chart does not know or care which asked.
    std::string emphasis_id;

    bool normalised = true;
    bool log_scale = false;
    bool show_benchmark = true;

    /// Theory ids the legend has switched off. Hiding one must not repaint any
    /// other -- that is what `SeriesColors` is for, and what the view-mode tests
    /// check.
    std::vector<std::string> hidden;
};

/// Builds the model. Pure: same snapshot and request in, same model out.
[[nodiscard]] ChartModel build_chart_model(const SessionSnapshot& snapshot,
                                           const SeriesColors& colors,
                                           const ChartRequest& request);

}  // namespace marketscan::app

#endif  // MARKETSCAN_APP_CHART_MODEL_HPP
