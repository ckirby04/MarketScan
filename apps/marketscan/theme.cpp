#include "theme.hpp"

#include <array>

#include <imgui.h>
#include <implot.h>

#include "marketscan/util/logging.hpp"

namespace marketscan::app {
namespace {

ImFont* g_ui_font = nullptr;
ImFont* g_mono_font = nullptr;
ImFont* g_large_font = nullptr;
ImFont* g_hero_font = nullptr;

constexpr unsigned int rgba(unsigned int r, unsigned int g, unsigned int b, unsigned int a = 255) {
    return (a << 24) | (b << 16) | (g << 8) | r;  // IM_COL32 order
}

/// The dark column of SPEC_VIEW_MODES.md section 5.
///
/// The chrome above the chart keeps the near-black blues this application was
/// built with; the *chart surface* and everything drawn on it are the pinned
/// values, because that is what the contrast and colour-vision work was done
/// against.
[[nodiscard]] ImVec4 to_vec(unsigned int color) {
    return ImGui::ColorConvertU32ToFloat4(color);
}

/// Dims a colour without changing its hue, for disabled and hovered states.
[[nodiscard]] ImVec4 shade(unsigned int color, float factor, float alpha = 1.0F) {
    ImVec4 v = to_vec(color);
    v.x *= factor;
    v.y *= factor;
    v.z *= factor;
    v.w *= alpha;
    return v;
}

}  // namespace

namespace theme {

void apply() {
    ImGuiStyle& style = ImGui::GetStyle();
    ImGui::StyleColorsDark();

    style.WindowRounding = 6.0F;
    style.ChildRounding = 6.0F;
    style.FrameRounding = 5.0F;
    style.PopupRounding = 6.0F;
    style.ScrollbarRounding = 8.0F;
    style.GrabRounding = 5.0F;
    style.TabRounding = 5.0F;

    style.WindowBorderSize = 1.0F;
    style.FrameBorderSize = 1.0F;
    style.PopupBorderSize = 1.0F;
    style.ChildBorderSize = 1.0F;

    style.WindowPadding = ImVec2(12.0F, 10.0F);
    style.FramePadding = ImVec2(9.0F, 5.0F);
    style.CellPadding = ImVec2(8.0F, 4.0F);
    style.ItemSpacing = ImVec2(9.0F, 7.0F);
    style.ItemInnerSpacing = ImVec2(7.0F, 5.0F);
    style.IndentSpacing = 20.0F;
    style.ScrollbarSize = 12.0F;
    style.GrabMinSize = 10.0F;

    style.WindowTitleAlign = ImVec2(0.0F, 0.5F);
    style.SeparatorTextBorderSize = 1.0F;

    ImVec4* c = style.Colors;
    c[ImGuiCol_WindowBg] = to_vec(colors().background);
    c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_PopupBg] = to_vec(colors().surface_raised);
    c[ImGuiCol_Border] = to_vec(colors().border);
    c[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);

    c[ImGuiCol_Text] = to_vec(colors().text);
    c[ImGuiCol_TextDisabled] = to_vec(colors().text_faint);

    c[ImGuiCol_FrameBg] = to_vec(colors().surface);
    c[ImGuiCol_FrameBgHovered] = shade(colors().surface_raised, 1.25F);
    c[ImGuiCol_FrameBgActive] = shade(colors().surface_raised, 1.45F);

    c[ImGuiCol_TitleBg] = to_vec(colors().surface);
    c[ImGuiCol_TitleBgActive] = to_vec(colors().surface_raised);
    c[ImGuiCol_TitleBgCollapsed] = shade(colors().surface, 0.7F);

    c[ImGuiCol_MenuBarBg] = to_vec(colors().surface);
    c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarGrab] = to_vec(colors().border);
    c[ImGuiCol_ScrollbarGrabHovered] = shade(colors().border, 1.4F);
    c[ImGuiCol_ScrollbarGrabActive] = to_vec(colors().accent_dim);

    c[ImGuiCol_CheckMark] = to_vec(colors().accent);
    c[ImGuiCol_SliderGrab] = to_vec(colors().accent_dim);
    c[ImGuiCol_SliderGrabActive] = to_vec(colors().accent);

    c[ImGuiCol_Button] = to_vec(colors().surface_raised);
    c[ImGuiCol_ButtonHovered] = to_vec(colors().accent_dim);
    c[ImGuiCol_ButtonActive] = to_vec(colors().accent);

    c[ImGuiCol_Header] = shade(colors().accent_dim, 1.0F, 0.55F);
    c[ImGuiCol_HeaderHovered] = shade(colors().accent_dim, 1.2F, 0.8F);
    c[ImGuiCol_HeaderActive] = to_vec(colors().accent_dim);

    c[ImGuiCol_Separator] = to_vec(colors().border);
    c[ImGuiCol_SeparatorHovered] = to_vec(colors().accent_dim);
    c[ImGuiCol_SeparatorActive] = to_vec(colors().accent);

