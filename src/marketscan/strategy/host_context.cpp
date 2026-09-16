#include "marketscan/strategy/host_context.hpp"

namespace marketscan {
namespace {

/// Every thunk starts here. A null services pointer is not a crash: the plugin
/// holds this table for its whole life, and it may outlive whatever set it up.
[[nodiscard]] HostServices* services_of(void* host) noexcept {
    return static_cast<HostServices*>(host);
}

}  // namespace

HostContext::HostContext(HostServices* services) noexcept {
    abi_.host = services;
    abi_.submit_order = &submit_order_thunk;
    abi_.cancel_order = &cancel_order_thunk;
    abi_.now = &now_thunk;
    abi_.random_uniform = &random_uniform_thunk;
    abi_.position = &position_thunk;
    abi_.cash = &cash_thunk;
    abi_.equity = &equity_thunk;
    abi_.symbol_id = &symbol_id_thunk;
    abi_.symbol_name = &symbol_name_thunk;
    abi_.symbol_count = &symbol_count_thunk;
    abi_.log = &log_thunk;
    abi_.publish_metric = &publish_metric_thunk;
}

std::uint64_t HostContext::submit_order_thunk(void* host, const MS_OrderRequest* request) noexcept {
    HostServices* services = services_of(host);
    if (services == nullptr || request == nullptr) {
        return 0;
    }
    return services->submit_order(*request);
}

void HostContext::cancel_order_thunk(void* host, std::uint64_t order_id) noexcept {
    if (HostServices* services = services_of(host); services != nullptr) {
        services->cancel_order(order_id);
    }
}

MS_Nanos HostContext::now_thunk(void* host) noexcept {
    HostServices* services = services_of(host);
    return services == nullptr ? 0 : services->now();
}

double HostContext::random_uniform_thunk(void* host) noexcept {
    HostServices* services = services_of(host);
    return services == nullptr ? 0.0 : services->random_uniform();
}

double HostContext::position_thunk(void* host, MS_SymbolId symbol) noexcept {
    HostServices* services = services_of(host);
    return services == nullptr ? 0.0 : services->position(symbol);
}

MS_Money HostContext::cash_thunk(void* host) noexcept {
    HostServices* services = services_of(host);
    return services == nullptr ? 0 : services->cash().to_abi();
}

MS_Money HostContext::equity_thunk(void* host) noexcept {
    HostServices* services = services_of(host);
    return services == nullptr ? 0 : services->equity().to_abi();
}

MS_SymbolId HostContext::symbol_id_thunk(void* host, const char* ticker) noexcept {
    HostServices* services = services_of(host);
    if (services == nullptr || ticker == nullptr) {
        return kInvalidSymbol;
    }
    return services->symbol_id(ticker);
}

const char* HostContext::symbol_name_thunk(void* host, MS_SymbolId symbol) noexcept {
    HostServices* services = services_of(host);
    // Never null, so a plugin that skips the check cannot fault on a strlen.
    return services == nullptr ? "" : services->symbol_name(symbol);
}

std::uint32_t HostContext::symbol_count_thunk(void* host) noexcept {
    HostServices* services = services_of(host);
    return services == nullptr ? 0 : services->symbol_count();
}

void HostContext::log_thunk(void* host, MS_LogLevel level, const char* message) noexcept {
    HostServices* services = services_of(host);
    if (services != nullptr && message != nullptr) {
        services->log(level, message);
    }
}

void HostContext::publish_metric_thunk(void* host, const char* name, double value) noexcept {
    HostServices* services = services_of(host);
    if (services != nullptr && name != nullptr) {
        services->publish_metric(name, value);
    }
}

}  // namespace marketscan
