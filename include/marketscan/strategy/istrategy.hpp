#ifndef MARKETSCAN_STRATEGY_ISTRATEGY_HPP
#define MARKETSCAN_STRATEGY_ISTRATEGY_HPP

/*
 * The theory author's header.
 *
 * Everything needed to write a strategy is here, and nothing here links
 * against the host. This file depends on plugin_abi.h, core/session_calendar.hpp
 * and the standard library, and on nothing else in MarketScan -- which is what
 * lets a theory be compiled on its own, against a copy of three headers, and
 * dropped into theories/ as a DLL. All three are headers; none of them has a
 * .cpp behind it.
 *
 * Write a strategy by deriving from IStrategy and invoking the export macro:
 *
 *     class MyTheory : public marketscan::IStrategy {
 *         void on_bar(const MS_Bar& bar) override { ... }
 *     };
 *     MARKETSCAN_EXPORT_STRATEGY(MyTheory, "my_theory", "My Theory", "Me")
 *
 * The macro generates the three C exports and a vtable of shims, each of which
 * catches everything. An exception must never unwind across the C boundary.
 */

#include <cstring>
#include <new>
#include <string>
#include <type_traits>
#include <vector>

#include "marketscan/abi/plugin_abi.h"
#include "marketscan/core/session_calendar.hpp"

namespace marketscan {

// ---------------------------------------------------------------------------
// Parameter helpers
// ---------------------------------------------------------------------------

/// Describes a tunable the GUI renders a slider for.
[[nodiscard]] constexpr MS_ParamDesc param_double(const char* name,
                                                  const char* description,
                                                  double min_value,
                                                  double max_value,
                                                  double default_value) noexcept {
    return MS_ParamDesc{
        name, description, MS_PARAM_DOUBLE, min_value, max_value, default_value, nullptr, 0};
}

[[nodiscard]] constexpr MS_ParamDesc param_int(const char* name,
                                               const char* description,
                                               double min_value,
                                               double max_value,
                                               double default_value) noexcept {
    return MS_ParamDesc{
        name, description, MS_PARAM_INT, min_value, max_value, default_value, nullptr, 0};
}

[[nodiscard]] constexpr MS_ParamDesc
param_bool(const char* name, const char* description, bool default_value) noexcept {
    return MS_ParamDesc{
        name, description, MS_PARAM_BOOL, 0.0, 1.0, default_value ? 1.0 : 0.0, nullptr, 0};
}

[[nodiscard]] constexpr MS_ParamDesc param_enum(const char* name,
                                                const char* description,
                                                const char* const* labels,
                                                uint32_t label_count,
                                                double default_index) noexcept {
    return MS_ParamDesc{name,
                        description,
                        MS_PARAM_ENUM,
                        0.0,
                        label_count == 0 ? 0.0 : static_cast<double>(label_count - 1),
                        default_index,
                        labels,
                        label_count};
}

// ---------------------------------------------------------------------------
// IStrategy
// ---------------------------------------------------------------------------

/// Base class for a theory.
///
/// Override the callbacks that matter and ignore the rest. Two rules make a
/// strategy replayable, and both are enforced by having no other option here:
///
///   - Time comes from now(), never from the system clock.
///   - Randomness comes from random_uniform(), never from rand() or an
///     unseeded generator.
///
/// A strategy that reads the wall clock produces a backtest that cannot be
/// reproduced, and an unreproducible backtest is not evidence of anything.
class IStrategy {
public:
    IStrategy() = default;
    IStrategy(const IStrategy&) = delete;
    IStrategy& operator=(const IStrategy&) = delete;
    IStrategy(IStrategy&&) = delete;
    IStrategy& operator=(IStrategy&&) = delete;
    virtual ~IStrategy() = default;

    // --- Lifecycle ------------------------------------------------------

    /// Called once before any event. Call the base implementation first if you
    /// override it, or host() will be null.
    virtual void on_start() {}

    virtual void on_bar(const MS_Bar&) {}

    virtual void on_quote(const MS_Quote&) {}

    virtual void on_trade(const MS_Trade&) {}

    virtual void on_fill(const MS_Fill&) {}

    /// Called when a trading session ends, before the first event of the next
    /// one, and once more after the final event of the run.
    ///
    /// `session_end` is the instant the closed session ended, exclusive: local
    /// midnight at the exchange, which is not UTC midnight and is not a fixed
    /// offset from it. See core/session_calendar.hpp.
    ///
    /// This is where a strategy that thinks in days rather than in ticks does
    /// its work. Everything it was shown during the session has been shown by
    /// the time this is called, and nothing from the next session has.
    ///
    /// The engine does not know this callback exists. Session boundaries are
    /// detected here, in the shim layer compiled into the plugin, from the
    /// timestamps the plugin is already being given -- which is why this is
    /// possible without touching a frozen ABI. A plugin built against an older
    /// copy of this header simply never has it called.
    virtual void on_session_close(MS_Nanos session_end) { (void)session_end; }

    /// Called once after the last event, before the instance is destroyed.
    virtual void on_stop() {}

