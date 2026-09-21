#ifndef MARKETSCAN_ABI_ABI_COMPAT_HPP
#define MARKETSCAN_ABI_ABI_COMPAT_HPP

/*
 * Loading a plugin that was compiled against an older ABI than this host.
 *
 * Host-side only. A plugin never includes this: it knows exactly one ABI, the
 * one it was compiled against, and that is the entire point of freezing it.
 *
 * WHY THIS EXISTS BEFORE IT IS NEEDED
 *
 * Written at v1, when there was nothing to be tolerant of. v2 is the first thing
 * it carries, and it carried it without changing a line of the mechanism -- one
 * case in slot_count_for, one clause in trim_to. That is what it was for.
 *
 * The reason it had to exist before it was needed is how the vtable is filled
 * in: the host declares an
 * MS_StrategyVTable, passes its address to ms_plugin_create, and the plugin
 * writes into it. The plugin writes its own idea of that struct.
 *
 * Which means the sizes had better agree, and when they do not, the direction
 * matters:
 *
 *   plugin older than host  -- the plugin writes fewer bytes than the host
 *                              declared. Safe, provided the host zeroed the
 *                              struct first and never calls a null slot.
 *                              This is the case to support.
 *
 *   plugin newer than host  -- the plugin writes MORE bytes than the host
 *                              declared, past the end of the host's object.
 *                              That is memory corruption, it is silent, and it
 *                              happens before a single callback runs. This is
 *                              the case to refuse, and it must be refused on
 *                              the declared version, before create is called.
 *
 * Writing this under deadline, alongside the v2 change that first needs it, is
 * how the second case gets missed.
 */

#include <cstddef>
#include <cstdint>

#include "marketscan/abi/plugin_abi.h"

namespace marketscan::abi {

/// Function pointers in MS_StrategyVTable as of ABI v1.
inline constexpr std::size_t kV1SlotCount = 9;

/// v2 appended on_order_status. Nothing before it moved.
inline constexpr std::size_t kV2SlotCount = 10;

/// If this fires, a slot was added to the vtable without deciding which ABI
/// version introduced it. Add the version to slot_count_for, extend trim_to,
/// and update this number -- in that order.
static_assert(sizeof(MS_StrategyVTable) == kV2SlotCount * sizeof(void (*)()),
              "MS_StrategyVTable changed size; abi_compat.hpp has not been updated");

/// How many leading vtable slots a plugin declaring `version` may have filled.
///
/// Zero means this host cannot load that plugin at all: either the version
/// predates anything that ever shipped, or it is newer than this host and its
/// layout is therefore unknown. An unknown layout is not a thing to guess at.
[[nodiscard]] constexpr std::size_t slot_count_for(std::uint32_t version) noexcept {
    switch (version) {
        case 1:
            // Still nine. This is the migration working: a theory compiled
            // against v1 keeps loading, keeps behaving identically, and simply
            // never has on_order_status delivered to it.
            return kV1SlotCount;
        case 2:
            return kV2SlotCount;
        // A future version adds its case here, returning its own slot count,
        // and every older case keeps returning the smaller one.
        default:
            return 0;
    }
}

[[nodiscard]] constexpr bool is_supported(std::uint32_t version) noexcept {
    return slot_count_for(version) != 0;
}

/// The newest version this host knows must account for the whole table.
///
/// Otherwise the host trims a slot away from a plugin that was entitled to it,
/// and a callback silently never arrives -- which looks like a strategy that
/// ignores an event rather than a host that dropped one.
static_assert(slot_count_for(MS_ABI_VERSION) == sizeof(MS_StrategyVTable) / sizeof(void (*)()),
              "the current ABI version does not account for every vtable slot");

/// Nulls every slot past the first `slots`, so the host cannot call one.
///
/// Belt to the braces of zero-initialising before create: a plugin that wrote
/// a pointer into a slot its declared version does not have is either broken
/// or lying, and calling it would be taking its word for the layout of
/// everything else too.
///
/// The order below is the declaration order in plugin_abi.h, walked backwards.
/// The static_assert above is what stops that going stale.
constexpr void trim_to(MS_StrategyVTable& table, std::size_t slots) noexcept {
    if (slots < 10) {
        table.on_order_status = nullptr;
    }
    if (slots < 9) {
        table.get_param = nullptr;
    }
    if (slots < 8) {
        table.set_param = nullptr;
    }
    if (slots < 7) {
        table.on_stop = nullptr;
    }
    if (slots < 6) {
        table.on_fill = nullptr;
    }
    if (slots < 5) {
        table.on_trade = nullptr;
    }
    if (slots < 4) {
        table.on_quote = nullptr;
    }
    if (slots < 3) {
        table.on_bar = nullptr;
    }
    if (slots < 2) {
        table.on_start = nullptr;
    }
    if (slots < 1) {
        table.destroy = nullptr;
    }
}

/// Trims a vtable to what the declared version is entitled to.
constexpr void trim_to_version(MS_StrategyVTable& table, std::uint32_t version) noexcept {
    trim_to(table, slot_count_for(version));
}

}  // namespace marketscan::abi

#endif  // MARKETSCAN_ABI_ABI_COMPAT_HPP
