// Drawdown, relative strength and the correlation matrix.
//
// Grouped because they are the same shape of thing: one selected theory drawn
// against the benchmark, plus one heatmap.

#include <algorithm>
#include <cmath>
#include <vector>

#include <imgui.h>
#include <implot.h>

#include "../theme.hpp"
#include "marketscan/metrics/performance.hpp"
#include "panels.hpp"

namespace marketscan::app::panels {
namespace {

/// Zeros the same length as a series, so a shaded region has something to fill
/// down to. ImPlot's PlotShaded needs an explicit baseline when the fill runs
/// to a value rather than to the axis.
const std::vector<double>& zeros_for(std::size_t count) {
    static std::vector<double> zeros;
    if (zeros.size() < count) {
        zeros.assign(count, 0.0);
    }
    return zeros;
}

void empty_message(const char* text) {
    ImGui::PushStyleColor(ImGuiCol_Text, theme::colors().text_dim);
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
}

}  // namespace

// ---------------------------------------------------------------------------
// Drawdown
// ---------------------------------------------------------------------------

void drawdown(Context& context) {
    if (!context.ui.show_drawdown) {
        return;
    }
    if (!ImGui::Begin("Drawdown", &context.ui.show_drawdown)) {
        ImGui::End();
        return;
    }

    const SessionSnapshot& snapshot = context.snapshot;
    if (snapshot.strategies.empty()) {
        empty_message("No theories loaded.");
        ImGui::End();
        return;
    }

    const std::size_t index = context.ui.selected_index(snapshot);
    const StrategySnapshot& row = snapshot.strategies[index];

    ImGui::Text("%s", row.display_name.c_str());
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, theme::colors().text_dim);
    ImGui::Text("   worst %s",
                metrics::is_defined(row.summary.max_drawdown)
                    ? format_percent(-row.summary.max_drawdown).c_str()
                    : "--");
    ImGui::PopStyleColor();
    help_marker("How far below its own high-water mark the account is, at every "
                "point. This is the number that decides whether a strategy is "
                "survivable: a 60% drawdown needs a 150% gain to get back, and most "
                "people stop before that.");

    if (ImPlot::BeginPlot("##drawdown", ImVec2(-1, -1))) {
        // Fitted for the same reason as the equity chart: a shallow drawdown
        // against a zero-anchored axis is invisible.
        ImPlot::SetupAxes("Time", "Below peak", ImPlotAxisFlags_AutoFit, ImPlotAxisFlags_AutoFit);
        ImPlot::SetupAxisScale(ImAxis_X1, ImPlotScale_Time);
        ImPlot::SetupAxisFormat(ImAxis_Y1, "%.1f%%");
        ImPlot::SetupLegend(ImPlotLocation_SouthWest, ImPlotLegendFlags_None);

        const auto plot_one = [&](const StrategySnapshot& series, unsigned int color, bool fill) {
            if (series.times.size() < 2 || series.drawdown.size() < 2) {
                return;
            }
            // Percentages rather than fractions: an axis labelled -0.12 is
            // read as twelve of something, and it is not.
            std::vector<double> percent;
            percent.reserve(series.drawdown.size());
            for (const double value : series.drawdown) {
                percent.push_back(value * 100.0);
            }
            const int count = static_cast<int>(std::min(series.times.size(), percent.size()));

            ImPlotSpec spec;
            spec.LineColor = ImGui::ColorConvertU32ToFloat4(color);
            spec.FillColor = ImGui::ColorConvertU32ToFloat4(color);
            spec.LineWeight = fill ? 1.8F : 1.2F;

            if (fill) {
                spec.FillAlpha = 0.22F;
                ImPlot::PlotShaded(series.display_name.c_str(),
                                   series.times.data(),
                                   percent.data(),
                                   zeros_for(static_cast<std::size_t>(count)).data(),
                                   count,
                                   spec);
            }
            ImPlot::PlotLine(
                series.display_name.c_str(), series.times.data(), percent.data(), count, spec);
        };

        // The benchmark underneath, dimmer, so "worse than just holding it" is
        // visible at a glance rather than something to be worked out.
        if (snapshot.benchmark_index != static_cast<std::size_t>(-1) &&
            snapshot.benchmark_index != index) {
            plot_one(
                snapshot.strategies[snapshot.benchmark_index], theme::colors().text_faint, false);
        }
        plot_one(row, SeriesColors(snapshot).color(row.id), true);

        ImPlot::EndPlot();
    }
    ImGui::End();
}

// ---------------------------------------------------------------------------
// Relative strength
// ---------------------------------------------------------------------------

