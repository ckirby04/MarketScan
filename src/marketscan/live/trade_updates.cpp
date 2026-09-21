#include "marketscan/live/trade_updates.hpp"

#include <nlohmann/json.hpp>

namespace marketscan::live {
namespace {

/// A number the venue sends as a JSON string. Returns 0.0 when absent or null,
/// which is correct for every field this is used on -- a fill with no price is
/// not a fill at zero, and the caller checks the quantity before booking one.
[[nodiscard]] double numeric(const nlohmann::json& obj, const char* key) {
    const auto it = obj.find(key);
    if (it == obj.end() || it->is_null()) {
        return 0.0;
    }
    if (it->is_number()) {
        return it->get<double>();
    }
    if (it->is_string()) {
        try {
            return std::stod(it->get<std::string>());
        } catch (const std::exception&) {
            return 0.0;
        }
    }
    return 0.0;
}

[[nodiscard]] std::string text(const nlohmann::json& obj, const char* key) {
    const auto it = obj.find(key);
    if (it == obj.end() || !it->is_string()) {
        return {};
    }
    return it->get<std::string>();
}

[[nodiscard]] UpdateKind classify(const std::string& event) noexcept {
    if (event == "fill") {
        return UpdateKind::Fill;
    }
    if (event == "partial_fill") {
        return UpdateKind::PartialFill;
    }
    if (event == "canceled" || event == "cancelled") {
        return UpdateKind::Cancelled;
    }
    if (event == "expired" || event == "done_for_day") {
        return UpdateKind::Expired;
    }
    if (event == "rejected") {
        return UpdateKind::Rejected;
    }
    if (event == "replaced") {
        return UpdateKind::Replaced;
    }
    if (event == "new" || event == "accepted") {
        return UpdateKind::Accepted;
    }
    // Live but not actionable. Listed rather than lumped with the unknown, so
    // that "unknown" keeps meaning something.
    if (event == "pending_new" || event == "pending_cancel" || event == "pending_replace" ||
        event == "calculated" || event == "stopped" || event == "suspended" ||
        event == "order_cancel_rejected" || event == "order_replace_rejected" ||
        event == "new_order_reject") {
        return UpdateKind::Transient;
    }
    return UpdateKind::Unknown;
}

/// Reads the venue's RFC3339 timestamp, which carries a numeric offset that
/// from_iso8601 refuses by design. Returns 0 when unreadable, and the caller
/// then uses its own clock rather than pretending it knows.
[[nodiscard]] Timestamp read_ts(const std::string& stamp) {
    if (stamp.empty()) {
        return 0;
    }
    Timestamp out = 0;
    if (time::from_iso8601(stamp, out)) {
        return out;
    }
    const std::size_t sign = stamp.find_last_of("+-");
    if (sign == std::string::npos || sign <= 10) {
        return 0;
    }
    if (!time::from_iso8601(stamp.substr(0, sign), out)) {
        return 0;
    }
    const int direction = stamp[sign] == '-' ? 1 : -1;
    const std::string rest = stamp.substr(sign + 1);
    if (rest.size() < 2) {
        return 0;
    }
    const int hours = std::atoi(rest.substr(0, 2).c_str());
    const int minutes = rest.size() >= 5 ? std::atoi(rest.substr(3, 2).c_str()) : 0;
    return out +
           static_cast<Timestamp>(direction) * (hours * 3600LL + minutes * 60LL) * 1'000'000'000LL;
}

void read_one(const nlohmann::json& data, TradeUpdateParse& out) {
    TradeUpdate update;
    update.raw_event = text(data, "event");
    update.kind = classify(update.raw_event);

    const auto order = data.find("order");
    if (order != data.end() && order->is_object()) {
        update.client_order_id = text(*order, "client_order_id");
        update.broker_order_id = text(*order, "id");
        update.symbol = text(*order, "symbol");
        update.cumulative_filled = numeric(*order, "filled_qty");
        update.average_fill_price = numeric(*order, "filled_avg_price");
        update.order_quantity = numeric(*order, "qty");
        update.side = text(*order, "side") == "sell" ? MS_SIDE_SELL : MS_SIDE_BUY;
    }

    update.fill_quantity = numeric(data, "qty");
    update.fill_price = numeric(data, "price");

    update.ts = read_ts(text(data, "timestamp"));
    if (update.ts == 0 && order != data.end() && order->is_object()) {
        update.ts = read_ts(text(*order, "updated_at"));
    }

    update.reason = text(data, "reason");
    if (update.reason.empty() && order != data.end() && order->is_object()) {
        update.reason = text(*order, "reject_reason");
    }

    out.updates.push_back(std::move(update));
}

}  // namespace

std::string_view to_string(UpdateKind kind) noexcept {
    switch (kind) {
        case UpdateKind::Accepted:
            return "accepted";
        case UpdateKind::PartialFill:
            return "partial fill";
        case UpdateKind::Fill:
            return "fill";
        case UpdateKind::Cancelled:
            return "cancelled";
        case UpdateKind::Expired:
            return "expired";
        case UpdateKind::Rejected:
            return "rejected";
        case UpdateKind::Replaced:
            return "replaced";
        case UpdateKind::Transient:
            return "transient";
        case UpdateKind::Unknown:
            return "unknown";
    }
    return "?";
}

TradeUpdateParse parse_trade_update(std::string_view raw) {
    TradeUpdateParse out;

    const nlohmann::json doc = nlohmann::json::parse(raw, nullptr, false);
    if (doc.is_discarded()) {
        out.error = "trade update was not valid JSON";
        return out;
    }

    // The stream sends one object per message, but an array costs nothing to
    // accept and a vendor that starts batching would otherwise silently drop
    // every fill in the batch.
    const auto handle = [&out](const nlohmann::json& message) {
        if (!message.is_object()) {
            out.error = "trade update was not a JSON object";
            return;
        }

        const std::string stream = text(message, "stream");
        if (stream == "authorization") {
            const auto data = message.find("data");
            const std::string status =
                data != message.end() && data->is_object() ? text(*data, "status") : std::string{};
            out.controls.push_back("authorization: " + status);
            // Anything other than an explicit success is a refusal. Defaulting
            // the other way would have a typo in a status string read as
            // authorised.
            out.unauthorised = status != "authorized";
            return;
        }
        if (stream == "listening") {
            out.controls.push_back("listening");
            return;
        }
        if (stream != "trade_updates") {
            // Some other stream, or a shape with no stream field at all.
            out.controls.push_back(stream.empty() ? "unnamed message" : stream);
            return;
        }

        const auto data = message.find("data");
        if (data == message.end() || !data->is_object()) {
            out.error = "trade_updates message had no data object";
            return;
        }
        read_one(*data, out);
    };

    if (doc.is_array()) {
        for (const nlohmann::json& message : doc) {
            handle(message);
        }
    } else {
        handle(doc);
    }
    return out;
}

}  // namespace marketscan::live
