#ifndef MARKETSCAN_METRICS_CORRELATION_HPP
#define MARKETSCAN_METRICS_CORRELATION_HPP

#include <cstddef>
#include <span>
#include <string>
#include <vector>

namespace marketscan::metrics {

/// Sample covariance, dividing by n-1. NaN for fewer than two usable pairs.
///
/// Pairwise complete: a period counts only when both series have a finite
/// observation for it. Dropping observations from one series but not the other
/// would silently compare different instants.
[[nodiscard]] double covariance(std::span<const double> a, std::span<const double> b) noexcept;

/// Pearson correlation on [-1, 1]. NaN when either series is constant, because
/// a flat line has no correlation with anything -- not zero correlation, which
/// is a claim about independence.
[[nodiscard]] double correlation(std::span<const double> a, std::span<const double> b) noexcept;

/// Pairwise correlations across N series.
///
/// Near-useless with two theories loaded, and load-bearing with twenty. It is
/// built now so the layout is settled before it matters.
class CorrelationMatrix {
public:
    CorrelationMatrix() = default;
    CorrelationMatrix(std::vector<std::string> labels, std::vector<double> values);

    [[nodiscard]] std::size_t size() const noexcept { return labels_.size(); }

    [[nodiscard]] bool empty() const noexcept { return labels_.empty(); }

    /// Row-major. Out of range returns NaN rather than reading past the end.
    [[nodiscard]] double at(std::size_t row, std::size_t column) const noexcept;

    [[nodiscard]] const std::vector<std::string>& labels() const noexcept { return labels_; }

    [[nodiscard]] const std::vector<double>& values() const noexcept { return values_; }

    /// The most correlated pair, ignoring the diagonal. Returns false when
    /// there is no pair to compare.
    [[nodiscard]] bool
    most_correlated(std::size_t& row, std::size_t& column, double& value) const noexcept;

private:
    std::vector<std::string> labels_;
    std::vector<double> values_;
};

/// Correlates every series with every other.
///
/// Series are truncated to the shortest, from the front. They come from one
/// run on one sampling schedule, so a length difference means one strategy
/// started later, and index i of each is the same instant.
///
/// The diagonal is 1 where a series has any variance at all, and NaN where it
/// does not: a strategy that never traded is not perfectly correlated with
/// itself, it is simply not something correlation applies to.
[[nodiscard]] CorrelationMatrix correlation_matrix(const std::vector<std::vector<double>>& series,
                                                   std::vector<std::string> labels);

}  // namespace marketscan::metrics

#endif  // MARKETSCAN_METRICS_CORRELATION_HPP
