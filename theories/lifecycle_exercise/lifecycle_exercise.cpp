// lifecycle_exercise -- NOT A STRATEGY.
//
// This exists to make an execution venue do every thing a venue can do, so
// that the code which handles those things has been run against a real one
// before anything that matters depends on it. It has no edge, seeks none, and
// would lose money on any account it were pointed at. That is not a caveat, it
// is the specification.
//
// WHAT IT DOES, AND WHY EACH PIECE IS THERE
//
//   - A marketable limit, priced through the touch, alternating buy and sell.
//     Fills, usually at once. Exercises the ordinary path, and alternating
//     keeps the position near flat -- which stopped being cosmetic the moment
//     the orders stopped being one share; see submit_marketable.
//   - A non-marketable limit a few basis points below the bid. It rests, and
//     is reachable, and is then cancelled after a dwell. Exercises a working
//     order, a partial, and a cancel -- states a zero-latency simulator cannot
//     produce, because it answers every order before the next event.
//   - A cancel sent a rotation after the marketable order. Sometimes it wins
//     and sometimes it loses the race, and the losing case is the one worth
//     having: a strategy whose cancels are routinely too late is assuming an
//     execution speed it does not have.
//   - One order with a sub-penny limit price, which the venue must refuse.
//     Exercises the rejection path in any session, unlike the short-sell IOC
//     it replaced -- that one was refused only *outside* market hours, so in a
//     regular session it silently became an ordinary short instead.
//
// SIZED SO A PARTIAL IS POSSIBLE AT ALL
//
// Until Session F it sent one share, which can never partially fill, so the
// partial-fill path was unreachable by construction. Orders are now sized from
// a notional target and the current ask -- the same dollar size whichever
// instrument is being worked, across a fourfold price range. See `notional`.
//
// WHY THE DECISIONS DO NOT DEPEND ON FILLS
//
// This is the plugin the equivalence test runs, live and then again against a
// recording of that same session. Live it gets the venue's fills; in replay it
// gets the simulator's, and those differ -- they are supposed to. So the
// *decisions* here are a function of the event stream alone: a counter of
// events seen, and nothing else. Fills and order statuses are counted into
// metrics and never consulted.
//
// That is a real constraint and it is worth being plain about what it buys and
// what it does not. It buys a comparison in which any difference is a
// difference in the pipeline, because nothing else could have caused one. It
// does not prove that a fill-dependent strategy would agree across the two --
// and such a strategy would not, which is a true statement about that
// strategy rather than a defect in the seam. See docs/EXECUTION.md.

#include <cmath>

#include "marketscan/strategy/istrategy.hpp"

namespace {

class LifecycleExercise final : public marketscan::IStrategy {
public:
    static constexpr const char* kDescription =
        "NOT A STRATEGY. Submits small orders, cancels some, rests others, and "
        "provokes one rejection, so that every order state a venue can produce "
        "is exercised against a real one. It has no edge and loses money by "
        "design.";

    static constexpr MS_ParamDesc kParams[] = {
        marketscan::param_int("symbol_index",
                              "Which instrument to work, as an index into the run's symbol "
                              "table. Tickers cannot be a parameter: the ABI's parameters "
                              "are all doubles. Only consulted when follow_first is 0.",
                              0.0,
                              63.0,
                              0.0),
        marketscan::param_int("follow_first",
                              "Work whichever instrument shows the first two-sided quote, "
                              "rather than a fixed index. 1 to enable.",
                              0.0,
                              1.0,
                              1.0),
        marketscan::param_int("period",
                              "Events between actions. Low enough to see several full "
                              "lifecycles in a short session, high enough not to send a "
                              "venue more traffic than it was asked for.",
                              10.0,
                              100000.0,
                              200.0),
        // SIZING IS BY NOTIONAL, NOT BY SHARE COUNT
        //
        // One share can never partially fill, so the earlier default of 1 made
        // the partial-fill path unreachable by construction -- it is the one
        // order state this exercise had never produced against a venue.
        //
        // A share count cannot replace it directly, because these instruments
        // span a fourfold price range: 200 shares is $36k of NVDA and $155k of
        // SPY, and one number cannot be both safe on a $100k account and large
        // enough to be taken in pieces. A notional target is the same size
        // whichever instrument the run happens to work.
        //
        // The ask comes from the event stream, so the derived share count is
        // identical in replay and the equivalence test still holds.
        marketscan::param_double("notional",
                                 "Target dollars per order. Shares are derived from this "
                                 "and the current ask, so the order is the same size "
                                 "whichever instrument is worked.",
                                 100.0,
                                 200000.0,
                                 15000.0),
        marketscan::param_double("max_shares",
                                 "Ceiling on the derived share count, so a cheap "
                                 "instrument cannot turn a modest notional into a large "
                                 "number of shares.",
                                 1.0,
                                 1000.0,
                                 300.0),
        marketscan::param_double("rest_bps",
                                 "How far below the bid the resting limit sits, in basis "
                                 "points. Was a percentage, and 20 per cent meant the "
                                 "order could never be reached at all -- an order that "
                                 "cannot fill cannot partially fill either. A few basis "
                                 "points rests, and is reachable within seconds.",
                                 0.5,
                                 5000.0,
                                 4.0),
        marketscan::param_int("rest_periods",
                              "Rotations the resting order is left alone before a cancel "
                              "reaches it. The cancel used to arrive one rotation later, "
                              "which is a second or two -- far too short for the market "
                              "to come down and take part of it.",
                              1.0,
                              500.0,
                              10.0),
        marketscan::param_int("provoke_rejection",
                              "Send one order the venue must refuse, to exercise the "
                              "rejection path. 1 to enable.",
                              0.0,
                              1.0,
                              1.0),
    };
    static constexpr uint32_t kParamCount = 8;

