#ifndef MARKETSCAN_UTIL_LATENCY_HISTOGRAM_HPP
#define MARKETSCAN_UTIL_LATENCY_HISTOGRAM_HPP

#include <chrono>
#include <cstdint>
#include <vector>

#include "marketscan/core/types.hpp"

namespace marketscan {

/// Latency distribution, in nanoseconds, on a logarithmic bucket scale.
///
/// Recording every sample would cost more memory than the run, and a mean
/// would hide the only part anyone cares about. What matters in an execution
/// path is the tail: p99 and p99.9 are where a strategy actually loses money,
/// and an average latency of 800ns tells you nothing about the one event in a
/// thousand that took 40 microseconds.
///
/// So this is an HdrHistogram-style structure: buckets whose width grows with
/// magnitude, giving constant *relative* precision across nine orders of
/// magnitude for a fixed, small amount of memory. Recording is a few integer
/// operations with no allocation and no branch on anything but the value, so
/// it is safe on the per-event path it measures.
///
/// **In replay these numbers are not trading latency.** They measure how long
/// this process took to run a strategy's callback, which in a backtest is a
/// property of the machine rather than of the market. The instrumentation
/// exists and is correct from the start so that the same code path is already
/// measured when it does mean something.
class LatencyHistogram {
public:
    /// Values are bucketed by their magnitude: `kSubBuckets` linear buckets
    /// within each power of two. 8 gives about 12% worst-case relative error,
    /// which is finer than the noise in any measurement this records.
    static constexpr std::size_t kSubBucketBits = 3;
    static constexpr std::size_t kSubBuckets = std::size_t{1} << kSubBucketBits;
    static constexpr std::size_t kMagnitudes = 40;  // up to ~2^40 ns, ~18 minutes
    static constexpr std::size_t kBucketCount = kMagnitudes * kSubBuckets;

    LatencyHistogram();

    void record(std::int64_t nanos) noexcept;

    void reset() noexcept;

    [[nodiscard]] std::uint64_t count() const noexcept { return count_; }

    [[nodiscard]] std::int64_t min() const noexcept { return count_ == 0 ? 0 : min_; }

    [[nodiscard]] std::int64_t max() const noexcept { return max_; }

    [[nodiscard]] double mean() const noexcept;

    /// `quantile` in [0, 1]. Returns the upper edge of the bucket the quantile
    /// falls in, so a reported value is never lower than the true one.
    [[nodiscard]] std::int64_t quantile(double quantile) const noexcept;

    [[nodiscard]] std::int64_t p50() const noexcept { return quantile(0.50); }

    [[nodiscard]] std::int64_t p90() const noexcept { return quantile(0.90); }

    [[nodiscard]] std::int64_t p99() const noexcept { return quantile(0.99); }

    [[nodiscard]] std::int64_t p999() const noexcept { return quantile(0.999); }

    struct Bar {
        std::int64_t lower_ns = 0;
        std::int64_t upper_ns = 0;
        std::uint64_t count = 0;
    };

    /// Non-empty buckets, ascending. For the histogram plot.
    [[nodiscard]] std::vector<Bar> bars() const;

    /// Folds another histogram in. For merging per-strategy measurements.
    void merge(const LatencyHistogram& other) noexcept;

private:
    [[nodiscard]] static std::size_t bucket_for(std::int64_t nanos) noexcept;
    [[nodiscard]] static std::int64_t bucket_lower_bound(std::size_t index) noexcept;
    [[nodiscard]] static std::int64_t bucket_upper_bound(std::size_t index) noexcept;

    std::vector<std::uint64_t> buckets_;
    std::uint64_t count_ = 0;
    std::int64_t min_ = 0;
    std::int64_t max_ = 0;
    double sum_ = 0.0;
};

/// Times a scope into a histogram.
///
/// steady_clock, not system_clock: this measures an elapsed duration, and
/// system_clock can step sideways when the machine syncs its time, which would
/// record a negative latency or a spike that never happened.
class ScopedLatency {
public:
    explicit ScopedLatency(LatencyHistogram& histogram) noexcept
        : histogram_(&histogram), start_(std::chrono::steady_clock::now()) {}

    ScopedLatency(const ScopedLatency&) = delete;
    ScopedLatency& operator=(const ScopedLatency&) = delete;
    ScopedLatency(ScopedLatency&&) = delete;
    ScopedLatency& operator=(ScopedLatency&&) = delete;

    ~ScopedLatency() {
        const auto elapsed = std::chrono::steady_clock::now() - start_;
        histogram_->record(std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count());
    }

private:
    LatencyHistogram* histogram_;
    std::chrono::steady_clock::time_point start_;
};

}  // namespace marketscan

#endif  // MARKETSCAN_UTIL_LATENCY_HISTOGRAM_HPP
