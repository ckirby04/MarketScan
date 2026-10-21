// The drawdown_entry rule. Specification: contracts/research/DRAWDOWN_ENTRY.md.
//
// Each block below cites the step it implements. Nothing here was derived from
// research/drawdown_entry.py, and nothing there from this: the two agreeing is
// evidence only while that stays true.

#include "marketscan/research/drawdown_entry.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

#include "marketscan/strategy/istrategy.hpp"
#include "marketscan/strategy/registry.hpp"

namespace marketscan::research {

std::string_view to_string(ExitReason reason) noexcept {
    switch (reason) {
        case ExitReason::None:
            return "none";
        case ExitReason::RecoverToHigh:
            return "recover_to_high";
        case ExitReason::TakeProfit:
            return "take_profit";
        case ExitReason::StopLoss:
            return "stop_loss";
        case ExitReason::TrailingStop:
            return "trailing_stop";
        case ExitReason::TimeStop:
            return "time_stop";
    }
    return "none";
}

}  // namespace marketscan::research

namespace {

namespace p = marketscan::research::drawdown_param;
using marketscan::research::ExitReason;
using marketscan::research::kBuyTag;
using marketscan::research::kMaxTranches;
using marketscan::research::kSellTag;
using marketscan::research::kTagPayload;
using marketscan::research::ReferenceKind;

constexpr double kMinusInfinity = -std::numeric_limits<double>::infinity();
constexpr MS_Nanos kDayNanos = 86'400LL * 1'000'000'000LL;

class DrawdownEntry final : public marketscan::IStrategy {
public:
    static constexpr const char* kDescription =
        "Sit in cash; buy when the instrument falls a given percent below a reference high; "
        "sell on the exit rules. The rule behind the Research view's drawdown_entry "
        "template, specified in contracts/research/DRAWDOWN_ENTRY.md.";

    static constexpr MS_ParamDesc kParams[] = {
        marketscan::param_int(p::kReference,
                              "0 all-time high, 1 rolling high, 2 high since last exit.",
                              0.0,
                              2.0,
                              0.0),
        marketscan::param_int(p::kWindow, "Rolling-high window, in bars.", 1.0, 5000.0, 252.0),
        marketscan::param_int(p::kTrancheCount, "Tranches in use.", 1.0, 4.0, 1.0),
        marketscan::param_double(p::kLevel[0], "Drawdown percent.", 0.0, 100.0, 20.0),
        marketscan::param_double(p::kFraction[0], "Percent of B.", 0.0, 100.0, 100.0),
        marketscan::param_double(p::kLevel[1], "Drawdown percent.", 0.0, 100.0, 0.0),
        marketscan::param_double(p::kFraction[1], "Percent of B.", 0.0, 100.0, 0.0),
        marketscan::param_double(p::kLevel[2], "Drawdown percent.", 0.0, 100.0, 0.0),
        marketscan::param_double(p::kFraction[2], "Percent of B.", 0.0, 100.0, 0.0),
        marketscan::param_double(p::kLevel[3], "Drawdown percent.", 0.0, 100.0, 0.0),
        marketscan::param_double(p::kFraction[3], "Percent of B.", 0.0, 100.0, 0.0),
        marketscan::param_bool(p::kRecoverToHigh, "Sell on regaining the entry high.", true),
        marketscan::param_double(p::kTakeProfit, "Percent; 0 is off.", 0.0, 10000.0, 0.0),
        marketscan::param_double(p::kStopLoss, "Percent; 0 is off.", 0.0, 100.0, 0.0),
        marketscan::param_double(p::kTrailingStop, "Percent; 0 is off.", 0.0, 100.0, 0.0),
        marketscan::param_int(p::kTimeStop, "Bars; 0 is off.", 0.0, 100000.0, 0.0),
        marketscan::param_bool(p::kReentry, "May a new campaign follow an exit.", true),
        marketscan::param_double(p::kReserve, "Sizing reserve, percent.", 0.0, 50.0, 2.0),
        marketscan::param_double(p::kCommission, "Dollars per order.", 0.0, 1000.0, 0.0),
        marketscan::param_int(
            p::kStartDay, "First decision day, days since 1970-01-01.", -1e6, 1e6, 0.0),
    };
    static constexpr uint32_t kParamCount = 20;

