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

constexpr unsigned int rgba(unsigned int r, unsigned int g, unsigned int b, unsigned int a = 255) {
    return (a << 24) | (b << 16) | (g << 8) | r;  // IM_COL32 order
}

const Palette kPalette{
    /* background    */ rgba(0x0F, 0x11, 0x15),
    /* surface       */ rgba(0x16, 0x1A, 0x21),
    /* surface_raised*/ rgba(0x1E, 0x23, 0x2C),
    /* border        */ rgba(0x2A, 0x31, 0x3D),

    /* text          */ rgba(0xE4, 0xE8, 0xEF),
    /* text_dim      */ rgba(0x9A, 0xA4, 0xB4),
    /* text_faint    */ rgba(0x60, 0x6B, 0x7B),

    /* accent        */ rgba(0x4C, 0x9A, 0xFF),
    /* accent_dim    */ rgba(0x2E, 0x5C, 0x99),

    // Teal for profit and amber-red for loss. Not the usual red/green pair:
    // that is the single most common form of colour blindness, and these two
    // differ in lightness as well as hue.
    /* profit        */ rgba(0x3F, 0xD1, 0xA8),
    /* loss          */ rgba(0xFF, 0x6B, 0x5B),

    /* warning       */ rgba(0xF2, 0xB1, 0x4A),
    /* danger        */ rgba(0xFF, 0x4D, 0x4D),

    /* series        */
    {
        rgba(0x4C, 0x9A, 0xFF),  // blue
        rgba(0x3F, 0xD1, 0xA8),  // teal
        rgba(0xF2, 0xB1, 0x4A),  // amber
        rgba(0xC9, 0x7B, 0xFF),  // violet
        rgba(0xFF, 0x6B, 0x5B),  // coral
        rgba(0x6B, 0xE5, 0x6B),  // green
        rgba(0xFF, 0x9F, 0x40),  // orange
        rgba(0x5A, 0xD4, 0xE6),  // cyan
        rgba(0xE8, 0x6B, 0xB0),  // pink
        rgba(0xA8, 0xB4, 0xC4),  // grey
    },
};

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

const Palette& colors() noexcept {
    return kPalette;
}

unsigned int series_color(std::size_t index) noexcept {
    return kPalette.series[index % Palette::kSeriesCount];
}

