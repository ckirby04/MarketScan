#ifndef MARKETSCAN_APPS_LIVE_EQUIVALENCE_HPP
#define MARKETSCAN_APPS_LIVE_EQUIVALENCE_HPP

/*
 * The equivalence test: does the same plugin binary make the same decisions
 * live and in replay?
 *
 * WHAT IS COMPARED
 *
 * The order sequence. For each order, in submission order: the symbol, the
 * side, the quantity, the type, the time in force, the limit price, the
 * strategy's own client order id, and the timestamp of the event in that
 * symbol which the strategy had finished being shown when it submitted. That
 * last one is the "triggering event", and it is recorded rather than inferred
 * -- see Order::decision_ts.
 *
 * WHAT IS NOT COMPARED, AND WHY NOT
 *
 * Fills. Live, the venue decides what fills and at what price; in replay, the
 * fill simulator does, against a book rebuilt from the recording. Those differ
 * and are supposed to: the point of the simulator is that it is a model, and a
 * model that agreed with a venue exactly would be a model that had been fitted
 * to one session.
 *
 * WHAT A PASS DOES AND DOES NOT ESTABLISH
 *
 * A pass says the event pipeline is the same on both sides: the same bytes
 * produce the same events in the same order with the same timestamps and the
 * same symbol ids, and a strategy fed them reaches the same decisions.
 *
 * It does not say a fill-dependent strategy would agree. One would not, and
 * that is a true statement about that strategy rather than a defect here --
 * live it saw the venue's fills and in replay it saw the simulator's. The
 * plugin this runs is written not to branch on fills for exactly that reason,
 * and says so in its own header comment.
 *
 * A divergence is reported as the first differing order with both sides
 * printed. The comparison is never relaxed to make it pass.
 */

#include <string>
#include <vector>

#include "marketscan/core/symbol_table.hpp"
#include "marketscan/exec/broker.hpp"

namespace marketscan::live {

/// One decision, in the form the comparison uses.
struct Decision {
    std::size_t index = 0;
    std::string symbol;
    Side side = MS_SIDE_BUY;
    MS_OrderType type = MS_ORD_MARKET;
    MS_TimeInForce tif = MS_TIF_DAY;
    Quantity quantity = 0.0;
    Price limit_price = 0.0;
    std::uint64_t client_order_id = 0;

    /// The information horizon: the last event in this symbol the strategy had
    /// finished being shown when it submitted.
    Timestamp decision_ts = kNever;

    [[nodiscard]] std::string describe() const;

    [[nodiscard]] friend bool operator==(const Decision& a, const Decision& b) noexcept;
};

/// Extracts the decision sequence from a broker's order log.
///
/// The order log *is* the decision log: every order ever submitted, in
/// submission order, with the request kept verbatim. Nothing extra had to be
/// built to make this comparison possible, which is the reason it is
/// trustworthy -- a log written specially for a test is a log that can be
/// written to make the test pass.
[[nodiscard]] std::vector<Decision> decisions_of(const IBroker& broker, const SymbolTable& symbols);

/// The first place two sequences differ, if they differ.
struct DecisionDiff {
    bool equal = false;
    std::size_t index = 0;
    std::string live;
    std::string replayed;
    std::string what;
};

[[nodiscard]] DecisionDiff compare(const std::vector<Decision>& live,
                                   const std::vector<Decision>& replayed);

/// The `ms_live equivalence` subcommand.
///
/// In its own translation unit because it is the only place that needs the
/// plugin loader, the engine and both brokers at once, and because the
/// comparison above is worth being able to read without the driver around it.
[[nodiscard]] int run_equivalence(int argc, char** argv);

/// The `ms_live probe` subcommand: how many TLS connections can this process
/// hold open before requests stop completing? See probe_cmd.cpp for why that
/// turned out to be a question worth measuring.
[[nodiscard]] int run_probe(int argc, char** argv);

/// The `ms_live positions` subcommand, and `--flatten` to close them.
///
/// Flatten exists because reconciliation is only useful while it can pass: a
/// stray position left by a killed run makes every later check red, and a
/// permanently red check hides the next real failure. See positions_cmd.cpp.
[[nodiscard]] int run_positions(int argc, char** argv);

/// The `ms_live orders` subcommand: what the venue thinks happened, order by
/// order. Written to investigate a reconciliation divergence rather than to
/// admire it. Reads only.
[[nodiscard]] int run_orders(int argc, char** argv);

}  // namespace marketscan::live

#endif  // MARKETSCAN_APPS_LIVE_EQUIVALENCE_HPP
