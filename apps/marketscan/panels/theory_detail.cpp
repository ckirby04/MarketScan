#include <algorithm>
#include <vector>

#include <imgui.h>
#include <implot.h>

#include "../theme.hpp"
#include "marketscan/metrics/performance.hpp"
#include "panels.hpp"

namespace marketscan::app::panels {
namespace {

void statistic(const char* label, const std::string& value, unsigned int color) {
    ImGui::TableNextColumn();
    ImGui::PushStyleColor(ImGuiCol_Text, theme::colors().text_dim);
    ImGui::TextUnformatted(label);
    ImGui::PopStyleColor();
    ImGui::SameLine();
    right_text(value, color);
}

}  // namespace

void theory_detail(Context& context) {
    if (!context.ui.show_detail) {
        return;
    }
    if (!ImGui::Begin("Theory Detail", &context.ui.show_detail)) {
        ImGui::End();
        return;
    }

    const SessionSnapshot& snapshot = context.snapshot;
    UiState& ui = context.ui;

    if (snapshot.strategies.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, theme::colors().text_dim);
        ImGui::TextUnformatted("No theories loaded.");
        ImGui::PopStyleColor();
        ImGui::End();
        return;
    }

    const std::size_t index = ui.selected_index(snapshot);
    const StrategySnapshot& row = snapshot.strategies[index];

    // --- Which theory ----------------------------------------------------
    ImGui::SetNextItemWidth(-1);
    if (ImGui::BeginCombo("##which", row.display_name.c_str())) {
        for (const StrategySnapshot& candidate : snapshot.strategies) {
            if (ImGui::Selectable(candidate.display_name.c_str(), candidate.id == row.id)) {
                ui.selected_theory = candidate.id;
            }
        }
        ImGui::EndCombo();
    }

