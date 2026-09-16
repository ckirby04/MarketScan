// A theory skeleton. Copy this directory, rename it, and start here.
//
// What you get:
//   - Two headers. No MarketScan library is linked into a plugin.
//   - A base class with the callbacks you need and defaults for the rest.
//   - Parameters that the GUI renders as sliders without being told to.
//
// What you must not do, because it makes a run unreproducible:
//   - Read the system clock. Use now().
//   - Call rand(), or seed your own generator. Use random_uniform().
//   - Touch the filesystem or the network from a callback.
//
// Build it in-tree with the rest of MarketScan, or on its own:
//
//     cmake -S . -B build -DMARKETSCAN_ROOT=/path/to/MarketScan
//     cmake --build build --config Release
//
// then drop the resulting DLL into the host's theories/ directory and press
// Rescan. Build it with the same preset as the host: mixing a Debug plugin
// with a Release host is undefined behaviour on MSVC, not an inconvenience.

#include "marketscan/strategy/istrategy.hpp"

namespace {

class MyTheory final : public marketscan::IStrategy {
public:
    // --- Metadata, all optional ----------------------------------------
    //
    // The export macro picks up whichever of these you declare. Leave one out
    // and the corresponding field is simply empty.

    static constexpr const char* kDescription =
        "A theory that watches bars and never trades. Copy this and make it "
        "do something.";

    static constexpr MS_ParamDesc kParams[] = {
        marketscan::param_int("lookback",
                              "How many bars of history to consider.",
                              1.0,    // minimum
                              500.0,  // maximum
                              20.0),  // default
        marketscan::param_double(
            "threshold", "How far the signal must move before acting.", 0.0, 10.0, 1.0),
        marketscan::param_bool("verbose", "Log every bar. Slow; for debugging.", false),
    };
    static constexpr uint32_t kParamCount = 3;

    // Declare this to be given only certain instruments. Leave it out to be
    // given every symbol in the run.
    //
    // static constexpr const char* kRequiredSymbols[] = {"SPY"};
    // static constexpr uint32_t kRequiredSymbolCount = 1;

    MyTheory() {
        // Bind each parameter to a member and the base class handles
        // set_param and get_param for you.
        bind_param("lookback", &lookback_);
        bind_param("threshold", &threshold_);
        bind_param("verbose", &verbose_);
    }

    // --- Lifecycle ------------------------------------------------------

    void on_start() override { log_info("my_theory starting"); }

    void on_bar(const MS_Bar& bar) override {
        ++bars_seen_;

        if (verbose_) {
            log(MS_LOG_DEBUG, "bar");
        }

        // Anything published here is plotted by the GUI automatically, and
        // shows up in the theory's detail panel.
        publish_metric("bars_seen", static_cast<double>(bars_seen_));
        publish_metric("last_close", bar.close);

        // To trade, ask for a size and submit. The order id is zero if the
        // order was refused -- for instance because this is a cash account and
        // the buy would have overdrawn it.
        //
        //   const uint64_t id = submit_market(bar.symbol, MS_SIDE_BUY, 10.0);
        //   if (id == 0) { log_warn("order refused"); }
    }

    void on_quote(const MS_Quote& quote) override {
        // The best bid and offer, as of this instant. A market order will pay
        // the ask and receive the bid; there is no way around that here,
        // because there is no way around it in a real market either.
        (void)quote;
    }

    void on_trade(const MS_Trade& trade) override { (void)trade; }

    void on_fill(const MS_Fill& fill) override {
        // fill.price is what was actually paid, which is not the price the
        // signal was computed from. The difference is the slippage column in
        // the blotter, and it is usually where the edge went.
        (void)fill;
    }

    void on_stop() override { log_info("my_theory finished"); }

private:
    int lookback_ = 20;
    double threshold_ = 1.0;
    bool verbose_ = false;

    long long bars_seen_ = 0;
};

}  // namespace

// Generates the three C exports and a vtable whose every entry catches
// everything. Place it at file scope, outside any namespace.
MARKETSCAN_EXPORT_STRATEGY(MyTheory, "my_theory", "My Theory", "Your Name")
