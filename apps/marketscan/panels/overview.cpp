// Overview -- one thing at a time, legible from across a room.
//
// The job of this view is a single question: *is this working, and is it beating
// the market?* Everything that does not answer that question is not here, and
// the entire value of the view is what it leaves out. No blotter, no positions,
// no order book, no latency histogram, no log, no correlation matrix, no plugin
// manager. If somebody needs those they press F2.
//
// It is not a lite version of Operator. It is a different question being asked
// of the same running engine, and both read the same published snapshot.
//
// NOTHING HERE COMPUTES A NUMBER
//
// Every figure is a field read off the snapshot. The hero figure is excess
// return, which is a subtraction -- and that subtraction happens in
// `Session::publish`, not here, because two views subtracting independently is
// how one ends up showing 12.4% while the other shows 12.1% and nobody notices
// until it is in a screenshot somebody sent somebody else.
//
// The chart is the same `EquityChart` Operator draws, at a different density.
// There is no second chart implementation and there must never be one.

#include <algorithm>
#include <cmath>
#include <string>

#include <imgui.h>

#include "../theme.hpp"
#include "equity_chart.hpp"
#include "panels.hpp"

namespace marketscan::app::panels {
namespace {

[[nodiscard]] ImVec4 to_vec(unsigned int color) {
    return ImGui::ColorConvertU32ToFloat4(color);
}

// Every percentage here goes through `format_percent`, which formats with "%+"
// and therefore always carries an explicit sign.
//
// This view briefly had its own signed_percent() wrapper on top of it, which
// produced "-+13.06%" -- a sign added to a string that already had one. The
// spec's requirement is that a P&L always carries a sign, not that every view
// implements its own signing; one shared formatter satisfies it, and a second
// one is how two views end up disagreeing about how a number looks.

/// The hero figure: excess return against the benchmark, and nothing else that
/// size.
///
/// A number, not a gauge, not a dial, not a one-bar chart. The thing the entire
/// project exists to determine, in the largest type on the screen.
void draw_hero(const StrategySnapshot& subject, const StrategySnapshot* benchmark) {
    const double excess = subject.excess_return;
    const unsigned int color = theme::pnl_color(excess);

    ImGui::PushFont(theme::hero_font());
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::TextUnformatted(format_percent(excess).c_str());
    ImGui::PopStyleColor();
    ImGui::PopFont();

    ImGui::PushStyleColor(ImGuiCol_Text, theme::colors().text_dim);
    ImGui::TextUnformatted("excess return vs benchmark");
    ImGui::PopStyleColor();

    // The absolutes underneath, in secondary ink: context for the headline, not
    // competitors to it.
    ImGui::PushStyleColor(ImGuiCol_Text, theme::colors().text_faint);
    if (benchmark != nullptr) {
        ImGui::Text("%s %s   -   benchmark %s",
                    subject.display_name.c_str(),
                    format_percent(subject.summary.total_return).c_str(),
                    format_percent(benchmark->summary.total_return).c_str());
    } else {
        ImGui::Text("%s %s   -   no benchmark loaded",
                    subject.display_name.c_str(),
                    format_percent(subject.summary.total_return).c_str());
    }
    ImGui::PopStyleColor();
}

/// One stat tile. Label in muted ink, value large in primary ink.
void tile(const char* label,
          const std::string& value,
          const char* subtitle,
          unsigned int value_color = 0) {
    const Palette& palette = theme::colors();
    ImGui::BeginGroup();

    ImGui::PushStyleColor(ImGuiCol_Text, palette.text_faint);
    ImGui::TextUnformatted(label);
    ImGui::PopStyleColor();

    ImGui::PushFont(theme::mono_font());
    ImGui::PushStyleColor(ImGuiCol_Text, value_color != 0 ? value_color : palette.text);
    ImGui::TextUnformatted(value.c_str());
    ImGui::PopStyleColor();
    ImGui::PopFont();

    if (subtitle != nullptr && subtitle[0] != '\0') {
        ImGui::PushStyleColor(ImGuiCol_Text, palette.text_faint);
        ImGui::TextUnformatted(subtitle);
        ImGui::PopStyleColor();
    }
    ImGui::EndGroup();
}

/// Exactly four. Not six, not eight.
void draw_kpis(const StrategySnapshot& subject) {
    const metrics::PerformanceSummary& summary = subject.summary;

    const float width = ImGui::GetContentRegionAvail().x / 4.0F;
    const auto column = [width](int index) {
        if (index > 0) {
            ImGui::SameLine(static_cast<float>(index) * width);
        }
    };

    column(0);
    tile("Information ratio",
         format_metric(summary.information_ratio),
         "vs benchmark",
         theme::pnl_color(summary.information_ratio));

    column(1);
    tile("Sharpe", format_metric(summary.sharpe), "annualised", theme::pnl_color(summary.sharpe));

    column(2);
    // A drawdown is a loss. It is shown negative, with the sign, rather than as
    // a bare positive number a reader has to know to read as a fall.
    const double drawdown =
        std::isfinite(summary.max_drawdown) ? -summary.max_drawdown : summary.max_drawdown;
    tile("Max drawdown",
         format_percent(drawdown, 1),
         "peak to trough",
         std::isfinite(drawdown) && drawdown < 0.0 ? theme::colors().loss : 0U);

    column(3);
    std::string period = "over ";
    if (std::isfinite(summary.years) && summary.years > 0.0) {
        period += format_metric(summary.years, 2) + " years";
    } else {
        period += "the run";
    }
    tile("Trades", format_count(static_cast<std::uint64_t>(subject.trades)), period.c_str());
}

/// The one interactive control. Nothing else in this view is clickable.
void draw_selector(Context& context, const SessionSnapshot& snapshot) {
    std::size_t current = context.ui.selected_index(snapshot);
    if (current < snapshot.strategies.size() && snapshot.strategies[current].is_benchmark) {
        for (std::size_t i = 0; i < snapshot.strategies.size(); ++i) {
            if (!snapshot.strategies[i].is_benchmark) {
                current = i;
                break;
            }
        }
    }
    if (current >= snapshot.strategies.size()) {
        return;
    }

    ImGui::SetNextItemWidth(260.0F);
    if (ImGui::BeginCombo("##strategy", snapshot.strategies[current].display_name.c_str())) {
        for (const StrategySnapshot& row : snapshot.strategies) {
            if (row.is_benchmark) {
                continue;
            }
            const bool selected = row.id == snapshot.strategies[current].id;
            if (ImGui::Selectable(row.display_name.c_str(), selected)) {
                context.ui.selected_theory = row.id;
            }
            if (selected) {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }
}

}  // namespace

void overview(Context& context) {
    const SessionSnapshot& snapshot = context.snapshot;
    const Palette& palette = theme::colors();

    if (snapshot.strategies.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, palette.text_dim);
        ImGui::TextUnformatted("No theories loaded.");
        ImGui::PopStyleColor();
        return;
    }

    // Which theory is the subject, and the colours every series will wear.
    // Built from the whole snapshot, so hiding or reordering cannot repaint it.
    const SeriesColors colors(snapshot);

    std::size_t index = context.ui.selected_index(snapshot);
    if (index < snapshot.strategies.size() && snapshot.strategies[index].is_benchmark) {
        for (std::size_t i = 0; i < snapshot.strategies.size(); ++i) {
            if (!snapshot.strategies[i].is_benchmark) {
                index = i;
                break;
            }
        }
    }
    if (index >= snapshot.strategies.size()) {
        return;
    }
    const StrategySnapshot& subject = snapshot.strategies[index];
    const StrategySnapshot* benchmark = snapshot.benchmark_index < snapshot.strategies.size()
                                            ? &snapshot.strategies[snapshot.benchmark_index]
                                            : nullptr;

    // --- Hero and selector, on one line ---------------------------------
    ImGui::BeginGroup();
    draw_hero(subject, benchmark);
    ImGui::EndGroup();

    ImGui::SameLine();
    const float right = ImGui::GetContentRegionAvail().x - 270.0F;
    if (right > 0.0F) {
        ImGui::SameLine(ImGui::GetCursorPosX() + right);
    }
    ImGui::BeginGroup();
    ImGui::PushStyleColor(ImGuiCol_Text, palette.text_faint);
    ImGui::TextUnformatted("showing");
    ImGui::PopStyleColor();
    draw_selector(context, snapshot);
    ImGui::EndGroup();

    if (!snapshot.fill_model_realistic) {
        ImGui::PushStyleColor(ImGuiCol_Text, palette.status_warning);
        ImGui::TextUnformatted("! optimistic fills - not a result");
        ImGui::PopStyleColor();
    }

    ImGui::Spacing();

    // --- The chart, and the strip sharing its x-axis ---------------------
    ChartRequest request;
    request.density = ChartDensity::Overview;
    request.emphasis_id = subject.id;
    request.normalised = true;  // Overview always compares shape, never dollars.
    request.log_scale = false;
    request.show_benchmark = true;
    request.hidden = context.ui.hidden_series;

    const ChartModel model = build_chart_model(snapshot, colors, request);

    const float available = ImGui::GetContentRegionAvail().y;
    const float strip_height = std::max(90.0F, available * 0.22F);
    const float kpi_height = ImGui::GetTextLineHeightWithSpacing() * 3.4F;
    const float chart_height = std::max(160.0F, available - strip_height - kpi_height - 24.0F);

    const std::string clicked =
        draw_equity_chart(model, "##overview_equity", ImVec2(-1.0F, chart_height));
    if (!clicked.empty()) {
        context.ui.selected_theory = clicked;
    }

    ImGui::PushStyleColor(ImGuiCol_Text, palette.text_faint);
    ImGui::TextUnformatted("drawdown");
    ImGui::PopStyleColor();
    draw_drawdown_strip(model, snapshot, "##overview_drawdown", ImVec2(-1.0F, strip_height));

    ImGui::Spacing();
    draw_kpis(subject);
}

}  // namespace marketscan::app::panels