    if (!row.author.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, theme::colors().text_faint);
        ImGui::Text("%s  ·  %s", row.id.c_str(), row.author.c_str());
        ImGui::PopStyleColor();
    }

    ImGui::Separator();

    // --- Statistics ------------------------------------------------------
    if (ImGui::BeginTable("##stats", 2, ImGuiTableFlags_SizingStretchSame)) {
        ImGui::TableNextRow();
        statistic("Equity", format_money(row.equity), theme::colors().text);
        statistic("Return",
                  format_percent(row.summary.total_return),
                  theme::pnl_color(row.summary.total_return));

        ImGui::TableNextRow();
        statistic("CAGR", format_percent(row.summary.cagr), theme::pnl_color(row.summary.cagr));
        statistic("Volatility", format_percent(row.summary.volatility), theme::colors().text);

        ImGui::TableNextRow();
        statistic("Sharpe", format_metric(row.summary.sharpe), theme::colors().text);
        statistic("Sortino", format_metric(row.summary.sortino), theme::colors().text);

        ImGui::TableNextRow();
        statistic("Max drawdown",
                  metrics::is_defined(row.summary.max_drawdown)
                      ? format_percent(-row.summary.max_drawdown)
                      : "--",
                  theme::colors().loss);
        statistic("Calmar", format_metric(row.summary.calmar), theme::colors().text);

        ImGui::TableNextRow();
        statistic("Beta", format_metric(row.summary.beta), theme::colors().text);
        statistic("Alpha", format_percent(row.summary.alpha), theme::pnl_color(row.summary.alpha));

        ImGui::TableNextRow();
        statistic("Info ratio", format_metric(row.summary.information_ratio), theme::colors().text);
        statistic("Tracking err", format_percent(row.summary.tracking_error), theme::colors().text);

        ImGui::TableNextRow();
        statistic("Trades", format_count(row.trades), theme::colors().text);
        statistic("Rejected",
                  format_count(row.rejected),
                  row.rejected > 0 ? theme::colors().warning : theme::colors().text_dim);

        ImGui::TableNextRow();
        statistic("Commission", format_money(row.commission), theme::colors().text_dim);
        statistic("Slippage", format_money(row.slippage), theme::colors().loss);
        ImGui::EndTable();
    }

    // A low r-squared means the alpha and beta above are describing very
    // little, and quoting them without saying so would be misleading.
    if (metrics::is_defined(row.summary.r_squared) && row.summary.r_squared < 0.3) {
        ImGui::PushStyleColor(ImGuiCol_Text, theme::colors().text_faint);
        ImGui::TextWrapped("The benchmark explains only %.0f%% of this theory's variance, so "
                           "its alpha and beta describe very little.",
                           row.summary.r_squared * 100.0);
        ImGui::PopStyleColor();
    }

    // --- Equity ----------------------------------------------------------
    if (ImGui::CollapsingHeader("Equity", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (ImPlot::BeginPlot(
                "##detail_equity", ImVec2(-1, 180), ImPlotFlags_NoLegend | ImPlotFlags_NoTitle)) {
            ImPlot::SetupAxes(nullptr, nullptr, ImPlotAxisFlags_NoTickLabels, ImPlotAxisFlags_None);
            ImPlot::SetupAxisScale(ImAxis_X1, ImPlotScale_Time);
            if (row.times.size() >= 2) {
                ImPlotSpec spec;
                spec.LineColor =
                    ImGui::ColorConvertU32ToFloat4(SeriesColors(snapshot).color(row.id));
                spec.LineWeight = 2.0F;
                ImPlot::PlotLine(
                    "equity",
                    row.times.data(),
                    row.equity_curve.data(),
                    static_cast<int>(std::min(row.times.size(), row.equity_curve.size())),
                    spec);
            }
            ImPlot::EndPlot();
        }
    }

    // --- Published metrics ----------------------------------------------
    if (ImGui::CollapsingHeader("Published metrics", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (row.published.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, theme::colors().text_dim);
            ImGui::TextWrapped("This theory publishes nothing. Call publish_metric(name, value) "
                               "and it appears here without any host change.");
            ImGui::PopStyleColor();
        } else if (ImGui::BeginTable("##published",
                                     3,
                                     ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                                         ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("Metric", ImGuiTableColumnFlags_WidthStretch, 1.6F);
            ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch, 1.0F);
            ImGui::TableSetupColumn("Updates", ImGuiTableColumnFlags_WidthStretch, 0.8F);
            ImGui::TableHeadersRow();
            for (const PublishedMetric& metric : row.published) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextUnformatted(metric.name.c_str());
                ImGui::TableSetColumnIndex(1);
                right_text(format_metric(metric.value, 4));
                ImGui::TableSetColumnIndex(2);
                right_text(format_count(metric.updates), theme::colors().text_faint);
            }
            ImGui::EndTable();
        }
    }

    // --- Live parameters -------------------------------------------------
    if (ImGui::CollapsingHeader("Parameters", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (row.params.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, theme::colors().text_dim);
            ImGui::TextWrapped("This theory exposes no parameters.");
            ImGui::PopStyleColor();
        } else {
            ImGui::PushStyleColor(ImGuiCol_Text, theme::colors().text_faint);
            ImGui::TextWrapped("Controls are generated from the theory's own MS_ParamDesc "
                               "array. The host knows nothing about what any of them mean.");
            ImGui::PopStyleColor();
            ImGui::Spacing();

            for (std::size_t i = 0; i < row.params.size(); ++i) {
                const ParamInfo& param = row.params[i];
                const double current =
                    i < row.param_values.size() ? row.param_values[i] : param.default_value;

                ImGui::PushID(static_cast<int>(i));
                bool changed = false;
                double next = current;

                switch (param.type) {
                    case MS_PARAM_BOOL: {
                        bool on = current != 0.0;
                        if (ImGui::Checkbox(param.name.c_str(), &on)) {
                            next = on ? 1.0 : 0.0;
                            changed = true;
                        }
                        break;
                    }
                    case MS_PARAM_INT: {
                        int value = static_cast<int>(current);
                        if (ImGui::SliderInt(param.name.c_str(),
                                             &value,
                                             static_cast<int>(param.min_value),
                                             static_cast<int>(param.max_value))) {
                            next = static_cast<double>(value);
                            changed = true;
                        }
                        break;
                    }
                    case MS_PARAM_ENUM: {
                        std::vector<const char*> labels;
                        labels.reserve(param.enum_labels.size());
                        for (const std::string& label : param.enum_labels) {
                            labels.push_back(label.c_str());
                        }
                        int value = static_cast<int>(current);
                        if (!labels.empty() && ImGui::Combo(param.name.c_str(),
                                                            &value,
                                                            labels.data(),
                                                            static_cast<int>(labels.size()))) {
                            next = static_cast<double>(value);
                            changed = true;
                        }
                        break;
                    }
                    case MS_PARAM_DOUBLE:
                    default: {
                        float value = static_cast<float>(current);
                        if (ImGui::SliderFloat(param.name.c_str(),
                                               &value,
                                               static_cast<float>(param.min_value),
                                               static_cast<float>(param.max_value),
                                               "%.4f")) {
                            next = static_cast<double>(value);
                            changed = true;
                        }
                        break;
                    }
                }

                if (!param.description.empty()) {
                    help_marker(param.description.c_str());
                }
                if (changed) {
                    context.session.set_parameter(row.id, param.name, next);
                }
                ImGui::PopID();
            }

            ImGui::Spacing();
            ImGui::PushStyleColor(ImGuiCol_Text, theme::colors().text_faint);
            ImGui::TextWrapped("Changing a parameter takes effect from the next event. It does "
                               "not re-run what has already happened, so the curve above is a "
                               "mixture of both settings -- restart for a clean comparison.");
            ImGui::PopStyleColor();
        }
    }

    ImGui::End();
}

}  // namespace marketscan::app::panels
