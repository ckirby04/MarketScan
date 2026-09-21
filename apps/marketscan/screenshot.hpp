#ifndef MARKETSCAN_APP_SCREENSHOT_HPP
#define MARKETSCAN_APP_SCREENSHOT_HPP

/*
 * The dashboard capturing itself.
 *
 * The README's screenshots are how somebody deciding whether to build this can
 * see what they would get, and a screenshot that cannot be regenerated slowly
 * becomes a picture of a version that no longer exists. So capturing one is a
 * command, not an act of window management.
 *
 * WHY THE APPLICATION DOES IT RATHER THAN THE DESKTOP
 *
 * `scripts/capture_screenshot.ps1` used to read the pixels off the screen,
 * which needs the window to be genuinely in front. Windows refuses
 * SetForegroundWindow to a process that is not already the foreground one, so
 * run from a script rather than by a person clicking, that yields a white
 * rectangle -- and, before the script learned to check, wrote it out as though
 * it were a screenshot. PrintWindow does not rescue it either: an OpenGL window
 * has no DWM redirection surface to hand over while it is behind something.
 *
 * Reading the back buffer has none of those problems. The window still has to
 * exist and be rendering -- this is not headless -- but it does not have to be
 * visible, in front, or unobstructed, and what lands in the file is exactly what
 * the application drew.
 *
 * WHY BMP
 *
 * A PNG needs a deflate encoder, and bringing in a compression library so that
 * a developer tool can write one file a year is the wrong trade. A 24-bit BMP is
 * forty lines and no dependency; `capture_screenshot.ps1` converts it to PNG
 * through System.Drawing, which is already there.
 */

#include <cstdint>
#include <filesystem>

namespace marketscan::app {

/// Reads the current back buffer and writes it as a 24-bit BMP.
///
/// Call after the frame is drawn and **before** the buffers are swapped: after
/// the swap, the frame is in the front buffer and what glReadPixels returns is
/// undefined on a double-buffered context.
///
/// Returns false if the file could not be written, having logged why. A failed
/// screenshot never stops the run.
[[nodiscard]] bool
capture_back_buffer(const std::filesystem::path& path, std::int32_t width, std::int32_t height);

}  // namespace marketscan::app

#endif  // MARKETSCAN_APP_SCREENSHOT_HPP
