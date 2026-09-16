#ifndef MARKETSCAN_APP_THEME_HPP
#define MARKETSCAN_APP_THEME_HPP

#include <cstddef>
#include <filesystem>

struct ImFont;
struct ImVec4;

namespace marketscan::app {

/// The application's colours, in one place.
///
/// Semantic names rather than raw values at the call site: a panel asks for
/// `theme::colors().profit`, not for a particular green, so that changing what
/// profit looks like is one edit rather than thirty.
struct Palette {
    unsigned int background;
    unsigned int surface;
    unsigned int surface_raised;
    unsigned int border;

    unsigned int text;
    unsigned int text_dim;
    unsigned int text_faint;

    unsigned int accent;
    unsigned int accent_dim;

    /// Gains and losses. Deliberately not pure red and green: a fair slice of
    /// people cannot tell those apart, so the loss colour carries a different
    /// lightness as well as a different hue, and every place that uses them
    /// also shows a sign.
    unsigned int profit;
    unsigned int loss;

    unsigned int warning;
    unsigned int danger;

    /// Series colours for charts, picked to stay distinguishable in order.
    static constexpr std::size_t kSeriesCount = 10;
    unsigned int series[kSeriesCount];
};

namespace theme {

/// Applies the dark style to the current ImGui context.
void apply();

[[nodiscard]] const Palette& colors() noexcept;

/// Colour for the nth loaded theory, wrapping.
[[nodiscard]] unsigned int series_color(std::size_t index) noexcept;

/// Colour for a value, by sign. Zero is neutral rather than green, because a
/// flat P&L is not a gain.
[[nodiscard]] unsigned int pnl_color(double value) noexcept;

/// Loads Inter for UI text and JetBrains Mono for numeric columns.
///
/// Returns false if neither file was found, in which case ImGui's built-in
/// bitmap font is used and the application still runs -- a missing font is a
/// cosmetic problem, not a reason to refuse to start.
bool load_fonts(const std::filesystem::path& assets_dir, float size_pixels = 16.0F);

/// Proportional face, for prose and labels. Never null once ImGui is up.
[[nodiscard]] ImFont* ui_font() noexcept;

/// Monospaced face, for columns of numbers. Every digit the same width, so
/// decimal points line up and two prices of different magnitude do not look
/// the same length.
[[nodiscard]] ImFont* mono_font() noexcept;

/// Larger proportional face, for headline figures.
[[nodiscard]] ImFont* large_font() noexcept;

}  // namespace theme
}  // namespace marketscan::app

#endif  // MARKETSCAN_APP_THEME_HPP
