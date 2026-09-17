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
 * MS_ABI_VERSION is 1 and there is nothing to be tolerant of yet. It is built
 * now because of how the vtable is filled in: the host declares an
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

/// If this fires, a slot was added to the vtable without deciding which ABI
/// version introduced it. Add the version to slot_count_for, extend trim_to,
/// and update this number -- in that order.
static_assert(sizeof(MS_StrategyVTable) == kV1SlotCount * sizeof(void (*)()),
              "MS_StrategyVTable changed size; abi_compat.hpp has not been updated");

/// How many leading vtable slots a plugin declaring `version` may have filled.
///
/// Zero means this host cannot load that plugin at all: either the version
/// predates anything that ever shipped, or it is newer than this host and its
/// layout is therefore unknown. An unknown layout is not a thing to guess at.
[[nodiscard]] constexpr std::size_t slot_count_for(std::uint32_t version) noexcept {
    switch (version) {
        case 1:
            return kV1SlotCount;
        // A future version adds its case here, returning its own slot count,
        // and every older case keeps returning the smaller one. That is the
        // whole migration: a v1 theory keeps loading and keeps behaving
        // identically, and simply never has the newer callbacks delivered.
        default:
            return 0;
    }
}

[[nodiscard]] constexpr bool is_supported(std::uint32_t version) noexcept {
    return slot_count_for(version) != 0;
}

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
