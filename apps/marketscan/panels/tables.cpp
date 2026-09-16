// The trade blotter and the positions panel.

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

#include <imgui.h>

#include "../theme.hpp"
#include "panels.hpp"

namespace marketscan::app::panels {
namespace {

[[nodiscard]] bool matches(const std::string& haystack, const char* needle) {
    if (needle == nullptr || needle[0] == '\0') {
        return true;
    }
    const auto lower = [](std::string s) {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        return s;
    };
    return lower(haystack).find(lower(std::string(needle))) != std::string::npos;
}

/// One blotter row, plus which theory it came from.
struct BlotterRow {
    const TradeRecord* record;
    const StrategySnapshot* strategy;
    std::size_t strategy_index;
};

}  // namespace

// ---------------------------------------------------------------------------
// Trade blotter
// ---------------------------------------------------------------------------

void trade_blotter(Context& context) {
    if (!context.ui.show_blotter) {
        return;
    }
    if (!ImGui::Begin("Blotter", &context.ui.show_blotter)) {
        ImGui::End();
        return;
    }

    const SessionSnapshot& snapshot = context.snapshot;
    UiState& ui = context.ui;

    ImGui::Checkbox("Selected theory only", &ui.blotter_selected_only);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(220.0F);
    ImGui::InputTextWithHint(
        "##search", "symbol or theory", ui.blotter_search, sizeof ui.blotter_search);

    // Gather first, so the totals below describe exactly what is shown.
    const std::size_t selected = ui.selected_index(snapshot);
    std::vector<BlotterRow> rows;
    Money total_slippage;
    Money total_commission;

    for (std::size_t i = 0; i < snapshot.strategies.size(); ++i) {
        if (ui.blotter_selected_only && i != selected) {
            continue;
        }
        const StrategySnapshot& strategy = snapshot.strategies[i];
        for (const TradeRecord& record : strategy.blotter) {
            const std::string symbol = record.symbol > 0 && record.symbol <= snapshot.symbols.size()
                                           ? snapshot.symbols[record.symbol - 1]
                                           : std::string("?");
            if (!matches(symbol, ui.blotter_search) &&
                !matches(strategy.display_name, ui.blotter_search)) {
                continue;
            }
            rows.push_back(BlotterRow{&record, &strategy, i});
            total_slippage += record.slippage;
            total_commission += record.commission;
        }
    }

    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, theme::colors().text_dim);
    ImGui::Text("   %zu fills    slippage %s    commission %s",
                rows.size(),
                format_money(total_slippage).c_str(),
                format_money(total_commission).c_str());
    ImGui::PopStyleColor();

    constexpr ImGuiTableFlags kFlags =
        ImGuiTableFlags_Resizable | ImGuiTableFlags_Reorderable | ImGuiTableFlags_Hideable |
        ImGuiTableFlags_Sortable | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersOuter |
        ImGuiTableFlags_BordersV | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp;

    if (ImGui::BeginTable("##blotter", 10, kFlags)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Time", ImGuiTableColumnFlags_WidthStretch, 1.6F);
        ImGui::TableSetupColumn("Theory", ImGuiTableColumnFlags_WidthStretch, 1.4F);
        ImGui::TableSetupColumn("Symbol", ImGuiTableColumnFlags_WidthStretch, 0.8F);
        ImGui::TableSetupColumn("Side", ImGuiTableColumnFlags_WidthStretch, 0.6F);
        ImGui::TableSetupColumn("Qty", ImGuiTableColumnFlags_WidthStretch, 0.8F);
        ImGui::TableSetupColumn("Signal", ImGuiTableColumnFlags_WidthStretch, 1.0F);
        ImGui::TableSetupColumn("Fill", ImGuiTableColumnFlags_WidthStretch, 1.0F);
        ImGui::TableSetupColumn("Slippage", ImGuiTableColumnFlags_WidthStretch, 1.0F);
        ImGui::TableSetupColumn("Commission", ImGuiTableColumnFlags_WidthStretch, 1.0F);
        ImGui::TableSetupColumn("Realized", ImGuiTableColumnFlags_WidthStretch, 1.0F);
        ImGui::TableHeadersRow();

        if (ImGui::TableGetColumnFlags(7) & ImGuiTableColumnFlags_IsHovered) {
            ImGui::SetTooltip("What the fill cost against the price the strategy could see "
                              "when it decided.\n\n"
                              "This has its own column because separating \"the signal was "
                              "wrong\" from \"I could not get filled\" is the entire point "
                              "of simulating execution. Positive is a cost.");
        }

        // Newest first by default: a running blotter is read from the top.
        if (const ImGuiTableSortSpecs* specs = ImGui::TableGetSortSpecs();
            specs != nullptr && specs->SpecsCount > 0 &&
            specs->Specs[0].SortDirection == ImGuiSortDirection_Ascending) {
            // ascending: leave as gathered
        } else {
            std::reverse(rows.begin(), rows.end());
        }

        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(rows.size()));
        while (clipper.Step()) {
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                const BlotterRow& row = rows[static_cast<std::size_t>(i)];
                const TradeRecord& record = *row.record;

                ImGui::TableNextRow();

                ImGui::TableSetColumnIndex(0);
                mono_text(format_timestamp(record.ts));

                ImGui::TableSetColumnIndex(1);
                ImGui::PushStyleColor(ImGuiCol_Text, theme::series_color(row.strategy_index));
                ImGui::TextUnformatted(row.strategy->display_name.c_str());
                ImGui::PopStyleColor();

                ImGui::TableSetColumnIndex(2);
                mono_text(record.symbol > 0 && record.symbol <= snapshot.symbols.size()
                              ? snapshot.symbols[record.symbol - 1]
                              : "?");

                ImGui::TableSetColumnIndex(3);
                ImGui::PushStyleColor(ImGuiCol_Text,
                                      record.side == MS_SIDE_BUY ? theme::colors().profit
                                                                 : theme::colors().loss);
                mono_text(std::string(marketscan::to_string(record.side)));
                ImGui::PopStyleColor();

                ImGui::TableSetColumnIndex(4);
                right_text(format_quantity(record.quantity));

                ImGui::TableSetColumnIndex(5);
                right_text(format_price(record.reference_price));

                ImGui::TableSetColumnIndex(6);
                right_text(format_price(record.fill_price));

                ImGui::TableSetColumnIndex(7);
                // Positive slippage is a cost, so it is coloured as a loss --
                // the opposite of the sign convention used for P&L.
                right_text(format_money(record.slippage, true),
                           theme::pnl_color(-record.slippage.to_double()));

                ImGui::TableSetColumnIndex(8);
                right_text(format_money(record.commission), theme::colors().text_dim);

                ImGui::TableSetColumnIndex(9);
                if (record.realized_pnl.is_zero()) {
                    right_text("--", theme::colors().text_faint);
                } else {
                    money_cell(record.realized_pnl);
                }
            }
        }
        clipper.End();
        ImGui::EndTable();
    }

    ImGui::End();
}

