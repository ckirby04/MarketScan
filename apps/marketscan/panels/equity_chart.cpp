#include "equity_chart.hpp"

#include <algorithm>
#include <cmath>

#include <imgui.h>
#include <implot.h>

#include "../theme.hpp"
#include "panels.hpp"

namespace marketscan::app::panels {
namespace {

[[nodiscard]] ImVec4 to_vec(unsigned int color) {
    return ImGui::ColorConvertU32ToFloat4(color);
}

/// Pins the plot's own surface and chrome to the token table.
///
/// ImPlot's default style paints its own background, and contrast and
/// colour-vision results are only meaningful against the surface actually
/// rendered on -- so the surface is set rather than inherited.
void push_chart_style(ChartDensity density) {
    const Palette& palette = theme::colors();
    ImPlot::PushStyleColor(ImPlotCol_PlotBg, to_vec(palette.chart_surface));
    ImPlot::PushStyleColor(ImPlotCol_FrameBg, to_vec(palette.chart_surface));
    ImPlot::PushStyleColor(ImPlotCol_AxisGrid, to_vec(palette.gridline));
    ImPlot::PushStyleColor(ImPlotCol_AxisText, to_vec(palette.text_faint));
    ImPlot::PushStyleColor(ImPlotCol_AxisBg, to_vec(palette.chart_surface));

    // Recessive chrome: hairline gridlines, no border round the plot.
    ImPlot::PushStyleVar(ImPlotStyleVar_PlotBorderSize, 0.0F);
    ImPlot::PushStyleVar(ImPlotStyleVar_MajorGridSize, ImVec2(1.0F, 1.0F));
    ImPlot::PushStyleVar(ImPlotStyleVar_MinorGridSize, ImVec2(1.0F, 1.0F));

    // Overview breathes; Operator packs.
    const float padding = density == ChartDensity::Overview ? 14.0F : 8.0F;
    ImPlot::PushStyleVar(ImPlotStyleVar_PlotPadding, ImVec2(padding, padding));
}

void pop_chart_style() {
    ImPlot::PopStyleVar(4);
    ImPlot::PopStyleColor(5);
}

}  // namespace

std::string draw_equity_chart(const ChartModel& model, const char* plot_id, ImVec2 size) {
    std::string clicked;

    push_chart_style(model.density);
    if (!ImPlot::BeginPlot(plot_id, size, ImPlotFlags_NoTitle)) {
        pop_chart_style();
        return clicked;
    }

    const bool overview = model.density == ChartDensity::Overview;

    // Fitted, always. A normalised curve sits a percent or two either side of
    // 1.0, and an axis starting at zero renders every theory as one flat line --
    // which is not a chart, it is a claim that nothing happened.
    //
    // ONE Y-AXIS. There is no second scale here and no way to configure one: a
    // dual-axis chart is not an acceptable output of this milestone under any
    // framing, so the model carries no field that could introduce one.
    const ImPlotAxisFlags fit = ImPlotAxisFlags_AutoFit;
    ImPlot::SetupAxes(
        overview ? nullptr : "Time", overview ? nullptr : model.y_label.c_str(), fit, fit);
    ImPlot::SetupAxisScale(ImAxis_X1, ImPlotScale_Time);
    if (model.log_scale) {
        ImPlot::SetupAxisScale(ImAxis_Y1, ImPlotScale_Log10);
    }

    if (!overview) {
        ImPlot::SetupLegend(ImPlotLocation_NorthWest, ImPlotLegendFlags_None);
    }
    // Overview sets up no legend at all: it labels the subject and the benchmark
    // against their own curves instead. A legend is a lookup table between a
    // colour and a name that the viewer has to perform, and this is the view
    // meant to be read from across a room.

    for (const ChartSeries& series : model.series) {
        const std::size_t points = series.points();
        if (points < 2) {
            continue;
        }
        ImPlotSpec spec;
        spec.LineColor = to_vec(series.drawn_color);
        spec.LineWeight = series.line_weight;
        if (overview) {
            // No key in Overview: every series stays out of it, and the two that
            // matter carry direct labels instead. In Operator the legend is also
            // the filter control, so entries stay.
            spec.Flags = ImPlotItemFlags_NoLegend;
        }

        ImPlot::PlotLine(series.label.c_str(),
                         series.times->data(),
                         series.values->data(),
                         static_cast<int>(points),
                         spec);

        if (series.direct_label) {
            // Against the end of its own curve, in the series' identity colour.
            const double x = (*series.times)[points - 1];
            const double y = (*series.values)[points - 1];
            ImPlot::Annotation(x,
                               y,
                               to_vec(series.identity_color),
                               ImVec2(-6.0F, -6.0F),
                               false,
                               "%s",
                               series.label.c_str());
        }

        if (ImPlot::IsLegendEntryHovered(series.label.c_str()) &&
            ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            clicked = series.id;
        }
    }

    // Where the replay has got to, so it is obvious rather than inferred from
    // where the lines stop.
    if (model.now_seconds > 0.0 && !model.series.empty()) {
        ImPlotSpec marker;
        marker.LineColor = to_vec(theme::colors().baseline);
        marker.LineWeight = 1.0F;
        // NoFit is the important flag: drawn as a segment spanning the current
        // limits it would be counted towards the fit, and the fit would then
        // reproduce those limits forever.
        marker.Flags = ImPlotItemFlags_NoLegend | ImPlotItemFlags_NoFit;
        double xs[1] = {model.now_seconds};
        ImPlot::PlotInfLines("##now", xs, 1, marker);
    }

    if (model.series.empty()) {
        ImPlot::Annotation(0.5,
                           0.5,
                           to_vec(theme::colors().text_dim),
                           ImVec2(0, 0),
                           false,
                           "Press play to run the replay");
    }

    ImPlot::EndPlot();
    pop_chart_style();
    return clicked;
}

void draw_drawdown_strip(const ChartModel& model,
                         const SessionSnapshot& snapshot,
                         const char* plot_id,
                         ImVec2 size) {
    const ChartSeries* subject = model.emphasis();
    if (subject == nullptr) {
        return;
    }

    // The series to draw, found by id in the snapshot. Read, never derived: the
    // underwater curve is already computed once, in the session.
    const auto find = [&snapshot](const std::string& id) -> const StrategySnapshot* {
        for (const StrategySnapshot& row : snapshot.strategies) {
            if (row.id == id) {
                return &row;
            }
        }
        return nullptr;
    };

    const StrategySnapshot* row = find(subject->id);
    if (row == nullptr || row->drawdown.size() < 2) {
        return;
    }

    push_chart_style(model.density);
    if (!ImPlot::BeginPlot(plot_id, size, ImPlotFlags_NoTitle | ImPlotFlags_NoLegend)) {
        pop_chart_style();
        return;
    }

    ImPlot::SetupAxes(nullptr, nullptr, ImPlotAxisFlags_AutoFit, ImPlotAxisFlags_AutoFit);
    ImPlot::SetupAxisScale(ImAxis_X1, ImPlotScale_Time);

    const auto plot = [](const StrategySnapshot& source, unsigned int color, float weight) {
        const std::size_t points = std::min(source.times.size(), source.drawdown.size());
        if (points < 2) {
            return;
        }
        ImPlotSpec spec;
        spec.LineColor = ImGui::ColorConvertU32ToFloat4(color);
        spec.LineWeight = weight;
        spec.FillAlpha = 0.20F;
        ImPlot::PlotShaded("##dd",
                           source.times.data(),
                           source.drawdown.data(),
                           static_cast<int>(points),
                           0.0,
                           spec);
        ImPlot::PlotLine("##ddline",
                         source.times.data(),
                         source.drawdown.data(),
                         static_cast<int>(points),
                         spec);
    };

    // The benchmark first, so the subject draws over it.
    if (const ChartSeries* benchmark = model.benchmark(); benchmark != nullptr) {
        if (const StrategySnapshot* bench = find(benchmark->id); bench != nullptr) {
            plot(*bench, benchmark->drawn_color, 1.2F);
        }
    }
    plot(*row, subject->identity_color, 2.0F);

    ImPlot::EndPlot();
    pop_chart_style();
}

}  // namespace marketscan::app::panels
