#include "research_view.hpp"

#include <limits>

namespace marketscan::app {

namespace {

using nlohmann::json;

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

/// A number or null, as a double. The only conversion in this file.
[[nodiscard]] double num(const json& value) {
    return value.is_null() ? kNaN : value.get<double>();
}

[[nodiscard]] MetricsView metrics(const json& m) {
    MetricsView out{};
    out.total_return_pct = num(m.at("total_return_pct"));
    out.cagr_pct = num(m.at("cagr_pct"));
    out.volatility_pct = num(m.at("volatility_pct"));
    out.sharpe = num(m.at("sharpe"));
    out.sortino = num(m.at("sortino"));
    out.max_drawdown_pct = num(m.at("max_drawdown_pct"));
    out.max_drawdown_days = num(m.at("max_drawdown_days"));
    out.calmar = num(m.at("calmar"));
    out.ulcer_index = num(m.at("ulcer_index"));
    out.time_in_market_pct = num(m.at("time_in_market_pct"));
    out.n_entries = m.at("n_entries").get<long long>();
    out.win_rate_pct = num(m.at("win_rate_pct"));
    out.avg_trade_return_pct = num(m.at("avg_trade_return_pct"));
    out.worst_trade_pct = num(m.at("worst_trade_pct"));
    return out;
}

[[nodiscard]] std::vector<WindowView> windows(const json& w) {
    std::vector<WindowView> out;
    // In reading order rather than the document's key order.
    for (const char* name : {"in_sample", "out_of_sample", "full"}) {
        if (w.contains(name)) {
            const json& window = w.at(name);
            out.push_back(WindowView{name,
                                     window.at("from").get<std::string>(),
                                     window.at("to").get<std::string>(),
                                     metrics(window.at("metrics"))});
        }
    }
    return out;
}

}  // namespace

const char* grade_words(const std::string& grade) noexcept {
    if (grade == "supported") {
        return "SUPPORTED";
    }
    if (grade == "fragile") {
        return "FRAGILE";
    }
    if (grade == "not_supported") {
        return "NOT SUPPORTED";
    }
    return "INSUFFICIENT EVIDENCE";
}

ResearchView read_results(const json& r) {
    ResearchView v{};
    const json& verdict = r.at("verdict");
    v.grade = verdict.at("grade").get<std::string>();
    v.data_grade = verdict.at("data_grade").get<std::string>();
    v.deciding_gate =
        verdict.at("deciding_gate").is_null() ? "" : verdict.at("deciding_gate").get<std::string>();
    const json& headline = verdict.at("headline");
    v.strategy_oos_cagr_pct = num(headline.at("strategy_oos_cagr_pct"));
    v.benchmark_oos_cagr_pct = num(headline.at("benchmark_oos_cagr_pct"));
    v.strategy_oos_max_drawdown_pct = num(headline.at("strategy_oos_max_drawdown_pct"));
    v.benchmark_oos_max_drawdown_pct = num(headline.at("benchmark_oos_max_drawdown_pct"));
    v.range_p5 = num(headline.at("outcome_range").at("cagr_p5_pct"));
    v.range_p50 = num(headline.at("outcome_range").at("cagr_p50_pct"));
    v.range_p95 = num(headline.at("outcome_range").at("cagr_p95_pct"));
    for (const json& gate : verdict.at("gates")) {
        v.gates.push_back(GateView{gate.at("id").get<std::string>(),
                                   gate.at("status").get<std::string>(),
                                   gate.at("summary").get<std::string>()});
    }
    for (const json& w : r.at("warnings")) {
        v.warnings.emplace_back(w.at("code").get<std::string>(),
                                w.at("message").get<std::string>());
    }

    const json& stats = r.at("stats");
    for (const auto& [path, value] : stats.at("selected").at("params").items()) {
        v.selected_params.emplace_back(path, value.get<double>());
    }
    v.windows = windows(stats.at("selected").at("windows"));
    for (const json& b : stats.at("benchmarks")) {
        v.benchmarks.emplace_back(b.at("symbol").get<std::string>(), windows(b.at("windows")));
    }
    for (const json& t : stats.at("trades")) {
        TradeView trade{};
        trade.entry_date = t.at("entry_date").get<std::string>();
        trade.entry_price = t.at("entry_price").get<double>();
        trade.exit_date = t.at("exit_date").is_null() ? "" : t.at("exit_date").get<std::string>();
        trade.exit_price = t.at("exit_price").get<double>();
        trade.return_pct = t.at("return_pct").get<double>();
        trade.holding_days = t.at("holding_days").get<long long>();
        trade.max_adverse_pct = t.at("max_adverse_pct").get<double>();
        trade.exit_reason = t.at("exit_reason").get<std::string>();
        trade.window = t.at("window").get<std::string>();
        v.trades.push_back(std::move(trade));
    }

    const json& surface = stats.at("parameter_surface");
    v.has_surface = !surface.is_null();
    if (v.has_surface) {
        v.surface.swept = surface.at("swept").get<std::vector<std::string>>();
        v.surface.metric = surface.at("metric").get<std::string>();
        v.surface.selected_index = surface.at("selected_index").get<std::size_t>();
        v.surface.plateau_ratio = num(surface.at("plateau_ratio"));
        v.surface.effective_n = num(surface.at("effective_n"));
        for (const json& p : surface.at("points")) {
            SurfacePointView point{};
            for (const std::string& path : v.surface.swept) {
                point.params.push_back(p.at("params").at(path).get<double>());
            }
            point.in_sample_metric = num(p.at("in_sample_metric"));
            point.out_of_sample_metric =
                p.contains("out_of_sample_metric") ? num(p.at("out_of_sample_metric")) : kNaN;
            point.n_entries = p.at("n_entries").get<long long>();
            point.excluded = p.at("excluded").get<bool>();
            v.surface.points.push_back(std::move(point));
        }
    }
    v.regimes = stats.at("regimes").size();

    const json& timing = stats.at("random_timing");
    v.timing_metric = timing.at("metric").get<std::string>();
    v.timing_draws = timing.at("n_simulations").get<long long>();
    v.timing_p5 = num(timing.at("null_p5"));
    v.timing_p50 = num(timing.at("null_p50"));
    v.timing_p95 = num(timing.at("null_p95"));
    v.timing_percentile = num(timing.at("strategy_percentile"));
    v.timing_p_value = num(timing.at("p_value"));
    v.timing_p_corrected = num(timing.at("p_value_corrected"));

    const json& boot = stats.at("bootstrap");
    v.bootstrap_paths = boot.at("n_paths").get<long long>();
    v.bootstrap_block_days = boot.at("mean_block_days").get<long long>();
    const char* keys[3] = {"cagr_p5_pct", "cagr_p50_pct", "cagr_p95_pct"};
    for (int i = 0; i < 3; ++i) {
        v.bootstrap_strategy[i] = num(boot.at("strategy").at(keys[i]));
        v.bootstrap_benchmark[i] = num(boot.at("benchmark").at(keys[i]));
    }
    v.bootstrap_prob_beats = num(boot.at("prob_beats_benchmark"));
    v.bootstrap_share_ruined = num(boot.at("share_paths_below_minus_90_pct"));

    for (const json& d : stats.at("data")) {
        v.data.push_back(DataView{d.at("security_id").get<std::string>(),
                                  d.at("series_id").get<std::string>(),
                                  d.at("quality_tier").get<std::string>(),
                                  d.at("used_from").get<std::string>(),
                                  d.at("used_to").get<std::string>(),
                                  !d.at("synthetic").is_null()});
    }
    const json& run = r.at("run");
    v.run_id = run.at("run_id").get<std::string>();
    v.started_at = run.at("started_at").get<std::string>();
    v.duration_ms = run.at("duration_ms").get<long long>();
    return v;
}

}  // namespace marketscan::app