    // --- Parameters -----------------------------------------------------
    //
    // Bind a member in the constructor and the base class handles the rest:
    //
    //     bind_param("fast", &fast_period_);
    //
    // Override set_param/get_param only for something a binding cannot
    // express, such as a value that has to invalidate cached state.

    virtual void set_param(const char* name, double value);

    [[nodiscard]] virtual double get_param(const char* name) const;

    // --- Host plumbing --------------------------------------------------
    //
    // Called by the generated shims. Not part of the author-facing surface.

    void attach_host(const MS_HostContext* host) noexcept { host_ = host; }

    /// Books a fill against the order it belongs to, before the strategy is
    /// told about it.
    ///
    /// Called by the generated shims. Not for strategies to call.
    void note_fill(const MS_Fill& fill) noexcept {
        for (std::size_t i = 0; i < pending_.size(); ++i) {
            if (pending_[i].id != fill.order_id) {
                continue;
            }
            pending_[i].filled += fill.quantity;

            // Floating quantities, so "fully filled" cannot be an equality
            // test. The tolerance is far below any tradeable size and far
            // above the rounding error of summing a few fills.
            if (pending_[i].filled >= pending_[i].quantity - 1e-9) {
                pending_.erase(pending_.begin() + static_cast<std::ptrdiff_t>(i));
            }
            return;
        }
        if (submitting_) {
            // Delivered from inside submit_order, before the order it belongs
            // to could be recorded. submit() applies it as soon as it has the
            // id. Only the quantity is kept: there is exactly one order in
            // flight at this point, the one being submitted.
            reentrant_filled_ += fill.quantity;
            return;
        }

        // A fill for an order this side is not tracking. Reachable after
        // forget_order() or a timeout, and harmless: the ledger is the
        // authority on what was traded, and this is only bookkeeping about
        // what is still expected.
    }

    /// Notes the timestamp of an event about to be delivered, and closes the
    /// previous session first if this one belongs to a new one.
    ///
    /// Called by the generated shims before every callback that carries a
    /// timestamp. Not for strategies to call.
    void advance_session(MS_Nanos ts) noexcept(false) {
        const std::int64_t day = session_day(session_zone_, ts);
        if (!session_started_) {
            session_started_ = true;
            session_day_ = day;
            return;
        }
        if (day == session_day_) {
            return;
        }
        // The end of the session that is closing, not the start of the one
        // opening: with a weekend or a holiday between them those are days
        // apart, and a strategy stamping its work with the wrong one would
        // date Friday's decisions to Monday.
        const MS_Nanos ended = session_start_of_day(session_zone_, session_day_ + 1);
        session_day_ = day;
        on_session_close(ended);
    }

    /// Closes the session still open when the run ends.
    ///
    /// Without this the final session is the one session a cross-sectional
    /// strategy never acts on, because nothing after it ever arrives to roll
    /// the date. Called by the stop shim, before on_stop.
    void close_final_session() noexcept(false) {
        if (!session_started_) {
            return;
        }
        session_started_ = false;
        on_session_close(session_start_of_day(session_zone_, session_day_ + 1));
    }

    /// Logs an exception that a shim caught. Never throws, and tolerates a
    /// host that was never attached.
    void report_exception(const char* where) noexcept;

protected:
    [[nodiscard]] const MS_HostContext* host() const noexcept { return host_; }

    /// Which wall clock session boundaries are measured against.
    ///
    /// Set it in the constructor if the instrument does not trade in New York.
    /// It cannot be negotiated with the host: the ABI is frozen at v1 and has
    /// nowhere to carry it, so host and plugin agree by both defaulting to the
    /// exchange the data is from. Changing this without changing the host's
    /// own calendar makes the two disagree about what day it is.
    void set_session_zone(ExchangeZone zone) noexcept { session_zone_ = zone; }

    [[nodiscard]] ExchangeZone session_zone() const noexcept { return session_zone_; }

    [[nodiscard]] bool has_host() const noexcept {
        return host_ != nullptr && host_->host != nullptr;
    }

    // --- Services -------------------------------------------------------

    /// The only clock a strategy may read.
    [[nodiscard]] MS_Nanos now() const noexcept {
        return has_host() && host_->now != nullptr ? host_->now(host_->host) : 0;
    }

    /// Uniform on [0, 1), from the run's seeded generator.
    [[nodiscard]] double random_uniform() const noexcept {
        return has_host() && host_->random_uniform != nullptr ? host_->random_uniform(host_->host)
                                                              : 0.0;
    }

    [[nodiscard]] double position(MS_SymbolId symbol) const noexcept {
        return has_host() && host_->position != nullptr ? host_->position(host_->host, symbol)
                                                        : 0.0;
    }

    [[nodiscard]] MS_Money cash() const noexcept {
        return has_host() && host_->cash != nullptr ? host_->cash(host_->host) : 0;
    }

    [[nodiscard]] MS_Money equity() const noexcept {
        return has_host() && host_->equity != nullptr ? host_->equity(host_->host) : 0;
    }

