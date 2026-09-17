// A theory built on CrossSectionalStrategy, which reports what it was handed.
//
// The thing being tested is that a strategy can see the whole universe at one
// instant: every symbol that traded in the session, in an order that is the
// same on every replay, delivered once at the close and not before.
//
// It checks the ordering itself rather than leaving that to the test, because
// the test can only see published metrics and "the bars were in symbol order"
// is not a number. What it publishes is the count of violations, which is.

#include "marketscan/strategy/istrategy.hpp"

namespace {

class CrossSectionProbe final : public marketscan::CrossSectionalStrategy {
public:
    static constexpr const char* kDescription =
        "Reports the shape of each session's cross-section. A test instrument, "
        "not a strategy: it never trades.";

    void on_start() override {
        // Sized up front so no session allocates. symbol_count() is the run's
        // whole universe, which is what the buffers are eventually going to
        // grow to anyway.
        reserve_symbols(symbol_count());
    }

protected:
    void on_cross_section(const marketscan::CrossSection& section) override {
        ++sections_;
        publish_metric("sections", static_cast<double>(sections_));
        publish_metric("last_count", static_cast<double>(section.count));

        total_bars_ += section.count;
        publish_metric("total_bars", static_cast<double>(total_bars_));

        // Ascending by symbol id, strictly: a repeated symbol would mean one
        // instrument appearing twice in a cross-section, which would let a
        // ranking strategy double-count a name.
        MS_SymbolId previous = 0;
        for (const MS_Bar& bar : section) {
            if (bar.symbol <= previous) {
                ++out_of_order_;
            }
            previous = bar.symbol;

            // Every bar handed over must belong to the session that just
            // closed. One belonging to the next session would mean the buffer
            // was flushed too late.
            if (bar.ts >= section.session_end) {
                ++from_the_future_;
            }
        }
        publish_metric("out_of_order", static_cast<double>(out_of_order_));
        publish_metric("from_the_future", static_cast<double>(from_the_future_));

        // find() must agree with iteration, or the convenience accessor is a
        // trap rather than a convenience.
        long long found = 0;
        for (const MS_Bar& bar : section) {
            const MS_Bar* looked_up = section.find(bar.symbol);
            if (looked_up != nullptr && looked_up->symbol == bar.symbol) {
                ++found;
            }
        }
        publish_metric("found_by_lookup", static_cast<double>(found));
        publish_metric("missing_lookup_is_null",
                       section.find(0) == nullptr && section.find(9999) == nullptr ? 1.0 : 0.0);
    }

    void on_session_bar(const MS_Bar& bar) override {
        (void)bar;
        ++bars_seen_;
        publish_metric("bars_seen", static_cast<double>(bars_seen_));

        // The count of sections as of the last bar. If a cross-section were
        // delivered mid-session this would disagree with the final total.
        publish_metric("sections_at_last_bar", static_cast<double>(sections_));
    }

private:
    long long sections_ = 0;
    long long total_bars_ = 0;
    long long bars_seen_ = 0;
    long long out_of_order_ = 0;
    long long from_the_future_ = 0;
};

}  // namespace

MARKETSCAN_EXPORT_STRATEGY(CrossSectionProbe,
                           "cross_section_probe",
                           "Cross Section Probe",
                           "MarketScan tests")
