#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include <imgui.h>

#include "../theme.hpp"
#include "marketscan/util/logging.hpp"
#include "panels.hpp"

namespace marketscan::app::panels {
namespace {

[[nodiscard]] unsigned int level_color(LogLevel level) {
    const Palette& palette = theme::colors();
    switch (level) {
        case MS_LOG_DEBUG:
            return palette.text_faint;
        case MS_LOG_INFO:
            return palette.text_dim;
        case MS_LOG_WARN:
            return palette.warning;
        case MS_LOG_ERROR:
            return palette.danger;
    }
    return palette.text;
}

[[nodiscard]] bool contains_ignoring_case(const std::string& haystack, const char* needle) {
    if (needle == nullptr || needle[0] == '\0') {
        return true;
    }
    const std::string lowered_needle = [needle] {
        std::string s(needle);
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        return s;
    }();
    std::string lowered = haystack;
    std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return lowered.find(lowered_needle) != std::string::npos;
}

}  // namespace

void log_console(Context& context) {
    if (!context.ui.show_log) {
        return;
    }
    if (!ImGui::Begin("Log", &context.ui.show_log)) {
        ImGui::End();
        return;
    }

    UiState& ui = context.ui;

    // --- Filters --------------------------------------------------------
    const char* levels[] = {"debug", "info", "warn", "error"};
    int level_index = static_cast<int>(ui.log_level);
    ImGui::SetNextItemWidth(90.0F);
    if (ImGui::Combo("##level", &level_index, levels, IM_ARRAYSIZE(levels))) {
        ui.log_level = static_cast<LogLevel>(level_index);
    }

    ImGui::SameLine();
    // Sources are the theory ids plus the subsystems, so a run with five
    // theories can be narrowed to the one misbehaving.
    std::vector<std::string> sources{"(all)"};
    for (const StrategySnapshot& row : context.snapshot.strategies) {
        sources.push_back(row.id);
    }
    for (const char* subsystem : {"app", "engine", "session", "loader"}) {
        sources.emplace_back(subsystem);
    }

    std::string current = ui.log_source_filter.empty() ? "(all)" : ui.log_source_filter;
    ImGui::SetNextItemWidth(160.0F);
    if (ImGui::BeginCombo("##source", current.c_str())) {
        for (const std::string& source : sources) {
            const bool selected = source == current;
            if (ImGui::Selectable(source.c_str(), selected)) {
                ui.log_source_filter = source == "(all)" ? std::string{} : source;
            }
        }
        ImGui::EndCombo();
    }

    ImGui::SameLine();
    ImGui::SetNextItemWidth(220.0F);
    ImGui::InputTextWithHint("##search", "search", ui.log_search, sizeof ui.log_search);

    ImGui::SameLine();
    ImGui::Checkbox("Autoscroll", &ui.log_autoscroll);

    ImGui::SameLine();
    if (ImGui::SmallButton("Clear")) {
        log::buffer().clear();
    }

    const std::uint64_t dropped = log::buffer().dropped();
    if (dropped > 0) {
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, theme::colors().text_faint);
        ImGui::Text("%s older lines dropped", format_count(dropped).c_str());
        ImGui::PopStyleColor();
        help_marker("The log is a fixed-size ring. A replay at ten thousand times "
                    "speed can produce millions of lines a second, and keeping them "
                    "all would be a memory leak with good intentions.");
    }

    ImGui::Separator();

    // --- Lines ----------------------------------------------------------
    if (ImGui::BeginChild(
            "##lines", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar)) {
        const std::vector<log::Record> records = log::buffer().snapshot();

        ImGui::PushFont(theme::mono_font(), 0.0F);
        ImGuiListClipper clipper;

        // Filtering first, so the clipper's row count matches what is drawn.
        std::vector<const log::Record*> visible;
        visible.reserve(records.size());
        for (const log::Record& record : records) {
            if (record.level < ui.log_level) {
                continue;
            }
            if (!ui.log_source_filter.empty() && record.source != ui.log_source_filter) {
                continue;
            }
            if (!contains_ignoring_case(record.message, ui.log_search) &&
                !contains_ignoring_case(record.source, ui.log_search)) {
                continue;
            }
            visible.push_back(&record);
        }

        // Only the rows on screen are laid out. Without this, a full ring of
        // twenty thousand lines costs a measurable slice of the frame budget
        // every frame, for text nobody is looking at.
        clipper.Begin(static_cast<int>(visible.size()));
        while (clipper.Step()) {
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                const log::Record& record = *visible[static_cast<std::size_t>(i)];

                ImGui::PushStyleColor(ImGuiCol_Text, theme::colors().text_faint);
                ImGui::TextUnformatted(format_time_of_day(record.ts).c_str());
                ImGui::PopStyleColor();

                ImGui::SameLine();
                ImGui::PushStyleColor(ImGuiCol_Text, level_color(record.level));
                ImGui::Text("%-5s", std::string(log::to_string(record.level)).c_str());
                ImGui::PopStyleColor();

                ImGui::SameLine();
                ImGui::PushStyleColor(ImGuiCol_Text, theme::colors().accent);
                ImGui::Text("%-16s", record.source.c_str());
                ImGui::PopStyleColor();

                ImGui::SameLine();
                ImGui::TextUnformatted(record.message.c_str());
            }
        }
        clipper.End();
        ImGui::PopFont();

        // Only stick to the bottom while already there, so scrolling back to
        // read something is not undone by the next line arriving.
        if (ui.log_autoscroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0F) {
            ImGui::SetScrollHereY(1.0F);
        }
    }
    ImGui::EndChild();

    ImGui::End();
}

}  // namespace marketscan::app::panels
