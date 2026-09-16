#include <algorithm>
#include <cmath>

#include <imgui.h>
#include <implot.h>

#include "../theme.hpp"
#include "panels.hpp"

namespace marketscan::app::panels {
namespace {

/// Every theory that has anything to draw yet.
[[nodiscard]] bool has_series(const StrategySnapshot& row) {
    return row.times.size() >= 2;
}

}  // namespace

void equity_curves(Context& context) {
    if (!context.ui.show_equity) {
        return;
    }
    if (!ImGui::Begin("Equity Curves", &context.ui.show_equity)) {
        ImGui::End();
        return;
    }

    UiState& ui = context.ui;
    const SessionSnapshot& snapshot = context.snapshot;

    // --- Controls -------------------------------------------------------
    ImGui::Checkbox("Normalise", &ui.equity_normalised);
    help_marker("Rebase every curve to its own starting capital, so two theories "
                "funded differently can be compared by shape. Off shows the actual "
                "account values.");

    ImGui::SameLine();
    ImGui::Checkbox("Log scale", &ui.equity_log_scale);
    help_marker("A logarithmic axis makes equal percentage moves the same height "
                "anywhere on the chart. On a linear axis a 10% move looks larger at "
                "the top than at the bottom, which flatters whatever went up first.");

    ImGui::SameLine();
    ImGui::Checkbox("Benchmark", &ui.equity_show_benchmark);

    if (!snapshot.fill_model_realistic) {
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, theme::colors().warning);
        ImGui::TextUnformatted("  optimistic fills - not a result");
        ImGui::PopStyleColor();
    }

    // --- Plot -----------------------------------------------------------
    const char* y_label = ui.equity_normalised ? "Growth of 1" : "Equity";

    if (ImPlot::BeginPlot("##equity", ImVec2(-1, -1))) {
        ImPlot::SetupAxes("Time", y_label, ImPlotAxisFlags_None, ImPlotAxisFlags_None);
        // The x axis is seconds since the Unix epoch, which is exactly what
        // ImPlot's time formatter expects, so the ticks read as clock times
        // rather than as a count of seconds.
        ImPlot::SetupAxisScale(ImAxis_X1, ImPlotScale_Time);
        if (ui.equity_log_scale) {
            ImPlot::SetupAxisScale(ImAxis_Y1, ImPlotScale_Log10);
        }
        ImPlot::SetupLegend(ImPlotLocation_NorthWest, ImPlotLegendFlags_None);

        bool drew_anything = false;
        for (std::size_t i = 0; i < snapshot.strategies.size(); ++i) {
            const StrategySnapshot& row = snapshot.strategies[i];
            if (!has_series(row)) {
                continue;
            }
            if (row.is_benchmark && !ui.equity_show_benchmark) {
                continue;
            }
            if (ui.is_hidden(row.id)) {
                continue;
            }

            const std::vector<double>& values =
                ui.equity_normalised ? row.normalised : row.equity_curve;

            // The benchmark is drawn dimmer and thinner: it is the thing being
            // compared against, not one of the things being compared.
            const unsigned int color =
                row.is_benchmark ? theme::colors().text_faint : theme::series_color(i);

            ImPlotSpec spec;
            spec.LineColor = ImGui::ColorConvertU32ToFloat4(color);
            spec.LineWeight = row.is_benchmark ? 1.4F : 2.0F;

            std::string label = row.display_name;
            if (row.is_benchmark) {
                label += " (benchmark)";
            }
            ImPlot::PlotLine(label.c_str(),
                             row.times.data(),
                             values.data(),
                             static_cast<int>(std::min(row.times.size(), values.size())),
                             spec);

            // Clicking a curve focuses that theory everywhere else.
            if (ImPlot::IsLegendEntryHovered(label.c_str()) &&
                ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                ui.selected_theory = row.id;
            }
            drew_anything = true;
        }

        // A marker at the current engine time, so it is obvious where the
        // replay has got to rather than having to infer it from the line end.
        if (snapshot.clock > 0 && drew_anything) {
            const double now =
                static_cast<double>(snapshot.clock) / static_cast<double>(time::kSecond);
            ImPlotSpec marker;
            marker.LineColor = ImGui::ColorConvertU32ToFloat4(theme::colors().accent_dim);
            marker.LineWeight = 1.0F;
            marker.Flags = ImPlotItemFlags_NoLegend;

            double xs[2] = {now, now};
            const ImPlotRect limits = ImPlot::GetPlotLimits();
            double ys[2] = {limits.Y.Min, limits.Y.Max};
            ImPlot::PlotLine("##now", xs, ys, 2, marker);
        }

        if (!drew_anything) {
            ImPlot::Annotation(0.5,
                               0.5,
                               ImGui::ColorConvertU32ToFloat4(theme::colors().text_dim),
                               ImVec2(0, 0),
                               false,
                               "Press play to run the replay");
        }
        ImPlot::EndPlot();
    }

    ImGui::End();
}

}  // namespace marketscan::app::panels
