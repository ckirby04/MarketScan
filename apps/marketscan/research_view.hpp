#ifndef MARKETSCAN_APP_RESEARCH_VIEW_HPP
#define MARKETSCAN_APP_RESEARCH_VIEW_HPP

/*
 * A Results document, typed, for the Research view to draw.
 *
 * SPEC_RESEARCH_VIEW.md section 3: one Results document, one renderer, and the
 * view computes nothing. This is the renderer's whole input. Every field is
 * read straight off the document -- a null becomes NaN, a missing optional
 * section becomes `has_* = false` -- and nothing is added, subtracted, rounded
 * or re-derived. tests/test_research_view.cpp checks that every field equals
 * its place in the document.
 *
 * Kept on this side of the ImGui boundary so that check needs no window.
 */

#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace marketscan::app {

struct MetricsView {
    double total_return_pct;
    double cagr_pct;
    double volatility_pct;
    double sharpe;
    double sortino;
    double max_drawdown_pct;
    double max_drawdown_days;
    double calmar;
    double ulcer_index;
    double time_in_market_pct;
    long long n_entries = 0;
    double win_rate_pct;
    double avg_trade_return_pct;
    double worst_trade_pct;
};

struct WindowView {
    std::string name;  // full | in_sample | out_of_sample
    std::string from;
    std::string to;
    MetricsView metrics{};
};

struct GateView {
    std::string id;
    std::string status;  // pass | fail | not_applicable
    std::string summary;
};

struct TradeView {
    std::string entry_date;
    double entry_price;
    std::string exit_date;  // empty when open
    double exit_price;
    double return_pct;
    long long holding_days = 0;
    double max_adverse_pct;
    std::string exit_reason;
    std::string window;
};

struct SurfacePointView {
    /// One value per swept dimension, in `SurfaceView::swept` order.
    std::vector<double> params;
    double in_sample_metric;
    double out_of_sample_metric;
    long long n_entries = 0;
    bool excluded = false;
};

struct SurfaceView {
    std::vector<std::string> swept;
    std::string metric;
    std::vector<SurfacePointView> points;
    std::size_t selected_index = 0;
    double plateau_ratio;
    double effective_n;
};

struct DataView {
    std::string security_id;
    std::string series_id;
    std::string quality_tier;
    std::string used_from;
    std::string used_to;
    bool synthetic = false;
};

struct ResearchView {
    // Verdict.
    std::string grade;
    std::string data_grade;
    std::string deciding_gate;  // empty when supported
    double strategy_oos_cagr_pct;
    double benchmark_oos_cagr_pct;
    double strategy_oos_max_drawdown_pct;
    double benchmark_oos_max_drawdown_pct;
    double range_p5;
    double range_p50;
    double range_p95;
    std::vector<std::pair<std::string, std::string>> warnings;  // code, message

    // Why.
    std::vector<GateView> gates;

    // Stats.
    std::vector<std::pair<std::string, double>> selected_params;
    std::vector<WindowView> windows;
    std::vector<std::pair<std::string, std::vector<WindowView>>> benchmarks;
    std::vector<TradeView> trades;
    bool has_surface = false;
    SurfaceView surface{};
    std::size_t regimes = 0;

    std::string timing_metric;
    long long timing_draws = 0;
    double timing_p5;
    double timing_p50;
    double timing_p95;
    double timing_percentile;
    double timing_p_value;
    double timing_p_corrected;

    long long bootstrap_paths = 0;
    long long bootstrap_block_days = 0;
    double bootstrap_strategy[3];
    double bootstrap_benchmark[3];
    double bootstrap_prob_beats;
    double bootstrap_share_ruined;

    std::vector<DataView> data;
    std::string run_id;
    std::string started_at;
    long long duration_ms = 0;
};

/// Reads a Results document. Throws nlohmann::json::exception on one that is
/// not shaped like results.schema.json -- which a document that passed the
/// schema cannot be.
[[nodiscard]] ResearchView read_results(const nlohmann::json& results);

/// The words the view uses for a grade. Never "good idea": SPEC_RESEARCH_VIEW.md
/// section 2.
[[nodiscard]] const char* grade_words(const std::string& grade) noexcept;

}  // namespace marketscan::app

#endif  // MARKETSCAN_APP_RESEARCH_VIEW_HPP
