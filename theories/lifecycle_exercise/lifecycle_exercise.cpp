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
//   - A marketable limit, priced through the touch. Fills, usually at once.
//     Exercises the ordinary path.
//   - A non-marketable limit, priced far away. Rests, and is then either
//     cancelled or left to expire at the close. Exercises a working order, a
//     cancel, and an expiry -- three states a zero-latency simulator cannot
//     produce, because it answers every order before the next event.
//   - A cancel sent one event after the marketable order. Sometimes it wins
//     and sometimes it loses the race, and the losing case is the one worth
//     having: a strategy whose cancels are routinely too late is assuming an
//     execution speed it does not have.
//   - An order for more shares than the account can pay for, once. Provokes a
//     rejection from the venue rather than from the simulator, which is the
//     only way to find out whether the rejection path works.
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
        marketscan::param_double("quantity",
                                 "Shares per order. One by default, because nothing here "
                                 "has any reason to send size.",
                                 1.0,
                                 10.0,
                                 1.0),
        marketscan::param_double("away_pct",
                                 "How far from the touch the resting limit is priced, in "
                                 "per cent. Far enough that it will not fill by accident.",
                                 1.0,
                                 50.0,
                                 20.0),
        marketscan::param_int("provoke_rejection",
                              "Send one order the account cannot possibly pay for, to "
                              "exercise the venue's rejection path. 1 to enable.",
                              0.0,
                              1.0,
                              1.0),
    };
    static constexpr uint32_t kParamCount = 6;

    LifecycleExercise() {
        bind_param("symbol_index", &symbol_index_);
        bind_param("follow_first", &follow_first_);
        bind_param("period", &period_);
        bind_param("quantity", &quantity_);
        bind_param("away_pct", &away_pct_);
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
        switch (turn % 4) {
            case 0:
                submit_marketable();
                break;
            case 1:
                submit_resting();
                break;
            case 2:
                cancel_the_resting_order();
                break;
            case 3:
                provoke_a_rejection();
                break;
            default:
                break;
        }
    }

    void submit_marketable() {
        // Through the touch, so it is marketable now and stays marketable for
        // the moment it takes to arrive. A limit rather than a market order:
        // a market order has no worst price, and nothing here is worth an
        // unbounded one.
        const double price = ask_ * 1.001;
        marketable_id_ = submit_limit(target(), MS_SIDE_BUY, quantity_, price, MS_TIF_DAY, 1);
        ++submits_;
        publish();
    }

    void submit_resting() {
        // Far below the bid. It will not fill; it will rest until it is
        // cancelled or the session ends, which is the state being exercised.
        const double price = bid_ * (1.0 - away_pct_ / 100.0);
        resting_id_ = submit_limit(target(), MS_SIDE_BUY, quantity_, price, MS_TIF_DAY, 2);
        ++submits_;
        publish();
    }

    void cancel_the_resting_order() {
        if (resting_id_ != 0) {
            cancel(resting_id_);
            ++cancels_sent_;
            resting_id_ = 0;
        }
        // And the marketable one too, which by now has almost certainly
        // filled. That is the point: the losing side of a cancel race is the
        // case worth having run.
        if (marketable_id_ != 0) {
            cancel(marketable_id_);
            ++cancels_sent_;
            marketable_id_ = 0;
        }
        publish();
    }

    void provoke_a_rejection() {
        if (provoke_rejection_ < 0.5 || rejection_provoked_) {
            return;
        }
        rejection_provoked_ = true;

        // Sell what is not held, on a cash account that does not permit it.
        // A refusal rather than a fill is the expected and desired outcome,
        // and it exercises a path nothing else here reaches.
        (void)submit_limit(target(), MS_SIDE_SELL, quantity_, bid_ * 0.5, MS_TIF_IOC, 3);
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
    }

    double symbol_index_ = 0.0;
    double follow_first_ = 1.0;
    MS_SymbolId followed_ = 0;
    double period_ = 200.0;
    double quantity_ = 1.0;
    double away_pct_ = 20.0;
    double provoke_rejection_ = 1.0;

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
    bool rejection_provoked_ = false;
};

}  // namespace

MARKETSCAN_EXPORT_STRATEGY(LifecycleExercise,
                           "lifecycle_exercise",
                           "Lifecycle Exercise (NOT A STRATEGY)",
                           "Clark Kirby")