    [[nodiscard]] MS_SymbolId symbol_id(const char* ticker) const noexcept {
        return has_host() && host_->symbol_id != nullptr ? host_->symbol_id(host_->host, ticker)
                                                         : 0;
    }

    [[nodiscard]] const char* symbol_name(MS_SymbolId symbol) const noexcept {
        return has_host() && host_->symbol_name != nullptr ? host_->symbol_name(host_->host, symbol)
                                                           : "";
    }

    [[nodiscard]] uint32_t symbol_count() const noexcept {
        return has_host() && host_->symbol_count != nullptr ? host_->symbol_count(host_->host) : 0;
    }

    void log(MS_LogLevel level, const char* message) const noexcept {
        if (has_host() && host_->log != nullptr && message != nullptr) {
            host_->log(host_->host, level, message);
        }
    }

    void log_info(const char* message) const noexcept { log(MS_LOG_INFO, message); }

    void log_warn(const char* message) const noexcept { log(MS_LOG_WARN, message); }

    /// Publishes a named scalar. The GUI plots it without being told to.
    void publish_metric(const char* name, double value) const noexcept {
        if (has_host() && host_->publish_metric != nullptr && name != nullptr) {
            host_->publish_metric(host_->host, name, value);
        }
    }

    // --- Orders ---------------------------------------------------------

    /// Returns the engine order id, or 0 if the order was refused. A refusal
    /// is not an exception and not a crash; check the id if you care.
    ///
    /// The order is recorded here as outstanding, and stops being outstanding
    /// when fills for it add up to its quantity. That bookkeeping is what
    /// makes has_pending_order() answerable.
    uint64_t submit(const MS_OrderRequest& request) noexcept {
        if (!has_host() || host_->submit_order == nullptr) {
            return 0;
        }

        // A fill can be delivered from inside submit_order, before it has
        // returned an id. That is exactly what a zero-latency simulator does,
        // and it is the whole reason this is not two lines: the fill arrives
        // before there is anything here to book it against, so it is caught
        // in flight and applied once the id is known.
        //
        // Live it never happens, which is precisely why it would be missed.
        submitting_ = true;
        reentrant_filled_ = 0.0;
        const uint64_t id = host_->submit_order(host_->host, &request);
        submitting_ = false;

        if (id == 0) {
            return 0;
        }
        if (reentrant_filled_ >= request.quantity - 1e-9) {
            // Filled in full before it was ever outstanding. Recording it now
            // would create an entry nothing will ever come along to retire.
            return id;
        }

        expire_stale_orders();
        pending_.push_back(PendingOrder{
            id, request.symbol, request.side, request.quantity, reentrant_filled_, now(), false});
        return id;
    }

    uint64_t submit_market(MS_SymbolId symbol,
                           MS_Side side,
                           double quantity,
                           uint64_t client_order_id = 0) noexcept {
        MS_OrderRequest request{};
        request.symbol = symbol;
        request.side = side;
        request.type = MS_ORD_MARKET;
        request.tif = MS_TIF_DAY;
        request.quantity = quantity;
        request.client_order_id = client_order_id;
        return submit(request);
    }

    uint64_t submit_limit(MS_SymbolId symbol,
                          MS_Side side,
                          double quantity,
                          double limit_price,
                          MS_TimeInForce tif = MS_TIF_DAY,
                          uint64_t client_order_id = 0) noexcept {
        MS_OrderRequest request{};
        request.symbol = symbol;
        request.side = side;
        request.type = MS_ORD_LIMIT;
        request.tif = tif;
        request.quantity = quantity;
        request.limit_price = limit_price;
        request.client_order_id = client_order_id;
        return submit(request);
    }

    /// Asks for the remainder of an order to be cancelled.
    ///
    /// Fills already received stand; a cancel reaches what has not traded and
    /// nothing else. The request may also simply lose: if the order fills
    /// before the cancel arrives, the cancel does nothing at all.
    ///
    /// The order stays outstanding here afterwards, flagged, because there is
    /// no way to find out which happened -- see "What this cannot know" below.
    /// A strategy that is willing to assume the cancel won should call
    /// forget_order().
    void cancel(uint64_t order_id) noexcept {
        for (PendingOrder& order : pending_) {
            if (order.id == order_id) {
                order.cancel_requested = true;
            }
        }
        if (has_host() && host_->cancel_order != nullptr) {
            host_->cancel_order(host_->host, order_id);
        }
    }

    // --- Outstanding orders ---------------------------------------------
    //
    // Live, an order is not a function call. It is sent, acknowledged, and
    // filled some time later -- or partly filled, or not filled at all. The
    // ABI has always had that shape: submit returns an id and on_fill is a
    // separate callback. But a fill simulator that answers instantly means no
    // strategy written against this engine has ever *experienced* a pending
    // order, and every one of them is silently relying on that.
    //
    // These answer the questions such a strategy needs to ask. They are
    // maintained here, in the plugin, from the two things it already sees:
    // what submit() returned, and what on_fill() delivered.
    //
    // WHAT THIS CANNOT KNOW. A cancellation, a rejection after acceptance, or
    // a DAY expiry produces no fill, so nothing reaches this side of the
    // boundary to say the order is dead. Until the ABI grows a terminal-status
    // callback -- v2, deliberately not now -- an order that ends without
    // filling stays outstanding here forever. Two ways out, both explicit:
    // forget_order() when the strategy knows, and set_order_timeout() when it
    // would rather assume after a while. The bias is deliberate: over-
    // reporting a pending order makes a strategy too cautious, and under-
    // reporting one makes it submit twice.

