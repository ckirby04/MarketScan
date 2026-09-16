#ifndef MARKETSCAN_STRATEGY_ISTRATEGY_HPP
#define MARKETSCAN_STRATEGY_ISTRATEGY_HPP

/*
 * The theory author's header.
 *
 * Everything needed to write a strategy is here, and nothing here links
 * against the host. This file depends on plugin_abi.h and the standard
 * library, and on nothing else in MarketScan -- which is what lets a theory be
 * compiled on its own, against a copy of two headers, and dropped into
 * theories/ as a DLL.
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

    /// Logs an exception that a shim caught. Never throws, and tolerates a
    /// host that was never attached.
    void report_exception(const char* where) noexcept;

protected:
    [[nodiscard]] const MS_HostContext* host() const noexcept { return host_; }

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
    uint64_t submit(const MS_OrderRequest& request) const noexcept {
        return has_host() && host_->submit_order != nullptr
                   ? host_->submit_order(host_->host, &request)
                   : 0;
    }

    uint64_t submit_market(MS_SymbolId symbol,
                           MS_Side side,
                           double quantity,
                           uint64_t client_order_id = 0) const noexcept {
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
                          uint64_t client_order_id = 0) const noexcept {
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

    void cancel(uint64_t order_id) const noexcept {
        if (has_host() && host_->cancel_order != nullptr) {
            host_->cancel_order(host_->host, order_id);
        }
    }

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

    const MS_HostContext* host_ = nullptr;
    std::vector<Binding> bindings_;
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
            instance->on_fill(*fill);
        }
    } catch (...) {
        instance->report_exception("on_fill");
    }
}

template<typename T>
void shim_on_stop(void* self) noexcept {
    auto* instance = static_cast<T*>(self);
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
