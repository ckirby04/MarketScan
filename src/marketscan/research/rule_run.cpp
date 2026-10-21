#include "marketscan/research/rule_run.hpp"

#include <algorithm>
#include <memory>
#include <utility>

#include "marketscan/core/engine.hpp"
#include "marketscan/core/symbol_table.hpp"
#include "marketscan/data/daily_feed.hpp"
#include "marketscan/exec/fill_model.hpp"
#include "marketscan/strategy/registry.hpp"
#include "marketscan/strategy/strategy_instance.hpp"

namespace marketscan::research {

namespace {

namespace p = drawdown_param;

/// Long enough to be strictly after the decision, short enough to land before
/// any next session. The value is irrelevant past that: the order fills on the
/// first market event after it arrives, which is the next bar's quote.
constexpr Duration kSubmitLatency = time::kMinute;

[[nodiscard]] std::string validate(const RuleRunConfig& config) {
    if (config.series.empty()) {
        return "no bars";
    }
    const RuleParams& rule = config.rule;
    if (rule.tranches.empty() || rule.tranches.size() > static_cast<std::size_t>(kMaxTranches)) {
        return "between 1 and " + std::to_string(kMaxTranches) + " tranches";
    }
    if (rule.reference == ReferenceKind::RollingHigh && rule.window_days < 1) {
        return "a rolling high needs a window of at least one bar";
    }
    return {};
}

void apply(StrategyInstance& strategy, const RuleRunConfig& config) {
    const RuleParams& rule = config.rule;
    strategy.set_parameter(p::kReference, static_cast<double>(static_cast<int>(rule.reference)));
    strategy.set_parameter(p::kWindow, static_cast<double>(rule.window_days));
    strategy.set_parameter(p::kTrancheCount, static_cast<double>(rule.tranches.size()));
    for (int k = 0; k < kMaxTranches; ++k) {
        const bool used = static_cast<std::size_t>(k) < rule.tranches.size();
        strategy.set_parameter(p::kLevel[k], used ? rule.tranches[k].level_pct : 0.0);
        strategy.set_parameter(p::kFraction[k], used ? rule.tranches[k].fraction_pct : 0.0);
    }
    strategy.set_parameter(p::kRecoverToHigh, rule.recover_to_high ? 1.0 : 0.0);
    strategy.set_parameter(p::kTakeProfit, rule.take_profit_pct);
    strategy.set_parameter(p::kStopLoss, rule.stop_loss_pct);
    strategy.set_parameter(p::kTrailingStop, rule.trailing_stop_pct);
    strategy.set_parameter(p::kTimeStop, static_cast<double>(rule.time_stop_days));
    strategy.set_parameter(p::kReentry, rule.reentry ? 1.0 : 0.0);
    strategy.set_parameter(p::kReserve, config.reserve_pct);
    strategy.set_parameter(p::kCommission, config.commission_per_order);
    strategy.set_parameter(p::kStartDay, static_cast<double>(config.start_day));
}

[[nodiscard]] std::size_t bar_index(const BarSeries& series, Timestamp ts) {
    const auto it = std::lower_bound(
        series.bars.begin(), series.bars.end(), ts, [](const StoredBar& bar, Timestamp value) {
            return bar.ts < value;
        });
    return static_cast<std::size_t>(it - series.bars.begin());
}

}  // namespace

RuleParams RuleParams::buy_and_hold() {
    RuleParams rule;
    rule.tranches = {Tranche{0.0, 100.0}};
    rule.recover_to_high = false;
    return rule;
}

RuleRun run_rule(const RuleRunConfig& config) {
    RuleRun out;
    if (!drawdown_entry_registered()) {
        out.error = "the drawdown_entry strategy is not registered";
        return out;
    }
    if (out.error = validate(config); !out.ok()) {
        return out;
    }

    SymbolTable symbols;
    Engine engine;

    DailyFeedOptions feed_options;
    feed_options.synthesize_quotes = true;
    feed_options.spread_bps = config.spread_bps;
    try {
        engine.add_source(std::make_unique<DailyBarFeed>(
            std::vector<BarSeries>{config.series}, symbols, feed_options));
    } catch (const std::exception& error) {
        out.error = error.what();
        return out;
    }

    FillModelConfig fills;
    fills.commission.per_share = Money{};
    fills.commission.per_order = Money::from_double(config.commission_per_order);
    fills.commission.minimum = Money{};
    fills.submit_latency = kSubmitLatency;
    const SpreadCrossingFillModel fill_model(fills);

    StrategyRuntimeConfig runtime;
    runtime.starting_cash = config.starting_cash;
    // One sample per timestamp: each bar's quote and the bar itself share a
    // stamp, so the sample is taken once, at the quote, after that bar's fills.
    runtime.equity_sample_interval = 1;

    StrategyInstance strategy(std::string(kDrawdownEntryId),
                              "Drawdown entry",
                              StrategyRegistry::instance().create(kDrawdownEntryId),
                              {},
                              runtime,
                              &fill_model,
                              &engine.market(),
                              &symbols,
                              &engine.clock());
    apply(strategy, config);
    strategy.resolve_subscriptions();
    engine.add_sink(&strategy);
    strategy.start();

    try {
        engine.run();
    } catch (const std::exception& error) {
        out.error = error.what();
        return out;
    }
    strategy.stop();

    const Ledger& ledger = strategy.ledger();
    out.invariant_held = ledger.check_invariant();
    out.rejected = strategy.broker().rejected_count();

    for (const EquityPoint& point : ledger.equity_curve()) {
        out.times.push_back(point.ts);
        out.equity.push_back(point.equity.to_double());
    }
    if (out.times.size() != config.series.size()) {
        out.error = "expected one equity point per bar, got " + std::to_string(out.times.size()) +
                    " for " + std::to_string(config.series.size()) + " bars";
        return out;
    }

    for (const TradeRecord& record : ledger.blotter()) {
        RuleFill fill;
        fill.bar = bar_index(config.series, record.ts);
        fill.ts = record.ts;
        fill.side = record.side;
        fill.quantity = record.quantity;
        fill.price = record.fill_price;
        fill.tag = record.client_order_id;
        fill.commission = record.commission;
        fill.decision_ts = record.decision_ts;
        fill.price_ts = record.price_ts;
        fill.honest = record.honest_instant();
        out.fills.push_back(fill);
    }
    return out;
}

}  // namespace marketscan::research
