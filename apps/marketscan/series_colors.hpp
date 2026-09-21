#ifndef MARKETSCAN_APP_SERIES_COLORS_HPP
#define MARKETSCAN_APP_SERIES_COLORS_HPP

/*
 * Which colour belongs to which theory.
 *
 * COLOUR FOLLOWS THE ENTITY, NEVER ITS RANK
 *
 * ImPlot assigns colours by plot order. That means hiding one series from the
 * legend repaints every series after it: a theory silently changes colour, and
 * the leaderboard no longer matches the chart. Nobody notices, because nothing
 * is *wrong* on either side -- they simply stop describing the same thing.
 *
 * So a slot is assigned from the theory's stable plugin id and held for the
 * session, regardless of draw order, filter state, sort order, or how many
 * theories are visible. Two modes drawing the same snapshot get the same
 * assignment because they ask the same function, not because they happen to
 * iterate in the same order.
 *
 * WHY SORTED BY ID RATHER THAN HASHED
 *
 * A hash of the id would also be draw-order independent, and it would collide:
 * two theories sharing a slot is the failure this exists to prevent, arrived at
 * by a different road. Sorting the ids and assigning in order is collision-free,
 * depends only on the set of loaded theories, and is stable across runs.
 *
 * EIGHT, AND THEN FOLD
 *
 * Eight is the ceiling the spec sets and it is not arbitrary: a ninth generated
 * hue is indistinguishable from one of the eight for a colour-blind viewer, so
 * it would not add an identity, it would counterfeit one. The ninth theory and
 * everything after it share the de-emphasis gray and are named "Other".
 */

#include <cstddef>
#include <string>
#include <vector>

#include "session.hpp"
#include "theme.hpp"

namespace marketscan::app {

/// A theory-to-slot assignment for one snapshot.
class SeriesColors {
public:
    /// The slot given to a theory that did not fit in the eight.
    static constexpr std::size_t kFolded = static_cast<std::size_t>(-1);

    SeriesColors() = default;

    /// Builds the assignment from **every** theory in the snapshot.
    ///
    /// Every one, including hidden and filtered ones: that is the whole point.
    /// An assignment built from the visible subset would change the moment
    /// somebody clicked a legend entry.
    explicit SeriesColors(const SessionSnapshot& snapshot);

    /// Slot for `id`, or `kFolded` past the eighth. Unknown ids fold too.
    [[nodiscard]] std::size_t slot(const std::string& id) const noexcept;

    /// The colour for `id` in the active appearance.
    ///
    /// The benchmark is excluded from the categorical slots entirely and always
    /// draws in its own fixed hue -- it is the thing being compared against, not
    /// one of the things being compared, and it must never be mistaken for one.
    [[nodiscard]] unsigned int color(const std::string& id) const noexcept;

    /// True when `id` is past the eighth slot and shares the folded gray.
    [[nodiscard]] bool folded(const std::string& id) const noexcept;

    /// How many theories were given a slot of their own.
    [[nodiscard]] std::size_t assigned() const noexcept { return order_.size(); }

    /// Theories that folded, in id order. Named so a legend can say "Other (3)"
    /// rather than showing three identical gray entries.
    [[nodiscard]] const std::vector<std::string>& folded_ids() const noexcept { return folded_; }

    /// The benchmark's fixed hue. Not a categorical slot.
    [[nodiscard]] static unsigned int benchmark_color() noexcept;

private:
    /// Ids holding slots 0..7, in slot order.
    std::vector<std::string> order_;
    std::vector<std::string> folded_;
    std::string benchmark_id_;
};

}  // namespace marketscan::app

#endif  // MARKETSCAN_APP_SERIES_COLORS_HPP
