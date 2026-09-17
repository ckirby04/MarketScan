#ifndef MARKETSCAN_DATA_SECURITY_MASTER_HPP
#define MARKETSCAN_DATA_SECURITY_MASTER_HPP

/*
 * Which company a ticker was, on a given date.
 *
 * A TICKER IS NOT AN IDENTITY
 *
 * It is an attribute a security holds over a date range, and conflating the two
 * produces two distinct failures:
 *
 *   RENAME   FB became META in 2022. One security, two ticker periods. Treating
 *            it as two securities breaks a continuity that genuinely exists: a
 *            strategy cannot compute momentum across the change, and a position
 *            held through it looks like a sale and a purchase.
 *
 *   REUSE    A ticker dropped in 2003 and reassigned to an unrelated company in
 *            2011. Two securities, one string. Treating it as one welds company
 *            A's price history to company B's and manufactures a return that
 *            never existed -- typically an enormous one, because the two prices
 *            have nothing to do with each other.
 *
 * The second is far worse than the first, and that asymmetry drives everything
 * here. A broken continuity is a visible, bounded loss of history: the strategy
 * has less to work with and can say so. A fabricated price series is invisible,
 * unbounded, and poisons every statistic computed downstream of it. So when the
 * evidence is ambiguous, this splits.
 *
 * WHAT THIS GUARANTEES
 *
 *   - SecurityId is permanent and never reused within a dataset. It is what
 *     positions, ledgers, universe membership and book state key on. Nothing
 *     downstream keys on a string.
 *   - Resolution takes a date. There is no dateless overload and its absence is
 *     a compile error rather than a documented convention -- see the deleted
 *     declaration below.
 *   - A SecurityId is stable across renames. A security carries its ticker
 *     periods in order.
 *
 * WHEN CRSP ARRIVES
 *
 * PERMNO is exactly this: a permanent security identifier that survives renames
 * and is never reused. At that point SecurityId becomes PERMNO directly and the
 * inference in security_history.hpp is deleted rather than adapted. This
 * interface is shaped so that deletion is the whole of the change.
 */

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "marketscan/core/symbol_table.hpp"
#include "marketscan/core/types.hpp"

namespace marketscan {

/// A permanent identifier for one security.
///
/// The same integer the rest of the engine calls SymbolId, deliberately: the
/// ledger, the order book and the universe already key on that, and a second
/// parallel identifier would mean two things to get wrong instead of one. What
/// changes is how it is *obtained* -- through a date, always.
using SecurityId = SymbolId;

inline constexpr SecurityId kInvalidSecurity = kInvalidSymbol;

/// A ticker a security traded under, over a half-open date range.
struct TickerPeriod {
    std::string ticker;

    /// Inclusive.
    Timestamp from = kNever;

    /// Exclusive. kForever while current.
    Timestamp until = kForever;

    [[nodiscard]] bool covers(Timestamp ts) const noexcept { return ts >= from && ts < until; }
};

/// One security, and every ticker it has ever held.
struct Security {
    SecurityId id = kInvalidSecurity;

    /// Ascending by `from`, non-overlapping.
    std::vector<TickerPeriod> tickers;

    /// Why this exists as a separate security from another of the same ticker.
    /// Empty for the ordinary case. Populated by the split heuristic so the
    /// reason survives into the audit report and the GUI.
    std::string split_reason;

    [[nodiscard]] Timestamp first_seen() const noexcept {
        return tickers.empty() ? kNever : tickers.front().from;
    }

    [[nodiscard]] Timestamp last_seen() const noexcept {
        return tickers.empty() ? kNever : tickers.back().until;
    }

    /// The ticker in force at `ts`, or the nearest one if `ts` is outside every
    /// period. Never empty for a valid security: something has to be displayed.
    [[nodiscard]] std::string_view ticker_at(Timestamp ts) const noexcept;