    /// How many orders have been submitted and not yet fully filled.
    [[nodiscard]] uint32_t outstanding_orders() const noexcept {
        return static_cast<uint32_t>(pending_.size());
    }

    /// Whether anything is outstanding for one symbol.
    ///
    /// The guard against the most common live-trading bug there is: acting on
    /// a signal, seeing no fill yet because the order is still in flight,
    /// reading the position as unchanged, and sending the order again.
    [[nodiscard]] bool has_pending_order(MS_SymbolId symbol) const noexcept {
        for (const PendingOrder& order : pending_) {
            if (order.symbol == symbol) {
                return true;
            }
        }
        return false;
    }

    /// Signed quantity working for a symbol: positive to buy, negative to
    /// sell. What a position *will* be if everything outstanding fills.
    [[nodiscard]] double pending_quantity(MS_SymbolId symbol) const noexcept {
        double total = 0.0;
        for (const PendingOrder& order : pending_) {
            if (order.symbol == symbol) {
                const double left = order.quantity - order.filled;
                total += (order.side == MS_SIDE_BUY ? 1.0 : -1.0) * (left > 0.0 ? left : 0.0);
            }
        }
        return total;
    }

    /// How much of one order has filled so far. Zero for an unknown id.
    [[nodiscard]] double filled_quantity(uint64_t order_id) const noexcept {
        const PendingOrder* order = find_pending(order_id);
        return order != nullptr ? order->filled : 0.0;
    }

    /// How much of one order has not filled. Zero for an unknown id, which is
    /// also the right answer for an order that has finished.
    [[nodiscard]] double remaining_quantity(uint64_t order_id) const noexcept {
        const PendingOrder* order = find_pending(order_id);
        if (order == nullptr) {
            return 0.0;
        }
        const double left = order->quantity - order->filled;
        return left > 0.0 ? left : 0.0;
    }

    /// Nanoseconds since the order was submitted. Zero for an unknown id.
    [[nodiscard]] MS_Nanos order_age(uint64_t order_id) const noexcept {
        const PendingOrder* order = find_pending(order_id);
        if (order == nullptr) {
            return 0;
        }
        const MS_Nanos elapsed = now() - order->submitted_ts;
        return elapsed > 0 ? elapsed : 0;
    }

    /// Whether a cancel has been asked for and not yet resolved.
    [[nodiscard]] bool cancel_pending(uint64_t order_id) const noexcept {
        const PendingOrder* order = find_pending(order_id);
        return order != nullptr && order->cancel_requested;
    }

    /// Stops tracking one order. For a strategy that has concluded from
    /// something outside this bookkeeping -- a position that matches, a
    /// cancel it is confident in -- that the order is done.
    void forget_order(uint64_t order_id) noexcept {
        for (std::size_t i = 0; i < pending_.size(); ++i) {
            if (pending_[i].id == order_id) {
                pending_.erase(pending_.begin() + static_cast<std::ptrdiff_t>(i));
                return;
            }
        }
    }

    /// Stops tracking an order once it has been outstanding this long.
    ///
    /// Zero, the default, never gives up -- which is correct and useless in a
    /// run where anything is ever cancelled. A timeout is an assumption, not a
    /// fact, so it has to be asked for. abandoned_orders() counts how often
    /// the assumption was made.
    void set_order_timeout(MS_Nanos timeout) noexcept { order_timeout_ = timeout; }

    [[nodiscard]] uint64_t abandoned_orders() const noexcept { return abandoned_; }

    // --- Parameter binding ----------------------------------------------

    void bind_param(const char* name, double* slot);
    void bind_param(const char* name, int* slot);
    void bind_param(const char* name, bool* slot);

private:
    enum class Slot : unsigned char { Double, Int, Bool };

    struct Binding {
        const char* name;
        void* slot;
        Slot kind;
    };

    [[nodiscard]] const Binding* find_binding(const char* name) const noexcept;

    /// One order submitted and not yet accounted for.
    struct PendingOrder {
        uint64_t id;
        MS_SymbolId symbol;
        MS_Side side;
        double quantity;
        double filled;
        MS_Nanos submitted_ts;
        bool cancel_requested;
    };

    [[nodiscard]] const PendingOrder* find_pending(uint64_t order_id) const noexcept {
        for (const PendingOrder& order : pending_) {
            if (order.id == order_id) {
                return &order;
            }
        }
        return nullptr;
    }

