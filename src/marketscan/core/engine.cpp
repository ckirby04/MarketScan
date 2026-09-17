#include "marketscan/core/engine.hpp"

#include <stdexcept>
#include <utility>

namespace marketscan {
namespace {

constexpr std::size_t kNoSource = static_cast<std::size_t>(-1);

}  // namespace

template<typename Clock>
EngineT<Clock>::EngineT(EngineConfig config) : config_(config) {}

template<typename Clock>
EngineT<Clock>::~EngineT() = default;

template<typename Clock>
void EngineT<Clock>::add_source(std::unique_ptr<IEventSource> source) {
    if (!source) {
        throw std::invalid_argument("Engine::add_source: null source");
    }
    if (primed_) {
        throw std::logic_error("Engine::add_source: sources cannot be added mid-run");
    }
    SourceSlot slot;
    slot.source = std::move(source);
    sources_.push_back(std::move(slot));
}

template<typename Clock>
void EngineT<Clock>::add_sink(IEventSink* sink) {
    if (sink == nullptr) {
        throw std::invalid_argument("Engine::add_sink: null sink");
    }
    sinks_.push_back(sink);
}

template<typename Clock>
void EngineT<Clock>::refill(SourceSlot& slot) {
    if (slot.exhausted) {
        return;
    }
    MarketEvent next;
    if (!slot.source->next(next)) {
        slot.exhausted = true;
        slot.has_pending = false;
        return;
    }

    // A source that goes backwards would make the merge below emit a
    // decreasing timestamp, which no strategy and no ledger can be expected to
    // survive. Fail loudly at the seam where the bad data entered rather than
    // somewhere downstream that looks like a strategy bug.
    if (slot.last_ts != kNever && next.ts() < slot.last_ts) {
        throw std::runtime_error(
            "event source '" + std::string(slot.source->name()) +
            "' produced an out-of-order event: " + time::to_iso8601(next.ts()) + " after " +
            time::to_iso8601(slot.last_ts) +
            ". Feeds must deliver events in non-decreasing "
            "timestamp order.");
    }

    slot.pending = next;
    slot.last_ts = next.ts();
    slot.has_pending = true;
}

template<typename Clock>
std::size_t EngineT<Clock>::select_next() const noexcept {
    // Linear scan. There is normally one source and rarely more than a
    // handful, so a heap would cost more in bookkeeping than it saves -- and
    // this way the tie-break is obviously registration order, which is what
    // the determinism contract promises.
    std::size_t best = kNoSource;
    Timestamp best_ts = 0;
    for (std::size_t i = 0; i < sources_.size(); ++i) {
        const SourceSlot& slot = sources_[i];
        if (!slot.has_pending) {
            continue;
        }
        const Timestamp ts = slot.pending.ts();
        if (best == kNoSource || ts < best_ts) {
            best = i;
            best_ts = ts;
        }
    }
    return best;
}

template<typename Clock>
void EngineT<Clock>::dispatch(const MarketEvent& event) {
    switch (event.type) {
        case EventType::Bar:
            for (IEventSink* sink : sinks_) {
                sink->on_bar(event.bar);
            }
            break;
        case EventType::Quote:
            for (IEventSink* sink : sinks_) {
                sink->on_quote(event.quote);
            }
            break;
        case EventType::Trade:
            for (IEventSink* sink : sinks_) {
                sink->on_trade(event.trade);
            }
            break;
    }

    for (IEventSink* sink : sinks_) {
        sink->on_event_complete(event);
    }
}

template<typename Clock>
bool EngineT<Clock>::step() {
    if (!primed_) {
        for (SourceSlot& slot : sources_) {
            refill(slot);
        }
        primed_ = true;
    }

    const std::size_t index = select_next();
    if (index == kNoSource) {
        return false;
    }

    const MarketEvent event = sources_[index].pending;

    // The clock moves before anything sees the event, so that a strategy
    // asking host->now() inside on_bar is told the bar's own timestamp and not
    // the previous one.
    clock_.advance_to(event.ts());

    // The book moves before anything is told about the event, so that a
    // strategy and the fill model pricing its order see the same market.
    market_.apply(event);

    if (!stats_.started) {
        stats_.first_event_ts = event.ts();
        stats_.started = true;
    }
    stats_.last_event_ts = event.ts();
    ++stats_.events_dispatched;
    if (event.type == EventType::Bar) {
        ++stats_.bars_dispatched;
    }

    dispatch(event);

    // Refill after dispatch, not before: a live source may have produced
    // something while the sinks were running, and refilling first would hold a
    // stale lookahead for an extra event.
    refill(sources_[index]);
    return true;
}

template<typename Clock>
bool EngineT<Clock>::has_pending_event() {
    if (!primed_) {
        for (SourceSlot& slot : sources_) {
            refill(slot);
        }
        primed_ = true;
    }
    return select_next() != kNoSource;
}

template<typename Clock>
Timestamp EngineT<Clock>::next_event_ts() {
    if (!has_pending_event()) {
        return kForever;
    }
    return sources_[select_next()].pending.ts();
}

template<typename Clock>
std::uint64_t EngineT<Clock>::run(std::uint64_t max_events) {
    std::uint64_t dispatched = 0;
    while (dispatched < max_events && step()) {
        ++dispatched;
    }
    return dispatched;
}

template<typename Clock>
std::uint64_t EngineT<Clock>::run_until(Timestamp ts) {
    if (!primed_) {
        for (SourceSlot& slot : sources_) {
            refill(slot);
        }
        primed_ = true;
    }

    std::uint64_t dispatched = 0;
    while (true) {
        const std::size_t index = select_next();
        if (index == kNoSource || sources_[index].pending.ts() > ts) {
            break;
        }
        if (!step()) {
            break;
        }
        ++dispatched;
    }
    return dispatched;
}

template<typename Clock>
bool EngineT<Clock>::reset() {
    // Ask every source before rewinding any of them, so that one source
    // refusing leaves the engine exactly as it was rather than half rewound.
    for (const SourceSlot& slot : sources_) {
        if (!slot.source->can_reset()) {
            return false;
        }
    }

    for (SourceSlot& slot : sources_) {
        slot.source->reset();
        slot.pending = MarketEvent{};
        slot.last_ts = kNever;
        slot.has_pending = false;
        slot.exhausted = false;
    }

    clock_.reset(0);
    market_.clear();
    stats_ = EngineStats{};
    primed_ = false;
    return true;
}

// Both clocks, compiled on every build.
//
// The second one is what makes "a strategy cannot tell whether it is being
// backtested or trading live" checkable rather than asserted: if the event
// loop ever grows something that only a rewindable clock can do, this line
// stops compiling, here, rather than in six months against a live venue.
template class EngineT<SimulatedClock>;
template class EngineT<SystemClock>;

}  // namespace marketscan