    LifecycleExercise() {
        bind_param("symbol_index", &symbol_index_);
        bind_param("follow_first", &follow_first_);
        bind_param("period", &period_);
        bind_param("notional", &notional_);
        bind_param("max_shares", &max_shares_);
        bind_param("rest_bps", &rest_bps_);
        bind_param("rest_periods", &rest_periods_);
        bind_param("provoke_rejection", &provoke_rejection_);
    }

    void on_start() override {
        log_info(
            "lifecycle exercise: this is not a strategy and has no edge. It submits, cancels "
            "and rests small orders to exercise the order lifecycle, and loses money by design.");
    }

    // The touch is remembered as it goes past, because the ABI gives a
    // strategy no way to ask what the current quote is -- which is what keeps
    // a strategy a function of the events it was shown.
    void on_quote(const MS_Quote& quote) override {
        if (follow_first_ >= 0.5 && followed_ == 0 && quote.bid_px > 0.0 && quote.ask_px > 0.0) {
            followed_ = quote.symbol;
            log_info("working the first instrument with a two-sided quote");
        }
        if (quote.symbol != target()) {
            return;
        }
        if (quote.bid_px > 0.0) {
            bid_ = quote.bid_px;
        }
        if (quote.ask_px > 0.0) {
            ask_ = quote.ask_px;
        }
        step();
    }

    void on_trade(const MS_Trade& trade) override {
        if (trade.symbol != target()) {
            return;
        }
        if (trade.price > 0.0 && bid_ <= 0.0) {
            // A session that has shown no quote yet still gives a price to
            // work from, so the exercise does not sit idle waiting.
            bid_ = trade.price;
            ask_ = trade.price;
        }
        step();
    }

    void on_bar(const MS_Bar& bar) override {
        if (bar.symbol != target()) {
            return;
        }
        if (bar.close > 0.0 && bid_ <= 0.0) {
            bid_ = bar.close;
            ask_ = bar.close;
        }
        step();
    }

    // Counted, published, and never consulted for a decision. See the header
    // comment: the decision sequence has to be a function of the event stream
    // alone for the equivalence test to mean anything.
    void on_fill(const MS_Fill& fill) override {
        ++fills_;
        filled_shares_ += fill.quantity;

        // A fill smaller than an order is a partial. Compared against the
        // current derived share count rather than against a remembered
        // per-order quantity: every order in a run is sized from the same
        // notional and a barely-moving ask, so the two differ by less than a
        // share, and this metric only has to tell "some" from "all".
        if (fill.quantity > 0.0 && fill.quantity < shares() - 0.5) {
            ++partial_fills_;
        }
        publish();
    }

    void on_order_status(const MS_OrderStatus& status) override {
        ++statuses_;
        if (status.terminal != 0u) {
            ++terminal_statuses_;
        }
        switch (status.state) {
            case MS_ORDER_REJECTED:
                ++rejections_;
                break;
            case MS_ORDER_CANCELLED:
                ++cancellations_;
                break;
            case MS_ORDER_EXPIRED:
                ++expiries_;
                break;
            default:
                break;
        }
        publish();
    }

