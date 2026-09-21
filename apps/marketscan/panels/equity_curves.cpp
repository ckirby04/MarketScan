#include <algorithm>
#include <cmath>

#include <imgui.h>
#include <implot.h>

#include "../theme.hpp"
#include "equity_chart.hpp"
#include "panels.hpp"

namespace marketscan::app::panels {
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
    //
    // Through the shared component, at Operator density. There is exactly one
    // equity chart in this application and this is it, drawn a second time --
    // not a second chart that happens to look similar.
    //
    // Everything that could differ between the two modes was decided in
    // `build_chart_model`, which is pure and compared by tests/test_view_modes.cpp.
    // What is left here is a window, four checkboxes and a call.
    const SeriesColors colors(snapshot);

    ChartRequest chart;
    chart.density = ChartDensity::Operator;
    chart.emphasis_id = ui.selected_theory;
    chart.normalised = ui.equity_normalised;
    chart.log_scale = ui.equity_log_scale;
    chart.show_benchmark = ui.equity_show_benchmark;
    chart.hidden = ui.hidden_series;

    const ChartModel model = build_chart_model(snapshot, colors, chart);
    const std::string clicked = draw_equity_chart(model, "##equity", ImVec2(-1, -1));
    if (!clicked.empty()) {
        ui.selected_theory = clicked;
    }

    ImGui::End();
}

}  // namespace marketscan::app::panels
