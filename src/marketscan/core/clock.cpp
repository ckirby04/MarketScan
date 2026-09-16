#include "marketscan/core/clock.hpp"

#include <chrono>

namespace marketscan {

Timestamp SystemClock::now() const noexcept {
    // system_clock is the right choice despite not being monotonic: this value
    // is a wall-clock instant that has to line up with exchange timestamps, not
    // a stopwatch. Latency measurement uses steady_clock instead -- see
    // util/latency_histogram.hpp.
    const auto since_epoch = std::chrono::system_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::nanoseconds>(since_epoch).count();
}

}  // namespace marketscan
