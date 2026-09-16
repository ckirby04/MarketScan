#include <string>
#include <vector>

#include <imgui.h>

#include "../theme.hpp"
#include "panels.hpp"

namespace marketscan::app::panels {
namespace {

[[nodiscard]] unsigned int state_color(PluginState state) {
    switch (state) {
        case PluginState::Loaded:
            return theme::colors().profit;
        case PluginState::Discovered:
            return theme::colors().text_dim;
        case PluginState::Failed:
            return theme::colors().danger;
    }
    return theme::colors().text;
}

}  // namespace

void plugin_manager(Context& context) {
    if (!context.ui.show_plugins) {
        return;
    }
    if (!ImGui::Begin("Plugin Manager", &context.ui.show_plugins)) {
        ImGui::End();
        return;
    }

    UiState& ui = context.ui;

    // Polled rather than watched with ReadDirectoryChangesW: at one second
    // this costs a stat per file, and a filesystem watcher would have to be
    // marshalled onto the engine thread anyway to be acted on safely.
    const double now = ImGui::GetTime();
    if (now - ui.last_disk_check > 1.0) {
        ui.last_disk_check = now;
        ui.reload_available = context.session.theories_changed_on_disk();
    }

    if (ImGui::Button("Rescan")) {
        context.session.rescan_theories();
        ui.reload_available = false;
    }
    help_marker("Re-reads theories/, loads anything new or changed, and rebuilds the "
                "run from the start.\n\n"
                "Never automatic: swapping a strategy's code out from under a running "
                "replay would silently invalidate every number on screen.");

    if (ui.reload_available) {
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, theme::colors().warning);
        ImGui::TextUnformatted("  a theory changed on disk");
        ImGui::PopStyleColor();
    }

    const std::vector<PluginInfo> plugins = context.session.plugins();

    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, theme::colors().text_faint);
    ImGui::Text("   %zu found", plugins.size());
    ImGui::PopStyleColor();

    ImGui::Separator();

    if (plugins.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, theme::colors().text_dim);
        ImGui::TextWrapped("Nothing in theories/ yet. Looked in:");
        ImGui::PopStyleColor();
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Text, theme::colors().text_faint);
        ImGui::TextWrapped("See theories/README.md for how to build one.");
        ImGui::PopStyleColor();
        ImGui::End();
        return;
    }

    for (const PluginInfo& info : plugins) {
        ImGui::PushID(info.id.c_str());

        ImGui::PushStyleColor(ImGuiCol_Text, state_color(info.state));
        const bool open = ImGui::CollapsingHeader(
            (info.display_name.empty() ? info.id : info.display_name).c_str(),
            ImGuiTreeNodeFlags_DefaultOpen);
        ImGui::PopStyleColor();

        if (open) {
            ImGui::Indent();

            if (ImGui::BeginTable("##fields",
                                  2,
                                  ImGuiTableFlags_SizingStretchProp |
                                      ImGuiTableFlags_BordersInnerV)) {
                ImGui::TableSetupColumn("##k", ImGuiTableColumnFlags_WidthStretch, 0.7F);
                ImGui::TableSetupColumn("##v", ImGuiTableColumnFlags_WidthStretch, 2.3F);

                const auto field = [](const char* key, const std::string& value) {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::PushStyleColor(ImGuiCol_Text, theme::colors().text_dim);
                    ImGui::TextUnformatted(key);
                    ImGui::PopStyleColor();
                    ImGui::TableSetColumnIndex(1);
                    ImGui::TextWrapped("%s", value.c_str());
                };

                field("id", info.id);
                field("state", std::string(to_string(info.state)));
                field("author", info.author.empty() ? "--" : info.author);
                field("ABI",
                      std::to_string(info.abi_version) +
                          (info.abi_version == MS_ABI_VERSION ? " (matches host)" : " (MISMATCH)"));
                field("path", info.path.string());
                if (!info.shadow_path.empty()) {
                    field("loaded from", info.shadow_path.string());
                }
                field("instances", std::to_string(info.live_instances));
                if (!info.params.empty()) {
                    field("parameters", std::to_string(info.params.size()));
                }
                if (!info.required_symbols.empty()) {
                    std::string symbols;
                    for (const std::string& symbol : info.required_symbols) {
                        if (!symbols.empty()) {
                            symbols += ", ";
                        }
                        symbols += symbol;
                    }
                    field("requires", symbols);
                }
                ImGui::EndTable();
            }

            if (!info.description.empty()) {
                ImGui::Spacing();
                ImGui::PushStyleColor(ImGuiCol_Text, theme::colors().text_dim);
                ImGui::TextWrapped("%s", info.description.c_str());
                ImGui::PopStyleColor();
            }

            // The error, verbatim and selectable. A loader message that only
            // exists in a log the user has not opened is a message that was
            // not delivered.
            if (!info.error.empty()) {
                ImGui::Spacing();
                ImGui::PushStyleColor(ImGuiCol_Text, theme::colors().danger);
                ImGui::TextWrapped("%s", info.error.c_str());
                ImGui::PopStyleColor();

                if (ImGui::SmallButton("Copy error")) {
                    ImGui::SetClipboardText(info.error.c_str());
                }
            }

            ImGui::Spacing();
            if (info.state == PluginState::Loaded) {
                if (ImGui::Button("Unload")) {
                    context.session.unload_plugin(info.id);
                }
                ImGui::SameLine();
                if (ImGui::Button("Reload")) {
                    context.session.reload_plugin(info.id);
                }
                help_marker("Frees the module and loads the file again. This is what "
                            "the shadow copy exists for: the original stays writable "
                            "while it is loaded, so a rebuild can land on top of it.");
            } else {
                if (ImGui::Button("Load")) {
                    context.session.load_plugin(info.path);
                }
            }

            ImGui::Unindent();
            ImGui::Spacing();
        }
        ImGui::PopID();
    }

    ImGui::End();
}

}  // namespace marketscan::app::panels
