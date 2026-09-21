/*
 * `ms_live positions` and `ms_live flatten`.
 *
 * WHY FLATTEN EXISTS
 *
 * Because reconciliation is only useful while it is capable of passing. The
 * first real equivalence session left six shares of AAPL on the paper account
 * -- a run that hung and was killed after its orders had filled -- and from
 * then on every reconciliation reported a divergence. A check that is
 * permanently red is a check whose next genuine failure nobody can see.
 *
 * So there is a way to put the account back to flat, and it asks first. It
 * sends real orders to a real venue, even though the money is not real, and
 * that is not something a tool should do because it seemed convenient.
 */

#include "equivalence.hpp"

#include <cstdio>
#include <cstring>
#include <string>

#include <nlohmann/json.hpp>

#include "marketscan/live/credentials.hpp"
#include "marketscan/live/endpoints.hpp"
#include "marketscan/live/http.hpp"

namespace marketscan::live {
namespace {

constexpr const char* kUserAgent = "MarketScan/0.1 (research tool)";

[[nodiscard]] bool has_flag(int argc, char** argv, const char* name) {
    for (int i = 0; i < argc; ++i) {
        if (std::strcmp(argv[i], name) == 0) {
            return true;
        }
    }
    return false;
}

}  // namespace

int run_positions(int argc, char** argv) {
    const bool flatten = has_flag(argc, argv, "--flatten");
    const bool confirmed = has_flag(argc, argv, "--yes");

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

    const HttpResponse response = client.get("/v2/positions");
    if (!response.ok()) {
        std::printf(
            "GET /v2/positions failed: status %d %s\n", response.status, response.error.c_str());
        return 2;
    }

    const nlohmann::json held = nlohmann::json::parse(response.body, nullptr, false);
    if (held.is_discarded() || !held.is_array()) {
        std::printf("positions response was not a JSON array\n");
        return 2;
    }

    if (held.empty()) {
        std::printf("no open positions on %s\n", client.host().c_str());
        return 0;
    }

    std::printf("%zu open position(s) on %s:\n\n", held.size(), client.host().c_str());
    for (const nlohmann::json& entry : held) {
        if (!entry.is_object()) {
            continue;
        }
        std::printf("  %-6s %10s shares  avg %-12s  market value %-14s  unrealised %s\n",
                    entry.value("symbol", "?").c_str(),
                    entry.value("qty", "?").c_str(),
                    entry.value("avg_entry_price", "?").c_str(),
                    entry.value("market_value", "?").c_str(),
                    entry.value("unrealized_pl", "?").c_str());
    }

    if (!flatten) {
        std::printf(
            "\nTo close them all: ms_live positions --flatten --yes\n"
            "That sends real orders to a real venue. The money is paper; the orders are not.\n");
        return 0;
    }

    if (!confirmed) {
        // Asked rather than assumed. --flatten alone lists and stops.
        std::printf("\nRefusing to flatten without --yes. This sends market orders for every\n"
                    "position above, to a venue, now.\n");
        return 1;
    }

    // Cancels working orders first, and not only for tidiness. A queued
    // market order on the other side of a position makes the venue refuse
    // every new order in that symbol as a potential wash trade -- which is
    // how one leftover close order silently turned a whole later session into
    // thirteen rejections and nothing else.
    std::printf("\ncancelling open orders ... ");
    const HttpResponse cancelled = client.del("/v2/orders");
    std::printf("status %d\n", cancelled.status);

    // DELETE /v2/positions closes with market orders, and a market order sent
    // outside 09:30-16:00 is queued for the next open rather than filled. So
    // outside those hours the close is sent as a marketable limit instead,
    // which the venue will trade in extended hours.
    const bool use_limits = has_flag(argc, argv, "--limit");
    if (!use_limits) {
        std::printf("closing all positions (market orders) ... ");
        const HttpResponse closed = client.del("/v2/positions?cancel_orders=true");
        std::printf("status %d\n", closed.status);
        if (!closed.ok() && closed.status != 207) {
            std::printf(
                "close failed: %s%s\n", closed.error.c_str(), closed.body.substr(0, 300).c_str());
            return 2;
        }
        std::printf(
            "\nOrders sent. Market orders fill when the venue can fill them, which outside\n"
            "09:30-16:00 means at the next open. Run with --limit to close in extended\n"
            "hours instead, and run this again to see whether the account is flat.\n");
        return 0;
    }

    std::printf("closing all positions (extended-hours limits):\n");
    int failures = 0;
    for (const nlohmann::json& entry : held) {
        if (!entry.is_object()) {
            continue;
        }
        const std::string symbol = entry.value("symbol", std::string{});
        const std::string qty = entry.value("qty", std::string{"0"});
        const std::string price = entry.value("current_price", std::string{});
        if (symbol.empty() || price.empty()) {
            continue;
        }

        double quantity = 0.0;
        double last = 0.0;
        try {
            quantity = std::stod(qty);
            last = std::stod(price);
        } catch (const std::exception&) {
            continue;
        }
        if (quantity == 0.0 || !(last > 0.0)) {
            continue;
        }

        // Through the touch by half a per cent, in whichever direction closes
        // the position. Aggressive on purpose: this is a close, not a trade
        // anyone is trying to be clever about.
        const bool selling = quantity > 0.0;
        const double limit = selling ? last * 0.995 : last * 1.005;

        char body[512];
        std::snprintf(body,
                      sizeof(body),
                      R"({"symbol":"%s","qty":"%.0f","side":"%s","type":"limit",)"
                      R"("time_in_force":"day","limit_price":"%.2f","extended_hours":true})",
                      symbol.c_str(),
                      selling ? quantity : -quantity,
                      selling ? "sell" : "buy",
                      limit);

        const HttpResponse order = client.post("/v2/orders", body);
        std::printf("  %-6s %s %.0f @ %.2f limit ... status %d",
                    symbol.c_str(),
                    selling ? "sell" : "buy",
                    selling ? quantity : -quantity,
                    limit,
                    order.status);
        if (!order.ok()) {
            std::printf("  %s", order.body.substr(0, 200).c_str());
            ++failures;
        }
        std::printf("\n");
    }

    std::printf("\nOrders sent. Run this again to see whether the account is flat.\n");
    return failures == 0 ? 0 : 2;
}

}  // namespace marketscan::live
