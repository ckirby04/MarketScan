// sma_crossover -- a calibration instrument, not a strategy.
//
// A fast simple moving average crossing a slow one is the oldest technical
// signal there is, and on any reasonably efficient instrument it has no edge.
// What it does have is *turnover*: on noisy intraday data the two averages
// cross constantly, so it trades often, and every trade pays the spread and a
// commission.
//
// That makes it a measuring instrument for the execution simulator rather than
// a trading idea:
//
//   On realistic fills this should come out flat to modestly negative after
//   costs. If it backtests profitably, the fill simulator is wrong.
//
// Run it against the optimistic fill model and it will look better -- that gap
// is precisely the cost of the spread, multiplied by how often it trades, and
// it is the number this theory exists to produce.

#include <cmath>
#include <cstring>

#include "marketscan/strategy/istrategy.hpp"

namespace {

class SmaCrossover final : public marketscan::IStrategy {
public:
    static constexpr const char* kDescription =
        "Fast/slow simple moving average crossover. A CALIBRATION INSTRUMENT, "
        "not a strategy: on realistic fills it should be flat to modestly "
        "negative after costs. If it is profitable, the fill simulator is "
        "wrong.";

    /// Bounds the history buffer, and therefore the slow period.
    static constexpr int kMaxPeriod = 400;

    static constexpr MS_ParamDesc kParams[] = {
        marketscan::param_int("fast",
                              "Length of the fast moving average, in bars.",
                              2.0,
                              200.0,
                              10.0),
        marketscan::param_int("slow",
                              "Length of the slow moving average, in bars. "
                              "Must exceed the fast length; if it does not, "
                              "the theory does nothing rather than inventing "
                              "a signal.",
                              3.0,
                              static_cast<double>(kMaxPeriod),
                              40.0),
        marketscan::param_int("symbol_index",
                              "Which instrument to trade, as an index into "
                              "the run's symbol table.",
                              0.0,
                              63.0,
                              0.0),
        marketscan::param_double("cash_reserve_pct",
                                 "Fraction of cash held back so that crossing "
                                 "the spread and paying commission cannot "
                                 "overdraw the account.",
                                 0.0,
                                 50.0,
                                 2.0),
        marketscan::param_bool("allow_shorts",
                               "Take the short side on a downward cross "
                               "instead of simply standing aside. Borrow cost "
                               "is not modelled, so a short here is cheaper "
                               "than a real one.",
                               false),
    };
    static constexpr uint32_t kParamCount = 5;

    SmaCrossover() {
        bind_param("fast", &fast_);
        bind_param("slow", &slow_);
        bind_param("symbol_index", &symbol_index_);
        bind_param("cash_reserve_pct", &cash_reserve_pct_);
        bind_param("allow_shorts", &allow_shorts_);
    }

    void on_start() override {
        log_info("sma_crossover is a calibration instrument, not a strategy");
    }

    // The ABI gives a strategy no way to ask what the current quote is, so the
    // touch is remembered as it goes past. This is what a market order will
    // actually pay, and sizing off the close instead is the mistake the
    // reserve exists to absorb.
    void on_quote(const MS_Quote& quote) override {
        if (quote.symbol != target()) {
            return;
        }
        if (quote.ask_px > 0.0) {
            last_ask_ = quote.ask_px;
        }
        if (quote.bid_px > 0.0) {
            last_bid_ = quote.bid_px;
        }
    }

    void on_bar(const MS_Bar& bar) override {
        if (bar.symbol != target() || !(bar.close > 0.0)) {
            return;
        }

        push(bar.close);

        const int fast = clamp_period(fast_);
        const int slow = clamp_period(slow_);

        // A fast average that is not faster than the slow one has no crossing
        // to detect. Doing nothing is the honest response; picking an
        // interpretation would invent a signal out of a misconfiguration.
        if (fast >= slow) {
            if (!warned_periods_) {
                warned_periods_ = true;
                log_warn("fast must be shorter than slow; standing aside");
            }
            return;
        }
        if (count_ < slow) {
            return;  // not enough history to form the slow average yet
        }

        const double fast_average = average(fast);
        const double slow_average = average(slow);
        publish_metric("fast_sma", fast_average);
        publish_metric("slow_sma", slow_average);
        publish_metric("spread_sma", fast_average - slow_average);

        const int signal = fast_average > slow_average ? 1 : -1;

        // Only act on a change. Acting on the state every bar would trade on
        // every bar, which measures the commission schedule rather than the
        // signal.
        if (previous_signal_ == 0) {
            previous_signal_ = signal;
            return;
        }
        if (signal == previous_signal_) {
            return;
        }
        previous_signal_ = signal;
        ++crossings_;
        publish_metric("crossings", static_cast<double>(crossings_));

        if (signal > 0) {
            go_long(bar.close);
        } else if (allow_shorts_) {
            go_short(bar.close);
        } else {
            flatten();
        }
    }

