// The body shared by the v1 and v2 reference plugins.
//
// One file of logic, exported twice: once declaring ABI v1 and filling the nine
// slots that version had, once declaring v2 and filling all ten. That is what
// makes "a v1 plugin behaves identically" an assertion rather than an argument
// -- the two plugins cannot behave differently, because there is only one
// behaviour, and any difference the test measures is therefore attributable to
// the version branch and to nothing else.
//
// It submits orders, deliberately. A reference plugin that only counted
// callbacks would give the host nothing to reject, and the test asserting that a
// v1 plugin receives no status events would pass because none were produced --
// which is not the same claim at all, and is exactly the kind of vacuous pass
// this session spent its first part removing.

#ifndef MARKETSCAN_TEST_PLUGINS_REFERENCE_BODY_HPP
#define MARKETSCAN_TEST_PLUGINS_REFERENCE_BODY_HPP

#include <cstdint>
#include <cstring>
#include <new>

#include "marketscan/abi/plugin_abi.h"

namespace reference_body {

struct State {
    const MS_HostContext* host = nullptr;

    double bars = 0.0;
    double quotes = 0.0;
    double trades = 0.0;
    double fills = 0.0;
    double started = 0.0;
    double stopped = 0.0;
    double submitted = 0.0;

    /// Incremented only by the v2 export. The v1 export has no slot to call, so
    /// this stays at zero there -- and if the loader ever stopped trimming the
    /// vtable, the host would call into slot ten of a nine-slot table, which is
    /// memory corruption rather than a wrong count. This exists so the failure
    /// is a number somebody can read.
    double order_status = 0.0;

    std::uint32_t bar_index = 0;
};

inline void destroy(void* self) noexcept {
    delete static_cast<State*>(self);
}

inline void on_start(void* self, const MS_HostContext* host) noexcept {
    auto* state = static_cast<State*>(self);
    state->host = host;
    state->started += 1.0;
}

inline void on_bar(void* self, const MS_Bar* bar) noexcept {
    auto* state = static_cast<State*>(self);
    state->bars += 1.0;
    if (bar == nullptr || state->host == nullptr || state->host->submit_order == nullptr) {
        return;
    }

    // One small order every twentieth bar. Small enough that cash never binds,
    // frequent enough that a rejection probability has something to act on, and
    // alternating side so the position does not run away.
    if (++state->bar_index % 20 != 0) {
        return;
    }
    MS_OrderRequest request{};
    request.symbol = bar->symbol;
    request.side = (state->bar_index / 20) % 2 == 0 ? MS_SIDE_BUY : MS_SIDE_SELL;
    request.type = MS_ORD_MARKET;
    request.tif = MS_TIF_IOC;
    request.quantity = 1.0;
    request.client_order_id = state->bar_index;
    state->host->submit_order(state->host->host, &request);
    state->submitted += 1.0;
}

inline void on_quote(void* self, const MS_Quote*) noexcept {
    static_cast<State*>(self)->quotes += 1.0;
}

inline void on_trade(void* self, const MS_Trade*) noexcept {
    static_cast<State*>(self)->trades += 1.0;
}

inline void on_fill(void* self, const MS_Fill*) noexcept {
    static_cast<State*>(self)->fills += 1.0;
}

inline void on_order_status(void* self, const MS_OrderStatus* status) noexcept {
    auto* state = static_cast<State*>(self);
    if (status != nullptr) {
        state->order_status += 1.0;
    }
}

inline void on_stop(void* self) noexcept {
    static_cast<State*>(self)->stopped += 1.0;
}

inline void set_param(void*, const char*, double) noexcept {}

inline double get_param(void* self, const char* name) noexcept {
    const auto* state = static_cast<const State*>(self);
    if (name == nullptr) {
        return 0.0;
    }
    if (std::strcmp(name, "bars") == 0) {
        return state->bars;
    }
    if (std::strcmp(name, "quotes") == 0) {
        return state->quotes;
    }
    if (std::strcmp(name, "trades") == 0) {
        return state->trades;
    }
    if (std::strcmp(name, "fills") == 0) {
        return state->fills;
    }
    if (std::strcmp(name, "started") == 0) {
        return state->started;
    }
    if (std::strcmp(name, "stopped") == 0) {
        return state->stopped;
    }
    if (std::strcmp(name, "submitted") == 0) {
        return state->submitted;
    }
    if (std::strcmp(name, "order_status") == 0) {
        return state->order_status;
    }
    return 0.0;
}

/// Fills the nine slots ABI v1 had, and not one byte more.
///
/// Written slot by slot rather than by assigning a whole struct: assigning an
/// MS_StrategyVTable would write the v2 struct's full size, which is precisely
/// the overrun abi_compat.hpp refuses in the other direction and would make this
/// fixture a lie about what a v1 plugin does.
inline void fill_v1_slots(MS_StrategyVTable* out) noexcept {
    out->destroy = &destroy;
    out->on_start = &on_start;
    out->on_bar = &on_bar;
    out->on_quote = &on_quote;
    out->on_trade = &on_trade;
    out->on_fill = &on_fill;
    out->on_stop = &on_stop;
    out->set_param = &set_param;
    out->get_param = &get_param;
}

}  // namespace reference_body

#endif  // MARKETSCAN_TEST_PLUGINS_REFERENCE_BODY_HPP