    void on_stop() override {
        publish();
        log_info("lifecycle exercise finished; it lost money, which was the plan");
    }

private:
    // WHY THIS IS NOT JUST AN INDEX
    //
    // An exercise that sits idle because the instrument it was pointed at is
    // quiet has exercised nothing. That is not hypothetical: the first real
    // session ran five symbols in extended hours, four of them produced no
    // events at all on this venue's feed, and the one index the exercise had
    // been given was one of the four. It watched 802 events and made no
    // decisions.
    //
    // So by default it works whichever instrument shows it the first two-sided
    // quote. That keeps its decisions a pure function of the event stream --
    // the same stream picks the same instrument in replay -- which is what the
    // equivalence test needs. See the header comment.
    [[nodiscard]] MS_SymbolId target() const noexcept {
        if (follow_first_ >= 0.5) {
            return followed_;
        }
        const auto index = static_cast<uint32_t>(symbol_index_);
        return index < symbol_count() ? index + 1 : 1;
    }

    void step() {
        ++events_;
        if (!(bid_ > 0.0) || !(ask_ > 0.0)) {
            return;
        }

        const auto period = static_cast<uint64_t>(period_ < 10.0 ? 10.0 : period_);
        if (events_ % period != 0) {
            return;
        }

        // Which of the four actions this turn takes. A fixed rotation rather
        // than a random choice: a run that does a different thing each time is
        // a run whose failures cannot be reproduced.
        const uint64_t turn = events_ / period;
        turn_ = turn;
        switch (turn % 4) {
            case 0:
                submit_marketable();
                break;
            case 1:
                submit_resting();
                break;
            case 2:
                cancel_the_marketable_order();
                break;
            case 3:
                provoke_a_rejection();
                break;
            default:
                break;
        }

        // Independent of the rotation, because the resting order's whole
        // purpose is to sit still for a while. It is cancelled once it has
        // been alone for rest_periods rotations -- counted, not timed, so
        // replay reaches the same decision.
        maybe_cancel_the_resting_order();
    }

    /// Rounds a price to a whole cent.
    ///
    /// The venue refuses a sub-penny limit price for any stock above a dollar,
    /// so a price derived by multiplying the touch has to be rounded or it is a
    /// rejection waiting to happen. `provoke_a_rejection` deliberately does not
    /// use this, which is the whole of its mechanism.
    [[nodiscard]] static double round_to_penny(double price) noexcept {
        return std::floor(price * 100.0 + 0.5) / 100.0;
    }

    /// Shares for one order, from the notional target and the current ask.
    ///
    /// Derived rather than configured: see the `notional` parameter. Rounded to
    /// a whole share and clamped, and never zero -- an order for no shares is a
    /// refusal the venue would have to explain.
    [[nodiscard]] double shares() const noexcept {
        if (!(ask_ > 0.0)) {
            return 1.0;
        }
        double count = std::floor(notional_ / ask_ + 0.5);
        if (count < 1.0) {
            count = 1.0;
        }
        if (count > max_shares_) {
            count = max_shares_;
        }
        return count;
    }

    void submit_marketable() {
        // Through the touch, so it is marketable now and stays marketable for
        // the moment it takes to arrive. A limit rather than a market order:
        // a market order has no worst price, and nothing here is worth an
        // unbounded one.
        //
        // ALTERNATING SIDES, AND WHY IT IS NOT DECORATION
        //
        // At one share a side did not matter. At a real notional it decides
        // whether the exercise is safe: buying every time, a session of fourteen
        // marketable orders at $25k asks for $350k of stock on a $100k account,
        // which is both refusable for buying power and about three and a half
        // times levered if it is not refused. A dry run against a recorded
        // session showed exactly that -- nineteen orders, $471k of demand
        // against $400k of buying power.
        //
        // Alternating, the position oscillates around flat and the account is
        // never asked for more than one order's worth. It also exercises the
        // sell side, which buying every time never did.
        //
        // Driven by a counter, so replay makes the same choice.
        const bool buying = (marketable_count_++ % 2) == 0;
        const double price = buying ? round_to_penny(ask_ * 1.001) : round_to_penny(bid_ * 0.999);
        marketable_id_ = submit_limit(
            target(), buying ? MS_SIDE_BUY : MS_SIDE_SELL, shares(), price, MS_TIF_DAY, 1);
        ++submits_;
        publish();
    }

