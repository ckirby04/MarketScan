#ifndef MARKETSCAN_LIVE_DETAIL_HPP
#define MARKETSCAN_LIVE_DETAIL_HPP

/*
 * Small helpers shared by the live layer's translation units.
 *
 * WHY THIS EXISTS, WHEN A PREVIOUS COMMENT SAID IT SHOULD NOT
 *
 * `order_tracker.cpp` used to carry `number()` with the note "local here as well
 * as in live_broker.cpp, because the alternative is a utility header nobody
 * asked for". That was the right call when it was one helper in two files. It
 * stopped being the right call at four helpers across four files, which is what
 * Sessions F and F2 left behind:
 *
 *   number()              live_broker.cpp, order_tracker.cpp
 *   local_now()           live_broker.cpp, live_feed.cpp
 *   interruptible_sleep() live_broker.cpp, live_feed.cpp
 *   tif_name()            live_broker.cpp, recorded_broker.cpp
 *
 * Every copy was byte-identical before it was moved here -- checked, not
 * assumed, because collapsing two functions that had quietly drifted apart
 * would be a behaviour change wearing a refactor's clothes.
 *
 * WHAT DOES NOT BELONG HERE
 *
 * `kReceiveTimeoutMs` is 250 in both live_broker.cpp and live_feed.cpp and is
 * deliberately left duplicated. They are two different sockets -- trade updates
 * and market data -- and two constants that happen to be equal are not one
 * constant. Collapsing them would couple two independent tuning knobs, so that
 * the next person to lengthen one silently lengthens the other.
 *
 * Anything a caller outside the live layer would want does not belong here
 * either; `detail` means what it says. These are formatting and timing helpers,
 * not API.
 */

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

#include "marketscan/abi/plugin_abi.h"
#include "marketscan/core/clock.hpp"
#include "marketscan/core/types.hpp"

namespace marketscan::live::detail {

/// Wall clock, for log lines and for timestamps the venue's answers get stamped
/// with.
///
/// The static is shared across translation units now rather than one per
/// translation unit as before, which is unobservable: `SystemClock` has no data
/// members and `now()` reads the system clock.
[[nodiscard]] inline Timestamp local_now() noexcept {
    static const SystemClock clock;
    return clock.now();
}

/// Sleeps in slices so a stop request is noticed inside a long backoff.
inline void interruptible_sleep(int total_ms, const std::atomic<bool>& stopping) {
    constexpr int kSlice = 100;
    int slept = 0;
    while (slept < total_ms && !stopping.load(std::memory_order_relaxed)) {
        const int slice = std::min(kSlice, total_ms - slept);
        std::this_thread::sleep_for(std::chrono::milliseconds(slice));
        slept += slice;
    }
}

/// The venue takes quantities and prices as strings; log lines want them fixed.
[[nodiscard]] inline std::string number(double value, int decimals) {
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.*f", decimals, value);
    return buffer;
}

[[nodiscard]] inline const char* tif_name(MS_TimeInForce tif) noexcept {
    switch (tif) {
        case MS_TIF_IOC:
            return "ioc";
        case MS_TIF_GTC:
            return "gtc";
        case MS_TIF_DAY:
        default:
            return "day";
    }
}

}  // namespace marketscan::live::detail

#endif  // MARKETSCAN_LIVE_DETAIL_HPP