    /// Drops anything that has been outstanding longer than the timeout.
    ///
    /// Run on submission rather than on a timer, because a strategy that is
    /// not trading is not accumulating orders either, and the per-event path
    /// is not the place to walk a list looking for nothing.
    void expire_stale_orders() noexcept {
        if (order_timeout_ <= 0 || pending_.empty()) {
            return;
        }
        const MS_Nanos cutoff = now() - order_timeout_;
        for (std::size_t i = pending_.size(); i > 0; --i) {
            if (pending_[i - 1].submitted_ts <= cutoff) {
                pending_.erase(pending_.begin() + static_cast<std::ptrdiff_t>(i - 1));
                ++abandoned_;
            }
        }
    }

    const MS_HostContext* host_ = nullptr;
    std::vector<Binding> bindings_;

    std::vector<PendingOrder> pending_;
    MS_Nanos order_timeout_ = 0;
    uint64_t abandoned_ = 0;

    /// Set while submit_order is on the stack, so a fill delivered from
    /// inside it can be recognised as belonging to an order with no id yet.
    bool submitting_ = false;
    double reentrant_filled_ = 0.0;

    ExchangeZone session_zone_ = ExchangeZone::UsEastern;
    std::int64_t session_day_ = 0;
    bool session_started_ = false;
};

// ---------------------------------------------------------------------------
// Inline implementation
// ---------------------------------------------------------------------------

inline void IStrategy::bind_param(const char* name, double* slot) {
    bindings_.push_back(Binding{name, slot, Slot::Double});
}

inline void IStrategy::bind_param(const char* name, int* slot) {
    bindings_.push_back(Binding{name, slot, Slot::Int});
}

inline void IStrategy::bind_param(const char* name, bool* slot) {
    bindings_.push_back(Binding{name, slot, Slot::Bool});
}

inline const IStrategy::Binding* IStrategy::find_binding(const char* name) const noexcept {
    if (name == nullptr) {
        return nullptr;
    }
    for (const Binding& binding : bindings_) {
        if (binding.name != nullptr && std::strcmp(binding.name, name) == 0) {
            return &binding;
        }
    }
    return nullptr;
}

inline void IStrategy::set_param(const char* name, double value) {
    const Binding* binding = find_binding(name);
    if (binding == nullptr) {
        return;
    }
    switch (binding->kind) {
        case Slot::Double:
            *static_cast<double*>(binding->slot) = value;
            break;
        case Slot::Int:
            // Truncates toward zero on purpose: a slider handing over 9.99999
            // for an integer period should mean 9, the same way every time,
            // rather than 10 on one machine and 9 on another.
            *static_cast<int*>(binding->slot) = static_cast<int>(value);
            break;
        case Slot::Bool:
            *static_cast<bool*>(binding->slot) = value != 0.0;
            break;
    }
}

inline double IStrategy::get_param(const char* name) const {
    const Binding* binding = find_binding(name);
    if (binding == nullptr) {
        return 0.0;
    }
    switch (binding->kind) {
        case Slot::Double:
            return *static_cast<const double*>(binding->slot);
        case Slot::Int:
            return static_cast<double>(*static_cast<const int*>(binding->slot));
        case Slot::Bool:
            return *static_cast<const bool*>(binding->slot) ? 1.0 : 0.0;
    }
    return 0.0;
}

inline void IStrategy::report_exception(const char* where) noexcept {
    // Deliberately defensive: this runs while something has already gone
    // wrong, and it must not be the thing that takes the host down.
    try {
        if (host_ != nullptr && host_->log != nullptr && host_->host != nullptr) {
            std::string message = "strategy threw in ";
            message += where != nullptr ? where : "an unknown callback";
            message += "; the exception was swallowed at the plugin boundary";
            host_->log(host_->host, MS_LOG_ERROR, message.c_str());
        }
    } catch (...) {
        // Nothing left to try.
    }
}

// ---------------------------------------------------------------------------
// CrossSectionalStrategy
// ---------------------------------------------------------------------------

/// Every symbol that traded in one session, as of that session's close.
///
/// Bars are in symbol-id order, which is first-seen order in the run's symbol
/// table and therefore the same on every replay. That is not decoration: a
/// strategy that ranks its universe and acts on the top decile gets a
/// different decile if the order it was handed depends on a hash.
struct CrossSection {
    /// Exclusive end of the session these bars belong to.
    MS_Nanos session_end = 0;

    /// Ascending by symbol id. Valid only for the duration of the callback.
    const MS_Bar* bars = nullptr;
    uint32_t count = 0;

    [[nodiscard]] const MS_Bar* begin() const noexcept { return bars; }

    [[nodiscard]] const MS_Bar* end() const noexcept { return bars + count; }

    [[nodiscard]] bool empty() const noexcept { return count == 0; }

