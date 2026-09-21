#include "palette.hpp"

namespace marketscan::app {
namespace {

constexpr unsigned int rgba(unsigned int r, unsigned int g, unsigned int b, unsigned int a = 255) {
    return (a << 24) | (b << 16) | (g << 8) | r;  // IM_COL32 order
}

const Palette kDark{
    /* background    */ rgba(0x0F, 0x11, 0x15),
    /* surface       */ rgba(0x16, 0x1A, 0x21),
    /* surface_raised*/ rgba(0x1E, 0x23, 0x2C),
    /* border        */ rgba(0x2A, 0x31, 0x3D),

    /* text          */ rgba(0xFF, 0xFF, 0xFF),
    /* text_dim      */ rgba(0xC3, 0xC2, 0xB7),
    /* text_faint    */ rgba(0x89, 0x87, 0x81),

    /* accent        */ rgba(0x39, 0x87, 0xE5),
    /* accent_dim    */ rgba(0x2E, 0x5C, 0x99),

    // The status palette, not a categorical hue. Always shown with a sign.
    /* profit        */ rgba(0x0C, 0xA3, 0x0C),
    /* loss          */ rgba(0xD0, 0x3B, 0x3B),

    /* warning       */ rgba(0xFA, 0xB2, 0x19),
    /* danger        */ rgba(0xD0, 0x3B, 0x3B),

    /* status_good    */ rgba(0x0C, 0xA3, 0x0C),
    /* status_warning */ rgba(0xFA, 0xB2, 0x19),
    /* status_serious */ rgba(0xEC, 0x83, 0x5A),
    /* status_critical*/ rgba(0xD0, 0x3B, 0x3B),

    /* chart_surface */ rgba(0x1A, 0x1A, 0x19),
    /* gridline      */ rgba(0x2C, 0x2C, 0x2A),
    /* baseline      */ rgba(0x38, 0x38, 0x35),
    /* emphasis_gray */ rgba(0x89, 0x87, 0x81),
    /* benchmark     */ rgba(0xC3, 0xC2, 0xB7),

    /* series        */
    {
        rgba(0x39, 0x87, 0xE5),  // 1 blue
        rgba(0xD9, 0x59, 0x26),  // 2 orange
        rgba(0x19, 0x9E, 0x70),  // 3 aqua
        rgba(0xC9, 0x85, 0x00),  // 4 yellow
        rgba(0xD5, 0x51, 0x81),  // 5 magenta
        rgba(0x00, 0x83, 0x00),  // 6 green
        rgba(0x90, 0x85, 0xE9),  // 7 violet
        rgba(0xE6, 0x67, 0x67),  // 8 red
    },
};

/// The light column. A selected set of steps for a light surface, not an
/// inversion of the dark one.
///
/// Worth having because dark themes wash out on a projector, and Overview is the
/// mode that will get projected.
const Palette kLight{
    /* background    */ rgba(0xF4, 0xF4, 0xF1),
    /* surface       */ rgba(0xFC, 0xFC, 0xFB),
    /* surface_raised*/ rgba(0xFF, 0xFF, 0xFF),
    /* border        */ rgba(0xC3, 0xC2, 0xB7),

    /* text          */ rgba(0x0B, 0x0B, 0x0B),
    /* text_dim      */ rgba(0x52, 0x51, 0x4E),
    /* text_faint    */ rgba(0x89, 0x87, 0x81),

    /* accent        */ rgba(0x2A, 0x78, 0xD6),
    /* accent_dim    */ rgba(0x8F, 0xB6, 0xE8),

    /* profit        */ rgba(0x0C, 0xA3, 0x0C),
    /* loss          */ rgba(0xD0, 0x3B, 0x3B),

    /* warning       */ rgba(0xFA, 0xB2, 0x19),
    /* danger        */ rgba(0xD0, 0x3B, 0x3B),

    /* status_good    */ rgba(0x0C, 0xA3, 0x0C),
    /* status_warning */ rgba(0xFA, 0xB2, 0x19),
    /* status_serious */ rgba(0xEC, 0x83, 0x5A),
    /* status_critical*/ rgba(0xD0, 0x3B, 0x3B),

    /* chart_surface */ rgba(0xFC, 0xFC, 0xFB),
    /* gridline      */ rgba(0xE1, 0xE0, 0xD9),
    /* baseline      */ rgba(0xC3, 0xC2, 0xB7),
    /* emphasis_gray */ rgba(0x89, 0x87, 0x81),
    /* benchmark     */ rgba(0x52, 0x51, 0x4E),

    /* series        */
    {
        rgba(0x2A, 0x78, 0xD6),  // 1 blue
        rgba(0xEB, 0x68, 0x34),  // 2 orange
        rgba(0x1B, 0xAF, 0x7A),  // 3 aqua
        rgba(0xED, 0xA1, 0x00),  // 4 yellow
        rgba(0xE8, 0x7B, 0xA4),  // 5 magenta
        rgba(0x00, 0x83, 0x00),  // 6 green
        rgba(0x4A, 0x3A, 0xA7),  // 7 violet
        rgba(0xE3, 0x49, 0x48),  // 8 red
    },
};

Appearance g_appearance = Appearance::Dark;

[[nodiscard]] const Palette& palette_for(Appearance appearance) noexcept {
    return appearance == Appearance::Light ? kLight : kDark;
}

}  // namespace

namespace theme {

const Palette& colors() noexcept {
    return palette_for(g_appearance);
}

const Palette& colors(Appearance which) noexcept {
    return palette_for(which);
}

Appearance appearance() noexcept {
    return g_appearance;
}

void set_appearance(Appearance which) noexcept {
    g_appearance = which;
}

unsigned int series_color(std::size_t index) noexcept {
    // Folds rather than wraps. A ninth theory wearing the first one's blue is
    // worse than a ninth theory in gray, because the gray is honest about having
    // run out of identities and the blue is not.
    const Palette& active = colors();
    if (index >= Palette::kSeriesCount) {
        return active.emphasis_gray;
    }
    return active.series[index];
}

unsigned int pnl_color(double value) noexcept {
    if (value > 0.0) {
        return colors().profit;
    }
    if (value < 0.0) {
        return colors().loss;
    }
    // Flat is not a gain.
    return colors().text_dim;
}

}  // namespace theme
}  // namespace marketscan::app
