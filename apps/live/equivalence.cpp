#include "equivalence.hpp"

#include <cstdio>

namespace marketscan::live {
namespace {

[[nodiscard]] const char* type_name(MS_OrderType type) noexcept {
    return type == MS_ORD_LIMIT ? "limit" : "market";
}

[[nodiscard]] const char* tif_name(MS_TimeInForce tif) noexcept {
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

}  // namespace

std::string Decision::describe() const {
    char buffer[256];
    std::snprintf(buffer,
                  sizeof(buffer),
                  "#%zu %s %s %.4f %s tif=%s limit=%.4f client=%llu trigger=%lld",
                  index,
                  side == MS_SIDE_SELL ? "SELL" : "BUY",
                  symbol.c_str(),
                  quantity,
                  type_name(type),
                  tif_name(tif),
                  limit_price,
                  static_cast<unsigned long long>(client_order_id),
                  static_cast<long long>(decision_ts));
    return buffer;
}

bool operator==(const Decision& a, const Decision& b) noexcept {
    // Exact on the discrete fields. The two prices are compared exactly too:
    // both sides computed them from the same doubles in the same order, so a
    // tolerance here would be hiding a real difference rather than absorbing
    // a rounding one. If this ever needs a tolerance, that is the finding.
    return a.symbol == b.symbol && a.side == b.side && a.type == b.type && a.tif == b.tif &&
           a.quantity == b.quantity && a.limit_price == b.limit_price &&
           a.client_order_id == b.client_order_id && a.decision_ts == b.decision_ts;
}

std::vector<Decision> decisions_of(const IBroker& broker, const SymbolTable& symbols) {
    std::vector<Decision> out;
    out.reserve(broker.orders().size());
    for (const Order& order : broker.orders()) {
        Decision decision;
        decision.index = out.size();
        decision.symbol = symbols.name(order.request.symbol);
        decision.side = order.request.side;
        decision.type = order.request.type;
        decision.tif = order.request.tif;
        decision.quantity = order.request.quantity;
        decision.limit_price = order.request.limit_price;
        decision.client_order_id = order.request.client_order_id;
        decision.decision_ts = order.decision_ts;
        out.push_back(std::move(decision));
    }
    return out;
}

DecisionDiff compare(const std::vector<Decision>& live, const std::vector<Decision>& replayed) {
    DecisionDiff diff;

    const std::size_t common = std::min(live.size(), replayed.size());
    for (std::size_t i = 0; i < common; ++i) {
        if (!(live[i] == replayed[i])) {
            diff.index = i;
            diff.live = live[i].describe();
            diff.replayed = replayed[i].describe();
            diff.what = "the two runs disagree at decision " + std::to_string(i);
            return diff;
        }
    }

    if (live.size() != replayed.size()) {
        diff.index = common;
        diff.what = "the two runs made a different number of decisions: live " +
                    std::to_string(live.size()) + ", replay " + std::to_string(replayed.size());
        diff.live = common < live.size() ? live[common].describe() : "(none)";
        diff.replayed = common < replayed.size() ? replayed[common].describe() : "(none)";
        return diff;
    }

    diff.equal = true;
    return diff;
}

}  // namespace marketscan::live
