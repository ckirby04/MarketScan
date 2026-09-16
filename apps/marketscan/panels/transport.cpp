#include <algorithm>
#include <cmath>
#include <vector>

#include <imgui.h>
#include <implot.h>

#include "../theme.hpp"
#include "panels.hpp"

namespace marketscan::app::panels {
namespace {

struct SpeedPreset {
    const char* label;
    double multiplier;
};

/// "max" is zero, meaning unthrottled. It is the useful default for research:
/// waiting out a day of market data in real time is not analysis.
constexpr SpeedPreset kSpeeds[] = {
    {"1x", 1.0},
    {"10x", 10.0},
    {"100x", 100.0},
    {"1000x", 1'000.0},
    {"10000x", 10'000.0},
    {"max", 0.0},
};

}  // namespace

void transport(Context& context) {
    if (!context.ui.show_transport) {
        return;
    }
    if (!ImGui::Begin("Transport", &context.ui.show_transport)) {
        ImGui::End();
        return;
    }

    Session& session = context.session;
    const SessionSnapshot& snapshot = context.snapshot;
    UiState& ui = context.ui;

    const bool running = snapshot.state == TransportState::Running;

    // --- Buttons ---------------------------------------------------------
    if (ImGui::Button(running ? "Pause" : "Play", ImVec2(90, 0))) {
        session.toggle_play();
    }
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, theme::colors().text_faint);
    ImGui::TextUnformatted("space");
    ImGui::PopStyleColor();

    ImGui::SameLine();
    if (ImGui::Button("Step event")) {
        session.step_event();
    }
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, theme::colors().text_faint);
    ImGui::TextUnformatted("right");
    ImGui::PopStyleColor();

    ImGui::SameLine();
    if (ImGui::Button("Step bar")) {
        session.step_bar();
    }
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, theme::colors().text_faint);
    ImGui::TextUnformatted("shift+right");
    ImGui::PopStyleColor();

    ImGui::SameLine();
    if (ImGui::Button("Restart")) {
        session.restart();
    }

    // --- Speed -----------------------------------------------------------
    ImGui::SetNextItemWidth(140.0F);
    if (ImGui::BeginCombo("Speed", kSpeeds[ui.speed_index].label)) {
        for (int i = 0; i < IM_ARRAYSIZE(kSpeeds); ++i) {
            if (ImGui::Selectable(kSpeeds[i].label, ui.speed_index == i)) {
                ui.speed_index = i;
                session.set_speed(kSpeeds[i].multiplier);
            }
        }
        ImGui::EndCombo();
    }
    help_marker("A multiple of real time: at 60x, one minute of market data takes one "
                "second. \"max\" runs as fast as the machine allows and is the usual "
                "choice for a replay.");

    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, theme::colors().text_dim);
    if (snapshot.events_per_second > 0.0) {
        ImGui::Text("  %.0f events/s", snapshot.events_per_second);
    } else {
        ImGui::TextUnformatted("  idle");
    }
    ImGui::PopStyleColor();

    ImGui::Separator();

    // --- Scrub bar -------------------------------------------------------
    ImGui::PushStyleColor(ImGuiCol_Text, theme::colors().text_dim);
    ImGui::TextUnformatted("Position");
    ImGui::PopStyleColor();
    ImGui::SameLine();
    mono_text(format_timestamp(snapshot.clock));

    if (!snapshot.span_known) {
        ImGui::PushStyleColor(ImGuiCol_Text, theme::colors().text_faint);
        ImGui::TextWrapped("This feed has no known extent, so there is nothing to scrub "
                           "through. A live transport cannot be rewound.");
        ImGui::PopStyleColor();
        ImGui::End();
        return;
    }

    // While dragging, the bar follows the mouse but the engine is not told.
    // Seeking rebuilds the run and replays it, and doing that on every
    // mouse-move would peg a core and make the bar feel glued to treacle.
    if (!ui.scrubbing) {
        ui.scrub_target = static_cast<float>(snapshot.progress);
    }

    ImGui::SetNextItemWidth(-1);
    ImGui::SliderFloat("##scrub", &ui.scrub_target, 0.0F, 1.0F, "");

    if (ImGui::IsItemActive()) {
        ui.scrubbing = true;
    } else if (ui.scrubbing && ImGui::IsItemDeactivatedAfterEdit()) {
        ui.scrubbing = false;
        const auto span = static_cast<double>(snapshot.last_ts - snapshot.first_ts);
        const auto target =
            snapshot.first_ts + static_cast<Timestamp>(span * static_cast<double>(ui.scrub_target));
        session.seek(target);
    } else if (ui.scrubbing && !ImGui::IsItemActive()) {
        ui.scrubbing = false;
    }

    // Where the handle currently points, which during a drag is not where the
    // engine is.
    if (ui.scrubbing) {
        const auto span = static_cast<double>(snapshot.last_ts - snapshot.first_ts);
        const auto preview =
            snapshot.first_ts + static_cast<Timestamp>(span * static_cast<double>(ui.scrub_target));
        ImGui::PushStyleColor(ImGuiCol_Text, theme::colors().accent);
        ImGui::Text("release to seek to %s", format_timestamp(preview).c_str());
        ImGui::PopStyleColor();
        help_marker("Scrubbing backwards replays the run from the beginning to the "
                    "point you chose. That is the only honest way to do it -- the "
                    "strategies hold state from a future they are about to be shown "
                    "again -- and it is exact, because the same input always produces "
                    "the same run.");
    }

    // --- State readout ---------------------------------------------------
    ImGui::Separator();
    if (ImGui::BeginTable("##state", 4, ImGuiTableFlags_SizingStretchSame)) {
        const auto cell = [](const char* label, const std::string& value, unsigned int color) {
            ImGui::TableNextColumn();
            ImGui::PushStyleColor(ImGuiCol_Text, theme::colors().text_dim);
            ImGui::TextUnformatted(label);
            ImGui::PopStyleColor();
            ImGui::PushStyleColor(ImGuiCol_Text, color);
            ImGui::TextUnformatted(value.c_str());
            ImGui::PopStyleColor();
        };

        ImGui::TableNextRow();
        cell("State",
             std::string(to_string(snapshot.state)),
             snapshot.state == TransportState::Failed ? theme::colors().danger
                                                      : theme::colors().text);
        cell("Events", format_count(snapshot.events), theme::colors().text);
        cell("Progress", format_metric(snapshot.progress * 100.0, 1) + "%", theme::colors().text);
        cell("Fills",
             snapshot.fill_model,
             snapshot.fill_model_realistic ? theme::colors().text : theme::colors().warning);
        ImGui::EndTable();
    }

    if (!snapshot.error.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, theme::colors().danger);
        ImGui::TextWrapped("%s", snapshot.error.c_str());
        ImGui::PopStyleColor();
    }

    // --- Fill model ------------------------------------------------------
    ImGui::Separator();
    bool optimistic = !snapshot.fill_model_realistic;
    if (ImGui::Checkbox("Optimistic fills", &optimistic)) {
        context.session.set_optimistic_fills(optimistic);
    }
    help_marker("Fills every order at the last trade price, instantly, in full, with "
                "no spread and no penalty for size.\n\n"
                "This is wrong. It exists so the two can be put side by side: the gap "
                "between them is what a backtest built on naive fills quietly awards "
                "itself on every single trade.");

    if (optimistic) {
        ImGui::PushStyleColor(ImGuiCol_Text, theme::colors().warning);
        ImGui::TextWrapped("Nothing on screen is a result while this is on.");
        ImGui::PopStyleColor();
    }

    ImGui::End();
}

}  // namespace marketscan::app::panels
