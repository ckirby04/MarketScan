#include "chart_model.hpp"

#include <algorithm>

#include "marketscan/core/types.hpp"
#include "palette.hpp"

namespace marketscan::app {
namespace {

/// Two points is the minimum that draws as a line rather than a dot.
[[nodiscard]] bool has_series(const StrategySnapshot& row) {
    return row.times.size() >= 2;
}

[[nodiscard]] bool contains(const std::vector<std::string>& haystack, const std::string& needle) {
    return std::find(haystack.begin(), haystack.end(), needle) != haystack.end();
}

}  // namespace

std::size_t ChartSeries::points() const noexcept {
    if (times == nullptr || values == nullptr) {
        return 0;
    }
    return std::min(times->size(), values->size());
}

const ChartSeries* ChartModel::emphasis() const noexcept {
    for (const ChartSeries& s : series) {
        if (s.emphasized) {
            return &s;
        }
    }
    return nullptr;
}

const ChartSeries* ChartModel::benchmark() const noexcept {
    for (const ChartSeries& s : series) {
        if (s.is_benchmark) {
            return &s;
        }
    }
    return nullptr;
}

namespace {

/// Everything density is allowed to change, decided once for both builders.
///
/// `series` arrives with its data, identity and role filled in; this sets how it
/// is drawn. Kept in one function so that a session's chart and a research
/// job's chart cannot come to disagree about what a curve looks like.
void present(ChartSeries& series, const SeriesColors& colors, ChartDensity density) {
    const bool overview = density == ChartDensity::Overview;

    if (series.is_benchmark) {
        series.label += " (benchmark)";
    } else if (series.folded) {
        // Named rather than given a ninth hue. A viewer seeing three
        // identical grays should be told they are Other, not left to guess.
        series.label += " (other)";
    }

    // The assignment. Identical in both densities -- it is a property of the
    // entity, not of how it is being drawn.
    series.identity_color = colors.color(series.id);

    // The presentation. This is what density is permitted to change.
    //
    // The benchmark is never greyed and never folded: it is always drawn, in
    // its own fixed hue, because it is the thing every other curve is being
    // compared against. Folding it into the context gray would quietly
    // remove the only reference point on the chart.
    const bool context = !series.emphasized && !series.is_benchmark;
    if (overview && context) {
        series.drawn_color = theme::colors().emphasis_gray;
    } else {
        series.drawn_color = series.identity_color;
    }

    if (overview) {
        series.line_weight = series.emphasized ? 3.0F : (series.is_benchmark ? 2.0F : 1.5F);
        // Only the subject and the benchmark are labelled against the curve.
        // Everything else is context and a label on context is noise.
        series.direct_label = series.emphasized || series.is_benchmark;
    } else {
        series.line_weight = series.is_benchmark ? 1.4F : 2.0F;
        series.direct_label = false;  // Operator has a legend instead.
    }
}

[[nodiscard]] ChartModel empty_model(const ChartRequest& request) {
    ChartModel model;
    model.density = request.density;
    model.normalised = request.normalised;
    model.log_scale = request.log_scale;
    model.y_label = request.normalised ? "Growth of 1" : "Equity";
    return model;
}

}  // namespace

ChartModel build_chart_model(const SessionSnapshot& snapshot,
                             const SeriesColors& colors,
                             const ChartRequest& request) {
    ChartModel model = empty_model(request);

    // Which theory is the subject. Resolved before the loop so that "the first
    // one" means the first in the snapshot's order rather than the first that
    // happened to survive the filters.
    std::string emphasis = request.emphasis_id;
    if (emphasis.empty()) {
        for (const StrategySnapshot& row : snapshot.strategies) {
            if (!row.is_benchmark && has_series(row)) {
                emphasis = row.id;
                break;
            }
        }
    }

    for (const StrategySnapshot& row : snapshot.strategies) {
        if (!has_series(row)) {
            continue;
        }
        if (row.is_benchmark && !request.show_benchmark) {
            continue;
        }
        if (contains(request.hidden, row.id)) {
            continue;
        }

        ChartSeries series;
        series.id = row.id;
        series.label = row.display_name;
        series.is_benchmark = row.is_benchmark;
        series.emphasized = !row.is_benchmark && row.id == emphasis;
        series.folded = colors.folded(row.id);
        series.times = &row.times;

        // The data, and nothing derived from it. Both series are already in the
        // snapshot; choosing between them is not a computation.
        series.values = request.normalised ? &row.normalised : &row.equity_curve;
        series.drawdown = &row.drawdown;

        present(series, colors, request.density);
        model.folded_count += series.folded ? 1 : 0;
        model.series.push_back(std::move(series));
    }

    if (snapshot.clock > 0 && !model.series.empty()) {
        model.now_seconds =
            static_cast<double>(snapshot.clock) / static_cast<double>(time::kSecond);
    }
    return model;
}

SeriesColors colors_for(const std::vector<research::JobCurve>& curves) {
    std::vector<std::pair<std::string, bool>> entities;
    bool benchmark_taken = false;
    for (const research::JobCurve& curve : curves) {
        const bool primary = curve.is_benchmark && !benchmark_taken;
        benchmark_taken = benchmark_taken || primary;
        entities.emplace_back(curve.id, primary);
    }
    return SeriesColors(entities);
}

ChartModel build_chart_model(const std::vector<research::JobCurve>& curves,
                             const SeriesColors& colors,
                             const ChartRequest& request) {
    ChartModel model = empty_model(request);

    std::string emphasis = request.emphasis_id;
    if (emphasis.empty()) {
        for (const research::JobCurve& curve : curves) {
            if (!curve.is_benchmark && curve.times.size() >= 2) {
                emphasis = curve.id;
                break;
            }
        }
    }

    bool benchmark_taken = false;
    for (const research::JobCurve& curve : curves) {
        const bool primary = curve.is_benchmark && !benchmark_taken;
        benchmark_taken = benchmark_taken || primary;
        if (curve.times.size() < 2 || contains(request.hidden, curve.id)) {
            continue;
        }
        if (primary && !request.show_benchmark) {
            continue;
        }

        ChartSeries series;
        series.id = curve.id;
        series.label = curve.label;
        series.is_benchmark = primary;
        series.emphasized = !primary && curve.id == emphasis;
        series.folded = colors.folded(curve.id);
        series.times = &curve.times;
        series.values = request.normalised ? &curve.normalised : &curve.equity;
        series.drawdown = &curve.drawdown;

        present(series, colors, request.density);
        model.folded_count += series.folded ? 1 : 0;
        model.series.push_back(std::move(series));
    }
    return model;
}

}  // namespace marketscan::app
