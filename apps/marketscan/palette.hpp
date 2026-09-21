#ifndef MARKETSCAN_APP_PALETTE_HPP
#define MARKETSCAN_APP_PALETTE_HPP

/*
 * The colour token table.
 *
 * Deliberately free of ImGui: a palette is data. Keeping it that way is what
 * lets `series_colors.hpp` and `chart_model.hpp` live in the testable half of
 * the application, and therefore what lets the dual-view rules -- one
 * assignment, one geometry, both modes -- be asserted by a test rather than by
 * looking at two screenshots.
 *
 * Values are pinned by `docs/specs/SPEC_VIEW_MODES.md` section 5.
 */

#include <cstddef>
#include <cstdint>

namespace marketscan::app {

/// Which set of steps the token table resolves to.
///
/// Not an inversion of one another. `SPEC_VIEW_MODES.md` pins both columns
/// separately, because a colour chosen to sit on a near-black surface does not
/// become the right colour on a near-white one by having its channels flipped.
enum class Appearance : std::uint8_t {
    Dark = 0,
    Light = 1,
};

/// The application's colours, in one place.
///
/// Semantic names rather than raw values at the call site: a panel asks for
/// `theme::colors().profit`, not for a particular green, so that changing what
/// profit looks like is one edit rather than thirty.
///
/// The values are pinned by `docs/specs/SPEC_VIEW_MODES.md` section 5 and are not
/// a matter of taste. Both view modes read this one table; a raw hex at a call
/// site is how two modes drift into disagreeing about what a theory looks like.
struct Palette {
    unsigned int background;
    unsigned int surface;
    unsigned int surface_raised;
    unsigned int border;

    // --- Ink --------------------------------------------------------------
    //
    // Text always wears an ink token, never a series colour. The coloured mark
    // beside a label carries identity; the label itself stays legible.
    unsigned int text;        ///< primary
    unsigned int text_dim;    ///< secondary
    unsigned int text_faint;  ///< muted -- axis labels, disabled rows

    unsigned int accent;
    unsigned int accent_dim;

    // --- P&L ---------------------------------------------------------------
    //
    // These are the *status* palette, not categorical hues, so a P&L colour can
    // never be mistaken for a theory's identity.
    //
    // Colour is reinforcement and nothing more. Every place that uses these also
    // shows an explicit sign or glyph, because red-against-green alone is the
    // most common accessibility failure in financial interfaces and this
    // application used to commit it.
    unsigned int profit;
    unsigned int loss;

    unsigned int warning;
    unsigned int danger;

    /// The reserved status palette. Never reused as a series colour.
    unsigned int status_good;
    unsigned int status_warning;
    unsigned int status_serious;
    unsigned int status_critical;

    // --- Chart chrome ------------------------------------------------------

    /// The plot background. Pinned: contrast and colour-vision results are only
    /// meaningful against the surface actually rendered on.
    unsigned int chart_surface;
    unsigned int gridline;
    unsigned int baseline;

    /// The de-emphasis step. What a non-emphasised series drops to in Overview,
    /// where one series is the subject and the rest are context.
    unsigned int emphasis_gray;

    /// The benchmark's fixed hue.
    ///
    /// Its own token, and **required to differ from `emphasis_gray`**. It was
    /// borrowed from the muted ink at first, which happens to be the same value:
    /// the benchmark then rendered identically to the context series it is
    /// supposed to be the reference for. A test caught it.
    ///
    /// Neutral rather than categorical, because the benchmark is the thing being
    /// compared against and must never be mistaken for one of the things being
    /// compared. Brighter than the context gray, because it is the one neutral on
    /// the chart that a viewer is meant to read.
    unsigned int benchmark;

    /// Categorical slots, in the order the spec fixes them.
    ///
    /// **Eight, and then fold.** A ninth theory does not get a generated hue: a
    /// ninth colour is indistinguishable from one of these for a colour-blind
    /// viewer and breaks the whole scheme. It goes to "Other". See
    /// `SeriesColors` in series_colors.hpp.
    static constexpr std::size_t kSeriesCount = 8;
    unsigned int series[kSeriesCount];
};

namespace theme {

/// Selects the appearance. Dark is the default.
///
/// Sets the token table only; re-styling the ImGui context is `theme::apply()`,
/// which lives on the GUI side. Split that way because the palette is data and
/// has no business depending on a UI toolkit -- which is also what lets the view
/// tests check colour assignment without a window.
void set_appearance(Appearance appearance) noexcept;

[[nodiscard]] Appearance appearance() noexcept;

[[nodiscard]] const Palette& colors() noexcept;

/// The palette for one appearance, whichever is active. For a test that wants to
/// check both columns without switching the live theme.
[[nodiscard]] const Palette& colors(Appearance appearance) noexcept;

/// Colour for categorical slot `index`.
///
/// Slots past the eighth fold onto the emphasis gray rather than wrapping: a
/// wrapped hue is a second theory wearing the first one's identity. Callers
/// should be assigning slots through `SeriesColors`, which caps and folds for
/// them; this is the raw lookup underneath it.
[[nodiscard]] unsigned int series_color(std::size_t index) noexcept;

/// Colour for a value, by sign. Zero is neutral rather than green, because a
/// flat P&L is not a gain.
[[nodiscard]] unsigned int pnl_color(double value) noexcept;

}  // namespace theme
}  // namespace marketscan::app

#endif  // MARKETSCAN_APP_PALETTE_HPP
