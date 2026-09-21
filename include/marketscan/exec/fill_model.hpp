#ifndef MARKETSCAN_EXEC_FILL_MODEL_HPP
#define MARKETSCAN_EXEC_FILL_MODEL_HPP

#include <string_view>

#include "marketscan/book/order_book.hpp"
#include "marketscan/exec/order.hpp"

namespace marketscan {

struct CommissionSchedule {
    /// Charged per unit traded. 0.005 is a plausible retail per-share rate.
    Money per_share = Money::from_micros(5'000);

    /// Charged per fill regardless of size.
    Money per_order;

    /// Floor applied after the two above.
    Money minimum;

    [[nodiscard]] Money charge(Quantity quantity) const noexcept;
};

struct FillModelConfig {
    /// Probability that an order is refused at submission, for its own sake.
    ///
    /// **Zero by default, and it must stay that way.** Every committed result in
    /// this repository was measured with no rejections, and a non-zero default
    /// would move all of them for no reason anybody asked for.
    ///
    /// It exists because ABI v2 added `on_order_status` for the sake of live
    /// execution, where rejections are ordinary -- insufficient buying power, a
    /// non-tradable symbol, a closed market. Against a fill simulator they are
    /// nearly unreachable, which means the handling path is one a strategy would
    /// meet for the first time with money on it. This makes it reachable in
    /// replay.
    ///
    /// Drawn from the strategy's own seeded stream, so a run with rejections on
    /// is exactly as reproducible as one without.
    double simulated_reject_probability = 0.0;

    CommissionSchedule commission;

    /// Adverse slippage applied on top of the spread, in basis points.
    ///
    /// Zero by default: the spread is already being crossed, and charging an
    /// extra fixed penalty on top without data to justify it would be making
    /// up a number. It exists so a run can be stress-tested against a worse
    /// execution assumption on purpose.
    double slippage_bps = 0.0;

    /// Shape of the cost of size beyond what the book displays.
    ///
    /// With top-of-book data there is no real depth to walk once the touch is
    /// consumed, so the remainder is priced through a square-root impact
    /// curve: an order twice the displayed size pays this many basis points
    /// more than the touch, an order five times the size pays twice that.
    ///
    /// The square root is the conventional shape and 10bp is a placeholder,
    /// not a measurement. It is a knob for making the cost of size visible,
    /// not a claim about any real market.
    double impact_coefficient_bps = 10.0;

    /// Simulated delay between an order being accepted and its first chance
    /// to fill, in engine nanoseconds.
    ///
    /// Zero by default, which is what every result committed before M1b was
    /// produced under, and what keeps them reproducible. A non-zero value is
    /// the difference between a backtest and a thing that could happen: live,
    /// an order crosses a network, and the market does not wait for it. An
    /// order in flight is still exposed -- it fills at the price when it
    /// arrives, not the price when it was sent.
    ///
    /// This is simulated time, never wall-clock time. A latency measured from
    /// the machine would make a replay depend on how busy the machine was.
    Duration submit_latency = 0;

    /// Upper bound on a uniform addition to that delay.
    ///
    /// Drawn from the run's seeded generator, so a replay reproduces the same
    /// jitter. Zero by default: fixed latency is already the larger effect,
    /// and an unnecessary source of variance makes a difference between two
    /// runs harder to attribute.
    Duration latency_jitter = 0;

    /// The largest fraction of the size displayed at the touch that a single
    /// event may fill.
    ///
    /// Zero -- the default -- means no limit, and an order clears in one fill
    /// at whatever the impact curve charges. A value like 0.25 means an order
    /// four times the displayed size arrives as several fills across several
    /// events, which is what large orders actually do. Each piece is its own
    /// fill callback with its own price; nothing averages them behind the
    /// strategy's back.
    double max_participation = 0.0;

    /// Setting this to zero disables the impact term entirely, which makes an
    /// order of any size fill at the touch. Useful for isolating a test; not a
    /// realistic configuration.
    [[nodiscard]] bool models_impact() const noexcept { return impact_coefficient_bps > 0.0; }