    DrawdownEntry() {
        bind_param(p::kReference, &reference_);
        bind_param(p::kWindow, &window_);
        bind_param(p::kTrancheCount, &tranche_count_);
        for (int k = 0; k < kMaxTranches; ++k) {
            bind_param(p::kLevel[k], &level_[k]);
            bind_param(p::kFraction[k], &fraction_[k]);
        }
        bind_param(p::kRecoverToHigh, &recover_to_high_);
        bind_param(p::kTakeProfit, &take_profit_);
        bind_param(p::kStopLoss, &stop_loss_);
        bind_param(p::kTrailingStop, &trailing_stop_);
        bind_param(p::kTimeStop, &time_stop_);
        bind_param(p::kReentry, &reentry_);
        bind_param(p::kReserve, &reserve_);
        bind_param(p::kCommission, &commission_);
        bind_param(p::kStartDay, &start_day_);
    }

    void on_bar(const MS_Bar& bar) override {
        if (bar.symbol != kTarget || !(bar.close > 0.0)) {
            return;
        }
        const double c = bar.close;
        const auto t = static_cast<long long>(closes_.size());
        closes_.push_back(c);

        // Steps 1 and 2: running quantities, on every bar including warmup.
        all_time_high_ = std::max(all_time_high_, c);
        since_exit_high_ = std::max(since_exit_high_, c);
        if (first_fill_bar_ >= 0) {
            peak_ = std::max(peak_, c);
        }
        const double high = reference_high();
        const double drawdown = std::max(0.0, 100.0 * (1.0 - c / high));

        if (bar.ts / kDayNanos < start_day_ || in_flight_) {
            return;
        }

        // Step 3: exits first.
        if (position(kTarget) > 0.0) {
            const ExitReason reason = exit_reason(c, t);
            if (reason != ExitReason::None) {
                send(MS_SIDE_SELL, position(kTarget), kSellTag | static_cast<uint64_t>(reason));
            }
            if (reason != ExitReason::None || !campaign_active_) {
                return;
            }
        }

        // Step 4: buys.
        if (exited_once_ && !reentry_) {
            return;
        }
        uint64_t mask = 0;
        double fraction = 0.0;
        for (int k = 0; k < tranche_count(); ++k) {
            if ((filled_mask_ & (1ULL << k)) == 0 && drawdown >= level_[k]) {
                mask |= 1ULL << k;
                fraction += fraction_[k];  // in tranche order, step 6
            }
        }
        if (mask == 0) {
            return;
        }

        // Step 5.
        const bool starting = !campaign_active_;
        if (starting) {
            campaign_active_ = true;
            base_ = equity();
            entry_high_ = high;
        }

        // Step 6.
        const auto commission = static_cast<MS_Money>(std::llround(commission_ * 1'000'000.0));
        const MS_Money wanted = std::llround(static_cast<double>(base_) * fraction / 100.0);
        const MS_Money budget = std::min(wanted, cash() - commission);
        const double estimate = c * (1.0 + reserve_ / 100.0);
        const double shares = std::floor((static_cast<double>(budget) / 1'000'000.0) / estimate);
        if (!(shares >= 1.0)) {
            if (starting) {
                campaign_active_ = false;
            }
            return;
        }

        // Step 7.
        send(MS_SIDE_BUY, shares, kBuyTag | mask);
    }

    void on_fill(const MS_Fill& fill) override {
        if (fill.symbol != kTarget) {
            return;
        }
        if ((fill.client_order_id & ~kTagPayload) == kBuyTag) {
            if (first_fill_bar_ < 0) {
                // The fill arrives on bar t+1's quote, before bar t+1 itself,
                // so the fill bar is the next one to be pushed.
                first_fill_bar_ = static_cast<long long>(closes_.size());
                peak_ = kMinusInfinity;
            }
            filled_mask_ |= fill.client_order_id & kTagPayload;
            cost_ += fill.price * fill.quantity;
            bought_ += fill.quantity;
        } else if (position(kTarget) <= 0.0) {
            // The exit filled: the campaign is over. since_last_exit restarts
            // from the fill bar, whose close the next on_bar contributes.
            end_campaign();
            exited_once_ = true;
            since_exit_high_ = kMinusInfinity;
        }
        in_flight_ = false;
    }

    void on_order_status(const MS_OrderStatus& status) override {
        if (status.terminal == 0 || status.filled_quantity > 0.0) {
            return;
        }
        in_flight_ = false;
        // Step 7: a refused buy leaves its tranches unfilled. A campaign with
        // nothing held and nothing in flight has ended, without an exit.
        if ((status.client_order_id & ~kTagPayload) == kBuyTag && position(kTarget) <= 0.0) {
            end_campaign();
        }
    }

private:
    static constexpr MS_SymbolId kTarget = 1;

