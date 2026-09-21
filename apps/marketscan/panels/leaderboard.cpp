#include <algorithm>
#include <vector>

#include <imgui.h>

#include "../theme.hpp"
#include "marketscan/metrics/performance.hpp"
#include "panels.hpp"

namespace marketscan::app::panels {
namespace {

enum Column : int {
    kName = 0,
    kEquity,
    kDayPnl,
    kTotalReturn,
    kSharpe,
    kMaxDrawdown,
    kTrades,
    kInformationRatio,
    kColumnCount,
};

/// Sorts by whichever column the header says, most-relevant-first.
///
/// Undefined statistics sort to the bottom in either direction. A NaN compares
/// false against everything, so leaving it to the comparator would scatter
/// them through the table according to whatever order the rows started in.
void sort_rows(std::vector<std::size_t>& order,
               const std::vector<StrategySnapshot>& rows,
               int column,
               bool ascending) {
    const auto value_of = [&](std::size_t index) -> double {
        const StrategySnapshot& row = rows[index];
        switch (column) {
            case kEquity:
                return row.equity.to_double();
            case kDayPnl:
                return row.day_pnl.to_double();
            case kTotalReturn:
                return row.summary.total_return;
            case kSharpe:
                return row.summary.sharpe;
            case kMaxDrawdown:
                return row.summary.max_drawdown;
            case kTrades:
                return static_cast<double>(row.trades);
            case kInformationRatio:
                return row.summary.information_ratio;
            default:
                return 0.0;
        }
    };

    std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
        if (column == kName) {
            return ascending ? rows[a].display_name < rows[b].display_name
                             : rows[b].display_name < rows[a].display_name;
        }
        const double left = value_of(a);
        const double right = value_of(b);

        const bool left_ok = metrics::is_defined(left);
        const bool right_ok = metrics::is_defined(right);
        if (left_ok != right_ok) {
            return left_ok;  // anything defined outranks anything that is not
        }
        if (!left_ok) {
            return false;
        }
        return ascending ? left < right : right < left;
    });
}

}  // namespace

