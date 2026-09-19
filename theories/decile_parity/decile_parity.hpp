// decile_parity -- the strategy the parity test runs on both sides.
//
// Long the top decile of the synthetic exposure, short the bottom, rebalanced
// monthly. Implemented once here as a C++ theory and once in the Python
// research pipeline; Spec 2 M9 runs both over the same panel and requires the
// equity curves to agree.
//
// It is not a trading idea and is not meant to be. It is the simplest rule that
// exercises the whole path -- a cross-section, a ranking, a rebalance, orders
// through the fill model, a ledger -- and that both implementations can express
// without either one having to be clever.
//
// WHERE THE EXPOSURE COMES FROM
//
// A theory sees bars. It has no file access, no network, and no side channel:
// the only thing that reaches it is the event stream. The synthetic exposure is
// not a price, so it has to arrive inside a bar, and the field with nothing else
// to do is `volume` -- which the execution layer never reads.
//
//     volume = (exposure + 10) * 1e6
//
// Ten to keep it positive, a million to keep six decimals through an integer
// CSV column. `research/parity.py` writes it, this decodes it, and both sides
// consume the same encoded number so the ranking cannot differ in the last bit.
//
// This is a **fixture convention, not a feature**. Real cached bars carry real
// volume, and pointing this theory at them would rank securities by a nonsense
// transform of turnover. It is named `decile_parity` rather than anything that
// sounds tradeable for that reason, and it refuses to run on bars whose volume
// does not decode into the range an exposure lives in.

#ifndef MARKETSCAN_THEORY_DECILE_PARITY_HPP
#define MARKETSCAN_THEORY_DECILE_PARITY_HPP

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "marketscan/strategy/istrategy.hpp"

namespace decile_parity {

/// The encoding `research/parity.py` writes into the volume column.
inline constexpr double kExposureOffset = 10.0;
inline constexpr double kExposureScale = 1'000'000.0;

/// Outside this the volume is not an encoded exposure and the theory stands
/// down rather than ranking on whatever it found.
inline constexpr double kExposureLimit = 9.0;

[[nodiscard]] inline double decode_exposure(double volume) noexcept {
    return volume / kExposureScale - kExposureOffset;
}

[[nodiscard]] inline double encode_exposure(double exposure) noexcept {
    return std::round((exposure + kExposureOffset) * kExposureScale);
}

class DecileParity final : public marketscan::CrossSectionalStrategy {
public:
    static constexpr const char* kDescription =
        "Long the top decile of a synthetic exposure, short the bottom, monthly. "
        "A PARITY FIXTURE, not a strategy: it reads its signal out of the volume "
        "column, which only research/parity.py puts anything meaningful into.";

    static constexpr MS_ParamDesc kParams[] = {
        marketscan::param_double("decile",
                                 "Fraction of the cross-section taken on each "
                                 "side. 0.1 is a decile.",
                                 0.01,
                                 0.5,
                                 0.1),
        marketscan::param_int("rebalance_sessions",
                             "Sessions between rebalances. 21 is a month of "
                             "business days.",
                             1.0,
                             252.0,
                             21.0),
        marketscan::param_double("gross",
                                 "Target gross exposure as a multiple of "
                                 "equity. 1.0 is fully invested long and short "
                                 "combined.",
                                 0.1,
                                 2.0,
                                 1.0),
    };
    static constexpr uint32_t kParamCount = 3;

    DecileParity() {
        bind_param("decile", &decile_);
        bind_param("rebalance_sessions", &rebalance_sessions_);
        bind_param("gross", &gross_);
    }

    void on_start() override {
        log_info("decile_parity is a parity fixture; its signal lives in the volume column");
        reserve_symbols(symbol_count());
        ranked_.reserve(symbol_count() + 1);
    }

protected:
    void on_cross_section(const marketscan::CrossSection& section) override {
        // Sessions are counted rather than timestamps differenced, so that a
        // holiday does not shift the schedule and put the two implementations
        // on different rebalance dates.
        const bool due = (sessions_++ % static_cast<std::int64_t>(rebalance_sessions_)) == 0;
        if (!due || section.empty()) {
            return;
        }

        ranked_.clear();
        for (const MS_Bar& bar : section) {
            const double exposure = decode_exposure(bar.volume);
            if (!std::isfinite(exposure) || std::abs(exposure) > kExposureLimit ||
                !(bar.close > 0.0)) {
                // Not an encoded exposure. Stand down for the whole session
                // rather than rank a subset: half a cross-section ranked
                // against itself is a different strategy.
                return;
            }
            ranked_.push_back(Candidate{bar.symbol, exposure, bar.close});
        }

        // Sorted by exposure, ties broken by symbol id. The tiebreak is not
        // decoration: two names with the same exposure have to fall on the same
        // side of the decile boundary in both implementations, and symbol id is
        // the only ordering both of them share.
        std::sort(ranked_.begin(), ranked_.end(), [](const Candidate& a, const Candidate& b) {
            if (a.exposure != b.exposure) {
                return a.exposure > b.exposure;
            }
            return a.symbol < b.symbol;
        });

        const auto count = static_cast<std::size_t>(ranked_.size());
        const auto side = static_cast<std::size_t>(std::floor(
            static_cast<double>(count) * decile_));
        if (side == 0 || side * 2 > count) {
            return;  // too few names to form two disjoint sides
        }

        // Target notional per name. Equity is in micro-dollars; the arithmetic
        // stays in dollars because a share count is a double either way.
        const double book = static_cast<double>(equity()) / 1'000'000.0 * gross_;
        const double per_name = book / static_cast<double>(2 * side);

        // Close first, open second. A rebalance that opens before it closes can
        // be refused for want of cash, and an order the fill model declines is a
        // divergence between the two implementations that has nothing to do
        // with the strategy.
        for (std::size_t i = side; i + side < count; ++i) {
            close_position(ranked_[i].symbol);
        }
        for (std::size_t i = 0; i < side; ++i) {
            target_position(ranked_[i], +per_name);
        }
        for (std::size_t i = count - side; i < count; ++i) {
            target_position(ranked_[i], -per_name);
        }
    }

private:
    struct Candidate {
        MS_SymbolId symbol;
        double exposure;
        double close;
    };

    void close_position(MS_SymbolId symbol) {
        const double held = position(symbol);
        if (held > 0.0) {
            submit_market(symbol, MS_SIDE_SELL, held);
        } else if (held < 0.0) {
            submit_market(symbol, MS_SIDE_BUY, -held);
        }
    }

    void target_position(const Candidate& candidate, double notional) {
        const double target_shares = notional / candidate.close;
        const double delta = target_shares - position(candidate.symbol);
        // A trade smaller than this is rounding, and submitting it would make
        // the two implementations disagree about how many fills happened while
        // agreeing about everything that matters.
        if (std::abs(delta) < 1e-9) {
            return;
        }
        if (delta > 0.0) {
            submit_market(candidate.symbol, MS_SIDE_BUY, delta);
        } else {
            submit_market(candidate.symbol, MS_SIDE_SELL, -delta);
        }
    }

    double decile_ = 0.1;
    double rebalance_sessions_ = 21.0;
    double gross_ = 1.0;
    std::int64_t sessions_ = 0;
    std::vector<Candidate> ranked_;
};

}  // namespace decile_parity

#endif  // MARKETSCAN_THEORY_DECILE_PARITY_HPP
