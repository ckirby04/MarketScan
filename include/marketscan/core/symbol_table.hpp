#ifndef MARKETSCAN_CORE_SYMBOL_TABLE_HPP
#define MARKETSCAN_CORE_SYMBOL_TABLE_HPP

#include <deque>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "marketscan/core/types.hpp"

namespace marketscan {

/// Maps tickers to dense integer ids, and back.
///
/// Ids are assigned in first-seen order starting at 1, so a given input
/// produces the same ids on every run. Zero means "not a symbol" and is what
/// lookup returns for an unknown ticker -- it is never handed out.
///
/// The lookup index is a std::map rather than a hash map. Not because lookup
/// order matters here (it never iterates), but because a hash container in the
/// engine is a standing invitation for someone later to iterate it in a path
/// that emits orders, and the cost at this size is unmeasurable.
///
/// Names are stored in a deque: the ABI hands plugins a `const char*` that must
/// stay valid for the run, and a vector would invalidate every one of those
/// pointers the moment it reallocated.
class SymbolTable {
public:
    SymbolTable() = default;

    /// Returns the existing id for `ticker`, or assigns the next one.
    /// Tickers are compared exactly: "spy" and "SPY" are different symbols,
    /// because silently folding case is how a dataset ends up with two
    /// positions in what the user thinks is one instrument.
    SymbolId intern(std::string_view ticker);

    /// The id for `ticker`, or kInvalidSymbol if it was never interned.
    [[nodiscard]] SymbolId find(std::string_view ticker) const noexcept;

    /// The ticker for `id`, or an empty view if the id is not valid.
    [[nodiscard]] std::string_view name(SymbolId id) const noexcept;

    /// Null-terminated ticker for `id`, valid for the lifetime of this table.
    /// This is what crosses the plugin boundary; returns "" for an invalid id
    /// rather than nullptr, so a plugin that forgets to check cannot crash on
    /// a strlen.
    [[nodiscard]] const char* c_name(SymbolId id) const noexcept;

    [[nodiscard]] std::uint32_t count() const noexcept {
        return static_cast<std::uint32_t>(names_.size());
    }

    [[nodiscard]] bool contains(SymbolId id) const noexcept {
        return id != kInvalidSymbol && id <= count();
    }

    [[nodiscard]] bool empty() const noexcept { return names_.empty(); }

    /// Every id in ascending order, which is also first-seen order.
    [[nodiscard]] std::vector<SymbolId> ids() const;

    void clear() noexcept;

private:
    // Index into names_ is id - 1.
    std::deque<std::string> names_;
    std::map<std::string, SymbolId, std::less<>> index_;
};

}  // namespace marketscan

#endif  // MARKETSCAN_CORE_SYMBOL_TABLE_HPP