unsigned int pnl_color(double value) noexcept {
    if (value > 0.0) {
        return kPalette.profit;
    }
    if (value < 0.0) {
        return kPalette.loss;
    }
    // Flat is not a gain.
    return kPalette.text_dim;
}

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
    c[ImGuiCol_WindowBg] = to_vec(kPalette.background);
    c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_PopupBg] = to_vec(kPalette.surface_raised);
    c[ImGuiCol_Border] = to_vec(kPalette.border);
    c[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);

    c[ImGuiCol_Text] = to_vec(kPalette.text);
    c[ImGuiCol_TextDisabled] = to_vec(kPalette.text_faint);

    c[ImGuiCol_FrameBg] = to_vec(kPalette.surface);
    c[ImGuiCol_FrameBgHovered] = shade(kPalette.surface_raised, 1.25F);
    c[ImGuiCol_FrameBgActive] = shade(kPalette.surface_raised, 1.45F);

    c[ImGuiCol_TitleBg] = to_vec(kPalette.surface);
    c[ImGuiCol_TitleBgActive] = to_vec(kPalette.surface_raised);
    c[ImGuiCol_TitleBgCollapsed] = shade(kPalette.surface, 0.7F);

    c[ImGuiCol_MenuBarBg] = to_vec(kPalette.surface);
    c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarGrab] = to_vec(kPalette.border);
    c[ImGuiCol_ScrollbarGrabHovered] = shade(kPalette.border, 1.4F);
    c[ImGuiCol_ScrollbarGrabActive] = to_vec(kPalette.accent_dim);

    c[ImGuiCol_CheckMark] = to_vec(kPalette.accent);
    c[ImGuiCol_SliderGrab] = to_vec(kPalette.accent_dim);
    c[ImGuiCol_SliderGrabActive] = to_vec(kPalette.accent);

    c[ImGuiCol_Button] = to_vec(kPalette.surface_raised);
    c[ImGuiCol_ButtonHovered] = to_vec(kPalette.accent_dim);
    c[ImGuiCol_ButtonActive] = to_vec(kPalette.accent);

    c[ImGuiCol_Header] = shade(kPalette.accent_dim, 1.0F, 0.55F);
    c[ImGuiCol_HeaderHovered] = shade(kPalette.accent_dim, 1.2F, 0.8F);
    c[ImGuiCol_HeaderActive] = to_vec(kPalette.accent_dim);

    c[ImGuiCol_Separator] = to_vec(kPalette.border);
    c[ImGuiCol_SeparatorHovered] = to_vec(kPalette.accent_dim);
    c[ImGuiCol_SeparatorActive] = to_vec(kPalette.accent);

    c[ImGuiCol_ResizeGrip] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ResizeGripHovered] = shade(kPalette.accent, 1.0F, 0.4F);
    c[ImGuiCol_ResizeGripActive] = shade(kPalette.accent, 1.0F, 0.7F);

    c[ImGuiCol_Tab] = to_vec(kPalette.surface);
    c[ImGuiCol_TabHovered] = to_vec(kPalette.accent_dim);
    c[ImGuiCol_TabSelected] = to_vec(kPalette.surface_raised);
    c[ImGuiCol_TabDimmed] = to_vec(kPalette.background);
    c[ImGuiCol_TabDimmedSelected] = to_vec(kPalette.surface);
    c[ImGuiCol_TabSelectedOverline] = to_vec(kPalette.accent);

    c[ImGuiCol_DockingPreview] = shade(kPalette.accent, 1.0F, 0.45F);
    c[ImGuiCol_DockingEmptyBg] = to_vec(kPalette.background);

    c[ImGuiCol_TableHeaderBg] = to_vec(kPalette.surface_raised);
    c[ImGuiCol_TableBorderStrong] = to_vec(kPalette.border);
    c[ImGuiCol_TableBorderLight] = shade(kPalette.border, 0.7F);
    c[ImGuiCol_TableRowBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TableRowBgAlt] = shade(kPalette.surface, 1.0F, 0.35F);

    c[ImGuiCol_TextSelectedBg] = shade(kPalette.accent, 1.0F, 0.35F);
    c[ImGuiCol_NavCursor] = to_vec(kPalette.accent);

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
    p[ImPlotCol_PlotBg] = shade(kPalette.surface, 1.0F, 0.55F);
    p[ImPlotCol_PlotBorder] = to_vec(kPalette.border);
    p[ImPlotCol_LegendBg] = shade(kPalette.surface_raised, 1.0F, 0.94F);
    p[ImPlotCol_LegendBorder] = to_vec(kPalette.border);
    p[ImPlotCol_LegendText] = to_vec(kPalette.text);
    p[ImPlotCol_TitleText] = to_vec(kPalette.text);
    p[ImPlotCol_InlayText] = to_vec(kPalette.text_dim);
    p[ImPlotCol_AxisText] = to_vec(kPalette.text_dim);
    p[ImPlotCol_AxisGrid] = shade(kPalette.border, 1.0F, 0.8F);
    p[ImPlotCol_Selection] = shade(kPalette.accent, 1.0F, 0.6F);
    p[ImPlotCol_Crosshairs] = to_vec(kPalette.text_faint);

    // The chart palette is the same list the tables use, so a theory is the
    // same colour wherever it appears. Registered once by name and selected
    // through the style; re-registering on every call to apply() would leak a
    // colormap slot per invocation.
    std::array<ImVec4, Palette::kSeriesCount> series{};
    for (std::size_t i = 0; i < Palette::kSeriesCount; ++i) {
        series[i] = to_vec(kPalette.series[i]);
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

ImFont* large_font() noexcept {
    return g_large_font;
}

}  // namespace theme
}  // namespace marketscan::app
