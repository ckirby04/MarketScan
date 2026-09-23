/*
 * `ms_live orders` -- what the venue thinks happened to this account's orders.
 *
 * WHY THIS EXISTS
 *
 * Because reconciliation said "this run says 6 shares of QQQ, the venue says 9"
 * and that is the beginning of an investigation rather than the end of one.
 * Three shares filled that this process did not book, and the only way to find
 * out which three is to ask the venue what it did, order by order, and compare
 * it against what the run recorded.
 *
 * It reads and prints. It sends no orders.
 */

#include "cli.hpp"
#include "equivalence.hpp"

#include <cstdio>
#include <cstring>
#include <string>

#include <nlohmann/json.hpp>

#include "marketscan/live/credentials.hpp"
#include "marketscan/live/endpoints.hpp"
#include "marketscan/live/http.hpp"
#include "marketscan/live/order_map.hpp"

namespace marketscan::live {
namespace {

/// Null-safe, for the same reason LiveBroker needs one: the venue sends null
/// for filled_avg_price on an order that never filled.
[[nodiscard]] std::string field(const nlohmann::json& obj, const char* key, const char* fallback) {
    const auto it = obj.find(key);
    if (it == obj.end() || it->is_null()) {
        return fallback;
    }
    if (it->is_string()) {
        return it->get<std::string>();
    }
    return it->dump();
}

}  // namespace

int run_orders(int argc, char** argv) {
    const char* limit_arg = option_value(argc, argv, "--limit");
    const char* run_arg = option_value(argc, argv, "--run");
    const int limit = limit_arg != nullptr ? std::atoi(limit_arg) : 50;

    const Credentials credentials = load_credentials();
    if (!credentials.ok()) {
        std::printf("error: %s\n", credentials.error.c_str());
        return 2;
    }

    HttpClient client(std::string(kPaperTradingHost), credentials, kUserAgent);
    if (!client.connected()) {
        std::printf("error: %s\n", client.error().c_str());
        return 2;
    }

    const HttpResponse response =
        client.get("/v2/orders?status=all&direction=desc&limit=" + std::to_string(limit));
    if (!response.ok()) {
        std::printf(
            "GET /v2/orders failed: status %d %s\n", response.status, response.error.c_str());
        return 2;
    }

    const nlohmann::json list = nlohmann::json::parse(response.body, nullptr, false);
    if (list.is_discarded() || !list.is_array()) {
        std::printf("orders response was not a JSON array\n");
        return 2;
    }

    std::printf("%zu order(s), newest first%s%s\n\n",
                list.size(),
                run_arg != nullptr ? ", run " : "",
                run_arg != nullptr ? run_arg : "");
    std::printf("  %-26s %-6s %-5s %-6s %-18s %-9s %-9s %s\n",
                "client order id",
                "symbol",
                "side",
                "qty",
                "status",
                "filled",
                "avg px",
                "submitted");

    double filled_total = 0.0;
    std::size_t shown = 0;
    for (const nlohmann::json& entry : list) {
        if (!entry.is_object()) {
            continue;
        }
        const std::string client_id = field(entry, "client_order_id", "");
        if (run_arg != nullptr && client_id.find(run_arg) == std::string::npos) {
            continue;
        }
        ++shown;

        const std::string filled = field(entry, "filled_qty", "0");
        const std::string side = field(entry, "side", "?");
        try {
            const double quantity = std::stod(filled);
            filled_total += side == "sell" ? -quantity : quantity;
        } catch (const std::exception&) {
        }

        std::printf("  %-26s %-6s %-5s %-6s %-18s %-9s %-9s %s\n",
                    client_id.c_str(),
                    field(entry, "symbol", "?").c_str(),
                    side.c_str(),
                    field(entry, "qty", "?").c_str(),
                    field(entry, "status", "?").c_str(),
                    filled.c_str(),
                    field(entry, "filled_avg_price", "-").c_str(),
                    field(entry, "submitted_at", "?").substr(11, 12).c_str());
    }

    std::printf("\n  %zu shown; net filled %.0f shares\n", shown, filled_total);
    if (run_arg == nullptr) {
        std::printf("  Narrow to one run with --run <token>, which is printed when a run "
                    "starts.\n");
    }
    return 0;
}

}  // namespace marketscan::live
