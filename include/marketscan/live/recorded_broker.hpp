#ifndef MARKETSCAN_LIVE_RECORDED_BROKER_HPP
#define MARKETSCAN_LIVE_RECORDED_BROKER_HPP

/*
 * An IBroker that answers from a recording.
 *
 * WHY THIS EXISTS
 *
 * Because the first regular-session equivalence run diverged, and the strategy
 * was not at fault. Live, twenty-seven orders were refused; in replay the fill
 * simulator accepted them. The strategy reacted correctly to both -- reacting to
 * a refusal is what a live strategy is *for* -- but it was reacting to different
 * information, so it made different decisions and the test blamed it.
 *
 * A theory that may not observe its own order state is a theory that cannot
 * trade. The fix belongs here: replay hands the strategy exactly what the venue
 * handed it.
 *
 * WHAT IT DOES NOT DO
 *
 * It does not simulate. There is no fill model behind this, no book, no
 * matching. Every answer it gives was recorded from a venue. A replay of a
 * synthetic panel still uses `Broker` and the fill simulator, and which one a
 * run gets is decided by what the source file contains -- see
 * RecordingContents::has_venue_side. No flag, and nothing in the engine or in a
 * strategy can tell which it has.
 *
 * THE CORRESPONDENCE ASSERTION
 *
 * The Nth order a strategy submits during replay is matched to the Nth recorded
 * submission and checked against it: symbol, side, quantity, type, price. If
 * they differ, the decisions have diverged, and this says so *there* -- with a
 * diff -- rather than feeding the wrong answer forward and letting the
 * divergence surface somewhere unrelated.
 *
 * That makes the equivalence test self-checking. As long as the decisions agree
 * the responses correspond by construction; the moment they do not, the failure
 * names the order.
 */

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "marketscan/core/symbol_table.hpp"
#include "marketscan/exec/broker.hpp"
#include "marketscan/live/order_tracker.hpp"
#include "marketscan/live/recorder.hpp"

namespace marketscan::live {

/// A submission that did not match what the recording expected.
struct CorrespondenceFailure {
    bool happened = false;

    /// Which submission, counting from zero.
    std::size_t index = 0;

    /// What the recording has, and what replay asked for.
    std::string expected;
    std::string actual;

    /// Which field first differed, named so the diff reads.
    std::string field;

    [[nodiscard]] std::string describe() const;
};

/// Replays one session's venue answers.
class RecordedBroker final : public IBroker {
public:
    /// `symbols` must already contain every ticker the recording names -- the
    /// feed interns them, and this looks them up. `starting_cash` should match
    /// what the live run used, or the ledgers are not comparable.
    RecordedBroker(std::vector<RecordedSubmission> submissions,
                   std::vector<RecordedResponse> responses,
                   const SymbolTable* symbols,
                   Money starting_cash,
                   OrderId order_id_base,
                   std::string run_token);

    void set_fill_listener(IFillListener* listener) noexcept override {
        tracker_.set_fill_listener(listener);
    }

    /// Matches this order against the next recorded submission, asserts they
    /// correspond, and returns the answer the live run got.
    OrderId
    submit(const MS_OrderRequest& request, Timestamp now, Timestamp decision_ts = kNever) override;

    /// A cancel is not itself recorded: what a cancel *did* arrives as a venue
    /// response, and that is what replay delivers. This therefore answers the
    /// way the live broker answered for an order already finished, and otherwise
    /// reports that the cancel was sent.
    bool cancel(OrderId id, Timestamp now) override;

    /// Delivers every recorded response that had arrived by this point in the
    /// recording.
    void on_market_event(SymbolId symbol, Timestamp now) override;

    [[nodiscard]] Ledger& ledger() noexcept override { return ledger_; }

    [[nodiscard]] const Ledger& ledger() const noexcept override { return ledger_; }

    [[nodiscard]] const std::vector<Order>& orders() const noexcept override {
        return tracker_.orders();
    }

    [[nodiscard]] const Order* find_order(OrderId id) const noexcept override {
        return tracker_.find_order(id);
    }

    [[nodiscard]] std::vector<const Order*> working_orders() const override {
        return tracker_.working_orders();
    }

    [[nodiscard]] std::uint64_t rejected_count() const noexcept override {
        return tracker_.stats().rejected;
    }

    [[nodiscard]] std::uint64_t late_cancel_count() const noexcept override {
        return tracker_.stats().lost_cancel_races;
    }

    /// Refuses, exactly as the live broker does. A recording is a fixed
    /// sequence, and replaying it twice from one object would need the cursor
    /// rewound and the ledger cleared -- which is a new run, not a reset.
    void reset() override;

    [[nodiscard]] std::string_view venue() const noexcept override { return "recorded"; }

    // --- Replay only ------------------------------------------------------

    /// Where to read the replay's progress from. The feed writes it.
    ///
    /// A plain counter shared between two objects that both read one file, and
    /// deliberately not a flag either of them branches on. Null means deliver
    /// nothing until drain_remaining.
    void set_position_source(const ReplayPosition* source) noexcept { position_ = source; }

    /// Delivers whatever is left, for the end of a replay.
    void drain_remaining(Timestamp now);

    [[nodiscard]] const CorrespondenceFailure& failure() const noexcept { return failure_; }

    [[nodiscard]] std::size_t submissions_matched() const noexcept { return cursor_; }

    [[nodiscard]] std::size_t submissions_recorded() const noexcept { return submissions_.size(); }

    [[nodiscard]] std::size_t responses_delivered() const noexcept { return delivered_; }

    [[nodiscard]] std::size_t responses_recorded() const noexcept { return responses_.size(); }

    [[nodiscard]] OrderTrackerStats stats() const { return tracker_.stats(); }

    /// Checks the tallies against the orders and the blotter they describe.
    [[nodiscard]] StatsReconciliation reconcile_stats() const { return tracker_.reconcile(); }

private:
    [[nodiscard]] bool corresponds(const RecordedSubmission& expected,
                                   const MS_OrderRequest& actual);

    std::vector<RecordedSubmission> submissions_;
    std::vector<RecordedResponse> responses_;
    const SymbolTable* symbols_;

    Ledger ledger_;
    OrderTracker tracker_;

    std::size_t cursor_ = 0;
    std::size_t delivered_ = 0;
    const ReplayPosition* position_ = nullptr;
    bool draining_ = false;
    OrderId order_id_base_ = 0;

    CorrespondenceFailure failure_;
};

}  // namespace marketscan::live

#endif  // MARKETSCAN_LIVE_RECORDED_BROKER_HPP
