#include "marketscan/metrics/correlation.hpp"

#include <cmath>
#include <limits>
#include <utility>

namespace marketscan::metrics {
namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

/// Accumulates the sums a correlation needs, over pairs where both series have
/// a finite observation.
struct PairwiseMoments {
    std::size_t count = 0;
    double mean_a = 0.0;
    double mean_b = 0.0;
    double saa = 0.0;
    double sbb = 0.0;
    double sab = 0.0;
};

[[nodiscard]] PairwiseMoments moments(std::span<const double> a,
                                      std::span<const double> b) noexcept {
    PairwiseMoments result;
    const std::size_t n = a.size() < b.size() ? a.size() : b.size();

    // Two passes: the means first, then the deviations. The single-pass form
    // loses the answer to cancellation on return series, whose values are tiny
    // relative to the precision of their own squares.
    double sum_a = 0.0;
    double sum_b = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        if (std::isfinite(a[i]) && std::isfinite(b[i])) {
            sum_a += a[i];
            sum_b += b[i];
            ++result.count;
        }
    }
    if (result.count < 2) {
        return result;
    }

    const auto count = static_cast<double>(result.count);
    result.mean_a = sum_a / count;
    result.mean_b = sum_b / count;

    for (std::size_t i = 0; i < n; ++i) {
        if (!std::isfinite(a[i]) || !std::isfinite(b[i])) {
            continue;
        }
        const double da = a[i] - result.mean_a;
        const double db = b[i] - result.mean_b;
        result.saa += da * da;
        result.sbb += db * db;
        result.sab += da * db;
    }
    return result;
}

}  // namespace

double covariance(std::span<const double> a, std::span<const double> b) noexcept {
    const PairwiseMoments m = moments(a, b);
    if (m.count < 2) {
        return kNaN;
    }
    return m.sab / static_cast<double>(m.count - 1);
}

double correlation(std::span<const double> a, std::span<const double> b) noexcept {
    const PairwiseMoments m = moments(a, b);
    if (m.count < 2) {
        return kNaN;
    }
    // A constant series has no correlation with anything. That is not zero
    // correlation, which would be a claim about independence.
    //
    // Compared against the scale of the data rather than against zero: summing
    // identical values accumulates rounding, so a constant series has a sum of
    // squared deviations that is tiny but positive, and dividing by its square
    // root produces a confident-looking number from pure noise.
    const double floor_a = m.mean_a * m.mean_a * static_cast<double>(m.count) * 1e-24;
    const double floor_b = m.mean_b * m.mean_b * static_cast<double>(m.count) * 1e-24;
    if (!(m.saa > floor_a) || !(m.sbb > floor_b)) {
        return kNaN;
    }
    const double r = m.sab / std::sqrt(m.saa * m.sbb);
    // Clamp: the arithmetic above can land a few ulps outside [-1, 1], and a
    // correlation of 1.0000000000000002 breaks every plot axis it touches.
    return r > 1.0 ? 1.0 : (r < -1.0 ? -1.0 : r);
}

CorrelationMatrix::CorrelationMatrix(std::vector<std::string> labels, std::vector<double> values)
    : labels_(std::move(labels)), values_(std::move(values)) {}

double CorrelationMatrix::at(std::size_t row, std::size_t column) const noexcept {
    const std::size_t n = labels_.size();
    if (row >= n || column >= n) {
        return kNaN;
    }
    return values_[row * n + column];
}

bool CorrelationMatrix::most_correlated(std::size_t& row,
                                        std::size_t& column,
                                        double& value) const noexcept {
    const std::size_t n = labels_.size();
    bool found = false;
    double best = -2.0;
    for (std::size_t i = 0; i < n; ++i) {
        for (std::size_t j = i + 1; j < n; ++j) {
            const double r = at(i, j);
            if (std::isfinite(r) && r > best) {
                best = r;
                row = i;
                column = j;
                found = true;
            }
        }
    }
    if (found) {
        value = best;
    }
    return found;
}

CorrelationMatrix correlation_matrix(const std::vector<std::vector<double>>& series,
                                     std::vector<std::string> labels) {
    const std::size_t n = series.size();
    labels.resize(n);

    std::vector<double> values(n * n, kNaN);
    for (std::size_t i = 0; i < n; ++i) {
        for (std::size_t j = i; j < n; ++j) {
            const double r =
                correlation(std::span<const double>(series[i]), std::span<const double>(series[j]));
            values[i * n + j] = r;
            values[j * n + i] = r;
        }
    }
    return CorrelationMatrix(std::move(labels), std::move(values));
}

}  // namespace marketscan::metrics