// ---------------------------------------------------------------------------
// Positions and P&L
// ---------------------------------------------------------------------------

void positions(Context& context) {
    if (!context.ui.show_positions) {
        return;
    }
    if (!ImGui::Begin("Positions & P&L", &context.ui.show_positions)) {
        ImGui::End();
        return;
    }

    const SessionSnapshot& snapshot = context.snapshot;
    if (snapshot.strategies.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, theme::colors().text_dim);
        ImGui::TextUnformatted("No theories loaded.");
        ImGui::PopStyleColor();
        ImGui::End();
        return;
    }

    const std::size_t index = context.ui.selected_index(snapshot);
    const StrategySnapshot& row = snapshot.strategies[index];

    // --- Account summary -------------------------------------------------
    if (ImGui::BeginTable(
            "##account", 4, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_BordersInnerV)) {
        const auto cell = [](const char* label, const std::string& value, unsigned int color) {
            ImGui::TableNextColumn();
            ImGui::PushStyleColor(ImGuiCol_Text, theme::colors().text_dim);
            ImGui::TextUnformatted(label);
            ImGui::PopStyleColor();
            ImGui::PushFont(theme::large_font(), 0.0F);
            ImGui::PushStyleColor(ImGuiCol_Text, color);
            ImGui::TextUnformatted(value.c_str());
            ImGui::PopStyleColor();
            ImGui::PopFont();
        };

        ImGui::TableNextRow();
        cell("Equity", format_money(row.equity), theme::colors().text);
        cell("Cash", format_money(row.cash), theme::colors().text);
        cell("Realized",
             format_money(row.realized_pnl, true),
             theme::pnl_color(row.realized_pnl.to_double()));
        cell("Unrealized",
             format_money(row.unrealized_pnl, true),
             theme::pnl_color(row.unrealized_pnl.to_double()));
        ImGui::EndTable();
    }

    ImGui::Separator();

    if (row.positions.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, theme::colors().text_dim);
        ImGui::TextUnformatted("Flat -- no open positions.");
        ImGui::PopStyleColor();
        ImGui::End();
        return;
    }

    constexpr ImGuiTableFlags kFlags = ImGuiTableFlags_Resizable | ImGuiTableFlags_RowBg |
                                       ImGuiTableFlags_BordersOuter | ImGuiTableFlags_BordersV |
                                       ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp;

    if (ImGui::BeginTable("##positions", 6, kFlags)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Symbol");
        ImGui::TableSetupColumn("Quantity");
        ImGui::TableSetupColumn("Avg cost");
        ImGui::TableSetupColumn("Mark");
        ImGui::TableSetupColumn("Market value");
        ImGui::TableSetupColumn("Unrealized");
        ImGui::TableHeadersRow();

        for (const Position& position : row.positions) {
            ImGui::TableNextRow();

            ImGui::TableSetColumnIndex(0);
            mono_text(position.symbol > 0 && position.symbol <= snapshot.symbols.size()
                          ? snapshot.symbols[position.symbol - 1]
                          : "?");

            ImGui::TableSetColumnIndex(1);
            // Short positions are negative and coloured, because a short is
            // not a small long and misreading one is expensive.
            right_text(format_quantity(position.quantity),
                       position.quantity < 0.0 ? theme::colors().loss : theme::colors().text);

            ImGui::TableSetColumnIndex(2);
            right_text(format_price(position.average_cost()));

            ImGui::TableSetColumnIndex(3);
            right_text(format_price(position.mark));

            ImGui::TableSetColumnIndex(4);
            right_text(format_money(position.market_value()));

            ImGui::TableSetColumnIndex(5);
            money_cell(position.unrealized_pnl());
        }
        ImGui::EndTable();
    }

    ImGui::End();
}

}  // namespace marketscan::app::panels
