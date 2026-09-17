// A theory that does nothing but notice when a session ends.
//
// on_session_close is detected in the shim layer from timestamps the plugin is
// already being handed, so the only way to test it honestly is from inside a
// real plugin, across a real DLL boundary, driven by a real feed. A host-side
// mock would test the mock.
//
// Everything it observes is published as a metric, because that is the one
// channel a plugin has for telling a test what happened.

#include "marketscan/strategy/istrategy.hpp"

namespace {

class SessionWatcher final : public marketscan::IStrategy {
public:
    static constexpr const char* kDescription =
        "Counts session closes and reports which days they fell on. A test "
        "instrument, not a strategy: it never trades.";

    void on_bar(const MS_Bar& bar) override {
        ++bars_;
        last_bar_ts_ = bar.ts;
        publish_metric("bars", static_cast<double>(bars_));

        // Published on every bar rather than only at a close, so a test can
        // tell "the close fired late" apart from "the close never fired".
        publish_metric("closes_at_last_bar", static_cast<double>(closes_));
    }

    void on_session_close(MS_Nanos session_end) override {
        ++closes_;
        publish_metric("closes", static_cast<double>(closes_));

        // The day number rather than the timestamp. A nanosecond timestamp is
        // around 1.7e18 and a double carries integers exactly only to 2^53, so
        // publishing the raw value would quietly round it -- by more than a
        // minute, which is enough to land on the wrong side of a boundary.
        publish_metric("close_day",
                       static_cast<double>(marketscan::session_day(session_zone(), session_end)));
        publish_metric("bars_at_close", static_cast<double>(bars_));

        // Every close must arrive after the last bar of the session it is
        // closing, and never before. A test asserts this stayed zero.
        if (last_bar_ts_ > session_end) {
            ++closes_out_of_order_;
        }
        publish_metric("closes_out_of_order", static_cast<double>(closes_out_of_order_));
    }

    void on_stop() override { publish_metric("closes_at_stop", static_cast<double>(closes_)); }

private:
    long long bars_ = 0;
    long long closes_ = 0;
    long long closes_out_of_order_ = 0;
    MS_Nanos last_bar_ts_ = 0;
};

}  // namespace

MARKETSCAN_EXPORT_STRATEGY(SessionWatcher, "session_watcher", "Session Watcher", "MarketScan tests")
