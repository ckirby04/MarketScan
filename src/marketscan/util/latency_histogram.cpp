#include "marketscan/util/latency_histogram.hpp"

#include <algorithm>
#include <bit>
#include <limits>

namespace marketscan {

LatencyHistogram::LatencyHistogram() : buckets_(kBucketCount, 0) {}

std::size_t LatencyHistogram::bucket_for(std::int64_t nanos) noexcept {
    const auto value = static_cast<std::uint64_t>(nanos < 0 ? 0 : nanos);

    // Below the sub-bucket count the buckets are one nanosecond wide, so small
    // values are recorded exactly rather than smeared across a log scale.
    if (value < kSubBuckets) {
        return static_cast<std::size_t>(value);
    }

    // Above it, each power of two is divided into kSubBuckets even slices,
    // which is what gives constant relative precision at every magnitude.
    const auto magnitude = static_cast<int>(63 - std::countl_zero(value));
    const int shift = magnitude - static_cast<int>(kSubBucketBits);
    const auto sub = static_cast<std::size_t>((value >> shift) & (kSubBuckets - 1));
    const std::size_t index =
        (static_cast<std::size_t>(magnitude) - kSubBucketBits + 1) * kSubBuckets + sub;
    return index < kBucketCount ? index : kBucketCount - 1;
}

std::int64_t LatencyHistogram::bucket_lower_bound(std::size_t index) noexcept {
    if (index < kSubBuckets) {
        return static_cast<std::int64_t>(index);
    }
    const std::size_t group = index / kSubBuckets;
    const std::size_t sub = index % kSubBuckets;
    const int shift = static_cast<int>(group) - 1;
    return static_cast<std::int64_t>((kSubBuckets + sub) << shift);
}

std::int64_t LatencyHistogram::bucket_upper_bound(std::size_t index) noexcept {
    if (index < kSubBuckets) {
        return static_cast<std::int64_t>(index) + 1;
    }
    const std::size_t group = index / kSubBuckets;
    const int shift = static_cast<int>(group) - 1;
    return bucket_lower_bound(index) + (std::int64_t{1} << shift);
}

void LatencyHistogram::record(std::int64_t nanos) noexcept {
    // A negative duration is not possible from steady_clock, but clamping
    // costs nothing and keeps a bad caller from indexing out of range.
    const std::int64_t value = nanos < 0 ? 0 : nanos;

    ++buckets_[bucket_for(value)];
    if (count_ == 0 || value < min_) {
        min_ = value;
    }
    if (value > max_) {
        max_ = value;
    }
    sum_ += static_cast<double>(value);
    ++count_;
}

void LatencyHistogram::reset() noexcept {
    std::fill(buckets_.begin(), buckets_.end(), std::uint64_t{0});
    count_ = 0;
    min_ = 0;
    max_ = 0;
    sum_ = 0.0;
}

double LatencyHistogram::mean() const noexcept {
    return count_ == 0 ? 0.0 : sum_ / static_cast<double>(count_);
}

std::int64_t LatencyHistogram::quantile(double q) const noexcept {
    if (count_ == 0) {
        return 0;
    }
    const double clamped = q < 0.0 ? 0.0 : (q > 1.0 ? 1.0 : q);
    const auto target =
        static_cast<std::uint64_t>(std::max(1.0, clamped * static_cast<double>(count_)));

    std::uint64_t seen = 0;
    for (std::size_t i = 0; i < buckets_.size(); ++i) {
        seen += buckets_[i];
        if (seen >= target) {
            // The bucket's upper edge, so a reported latency is never lower
            // than one that actually happened.
            return std::min(bucket_upper_bound(i), max_);
        }
    }
    return max_;
}

std::vector<LatencyHistogram::Bar> LatencyHistogram::bars() const {
    std::vector<Bar> result;
    for (std::size_t i = 0; i < buckets_.size(); ++i) {
        if (buckets_[i] != 0) {
            result.push_back(Bar{bucket_lower_bound(i), bucket_upper_bound(i), buckets_[i]});
        }
    }
    return result;
}

void LatencyHistogram::merge(const LatencyHistogram& other) noexcept {
    if (other.count_ == 0) {
        return;
    }
    for (std::size_t i = 0; i < buckets_.size(); ++i) {
        buckets_[i] += other.buckets_[i];
    }
    if (count_ == 0 || other.min_ < min_) {
        min_ = other.min_;
    }
    if (other.max_ > max_) {
        max_ = other.max_;
    }
    sum_ += other.sum_;
    count_ += other.count_;
}

}  // namespace marketscan