void leaderboard(Context& context) {
    if (!context.ui.show_leaderboard) {
        return;
    }
    if (!ImGui::Begin("Leaderboard", &context.ui.show_leaderboard)) {
        ImGui::End();
        return;
    }

    const SessionSnapshot& snapshot = context.snapshot;

    // Colour follows the entity, so the swatch beside a row matches the chart
    // whatever the table is sorted by and whatever is hidden.
    const SeriesColors colors(snapshot);
    UiState& ui = context.ui;

    if (snapshot.strategies.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, theme::colors().text_dim);
        ImGui::TextWrapped("No theories loaded. Build one into theories/ and use "
                           "Theories > Rescan, or see theories/README.md.");
        ImGui::PopStyleColor();
        ImGui::End();
        return;
    }

    constexpr ImGuiTableFlags kFlags =
        ImGuiTableFlags_Resizable | ImGuiTableFlags_Reorderable | ImGuiTableFlags_Hideable |
        ImGuiTableFlags_Sortable | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersOuter |
        ImGuiTableFlags_BordersV | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp;

    if (ImGui::BeginTable("##leaderboard", kColumnCount, kFlags)) {
        ImGui::TableSetupScrollFreeze(1, 1);
        ImGui::TableSetupColumn(
            "Theory", ImGuiTableColumnFlags_DefaultSort | ImGuiTableColumnFlags_WidthStretch, 2.0F);
        ImGui::TableSetupColumn("Equity", ImGuiTableColumnFlags_WidthStretch, 1.3F);
        ImGui::TableSetupColumn("Day P&L", ImGuiTableColumnFlags_WidthStretch, 1.1F);
        ImGui::TableSetupColumn("Return", ImGuiTableColumnFlags_WidthStretch, 1.0F);
        ImGui::TableSetupColumn("Sharpe", ImGuiTableColumnFlags_WidthStretch, 0.9F);
        ImGui::TableSetupColumn("Max DD", ImGuiTableColumnFlags_WidthStretch, 0.9F);
        ImGui::TableSetupColumn("Trades", ImGuiTableColumnFlags_WidthStretch, 0.8F);
        ImGui::TableSetupColumn("IR", ImGuiTableColumnFlags_WidthStretch, 0.8F);
        ImGui::TableHeadersRow();

        // Tooltips on the headers that need one. A leaderboard whose columns
        // have to be guessed at is a leaderboard that gets misread.
        if (ImGui::TableGetColumnFlags(kSharpe) & ImGuiTableColumnFlags_IsHovered) {
            ImGui::SetTooltip("Annualised return per unit of volatility.\n"
                              "A dash means the run is too short, or never moved, to have "
                              "one -- which is not the same as zero.");
        }
        if (ImGui::TableGetColumnFlags(kInformationRatio) & ImGuiTableColumnFlags_IsHovered) {
            ImGui::SetTooltip("Active return over tracking error, against the benchmark.\n"
                              "The benchmark's own IR is undefined: it does not beat itself.");
        }

        std::vector<std::size_t> order(snapshot.strategies.size());
        for (std::size_t i = 0; i < order.size(); ++i) {
            order[i] = i;
        }
        if (const ImGuiTableSortSpecs* specs = ImGui::TableGetSortSpecs();
            specs != nullptr && specs->SpecsCount > 0) {
            sort_rows(order,
                      snapshot.strategies,
                      specs->Specs[0].ColumnUserID != 0
                          ? static_cast<int>(specs->Specs[0].ColumnUserID)
                          : specs->Specs[0].ColumnIndex,
                      specs->Specs[0].SortDirection == ImGuiSortDirection_Ascending);
        }

        for (const std::size_t index : order) {
            const StrategySnapshot& row = snapshot.strategies[index];
            ImGui::TableNextRow();
            ImGui::PushID(static_cast<int>(index));

            // --- Theory ---
            ImGui::TableSetColumnIndex(kName);
            const bool selected = row.id == ui.selected_theory;

            // A colour swatch matching the chart line, so a row and a curve
            // can be connected without reading either label.
            const ImVec2 cursor = ImGui::GetCursorScreenPos();
            const float height = ImGui::GetTextLineHeight();
            ImGui::GetWindowDrawList()->AddRectFilled(
                ImVec2(cursor.x, cursor.y + 2.0F),
                ImVec2(cursor.x + 3.0F, cursor.y + height - 2.0F),
                colors.color(row.id));
            ImGui::Dummy(ImVec2(8.0F, 0.0F));
            ImGui::SameLine();

            if (ImGui::Selectable(row.display_name.c_str(),
                                  selected,
                                  ImGuiSelectableFlags_SpanAllColumns |
                                      ImGuiSelectableFlags_AllowOverlap)) {
                ui.selected_theory = row.id;
            }
            if (row.is_benchmark) {
                ImGui::SameLine();
                ImGui::PushStyleColor(ImGuiCol_Text, theme::colors().text_faint);
                ImGui::TextUnformatted("(benchmark)");
                ImGui::PopStyleColor();
            }
            if (row.rejected > 0) {
                ImGui::SameLine();
                ImGui::PushStyleColor(ImGuiCol_Text, theme::colors().warning);
                ImGui::Text("%llu rejected", static_cast<unsigned long long>(row.rejected));
                ImGui::PopStyleColor();
                help_marker("Orders this theory submitted that were refused -- almost "
                            "always because this is a cash account and the buy would "
                            "have overdrawn it.");
            }

            ImGui::TableSetColumnIndex(kEquity);
            right_text(format_money(row.equity));

            ImGui::TableSetColumnIndex(kDayPnl);
            money_cell(row.day_pnl);

            ImGui::TableSetColumnIndex(kTotalReturn);
            right_text(format_percent(row.summary.total_return),
                       theme::pnl_color(row.summary.total_return));

            ImGui::TableSetColumnIndex(kSharpe);
            metric_cell(row.summary.sharpe);

            ImGui::TableSetColumnIndex(kMaxDrawdown);
            right_text(metrics::is_defined(row.summary.max_drawdown)
                           ? format_percent(-row.summary.max_drawdown)
                           : "--",
                       row.summary.max_drawdown > 0.0 ? theme::colors().loss
                                                      : theme::colors().text_dim);

            ImGui::TableSetColumnIndex(kTrades);
            right_text(format_count(row.trades));

            ImGui::TableSetColumnIndex(kInformationRatio);
            metric_cell(row.summary.information_ratio);

            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    ImGui::End();
}

}  // namespace marketscan::app::panels
