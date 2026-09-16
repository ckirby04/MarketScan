#ifndef MARKETSCAN_STRATEGY_HOST_CONTEXT_HPP
#define MARKETSCAN_STRATEGY_HOST_CONTEXT_HPP

#include "marketscan/abi/plugin_abi.h"
#include "marketscan/core/money.hpp"
#include "marketscan/core/types.hpp"

namespace marketscan {

/// Everything the host offers a strategy, as C++.
///
/// HostContext turns this into the MS_HostContext table of C function pointers
/// that actually crosses the boundary. Splitting the two keeps the C thunks
/// trivial -- each one is a cast and a call -- and keeps the interesting part
/// testable without a DLL in sight.
class HostServices {
public:
    HostServices() = default;
    HostServices(const HostServices&) = delete;
    HostServices& operator=(const HostServices&) = delete;
    HostServices(HostServices&&) = delete;
    HostServices& operator=(HostServices&&) = delete;
    virtual ~HostServices() = default;

    /// Returns the engine order id, or 0 if refused.
    virtual std::uint64_t submit_order(const MS_OrderRequest& request) = 0;
    virtual void cancel_order(std::uint64_t order_id) = 0;

    [[nodiscard]] virtual Timestamp now() const = 0;

    /// Uniform on [0, 1) from this strategy's own seeded stream.
    virtual double random_uniform() = 0;

    [[nodiscard]] virtual double position(SymbolId symbol) const = 0;
    [[nodiscard]] virtual Money cash() const = 0;
    [[nodiscard]] virtual Money equity() const = 0;

    [[nodiscard]] virtual SymbolId symbol_id(const char* ticker) const = 0;
    [[nodiscard]] virtual const char* symbol_name(SymbolId symbol) const = 0;
    [[nodiscard]] virtual std::uint32_t symbol_count() const = 0;

    virtual void log(LogLevel level, const char* message) = 0;
    virtual void publish_metric(const char* name, double value) = 0;
};

/// The MS_HostContext a plugin is handed.
///
/// `abi()->host` is the HostServices pointer, and every thunk casts it back.
/// Each thunk is noexcept and tolerates a null services pointer, because the
/// plugin holds this table for its whole life and a half-constructed host is
/// not a reason to crash inside somebody else's DLL.
class HostContext {
public:
    explicit HostContext(HostServices* services) noexcept;

    HostContext(const HostContext&) = delete;
    HostContext& operator=(const HostContext&) = delete;
    HostContext(HostContext&&) = delete;
    HostContext& operator=(HostContext&&) = delete;
    ~HostContext() = default;

    [[nodiscard]] const MS_HostContext* abi() const noexcept { return &abi_; }

private:
    static std::uint64_t submit_order_thunk(void* host, const MS_OrderRequest* request) noexcept;
    static void cancel_order_thunk(void* host, std::uint64_t order_id) noexcept;
    static MS_Nanos now_thunk(void* host) noexcept;
    static double random_uniform_thunk(void* host) noexcept;
    static double position_thunk(void* host, MS_SymbolId symbol) noexcept;
    static MS_Money cash_thunk(void* host) noexcept;
    static MS_Money equity_thunk(void* host) noexcept;
    static MS_SymbolId symbol_id_thunk(void* host, const char* ticker) noexcept;
    static const char* symbol_name_thunk(void* host, MS_SymbolId symbol) noexcept;
    static std::uint32_t symbol_count_thunk(void* host) noexcept;
    static void log_thunk(void* host, MS_LogLevel level, const char* message) noexcept;
    static void publish_metric_thunk(void* host, const char* name, double value) noexcept;

    MS_HostContext abi_{};
};

}  // namespace marketscan

#endif  // MARKETSCAN_STRATEGY_HOST_CONTEXT_HPP
