#ifndef MARKETSCAN_EXEC_DELISTING_HPP
#define MARKETSCAN_EXEC_DELISTING_HPP

/*
 * What a position is worth after the last quoted price. The assumed exit, named.
 *
 * Session D Part 2. `docs/EXECUTION.md` calls the shared limitation **the
 * assumed exit**: neither layer of this project knows what a delisting actually
 * cost, so both substitute something. This is the engine's half of the switch and
 * `research/delisting.py` is the research layer's. The setting names are
 * identical on both sides on purpose -- when CRSP delisting returns arrive,
 * turning this on is the whole of the change.
 *
 * THE TWO SETTINGS
 *
 * `last_price` liquidates at the final quoted price and applies no delisting
 * return. **This is the default, and it is the optimistic one.** It is what
 * `StrategyInstance::sweep_removals` has always done, and it is what a backtest
 * does when nobody has thought about the question.
 *
 * `apply_true_return` uses the delisting return where the data supplies it. On a
 * synthetic panel the generator supplies it by construction; on real data only a
 * vendor who measured it can. A security the table says nothing about is
 * liquidated at the last price either way -- a missing row is a coverage limit,
 * not a delisting return of zero, and the two are told apart by
 * `DelistingReturns::has`.
 *
 * WHY THE DEFAULT IS THE WRONG-BUT-CURRENT ONE
 *
 * Because changing a default silently moves every committed number measured
 * under it. The flattery this assumption buys is worth measuring, not worth
 * assuming away by fiat; docs/PATHOLOGIES.md carries the measurement.
 */

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "marketscan/core/money.hpp"
#include "marketscan/core/symbol_table.hpp"
#include "marketscan/core/types.hpp"

namespace marketscan {

/// How a position in a security that has left the universe is valued.
enum class DelistingReturnModel {
    /// Liquidate at the final quoted price. No delisting return. The default.
    LastPrice,

    /// Apply the delisting return where the data supplies one.
    ApplyTrueReturn,
};

/// The current behaviour, kept as the default so no committed result moves.
inline constexpr DelistingReturnModel kDefaultDelistingReturnModel =
    DelistingReturnModel::LastPrice;

/// The setting's name, spelled exactly as `research/delisting.py` spells it.
///
/// Identical strings on both sides, because a report that says which assumption
/// produced it has to be comparable across the two layers.
[[nodiscard]] std::string_view name_of(DelistingReturnModel model) noexcept;

/// Parses a name from a config file or a command line. False for anything else,
/// rather than falling back to a default: a misspelled model silently meaning
/// `last_price` is how a run gets measured under an assumption nobody chose.
[[nodiscard]] bool parse_delisting_return_model(std::string_view name,
                                                DelistingReturnModel& out) noexcept;

/// One line, for a report that has to say which assumption produced it.
[[nodiscard]] std::string_view describe(DelistingReturnModel model) noexcept;

/// The cash a delisting return is worth on a position.
///
/// `mark` is the last price the book held, `quantity` is signed, and the return
/// is measured from that mark: a long that delists at -0.6 gives up six tenths of
/// its marked value and a short of the same name gains it.
///
/// The expression order is load-bearing and `research/sizing.notional_micros` is
/// handed the same two doubles in the same order. `mark * delisting_return` is
/// formed first, then rounded once against the quantity -- rounding twice, or
/// multiplying in the other order, puts the two layers a micro-dollar apart on
/// some positions and the parity assertion carries no tolerance.
[[nodiscard]] inline Money
delisting_adjustment(Price mark, Quantity quantity, double delisting_return) noexcept {
    return Money::notional(mark * delisting_return, quantity);
}

/// Delisting returns, by security.
///
/// A vector kept in ascending symbol order rather than a hash map: this is read
/// from the forced-exit path, which emits orders, and anything iterating a hash
/// container there would make a run depend on a hash seed. It is also tiny --
/// one row per security that ever left -- so a linear search costs less than the
/// hash it would replace.
class DelistingReturns {
public:
    struct Entry {
        SymbolId symbol = kInvalidSymbol;

        /// Return from the last quoted price to what the position was worth.
        /// -1.0 is a total loss.
        double delisting_return = 0.0;

        /// Free text from the source, for the audit trail. The engine never
        /// branches on it.
        std::string cause;
    };

    /// Adds or replaces the row for `symbol`.
    void add(SymbolId symbol, double delisting_return, std::string cause = {});

    /// Whether the data says anything at all about this security.
    ///
    /// Distinct from a return of zero on purpose. A security with no row is one
    /// nobody measured, and it is liquidated at the last price under either
    /// model; a row of 0.0 is a measurement that the delisting cost nothing.
    [[nodiscard]] bool has(SymbolId symbol) const noexcept;

    /// The return for `symbol`, or `fallback` when there is no row.
    [[nodiscard]] double find(SymbolId symbol, double fallback = 0.0) const noexcept;

    [[nodiscard]] const std::vector<Entry>& entries() const noexcept { return entries_; }

    [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }

    [[nodiscard]] bool empty() const noexcept { return entries_.empty(); }

private:
    std::vector<Entry> entries_;
};

/// A delisting return table, as loaded.
struct DelistingReturnsFile {
    DelistingReturns returns;

    /// Rows the source had that could not be used, with the reason. Reported
    /// rather than dropped: a table whose tickers do not match the feed's would
    /// otherwise look exactly like a universe nobody ever left.
    std::vector<std::string> rejected;
};

/// Reads `symbol,delisting_return[,cause]` with a header row.
///
/// Tickers are interned, so a name that appears here and nowhere in the price
/// data still gets an id -- which is what makes a row for a security the feed
/// does not carry visible as an unused row rather than a rejected one.
[[nodiscard]] DelistingReturnsFile load_delisting_returns(const std::filesystem::path& path,
                                                          SymbolTable& symbols);

}  // namespace marketscan

#endif  // MARKETSCAN_EXEC_DELISTING_HPP