    /// The bar for one symbol, or null if it did not trade this session.
    [[nodiscard]] const MS_Bar* find(MS_SymbolId symbol) const noexcept {
        uint32_t low = 0;
        uint32_t high = count;
        while (low < high) {
            const uint32_t mid = low + (high - low) / 2;
            if (bars[mid].symbol < symbol) {
                low = mid + 1;
            } else {
                high = mid;
            }
        }
        return low < count && bars[low].symbol == symbol ? &bars[low] : nullptr;
    }
};

/// Base class for a strategy that thinks about every symbol at once.
///
/// The per-symbol on_bar dispatch cannot express "rank the universe and buy
/// the top decile", because at the moment any one bar arrives the rest of the
/// session has not been shown yet. This buffers the session and hands the
/// whole cross-section over at its close.
///
///     class Deciles final : public marketscan::CrossSectionalStrategy {
///         void on_cross_section(const marketscan::CrossSection& day) override {
///             for (const MS_Bar& bar : day) { ... }
///         }
///     };
///
/// The last bar seen for a symbol in a session is the one that represents it,
/// which for daily data is the only one and for intraday data is the close.
/// Smallest trade a target book will ask for.
///
/// Must match `marketscan::sizing::kMinimumTrade`; `decile_parity.hpp`
/// static_asserts it, being the one place both headers are visible.
inline constexpr double kMinimumTargetTrade = 1e-9;

class CrossSectionalStrategy : public IStrategy {
public:
    /// Sealed. Override on_session_bar to see bars as they arrive as well.
    void on_bar(const MS_Bar& bar) final {
        const auto index = static_cast<std::size_t>(bar.symbol);
        if (index >= latest_.size()) {
            // Grows to the symbol count and then never again. A strategy that
            // wants no allocation at all after the first session can call
            // reserve_symbols() in on_start.
            grow(index + 1);
        }
        latest_[index] = bar;
        present_[index] = 1;
        on_session_bar(bar);

        // After the strategy has seen the bar, and this is the whole of the fix
        // for the lookahead: a target set from session N's closes becomes an
        // order here, against the first price the symbol printed afterwards.
        release_target(bar.symbol);
    }

    /// Sealed. Override on_cross_section.
    void on_session_close(MS_Nanos session_end) final {
        // Anything still outstanding never got a print and is dropped rather than
        // carried: a target from two sessions ago, filled now, is a decision
        // acting on information it no longer has. Counted, because a strategy
        // whose targets routinely expire is one whose universe is leaving under
        // it -- which is a finding, not a detail.
        abandon_targets();

        section_.clear();
        for (std::size_t i = 0; i < present_.size(); ++i) {
            if (present_[i] != 0) {
                section_.push_back(latest_[i]);
                present_[i] = 0;
            }
        }

        CrossSection view;
        view.session_end = session_end;
        view.bars = section_.empty() ? nullptr : section_.data();
        view.count = static_cast<uint32_t>(section_.size());

        // Called even when nothing traded. A strategy holding positions has
        // decisions to make on a day the tape was empty, and swallowing the
        // callback would hide that day from it entirely.
        on_cross_section(view);
    }

protected:
    /// The session's bars, one per symbol, in symbol-id order.
    virtual void on_cross_section(const CrossSection& section) = 0;

    /// Individual bars, as they arrive. Empty by default.
    virtual void on_session_bar(const MS_Bar&) {}

    // --- The target book ------------------------------------------------
    //
    // A cross-sectional decision is taken from a session's closing
    // cross-section, and the earliest price it can honestly trade at is the next
    // one the instrument prints. So `on_cross_section` states a target book and
    // this turns each name's target into an order when that name next prints.
    //
    // WHY NOT JUST SUBMIT FROM on_cross_section
    //
    // Because the order would fill at the price the decision was made from, and
    // that is free money that does not exist. The engine applies a bar to its
    // book before dispatching it, so that a strategy reacting to a print and the
    // fill model pricing its order see the same market -- right for a strategy
    // reacting to a print, wrong here, because `advance_session` fires
    // `on_session_close(N)` from inside the dispatch of the first bar of session
    // N+1. Submitting there prices against session N's book for every symbol
    // except the one whose bar rolled the session, which prices against N+1's.
    //
    // Measured before this existed, on both committed parity fixtures: **every
    // strategy fill had `decision_ts == price_ts`** -- 56 of 56 and 55 of 55.
    // See `StrategyInstance::audit_instants` and the lookahead invariant in
    // `docs/EXECUTION.md`.
    //
    // A strategy may still call `submit_market` from `on_cross_section`. Nothing
    // stops it, and nothing should pretend to: the ABI is frozen and the host
    // cannot tell such an order from one submitted out of `on_bar`. What it will
    // get is a fill the audit reports as simultaneous, which is the point of
    // recording the triple rather than policing it.

    /// Asks for `shares` of `symbol` as of the next price it prints.
    ///
    /// Not an order. Replaces any target already outstanding for that symbol,
    /// because two targets for one name is not a thing a book can mean.
    void set_target(MS_SymbolId symbol, double shares) {
        const auto index = static_cast<std::size_t>(symbol);
        if (index >= target_.size()) {
            grow(index + 1);
        }
        target_[index] = shares;
        has_target_[index] = 1;
    }

    /// Asks to be flat in `symbol` as of its next print.
    void set_flat(MS_SymbolId symbol) { set_target(symbol, 0.0); }

    /// Targets set and not yet released.
    [[nodiscard]] uint32_t targets_outstanding() const noexcept {
        uint32_t count = 0;
        for (unsigned char flag : has_target_) {
            count += flag != 0 ? 1u : 0u;
        }
        return count;
    }