void relative_strength(Context& context) {
    if (!context.ui.show_relative) {
        return;
    }
    if (!ImGui::Begin("Relative Strength", &context.ui.show_relative)) {
        ImGui::End();
        return;
    }

    const SessionSnapshot& snapshot = context.snapshot;
    if (snapshot.strategies.empty()) {
        empty_message("No theories loaded.");
        ImGui::End();
        return;
    }
    if (snapshot.benchmark_index == static_cast<std::size_t>(-1)) {
        empty_message("No benchmark loaded. Relative strength needs something to be "
                      "relative to; buy_and_hold is the default.");
        ImGui::End();
        return;
    }

    const std::size_t index = context.ui.selected_index(snapshot);
    const StrategySnapshot& row = snapshot.strategies[index];
    const StrategySnapshot& benchmark = snapshot.strategies[snapshot.benchmark_index];

    ImGui::Text("%s  /  %s", row.display_name.c_str(), benchmark.display_name.c_str());
    help_marker("The selected theory divided by the benchmark, both rebased to 1.\n\n"
                "Rising means it is pulling ahead; falling means it is losing ground. "
                "A flat stretch means the two moved together, which an equity curve "
                "hides completely -- two lines both going up look like two successes.");

    if (ImPlot::BeginPlot("##relative", ImVec2(-1, -1))) {
        ImPlot::SetupAxes("Time", "Ratio", ImPlotAxisFlags_AutoFit, ImPlotAxisFlags_AutoFit);
        ImPlot::SetupAxisScale(ImAxis_X1, ImPlotScale_Time);
        ImPlot::SetupLegend(ImPlotLocation_NorthWest, ImPlotLegendFlags_None);

        if (row.relative.size() >= 2 && row.times.size() >= 2) {
            const int count = static_cast<int>(std::min(row.times.size(), row.relative.size()));

            // A line at 1.0: above it the theory is ahead, below it behind.
            const ImPlotRect limits = ImPlot::GetPlotLimits();
            double parity_x[2] = {limits.X.Min, limits.X.Max};
            double parity_y[2] = {1.0, 1.0};
            ImPlotSpec parity;
            parity.LineColor = ImGui::ColorConvertU32ToFloat4(theme::colors().text_faint);
            parity.LineWeight = 1.0F;
            ImPlot::PlotLine("parity", parity_x, parity_y, 2, parity);

            ImPlotSpec spec;
            spec.LineColor = ImGui::ColorConvertU32ToFloat4(SeriesColors(snapshot).color(row.id));
            spec.LineWeight = 2.0F;
            ImPlot::PlotLine(
                row.display_name.c_str(), row.times.data(), row.relative.data(), count, spec);
        } else {
            ImPlot::Annotation(0.5,
                               0.5,
                               ImGui::ColorConvertU32ToFloat4(theme::colors().text_dim),
                               ImVec2(0, 0),
                               false,
                               "Not enough data yet");
        }
        ImPlot::EndPlot();
    }
    ImGui::End();
}

// ---------------------------------------------------------------------------
// Correlation matrix
// ---------------------------------------------------------------------------

void correlation_matrix(Context& context) {
    if (!context.ui.show_correlation) {
        return;
    }
    if (!ImGui::Begin("Correlation", &context.ui.show_correlation)) {
        ImGui::End();
        return;
    }

    const metrics::CorrelationMatrix& matrix = context.snapshot.correlation;

    ImGui::TextUnformatted("Return correlation");
    help_marker("How much each pair of theories moves together, period by period.\n\n"
                "Near-useless with two theories and load-bearing with twenty: two "
                "strategies with good Sharpe ratios and a correlation of 0.95 are one "
                "strategy wearing two hats, and stacking them doubles the risk without "
                "doubling anything else.");

    if (matrix.size() < 2) {
        empty_message("At least two theories with return history are needed before there "
                      "is anything to correlate.");
        ImGui::End();
        return;
    }

    const int n = static_cast<int>(matrix.size());

    // Row-major, ImPlot wants a flat array.
    std::vector<double> values;
    values.reserve(matrix.values().size());
    for (const double value : matrix.values()) {
        // A heatmap cannot draw NaN. Undefined pairs -- a theory that never
        // traded has no returns to correlate -- are shown as zero and called
        // out in the readout below rather than left as a blank hole.
        values.push_back(metrics::is_defined(value) ? value : 0.0);
    }

    std::vector<const char*> labels;
    labels.reserve(matrix.labels().size());
    for (const std::string& label : matrix.labels()) {
        labels.push_back(label.c_str());
    }

    ImPlot::PushColormap(ImPlotColormap_RdBu);
    if (ImPlot::BeginPlot(
            "##correlation", ImVec2(-1, -40), ImPlotFlags_NoLegend | ImPlotFlags_NoMouseText)) {
        constexpr ImPlotAxisFlags kAxisFlags =
            ImPlotAxisFlags_Lock | ImPlotAxisFlags_NoGridLines | ImPlotAxisFlags_NoTickMarks;
        ImPlot::SetupAxes(nullptr, nullptr, kAxisFlags, kAxisFlags);
        ImPlot::SetupAxisTicks(
            ImAxis_X1, 0.0 + 1.0 / (2.0 * n), 1.0 - 1.0 / (2.0 * n), n, labels.data());
        ImPlot::SetupAxisTicks(
            ImAxis_Y1, 1.0 - 1.0 / (2.0 * n), 0.0 + 1.0 / (2.0 * n), n, labels.data());

        const ImPlotSpec spec;
        ImPlot::PlotHeatmap("##cells",
                            values.data(),
                            n,
                            n,
                            -1.0,  // scale_min: perfectly anti-correlated
                            1.0,   // scale_max: perfectly correlated
                            "%.2f",
                            ImPlotPoint(0, 0),
                            ImPlotPoint(1, 1),
                            spec);
        ImPlot::EndPlot();
    }
    ImPlot::ColormapScale("##scale", -1.0, 1.0, ImVec2(60, -40));
    ImPlot::PopColormap();

    std::size_t row = 0;
    std::size_t column = 0;
    double value = 0.0;
    if (matrix.most_correlated(row, column, value) && value > 0.8) {
        ImGui::PushStyleColor(ImGuiCol_Text, theme::colors().warning);
        ImGui::TextWrapped("%s and %s move together (%.2f) -- close to one idea, not two.",
                           matrix.labels()[row].c_str(),
                           matrix.labels()[column].c_str(),
                           value);
        ImGui::PopStyleColor();
    }

    ImGui::End();
}

}  // namespace marketscan::app::panels
