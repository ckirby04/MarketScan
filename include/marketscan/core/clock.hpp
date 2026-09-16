#ifndef MARKETSCAN_CORE_CLOCK_HPP
#define MARKETSCAN_CORE_CLOCK_HPP

#include <atomic>
#include <cassert>

#include "marketscan/core/types.hpp"

namespace marketscan {

/// The seam between replay and live.
///
/// This interface is the reason there is no "backtest mode" anywhere in the
/// engine. A strategy asks the host what time it is; in replay the host is
/// holding a SimulatedClock that the event loop winds forward to each event's
/// timestamp, and live it is holding a SystemClock. Nothing else changes, and
/// no strategy can tell which one it got.
class IClock {
public:
    IClock() = default;
    IClock(const IClock&) = delete;
    IClock& operator=(const IClock&) = delete;
    IClock(IClock&&) = delete;
    IClock& operator=(IClock&&) = delete;
    virtual ~IClock() = default;

    [[nodiscard]] virtual Timestamp now() const noexcept = 0;
};

/// A clock the replay driver owns and winds forward.
///
/// Reads are atomic because the GUI thread samples the engine's clock to draw
/// a progress bar while the engine thread advances it. Only the engine thread
/// ever writes.
class SimulatedClock final : public IClock {
public:
    explicit SimulatedClock(Timestamp start = 0) noexcept : now_(start) {}

    [[nodiscard]] Timestamp now() const noexcept override {
        return now_.load(std::memory_order_relaxed);
    }

    /// Wind forward to `ts`.
    ///
    /// Time never runs backwards inside a run. Scrubbing the transport
    /// backwards rebuilds the engine from the start rather than rewinding this
    /// clock, because rewinding would leave every strategy holding state from
    /// a future it is about to be shown again.
    void advance_to(Timestamp ts) noexcept {
        assert(ts >= now_.load(std::memory_order_relaxed) &&
               "SimulatedClock moved backwards; replay must restart instead");
        now_.store(ts, std::memory_order_relaxed);
    }

    void advance_by(Duration d) noexcept {
        assert(d >= 0 && "SimulatedClock advanced by a negative duration");
        now_.store(now_.load(std::memory_order_relaxed) + d, std::memory_order_relaxed);
    }

    /// Drops the monotonicity guarantee. Only the engine's own reset path
    /// calls this, when it is rebuilding a run from the beginning.
    void reset(Timestamp ts = 0) noexcept { now_.store(ts, std::memory_order_relaxed); }

private:
    std::atomic<Timestamp> now_;
};

/// Wall clock, for live trading. Reads the OS.
class SystemClock final : public IClock {
public:
    [[nodiscard]] Timestamp now() const noexcept override;
};

}  // namespace marketscan

#endif  // MARKETSCAN_CORE_CLOCK_HPP