    /// Targets that reached a price and became an order or a no-op.
    [[nodiscard]] uint64_t targets_released() const noexcept { return released_; }

    /// Targets dropped because the symbol never printed again.
    [[nodiscard]] uint64_t targets_abandoned() const noexcept { return abandoned_; }

    /// Sizes the buffers up front, so no session allocates.
    void reserve_symbols(uint32_t count) {
        grow(static_cast<std::size_t>(count) + 1);
        section_.reserve(static_cast<std::size_t>(count) + 1);
    }

private:
    void grow(std::size_t size) {
        latest_.resize(size);
        present_.resize(size, 0);
        target_.resize(size, 0.0);
        has_target_.resize(size, 0);
    }

    void release_target(MS_SymbolId symbol) {
        const auto index = static_cast<std::size_t>(symbol);
        if (index >= has_target_.size() || has_target_[index] == 0) {
            return;
        }
        const double target = target_[index];
        has_target_[index] = 0;
        ++released_;

        // Against the position as it stands now, not as it stood when the target
        // was set. For one symbol those are the same number -- only this symbol's
        // own fills move it -- but reading it now is what makes that true rather
        // than assumed.
        const double delta = target - position(symbol);
        if (!(delta > kMinimumTargetTrade) && !(delta < -kMinimumTargetTrade)) {
            return;
        }
        if (delta > 0.0) {
            submit_market(symbol, MS_SIDE_BUY, delta);
        } else {
            submit_market(symbol, MS_SIDE_SELL, -delta);
        }
    }

    void abandon_targets() {
        for (std::size_t i = 0; i < has_target_.size(); ++i) {
            if (has_target_[i] != 0) {
                has_target_[i] = 0;
                ++abandoned_;
            }
        }
    }