    /// The most recent ticker. What a person means when they name the company.
    [[nodiscard]] std::string_view primary_ticker() const noexcept {
        return tickers.empty() ? std::string_view{} : std::string_view(tickers.back().ticker);
    }
};

/// The identity layer: tickers in, permanent ids out.
///
/// Owns a SymbolTable and assigns ids through it, so everything downstream that
/// already keys on SymbolId keeps working unchanged. Two incarnations of a
/// reused ticker are interned under distinct internal keys -- "XYZ" and
/// "XYZ~2" -- which is what makes them different ids. Those keys are an
/// implementation detail; ticker_at() is what anything user-facing displays.
class SecurityMaster {
public:
    explicit SecurityMaster(SymbolTable& symbols) : symbols_(&symbols) {}

    /// Registers a ticker period, returning the security it belongs to.
    ///
    /// A period whose ticker already exists extends that security if it is
    /// contiguous with it, and creates a new one if `reason` is non-empty --
    /// which is how the split heuristic says "this is a different company".
    SecurityId
    declare(std::string_view ticker, Timestamp from, Timestamp until, std::string reason = {});

    /// Adds another ticker period to a security that already exists.
    ///
    /// This is how a **rename** is expressed, and it has to be declared rather
    /// than inferred. FB became META in 2022; from ticker-keyed membership data
    /// that is indistinguishable from FB being removed and META being added,
    /// because the two events look identical and nothing in the source says
    /// otherwise. Only something that knows about the company can say which it
    /// was -- a corporate-actions file, or a person.
    ///
    /// The consequence, stated plainly: **renames are not detected from the
    /// free data stack.** A renamed company appears as two securities, which
    /// breaks continuity across the change. That is the safe direction of the
    /// two errors -- see the header comment -- but it is an error, and CRSP's
    /// PERMNO fixes it for free by supplying identity directly.
    void add_ticker(SecurityId id, std::string_view ticker, Timestamp from, Timestamp until);

    /// The security trading as `ticker` on `as_of`, or kInvalidSecurity.
    [[nodiscard]] SecurityId resolve(std::string_view ticker, Timestamp as_of) const noexcept;

    /// Deleted on purpose, and this is the enforcement the specification asks
    /// for rather than a convention in a comment.
    ///
    /// "Which company is XYZ" has no answer without a date. Writing it is a
    /// compile error here, so the question cannot be asked by accident -- which
    /// is the only kind of asking that produces a welded price series.
    SecurityId resolve(std::string_view ticker) const = delete;

    [[nodiscard]] const Security* find(SecurityId id) const noexcept;

    /// The ticker `id` traded under at `ts`. Empty for an unknown id.
    [[nodiscard]] std::string_view ticker_at(SecurityId id, Timestamp ts) const noexcept;

    [[nodiscard]] std::size_t size() const noexcept { return securities_.size(); }

    /// Every security, in id order.
    [[nodiscard]] const std::vector<Security>& securities() const noexcept { return securities_; }

    /// How many distinct securities have ever held `ticker`. Greater than one
    /// means the string was reused.
    [[nodiscard]] std::size_t incarnations(std::string_view ticker) const noexcept;

    [[nodiscard]] SymbolTable& symbols() noexcept { return *symbols_; }

    [[nodiscard]] const SymbolTable& symbols() const noexcept { return *symbols_; }

private:
    /// The internal key a security is interned under: the ticker for the first
    /// incarnation, "TICKER~n" for later ones.
    [[nodiscard]] static std::string intern_key(std::string_view ticker, std::size_t incarnation);

    SymbolTable* symbols_;

    /// Indexed by id - 1, so securities_[i].id == i + 1.
    std::vector<Security> securities_;

    /// Ticker to the ids that have held it, in declaration order. A std::map
    /// and not a hash map, for the reason every ordered container in this
    /// project is one: nothing may iterate into an order-emitting path.
    std::map<std::string, std::vector<SecurityId>, std::less<>> by_ticker_;
};

}  // namespace marketscan

#endif  // MARKETSCAN_DATA_SECURITY_MASTER_HPP
