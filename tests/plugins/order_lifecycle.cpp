// A theory that has to cope with an order that has not filled yet.
//
// This is the fixture for the bug the whole latency milestone exists to make
// reachable: act on a signal, see no fill because the order is still crossing
// the wire, read the position as unchanged, and send the order again. It is
// the most common mistake in live trading and it is *unreachable* against a
// simulator that answers instantly, which is why every theory written against
// this engine so far has been quietly relying on one.
//
// Three modes, selected by parameter, because the guarded and unguarded
// versions have to be the same code in every other respect for the comparison
// between them to mean anything.

#include "marketscan/strategy/istrategy.hpp"

namespace {

class OrderLifecycle final : public marketscan::IStrategy {
public:
    static constexpr const char* kDescription =
        "Submits on every bar, with or without checking for an order already "
        "in flight. A test instrument, not a strategy.";

    enum Mode : int {
        /// Checks has_pending_order() first. What a correct strategy does.
        kGuarded = 0,
        /// Submits regardless. What the bug looks like.
        kUnguarded = 1,
        /// Submits once, then tries to cancel it while it is still in flight.
        kCancelInFlight = 2,
    };

    static constexpr MS_ParamDesc kParams[] = {
        marketscan::param_int("mode", "0 guarded, 1 unguarded, 2 cancel in flight.", 0.0, 2.0, 0.0),
        marketscan::param_int("size", "Shares per order.", 1.0, 100'000.0, 10.0),
    };
    static constexpr uint32_t kParamCount = 2;

    OrderLifecycle() {
        bind_param("mode", &mode_);
        bind_param("size", &size_);
    }

    void on_bar(const MS_Bar& bar) override {
        ++bars_;
        symbol_ = bar.symbol;

        if (mode_ == kCancelInFlight) {
            cancel_mode(bar);
        } else {
            submit_mode(bar);
        }

        // Published every bar rather than at the end, so a test can tell a
        // strategy that never had an order outstanding apart from one that
        // had one the whole time.
        if (outstanding_orders() > peak_outstanding_) {
            peak_outstanding_ = outstanding_orders();
        }
        publish_metric("bars", static_cast<double>(bars_));
        publish_metric("submits", static_cast<double>(submits_));
        publish_metric("peak_outstanding", static_cast<double>(peak_outstanding_));
        publish_metric("outstanding", static_cast<double>(outstanding_orders()));
        publish_metric("position", position(bar.symbol));
        publish_metric("pending_quantity", pending_quantity(bar.symbol));
    }

    void on_fill(const MS_Fill& fill) override {
        ++fills_;
        filled_quantity_ += fill.quantity;
        if (fill.order_id == first_order_) {
            ++fills_on_first_order_;
        }
        publish_metric("fills", static_cast<double>(fills_));
        publish_metric("filled_quantity", filled_quantity_);
        publish_metric("fills_on_first_order", static_cast<double>(fills_on_first_order_));
    }

    void on_stop() override {
        // Position is read here rather than in on_bar. Fills for resting
        // orders are emitted after the bar that triggered them, so a position
        // published from on_bar is always one event stale -- which is fine for
        // a chart and wrong for an assertion.
        publish_metric("final_position", position(symbol_));
        publish_metric("final_outstanding", static_cast<double>(outstanding_orders()));
        publish_metric("cancels", static_cast<double>(cancels_));
        publish_metric("saw_order_age", saw_order_age_ ? 1.0 : 0.0);
    }

private:
    void submit_mode(const MS_Bar& bar) {
        // The whole point. In guarded mode an order already working for this
        // symbol means there is nothing to decide: the decision was made, it
        // is in the market, and sending it again would double the position.
        if (mode_ == kGuarded && has_pending_order(bar.symbol)) {
            return;
        }
        const uint64_t id = submit_market(bar.symbol, MS_SIDE_BUY, static_cast<double>(size_));
        if (id != 0) {
            ++submits_;
            if (first_order_ == 0) {
                first_order_ = id;
            }
        }
    }

    void cancel_mode(const MS_Bar& bar) {
        if (first_order_ == 0) {
            first_order_ = submit_market(bar.symbol, MS_SIDE_BUY, static_cast<double>(size_));
            if (first_order_ != 0) {
                ++submits_;
            }
            return;
        }
        if (cancels_ == 0 && remaining_quantity(first_order_) > 0.0) {
            // An order that has been alive for a measurable time and has not
            // filled. order_age() is the only way to know that, and reading a
            // non-zero one at all is worth asserting: it means the strategy
            // genuinely observed a pending order rather than a fill it missed.
            if (order_age(first_order_) > 0) {
                saw_order_age_ = true;
            }
            cancel(first_order_);
            ++cancels_;
        }
    }

    int mode_ = kGuarded;
    int size_ = 10;

    long long bars_ = 0;
    long long submits_ = 0;
    long long fills_ = 0;
    long long cancels_ = 0;
    long long fills_on_first_order_ = 0;
    uint32_t peak_outstanding_ = 0;
    double filled_quantity_ = 0.0;
    uint64_t first_order_ = 0;
    MS_SymbolId symbol_ = 0;
    bool saw_order_age_ = false;
};

}  // namespace

MARKETSCAN_EXPORT_STRATEGY(OrderLifecycle, "order_lifecycle", "Order Lifecycle", "MarketScan tests")