    c[ImGuiCol_ResizeGrip] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ResizeGripHovered] = shade(colors().accent, 1.0F, 0.4F);
    c[ImGuiCol_ResizeGripActive] = shade(colors().accent, 1.0F, 0.7F);

    c[ImGuiCol_Tab] = to_vec(colors().surface);
    c[ImGuiCol_TabHovered] = to_vec(colors().accent_dim);
    c[ImGuiCol_TabSelected] = to_vec(colors().surface_raised);
    c[ImGuiCol_TabDimmed] = to_vec(colors().background);
    c[ImGuiCol_TabDimmedSelected] = to_vec(colors().surface);
    c[ImGuiCol_TabSelectedOverline] = to_vec(colors().accent);

    c[ImGuiCol_DockingPreview] = shade(colors().accent, 1.0F, 0.45F);
    c[ImGuiCol_DockingEmptyBg] = to_vec(colors().background);

    c[ImGuiCol_TableHeaderBg] = to_vec(colors().surface_raised);
    c[ImGuiCol_TableBorderStrong] = to_vec(colors().border);
    c[ImGuiCol_TableBorderLight] = shade(colors().border, 0.7F);
    c[ImGuiCol_TableRowBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TableRowBgAlt] = shade(colors().surface, 1.0F, 0.35F);

    c[ImGuiCol_TextSelectedBg] = shade(colors().accent, 1.0F, 0.35F);
    c[ImGuiCol_NavCursor] = to_vec(colors().accent);

    // --- ImPlot -----------------------------------------------------------
    // ImPlot 1.0 moved per-item styling (line weight, markers, fill) out of
    // the global style and into the ImPlotSpec passed to each plot call, so
    // only the frame-level settings live here.
    ImPlotStyle& plot = ImPlot::GetStyle();
    plot.PlotPadding = ImVec2(10.0F, 8.0F);
    plot.LabelPadding = ImVec2(5.0F, 4.0F);
    plot.LegendPadding = ImVec2(8.0F, 8.0F);
    plot.MinorAlpha = 0.22F;
    plot.PlotDefaultSize = ImVec2(420.0F, 260.0F);

    ImVec4* p = plot.Colors;
    p[ImPlotCol_FrameBg] = ImVec4(0, 0, 0, 0);
    p[ImPlotCol_PlotBg] = shade(colors().surface, 1.0F, 0.55F);
    p[ImPlotCol_PlotBorder] = to_vec(colors().border);
    p[ImPlotCol_LegendBg] = shade(colors().surface_raised, 1.0F, 0.94F);
    p[ImPlotCol_LegendBorder] = to_vec(colors().border);
    p[ImPlotCol_LegendText] = to_vec(colors().text);
    p[ImPlotCol_TitleText] = to_vec(colors().text);
    p[ImPlotCol_InlayText] = to_vec(colors().text_dim);
    p[ImPlotCol_AxisText] = to_vec(colors().text_dim);
    p[ImPlotCol_AxisGrid] = shade(colors().border, 1.0F, 0.8F);
    p[ImPlotCol_Selection] = shade(colors().accent, 1.0F, 0.6F);
    p[ImPlotCol_Crosshairs] = to_vec(colors().text_faint);

    // The chart palette is the same list the tables use, so a theory is the
    // same colour wherever it appears. Registered once by name and selected
    // through the style; re-registering on every call to apply() would leak a
    // colormap slot per invocation.
    std::array<ImVec4, Palette::kSeriesCount> series{};
    for (std::size_t i = 0; i < Palette::kSeriesCount; ++i) {
        series[i] = to_vec(colors().series[i]);
    }
    ImPlotColormap colormap = ImPlot::GetColormapIndex("MarketScan");
    if (colormap == -1) {
        colormap =
            ImPlot::AddColormap("MarketScan", series.data(), static_cast<int>(series.size()), true);
    }
    plot.Colormap = colormap;
}

bool load_fonts(const std::filesystem::path& assets_dir, float size_pixels) {
    ImGuiIO& io = ImGui::GetIO();

    const std::filesystem::path ui_path = assets_dir / "fonts" / "Inter-Regular.ttf";
    const std::filesystem::path mono_path = assets_dir / "fonts" / "JetBrainsMono-Regular.ttf";

    ImFontConfig config;
    config.OversampleH = 2;
    config.OversampleV = 1;
    config.PixelSnapH = true;

    if (std::filesystem::exists(ui_path)) {
        const std::string path = ui_path.string();
        g_ui_font = io.Fonts->AddFontFromFileTTF(path.c_str(), size_pixels, &config);
        g_large_font = io.Fonts->AddFontFromFileTTF(path.c_str(), size_pixels * 1.55F, &config);
        // Overview's hero figure. SPEC_VIEW_MODES section 3.1 pins it at >= 48px
        // and the number is the whole point of that view -- at 16px base this is
        // 52, which clears it with room for a smaller base font.
        g_hero_font = io.Fonts->AddFontFromFileTTF(path.c_str(), size_pixels * 3.25F, &config);
    } else {
        log::warn("app", "UI font not found at " + ui_path.string() + "; using the built-in one");
    }

    if (std::filesystem::exists(mono_path)) {
        const std::string path = mono_path.string();
        g_mono_font = io.Fonts->AddFontFromFileTTF(path.c_str(), size_pixels - 1.0F, &config);
    } else {
        log::warn("app",
                  "monospace font not found at " + mono_path.string() +
                      "; numeric columns will not align");
    }

    // A missing font is cosmetic. Falling back keeps the application usable
    // rather than refusing to start over a typeface.
    if (g_ui_font == nullptr) {
        g_ui_font = io.Fonts->AddFontDefault();
    }
    if (g_mono_font == nullptr) {
        g_mono_font = g_ui_font;
    }
    if (g_hero_font == nullptr) {
        g_hero_font = g_large_font != nullptr ? g_large_font : g_ui_font;
    }
    if (g_large_font == nullptr) {
        g_large_font = g_ui_font;
    }
    io.FontDefault = g_ui_font;
    return std::filesystem::exists(ui_path) || std::filesystem::exists(mono_path);
}

ImFont* ui_font() noexcept {
    return g_ui_font;
}

ImFont* mono_font() noexcept {
    return g_mono_font;
}

ImFont* hero_font() noexcept {
    return g_hero_font != nullptr ? g_hero_font : g_large_font;
}

ImFont* large_font() noexcept {
    return g_large_font;
}

}  // namespace theme
}  // namespace marketscan::app