    std::vector<MS_Bar> latest_;
    std::vector<unsigned char> present_;
    std::vector<MS_Bar> section_;
    std::vector<double> target_;
    std::vector<unsigned char> has_target_;
    uint64_t released_ = 0;
    uint64_t abandoned_ = 0;
};

// ---------------------------------------------------------------------------
// Descriptor detection
//
// A theory supplies its optional metadata as static members. These concepts
// let the export macro pick up whichever ones exist without the author having
// to declare the ones they do not need.
// ---------------------------------------------------------------------------

namespace detail {

template<typename T>
concept HasDescription = requires {
    { T::kDescription } -> std::convertible_to<const char*>;
};

template<typename T>
concept HasParams = requires {
    { T::kParams };
    { T::kParamCount } -> std::convertible_to<uint32_t>;
};

template<typename T>
concept HasRequiredSymbols = requires {
    { T::kRequiredSymbols };
    { T::kRequiredSymbolCount } -> std::convertible_to<uint32_t>;
};

template<typename T>
[[nodiscard]] MS_StrategyDesc
make_descriptor(const char* id, const char* display_name, const char* author) noexcept {
    MS_StrategyDesc desc{};
    desc.abi_version = MS_ABI_VERSION;
    desc.id = id;
    desc.display_name = display_name;
    desc.author = author;
    desc.description = "";
    desc.params = nullptr;
    desc.param_count = 0;
    desc.required_symbols = nullptr;
    desc.required_symbol_count = 0;

    if constexpr (HasDescription<T>) {
        desc.description = T::kDescription;
    }
    if constexpr (HasParams<T>) {
        desc.params = T::kParams;
        desc.param_count = static_cast<uint32_t>(T::kParamCount);
    }
    if constexpr (HasRequiredSymbols<T>) {
        desc.required_symbols = T::kRequiredSymbols;
        desc.required_symbol_count = static_cast<uint32_t>(T::kRequiredSymbolCount);
    }
    return desc;
}

// --- vtable shims ------------------------------------------------------
//
// Every one of these catches everything. An exception crossing the C boundary
// is undefined behaviour, and on MSVC it is the kind that corrupts the stack
// rather than the kind that prints a message.
//
// They also carry the session clock. Each callback that has a timestamp offers
// it to advance_session first, which fires on_session_close when the date
// rolls. That bookkeeping lives here, on the plugin's side of the boundary,
// rather than in the host -- the host cannot call a method the frozen vtable
// has no slot for, and the timestamps needed to work it out are already
// crossing anyway.

template<typename T>
void shim_destroy(void* self) noexcept {
    try {
        delete static_cast<T*>(self);
    } catch (...) {
        // A destructor threw. There is nowhere to report it to, because the
        // object that held the host pointer is what just failed to die.
    }
}

template<typename T>
void shim_on_start(void* self, const MS_HostContext* host) noexcept {
    auto* instance = static_cast<T*>(self);
    try {
        instance->attach_host(host);
        instance->on_start();
    } catch (...) {
        instance->report_exception("on_start");
    }
}

template<typename T>
void shim_on_bar(void* self, const MS_Bar* bar) noexcept {
    auto* instance = static_cast<T*>(self);
    try {
        if (bar != nullptr) {
            instance->advance_session(bar->ts);
            instance->on_bar(*bar);
        }
    } catch (...) {
        instance->report_exception("on_bar");
    }
}

template<typename T>
void shim_on_quote(void* self, const MS_Quote* quote) noexcept {
    auto* instance = static_cast<T*>(self);
    try {
        if (quote != nullptr) {
            instance->advance_session(quote->ts);
            instance->on_quote(*quote);
        }
    } catch (...) {
        instance->report_exception("on_quote");
    }
}

template<typename T>
void shim_on_trade(void* self, const MS_Trade* trade) noexcept {
    auto* instance = static_cast<T*>(self);
    try {
        if (trade != nullptr) {
            instance->advance_session(trade->ts);
            instance->on_trade(*trade);
        }
    } catch (...) {
        instance->report_exception("on_trade");
    }
}

template<typename T>
void shim_on_fill(void* self, const MS_Fill* fill) noexcept {
    auto* instance = static_cast<T*>(self);
    try {
        if (fill != nullptr) {
            instance->advance_session(fill->ts);
            instance->note_fill(*fill);
            instance->on_fill(*fill);
        }
    } catch (...) {
        instance->report_exception("on_fill");
    }
}

template<typename T>
void shim_on_stop(void* self) noexcept {
    auto* instance = static_cast<T*>(self);

    // Separately guarded from on_stop. A strategy whose final cross-section
    // throws must still get its on_stop, or it never releases whatever it
    // opened in on_start.
    try {
        instance->close_final_session();
    } catch (...) {
        instance->report_exception("on_session_close");
    }
    try {
        instance->on_stop();
    } catch (...) {
        instance->report_exception("on_stop");
    }
}

template<typename T>
void shim_set_param(void* self, const char* name, double value) noexcept {
    auto* instance = static_cast<T*>(self);
    try {
        instance->set_param(name, value);
    } catch (...) {
        instance->report_exception("set_param");
    }
}

template<typename T>
double shim_get_param(void* self, const char* name) noexcept {
    auto* instance = static_cast<T*>(self);
    try {
        return instance->get_param(name);
    } catch (...) {
        instance->report_exception("get_param");
        return 0.0;
    }
}

template<typename T>
[[nodiscard]] MS_StrategyVTable vtable_for() noexcept {
    MS_StrategyVTable table{};
    table.destroy = &shim_destroy<T>;
    table.on_start = &shim_on_start<T>;
    table.on_bar = &shim_on_bar<T>;
    table.on_quote = &shim_on_quote<T>;
    table.on_trade = &shim_on_trade<T>;
    table.on_fill = &shim_on_fill<T>;
    table.on_stop = &shim_on_stop<T>;
    table.set_param = &shim_set_param<T>;
    table.get_param = &shim_get_param<T>;
    return table;
}

template<typename T>
[[nodiscard]] void* create_instance(MS_StrategyVTable* out_vtable) noexcept {
    static_assert(std::is_base_of_v<IStrategy, T>,
                  "a MarketScan theory must derive from marketscan::IStrategy");
    static_assert(std::is_default_constructible_v<T>,
                  "a MarketScan theory must be default constructible; take its "
                  "configuration through parameters instead");
    if (out_vtable == nullptr) {
        return nullptr;
    }
    try {
        T* instance = new T();
        *out_vtable = vtable_for<T>();
        return instance;
    } catch (...) {
        // The host checks for null and reports a clear error. Throwing across
        // the boundary instead would take the whole application with it.
        return nullptr;
    }
}

}  // namespace detail
}  // namespace marketscan

// ---------------------------------------------------------------------------
// The export macro
// ---------------------------------------------------------------------------

/// Generates the three C exports for `ClassName`.
///
/// Place it at file scope, outside any namespace:
///
///     MARKETSCAN_EXPORT_STRATEGY(MyTheory, "my_theory", "My Theory", "Me")
///
/// Optional metadata is picked up from static members if they exist:
///   static constexpr const char* kDescription;
///   static constexpr MS_ParamDesc kParams[]; static constexpr uint32_t kParamCount;
///   static constexpr const char* kRequiredSymbols[];
///   static constexpr uint32_t kRequiredSymbolCount;
#define MARKETSCAN_EXPORT_STRATEGY(ClassName, IdLiteral, DisplayNameLiteral, AuthorLiteral)        \
    extern "C" {                                                                                   \
    MS_EXPORT uint32_t ms_plugin_abi_version(void) {                                               \
        return MS_ABI_VERSION;                                                                     \
    }                                                                                              \
    MS_EXPORT const MS_StrategyDesc* ms_plugin_describe(void) {                                    \
        static const MS_StrategyDesc descriptor =                                                  \
            ::marketscan::detail::make_descriptor<ClassName>(                                      \
                IdLiteral, DisplayNameLiteral, AuthorLiteral);                                     \
        return &descriptor;                                                                        \
    }                                                                                              \
    MS_EXPORT void* ms_plugin_create(MS_StrategyVTable* out_vtable) {                              \
        return ::marketscan::detail::create_instance<ClassName>(out_vtable);                       \
    }                                                                                              \
    }

#endif  // MARKETSCAN_STRATEGY_ISTRATEGY_HPP
