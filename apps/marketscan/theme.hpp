#ifndef MARKETSCAN_APP_THEME_HPP
#define MARKETSCAN_APP_THEME_HPP

#include <filesystem>

#include "palette.hpp"

struct ImFont;
struct ImVec4;

namespace marketscan::app {
namespace theme {

/// Applies the current appearance's style to the ImGui context.
///
/// Call after `set_appearance`. Separate from it because the palette is data
/// and ImGui styling is not -- see palette.hpp.
void apply();

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

/// Inter SemiBold at the UI size, for bold prose. Null when the file is missing.
[[nodiscard]] ImFont* bold_font() noexcept;

/// Larger proportional face, for headline figures.
[[nodiscard]] ImFont* large_font() noexcept;

/// The hero face: Overview's single big number, at least 48 pixels.
///
/// Its own face rather than a scaled `large_font`, because the spec pins a size
/// and a size that is "whatever the headline face happens to be" is not pinned.
[[nodiscard]] ImFont* hero_font() noexcept;

}  // namespace theme
}  // namespace marketscan::app

#endif  // MARKETSCAN_APP_THEME_HPP