    void on_fill(const MS_Fill&) override {
        ++fills_;
        publish_metric("fills", static_cast<double>(fills_));
    }

    void on_stop() override {
        publish_metric("final_equity", static_cast<double>(equity()) / 1'000'000.0);
        if (crossings_ > 0 && fills_ == 0) {
            log_warn("crossed but never traded; check the cash reserve");
        }
    }

private:
    [[nodiscard]] MS_SymbolId target() const noexcept {
        const auto index = static_cast<uint32_t>(symbol_index_ < 0 ? 0 : symbol_index_);
        return index < symbol_count() ? static_cast<MS_SymbolId>(index + 1) : 1;
    }

    [[nodiscard]] static int clamp_period(int value) noexcept {
        if (value < 1) {
            return 1;
        }
        return value > kMaxPeriod ? kMaxPeriod : value;
    }

    void push(double close) noexcept {
        closes_[next_] = close;
        next_ = (next_ + 1) % kMaxPeriod;
        if (count_ < kMaxPeriod) {
            ++count_;
        }
    }

    /// Mean of the most recent `period` closes.
    ///
    /// Recomputed rather than carried as a running sum: the period is a live
    /// parameter the GUI can move mid-run, and a running sum would then be a
    /// sum over a window that no longer exists.
    [[nodiscard]] double average(int period) const noexcept {
        const int usable = period < count_ ? period : count_;
        if (usable <= 0) {
            return 0.0;
        }
        double total = 0.0;
        for (int i = 0; i < usable; ++i) {
            const int index = (next_ - 1 - i + 2 * kMaxPeriod) % kMaxPeriod;
            total += closes_[index];
        }
        return total / static_cast<double>(usable);
    }

    [[nodiscard]] double buy_price(double close) const noexcept {
        return last_ask_ > 0.0 ? last_ask_ : close;
    }

    [[nodiscard]] double sell_price(double close) const noexcept {
        return last_bid_ > 0.0 ? last_bid_ : close;
    }

    [[nodiscard]] double reserve_fraction() const noexcept {
        const double pct = cash_reserve_pct_ < 0.0 ? 0.0
                                                   : (cash_reserve_pct_ > 90.0 ? 90.0
                                                                               : cash_reserve_pct_);
        return pct / 100.0;
    }

    /// Shares affordable at `price` with the cash currently on hand.
    [[nodiscard]] double affordable(double price) const noexcept {
        if (!(price > 0.0)) {
            return 0.0;
        }
        const double budget =
            static_cast<double>(cash()) / 1'000'000.0 * (1.0 - reserve_fraction());
        return std::floor(budget / price);
    }

    void flatten() {
        const double held = position(target());
        if (held > 0.0) {
            submit_market(target(), MS_SIDE_SELL, held);
        } else if (held < 0.0) {
            submit_market(target(), MS_SIDE_BUY, -held);
        }
    }

    void go_long(double close) {
        // Close whatever is open first. Market orders fill synchronously, so
        // cash() below already reflects this.
        flatten();

        const double quantity = affordable(buy_price(close));
        if (quantity >= 1.0) {
            submit_market(target(), MS_SIDE_BUY, quantity);
        }
    }

    void go_short(double close) {
        flatten();

        // Sized against cash even though a short credits rather than costs it,
        // so the position is comparable to the long side rather than unbounded.
        const double quantity = affordable(sell_price(close));
        if (quantity >= 1.0) {
            submit_market(target(), MS_SIDE_SELL, quantity);
        }
    }

    int fast_ = 10;
    int slow_ = 40;
    int symbol_index_ = 0;
    double cash_reserve_pct_ = 2.0;
    bool allow_shorts_ = false;

    double closes_[kMaxPeriod]{};
    int count_ = 0;
    int next_ = 0;

    double last_ask_ = 0.0;
    double last_bid_ = 0.0;

    int previous_signal_ = 0;
    long long crossings_ = 0;
    long long fills_ = 0;
    bool warned_periods_ = false;
};

}  // namespace

MARKETSCAN_EXPORT_STRATEGY(SmaCrossover, "sma_crossover", "SMA Crossover", "Clark Kirby")
