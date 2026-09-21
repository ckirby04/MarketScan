#include "marketscan/live/order_map.hpp"

#include <charconv>
#include <stdexcept>

#include "marketscan/core/clock.hpp"

#if defined(_WIN32)
#    include <process.h>
#endif

namespace marketscan::live {
namespace {

constexpr std::string_view kPrefix = "ms-";

}  // namespace

OrderMap::OrderMap(std::string run_token) : run_token_(std::move(run_token)) {
    if (run_token_.empty()) {
        throw std::invalid_argument("OrderMap needs a non-empty run token");
    }
    if (run_token_.find('-') != std::string::npos) {
        throw std::invalid_argument(
            "OrderMap run token must not contain '-': the client order id format is "
            "ms-<run>-<order id> and a token with a separator in it cannot be read back");
    }
}

std::string OrderMap::client_id_for(OrderId engine_id) const {
    return std::string(kPrefix) + run_token_ + "-" + std::to_string(engine_id);
}

std::optional<OrderId> OrderMap::engine_id_for(std::string_view client_id) const {
    if (client_id.size() <= kPrefix.size() || client_id.substr(0, kPrefix.size()) != kPrefix) {
        return std::nullopt;
    }
    client_id.remove_prefix(kPrefix.size());

    if (client_id.size() <= run_token_.size() ||
        client_id.substr(0, run_token_.size()) != run_token_) {
        // Another run's order, or something nobody here wrote. Not ours, and
        // deliberately not an error: the venue's order list contains other
        // runs' orders and this has to be able to say so.
        return std::nullopt;
    }
    client_id.remove_prefix(run_token_.size());

    if (client_id.empty() || client_id.front() != '-') {
        return std::nullopt;
    }
    client_id.remove_prefix(1);
    if (client_id.empty()) {
        return std::nullopt;
    }

    OrderId value = 0;
    const char* first = client_id.data();
    const char* last = first + client_id.size();
    const std::from_chars_result result = std::from_chars(first, last, value);
    if (result.ec != std::errc{} || result.ptr != last) {
        return std::nullopt;
    }
    if (value == kInvalidOrderId) {
        return std::nullopt;
    }
    return value;
}

void OrderMap::record_submitted(OrderId engine_id) {
    OrderLink& link = links_[engine_id];
    link.engine_id = engine_id;
    link.client_order_id = client_id_for(engine_id);
    // Acknowledged stays where it is: re-recording a submit must not un-ack an
    // order the venue has already confirmed.
}

void OrderMap::record_acknowledged(OrderId engine_id, std::string broker_order_id) {
    OrderLink& link = links_[engine_id];
    link.engine_id = engine_id;
    if (link.client_order_id.empty()) {
        link.client_order_id = client_id_for(engine_id);
    }
    link.broker_order_id = std::move(broker_order_id);
    link.acknowledged = true;
}

void OrderMap::retire(OrderId engine_id) {
    links_.erase(engine_id);
}

const OrderLink* OrderMap::find(OrderId engine_id) const noexcept {
    const auto it = links_.find(engine_id);
    return it == links_.end() ? nullptr : &it->second;
}

std::vector<OrderId> OrderMap::unacknowledged() const {
    std::vector<OrderId> out;
    for (const auto& [id, link] : links_) {
        if (!link.acknowledged) {
            out.push_back(id);
        }
    }
    return out;
}

std::vector<OrderLink> OrderMap::live() const {
    std::vector<OrderLink> out;
    out.reserve(links_.size());
    for (const auto& [id, link] : links_) {
        (void)id;
        out.push_back(link);
    }
    return out;
}

std::string make_run_token() {
    const SystemClock clock;
    // Seconds since the epoch, then the process id. Readable, sortable, and
    // distinct between two runs started in the same second.
    const std::uint64_t seconds = static_cast<std::uint64_t>(clock.now() / 1'000'000'000LL);
#if defined(_WIN32)
    const std::uint64_t pid = static_cast<std::uint64_t>(::_getpid());
#else
    const std::uint64_t pid = 0;
#endif
    return std::to_string(seconds) + "p" + std::to_string(pid);
}

}  // namespace marketscan::live
