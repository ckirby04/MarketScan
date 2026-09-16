// buy_and_hold -- the benchmark.
//
// Buys one instrument with the cash it has, on the first bar it can, and then
// does nothing at all.
//
// It is a plugin rather than a special case inside the host on purpose. As a
// plugin it pays the same spread, runs through the same ledger, is charged the
// same commission and is measured by the same metrics as anything it is being
// compared against. A benchmark implemented in the host would be none of those
// things, and every "we beat the benchmark" claim made against it would be
// measuring the difference between two execution models rather than between
// two ideas.
//
// It is also the smallest complete example of a theory: one callback that
// does something, one that watches the market, and a couple of parameters.

#include <cmath>

#include "marketscan/strategy/istrategy.hpp"

namespace {

class BuyAndHold final : public marketscan::IStrategy {
public:
    static constexpr const char* kDescription =
        "Buys one instrument with all available cash on the first bar and "
        "holds it. The default benchmark. Pays the same spread and commission "
        "as every other theory, so comparisons against it are honest.";

    static constexpr MS_ParamDesc kParams[] = {
        marketscan::param_int("symbol_index",
                              "Which instrument to buy, as an index into the run's symbol "
                              "table. Tickers cannot be expressed as a parameter, because "
                              "the ABI's parameters are all doubles.",
                              0.0,
                              63.0,
                              0.0),
        marketscan::param_double("cash_reserve_pct",
                                 "Fraction of cash held back, so that crossing the spread "
                                 "and paying commission cannot overdraw the account. This "
                                 "is a cash account: an order it cannot pay for is refused "
                                 "whole, not filled part way.",
                                 0.0,
                                 25.0,
                                 1.0),
    };
    static constexpr uint32_t kParamCount = 2;

    BuyAndHold() {
        bind_param("symbol_index", &symbol_index_);
        bind_param("cash_reserve_pct", &cash_reserve_pct_);
    }

    void on_start() override { log_info("waiting for the first bar to buy"); }

    // The ABI gives a strategy no way to ask what the current quote is, which
    // is deliberate -- it keeps a strategy a function of the events it was
    // shown. So the ask is remembered as it goes past.
    void on_quote(const MS_Quote& quote) override {
        if (quote.symbol == target() && quote.ask_px > 0.0) {
            last_ask_ = quote.ask_px;
        }
    }

    void on_bar(const MS_Bar& bar) override {
        if (bought_ || bar.symbol != target()) {
            return;
        }

        // Prefer the ask: it is what a market buy will actually pay. The close
        // is the fallback for a dataset with no quotes in it, and sizing off
        // it is exactly the mistake this theory exists to not make -- hence
        // the reserve.
        const double price = last_ask_ > 0.0 ? last_ask_ : bar.close;
        if (!(price > 0.0)) {
            return;
        }

        const double available =
            static_cast<double>(cash()) / 1'000'000.0 * (1.0 - reserve_fraction());
        const double quantity = std::floor(available / price);
        if (quantity < 1.0) {
            if (!warned_no_cash_) {
                warned_no_cash_ = true;
                log_warn("not enough cash to buy a single unit; holding nothing");
            }
            return;
        }

        const uint64_t order_id = submit_market(target(), MS_SIDE_BUY, quantity, 1);
        if (order_id == 0) {
            // Refused. Almost always because the reserve was too thin to cover
            // the spread at this size. Widen it and try again on the next bar
            // rather than silently never trading.
            shrink_ += 1.0;
            if (shrink_ <= kMaxShrinkSteps) {
                log_warn("buy refused; widening the cash reserve and retrying");
            } else if (!warned_gave_up_) {
                warned_gave_up_ = true;
                log_warn("buy refused repeatedly; giving up");
            }
            return;
        }

        bought_ = true;
        publish_metric("target_quantity", quantity);
        publish_metric("entry_price", price);
    }

    void on_fill(const MS_Fill& fill) override {
        filled_quantity_ += fill.quantity;
        publish_metric("shares_held", filled_quantity_);
        publish_metric("fill_price", fill.price);
    }

    void on_stop() override {
        publish_metric("final_equity", static_cast<double>(equity()) / 1'000'000.0);
    }

private:
    static constexpr double kMaxShrinkSteps = 20.0;

    [[nodiscard]] MS_SymbolId target() const noexcept {
        // Symbol ids start at 1; zero means "not a symbol".
        const auto index = static_cast<uint32_t>(symbol_index_ < 0 ? 0 : symbol_index_);
        return index < symbol_count() ? static_cast<MS_SymbolId>(index + 1) : 1;
    }

    [[nodiscard]] double reserve_fraction() const noexcept {
        const double pct = cash_reserve_pct_ + shrink_;
        const double clamped = pct < 0.0 ? 0.0 : (pct > 95.0 ? 95.0 : pct);
        return clamped / 100.0;
    }

    int symbol_index_ = 0;
    double cash_reserve_pct_ = 1.0;

    double last_ask_ = 0.0;
    double filled_quantity_ = 0.0;
    double shrink_ = 0.0;
    bool bought_ = false;
    bool warned_no_cash_ = false;
    bool warned_gave_up_ = false;
};

}  // namespace

MARKETSCAN_EXPORT_STRATEGY(BuyAndHold, "buy_and_hold", "Buy and Hold", "Clark Kirby")