    [[nodiscard]] int tranche_count() const noexcept {
        return std::clamp(tranche_count_, 1, kMaxTranches);
    }

    // Step 1.
    [[nodiscard]] double reference_high() const noexcept {
        switch (static_cast<ReferenceKind>(reference_)) {
            case ReferenceKind::RollingHigh: {
                const std::size_t window = static_cast<std::size_t>(std::max(window_, 1));
                const std::size_t from = closes_.size() > window ? closes_.size() - window : 0;
                return *std::max_element(closes_.begin() + static_cast<std::ptrdiff_t>(from),
                                         closes_.end());
            }
            case ReferenceKind::SinceLastExit:
                return exited_once_ ? since_exit_high_ : all_time_high_;
            case ReferenceKind::AllTimeHigh:
            default:
                return all_time_high_;
        }
    }

    // Step 3, in its fixed order.
    [[nodiscard]] ExitReason exit_reason(double c, long long t) const noexcept {
        const double average = cost_ / bought_;
        if (recover_to_high_ && c >= entry_high_) {
            return ExitReason::RecoverToHigh;
        }
        if (take_profit_ > 0.0 && c >= average * (1.0 + take_profit_ / 100.0)) {
            return ExitReason::TakeProfit;
        }
        if (stop_loss_ > 0.0 && c <= average * (1.0 - stop_loss_ / 100.0)) {
            return ExitReason::StopLoss;
        }
        if (trailing_stop_ > 0.0 && c <= peak_ * (1.0 - trailing_stop_ / 100.0)) {
            return ExitReason::TrailingStop;
        }
        if (time_stop_ > 0 && t - first_fill_bar_ >= time_stop_) {
            return ExitReason::TimeStop;
        }
        return ExitReason::None;
    }

    // GTC rather than the SDK's DAY default: the order is in flight across the
    // date change, and a DAY order would expire there before it could fill.
    void send(MS_Side side, double quantity, uint64_t tag) {
        MS_OrderRequest request{};
        request.symbol = kTarget;
        request.side = side;
        request.type = MS_ORD_MARKET;
        request.tif = MS_TIF_GTC;
        request.quantity = quantity;
        request.client_order_id = tag;
        // Set before submitting: a fill delivered from inside submit() clears
        // it, and assigning the result afterwards would set it again.
        in_flight_ = true;
        if (!submit(request).sent()) {
            in_flight_ = false;
        }
    }

    void end_campaign() noexcept {
        campaign_active_ = false;
        filled_mask_ = 0;
        first_fill_bar_ = -1;
        cost_ = 0.0;
        bought_ = 0.0;
        peak_ = kMinusInfinity;
    }

    // Parameters.
    int reference_ = 0;
    int window_ = 252;
    int tranche_count_ = 1;
    double level_[kMaxTranches] = {20.0, 0.0, 0.0, 0.0};
    double fraction_[kMaxTranches] = {100.0, 0.0, 0.0, 0.0};
    bool recover_to_high_ = true;
    double take_profit_ = 0.0;
    double stop_loss_ = 0.0;
    double trailing_stop_ = 0.0;
    int time_stop_ = 0;
    bool reentry_ = true;
    double reserve_ = 2.0;
    double commission_ = 0.0;
    int start_day_ = 0;

    // Running quantities.
    std::vector<double> closes_;
    double all_time_high_ = kMinusInfinity;
    double since_exit_high_ = kMinusInfinity;
    bool exited_once_ = false;
    bool in_flight_ = false;

    // The campaign.
    bool campaign_active_ = false;
    MS_Money base_ = 0;
    double entry_high_ = 0.0;
    uint64_t filled_mask_ = 0;
    long long first_fill_bar_ = -1;
    double cost_ = 0.0;
    double bought_ = 0.0;
    double peak_ = kMinusInfinity;
};

}  // namespace

MARKETSCAN_REGISTER_STRATEGY(DrawdownEntry, "drawdown_entry", "Drawdown entry", "MarketScan")

namespace marketscan::research {

bool drawdown_entry_registered() {
    return StrategyRegistry::instance().find(kDrawdownEntryId) != nullptr;
}

}  // namespace marketscan::research
