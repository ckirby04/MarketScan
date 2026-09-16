#include <algorithm>
#include <vector>

#include <imgui.h>
#include <implot.h>

#include "../theme.hpp"
#include "panels.hpp"

namespace marketscan::app::panels {

void latency(Context& context) {
    if (!context.ui.show_latency) {
        return;
    }
    if (!ImGui::Begin("Latency", &context.ui.show_latency)) {
        ImGui::End();
        return;
    }

    const LatencyHistogram& histogram = context.snapshot.latency;

    // Said plainly and first. These numbers are a property of this machine,
    // not of any market, and a latency panel that does not say so invites
    // exactly the wrong conclusion.
    ImGui::PushStyleColor(ImGuiCol_Text, theme::colors().text_faint);
    ImGui::TextWrapped("Time from an event reaching a strategy to its callback returning -- "
                       "which is where an order is submitted, priced and booked. In replay "
                       "this measures this process, not the market. It is instrumented from "
                       "the start so the same path is already measured when it matters.");
    ImGui::PopStyleColor();

    ImGui::Separator();

    if (histogram.count() == 0) {
        ImGui::PushStyleColor(ImGuiCol_Text, theme::colors().text_dim);
        ImGui::TextUnformatted("Nothing measured yet. Press play.");
        ImGui::PopStyleColor();
        ImGui::End();
        return;
    }

    // --- Percentiles -----------------------------------------------------
    if (ImGui::BeginTable("##percentiles",
                          6,
                          ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_BordersInnerV)) {
        const auto cell = [](const char* label, const std::string& value, unsigned int color) {
            ImGui::TableNextColumn();
            ImGui::PushStyleColor(ImGuiCol_Text, theme::colors().text_dim);
            ImGui::TextUnformatted(label);
            ImGui::PopStyleColor();
            ImGui::PushFont(theme::mono_font(), 0.0F);
            ImGui::PushStyleColor(ImGuiCol_Text, color);
            ImGui::TextUnformatted(value.c_str());
            ImGui::PopStyleColor();
            ImGui::PopFont();
        };

        ImGui::TableNextRow();
        cell("p50", format_duration(histogram.p50()), theme::colors().text);
        cell("p90", format_duration(histogram.p90()), theme::colors().text);
        cell("p99", format_duration(histogram.p99()), theme::colors().warning);
        cell("p99.9", format_duration(histogram.p999()), theme::colors().danger);
        cell("max", format_duration(histogram.max()), theme::colors().danger);
        cell("samples", format_count(histogram.count()), theme::colors().text_dim);
        ImGui::EndTable();
    }

    ImGui::PushStyleColor(ImGuiCol_Text, theme::colors().text_faint);
    ImGui::TextWrapped("The tail is the part that matters. A mean of %s says nothing about "
                       "the one event in a thousand that took %s.",
                       format_duration(static_cast<Duration>(histogram.mean())).c_str(),
                       format_duration(histogram.p999()).c_str());
    ImGui::PopStyleColor();

    ImGui::Separator();

    // --- Distribution ----------------------------------------------------
    const std::vector<LatencyHistogram::Bar> bars = histogram.bars();
    if (bars.empty()) {
        ImGui::End();
        return;
    }

    std::vector<double> centres;
    std::vector<double> counts;
    centres.reserve(bars.size());
    counts.reserve(bars.size());
    for (const LatencyHistogram::Bar& bar : bars) {
        // Plotted at the bucket's midpoint on a log axis, because the buckets
        // themselves are logarithmic: drawing them evenly spaced would make a
        // handful of slow events look like half the distribution.
        centres.push_back(0.5 * static_cast<double>(bar.lower_ns + bar.upper_ns));
        counts.push_back(static_cast<double>(bar.count));
    }

    if (ImPlot::BeginPlot("##latency", ImVec2(-1, -1), ImPlotFlags_NoLegend)) {
        ImPlot::SetupAxes("Latency (ns)", "Events", ImPlotAxisFlags_None, ImPlotAxisFlags_AutoFit);
        ImPlot::SetupAxisScale(ImAxis_X1, ImPlotScale_Log10);
        ImPlot::SetupAxisScale(ImAxis_Y1, ImPlotScale_Log10);

        ImPlotSpec spec;
        spec.FillColor = ImGui::ColorConvertU32ToFloat4(theme::colors().accent);
        spec.FillAlpha = 0.75F;
        spec.LineColor = ImGui::ColorConvertU32ToFloat4(theme::colors().accent);

        ImPlot::PlotStems(
            "events", centres.data(), counts.data(), static_cast<int>(centres.size()), 0.5, spec);

        // Percentile markers, so the table above and the shape below can be
        // read against each other.
        const ImPlotRect limits = ImPlot::GetPlotLimits();
        const auto mark = [&](std::int64_t value, unsigned int color) {
            if (value <= 0) {
                return;
            }
            double xs[2] = {static_cast<double>(value), static_cast<double>(value)};
            double ys[2] = {limits.Y.Min, limits.Y.Max};
            ImPlotSpec line;
            line.LineColor = ImGui::ColorConvertU32ToFloat4(color);
            line.LineWeight = 1.2F;
            line.Flags = ImPlotItemFlags_NoLegend;
            ImPlot::PlotLine("##mark", xs, ys, 2, line);
        };
        mark(histogram.p50(), theme::colors().text_faint);
        mark(histogram.p99(), theme::colors().warning);
        mark(histogram.p999(), theme::colors().danger);

        ImPlot::EndPlot();
    }

    ImGui::End();
}

}  // namespace marketscan::app::panels