    void submit_resting() {
        // Just below the bid: not marketable, so it rests, but close enough
        // that an ordinary downtick reaches it. That is the point -- a resting
        // order partially taken during a brief dip is the likeliest way a
        // partial fill actually happens against paper.
        if (resting_id_ != 0) {
            return;  // One at a time, so the dwell means something.
        }
        const double price = round_to_penny(bid_ * (1.0 - rest_bps_ / 10'000.0));
        resting_id_ = submit_limit(target(), MS_SIDE_BUY, shares(), price, MS_TIF_DAY, 2);
        resting_turn_ = turn_;
        ++submits_;
        publish();
    }

    void cancel_the_marketable_order() {
        // The marketable order, which by now has almost certainly filled. That
        // is the point: the losing side of a cancel race is the case worth
        // having run, and it is the only way to find out how often this process
        // is too slow.
        if (marketable_id_ != 0) {
            cancel(marketable_id_);
            ++cancels_sent_;
            marketable_id_ = 0;
        }
        publish();
    }

    void maybe_cancel_the_resting_order() {
        if (resting_id_ == 0) {
            return;
        }
        const auto dwell = static_cast<uint64_t>(rest_periods_ < 1.0 ? 1.0 : rest_periods_);
        if (turn_ < resting_turn_ + dwell) {
            return;
        }
        cancel(resting_id_);
        ++cancels_sent_;
        resting_id_ = 0;
        publish();
    }

    void provoke_a_rejection() {
        if (provoke_rejection_ < 0.5 || rejection_provoked_) {
            return;
        }
        rejection_provoked_ = true;

        // A sub-penny limit price, which the venue refuses for any stock priced
        // above a dollar. No order is created and nothing is risked.
        //
        // WHY NOT THE PREVIOUS MECHANISM
        //
        // It used to sell short with an IOC, and the refusal it produced said
        // "ioc orders are only accepted during market hours" -- which means it
        // worked only *outside* market hours. In a regular session an IOC is
        // accepted, and this account has shorting enabled, so that order would
        // simply have opened a short position and the rejection path would have
        // gone unexercised in the one run that was meant to exercise it.
        //
        // A tick-size violation is refused in every session, on every account,
        // for a reason that has nothing to do with the clock.
        const double price = ask_ * 0.5 + 0.001234;
        (void)submit_limit(target(), MS_SIDE_BUY, shares(), price, MS_TIF_DAY, 3);
        ++submits_;
        publish();
    }

    void publish() {
        publish_metric("orders_submitted", static_cast<double>(submits_));
        publish_metric("cancels_sent", static_cast<double>(cancels_sent_));
        publish_metric("fills", static_cast<double>(fills_));
        publish_metric("filled_shares", filled_shares_);
        publish_metric("order_status_events", static_cast<double>(statuses_));
        publish_metric("terminal_statuses", static_cast<double>(terminal_statuses_));
        publish_metric("rejections", static_cast<double>(rejections_));
        publish_metric("cancellations", static_cast<double>(cancellations_));
        publish_metric("expiries", static_cast<double>(expiries_));
        publish_metric("events_seen", static_cast<double>(events_));
        publish_metric("instrument", static_cast<double>(target()));

        // The derived share count and what it is worth, so a report says how
        // big the orders actually were rather than only what was asked for.
        publish_metric("shares_per_order", shares());
        publish_metric("notional_per_order", shares() * ask_);
        publish_metric("partial_fills", static_cast<double>(partial_fills_));
    }

    double symbol_index_ = 0.0;
    double follow_first_ = 1.0;
    MS_SymbolId followed_ = 0;
    double period_ = 200.0;
    double notional_ = 15'000.0;
    double max_shares_ = 300.0;
    double rest_bps_ = 4.0;
    double rest_periods_ = 10.0;
    double provoke_rejection_ = 1.0;

    /// The current rotation, and the one the resting order was placed on.
    /// Counters rather than clock readings: `now()` is the wall clock live and
    /// the simulated clock in replay, so a decision taken from it would not
    /// reproduce. See the header comment.
    uint64_t turn_ = 0;
    uint64_t resting_turn_ = 0;

    /// Alternates the marketable order's side. A counter, not a clock and
    /// not a coin: replay has to make the same choice.
    uint64_t marketable_count_ = 0;

    double bid_ = 0.0;
    double ask_ = 0.0;

    uint64_t events_ = 0;
    uint64_t submits_ = 0;
    uint64_t cancels_sent_ = 0;
    uint64_t fills_ = 0;
    double filled_shares_ = 0.0;
    uint64_t statuses_ = 0;
    uint64_t terminal_statuses_ = 0;
    uint64_t rejections_ = 0;
    uint64_t cancellations_ = 0;
    uint64_t expiries_ = 0;

    uint64_t marketable_id_ = 0;
    uint64_t resting_id_ = 0;
    uint64_t partial_fills_ = 0;
    bool rejection_provoked_ = false;
};

}  // namespace

MARKETSCAN_EXPORT_STRATEGY(LifecycleExercise,
                           "lifecycle_exercise",
                           "Lifecycle Exercise (NOT A STRATEGY)",
                           "Clark Kirby")