    [[nodiscard]] bool models_latency() const noexcept {
        return submit_latency > 0 || latency_jitter > 0;
    }

    [[nodiscard]] bool models_partial_fills() const noexcept { return max_participation > 0.0; }
};

/// What the model is being asked to price.
struct FillRequest {
    const Order* order = nullptr;
    const OrderBook* book = nullptr;
    Timestamp ts = 0;
};

struct FillResult {
    /// Quantity that fills now. Zero means nothing filled.
    Quantity quantity = 0.0;

    /// Quantity-weighted average price of that fill.
    Price price = 0.0;

    Money commission;

    /// Set when nothing filled and the reason is permanent for this order.
    /// RejectReason::None with a zero quantity means "not yet, try again on
    /// the next event" -- a resting limit order waiting for its price.
    RejectReason reason = RejectReason::None;

    [[nodiscard]] bool filled() const noexcept { return quantity > 0.0; }
};

/// How an order becomes a fill.
///
/// Deliberately stateless and const: a model is shared across every strategy
/// in a run, and two strategies submitting the same order against the same
/// book must be priced identically. Anything stateful -- queue position,
/// participation limits -- belongs in a subclass that owns its own state and
/// is instantiated per account.
class IFillModel {
public:
    IFillModel() = default;
    IFillModel(const IFillModel&) = delete;
    IFillModel& operator=(const IFillModel&) = delete;
    IFillModel(IFillModel&&) = delete;
    IFillModel& operator=(IFillModel&&) = delete;
    virtual ~IFillModel() = default;

    [[nodiscard]] virtual FillResult evaluate(const FillRequest& request) const = 0;

    [[nodiscard]] virtual std::string_view name() const noexcept = 0;

    /// False for models that exist only as a comparison baseline. The GUI
    /// labels those, loudly, so nobody reads their equity curve as a result.
    [[nodiscard]] virtual bool is_realistic() const noexcept = 0;

    [[nodiscard]] virtual const FillModelConfig& config() const noexcept = 0;
};

/// The default, and the only one whose output means anything.
///
/// A market buy lifts the ask, a market sell hits the bid, using the book in
/// effect at the signal timestamp. Size beyond the displayed touch walks to
/// worse levels, and beyond the last level it pays the impact curve. An order
/// never fills better than the touch, in any configuration.
///
/// A limit order fills only the quantity whose average price stays within its
/// limit. Because the impact curve is monotonic in size, that quantity has a
/// closed form and is solved for directly rather than searched.
class SpreadCrossingFillModel final : public IFillModel {
public:
    explicit SpreadCrossingFillModel(FillModelConfig config = {}) : config_(config) {}

    [[nodiscard]] FillResult evaluate(const FillRequest& request) const override;

    [[nodiscard]] std::string_view name() const noexcept override { return "spread-crossing"; }

    [[nodiscard]] bool is_realistic() const noexcept override { return true; }

    [[nodiscard]] const FillModelConfig& config() const noexcept override { return config_; }

private:
    FillModelConfig config_;
};

/// Fills everything at the last trade price, instantly, in full.
///
/// This model is wrong. It exists so the GUI can put it beside the realistic
/// one and make the cost of naive assumptions visible: the gap between the two
/// equity curves is what a backtest built on optimistic fills is quietly
/// awarding itself. It is never the default and the UI labels it unrealistic.
class OptimisticFillModel final : public IFillModel {
public:
    explicit OptimisticFillModel(FillModelConfig config = {}) : config_(config) {}

    [[nodiscard]] FillResult evaluate(const FillRequest& request) const override;

    [[nodiscard]] std::string_view name() const noexcept override {
        return "optimistic (unrealistic)";
    }

    [[nodiscard]] bool is_realistic() const noexcept override { return false; }

    [[nodiscard]] const FillModelConfig& config() const noexcept override { return config_; }

private:
    FillModelConfig config_;
};

}  // namespace marketscan

#endif  // MARKETSCAN_EXEC_FILL_MODEL_HPP
