/*
 * `ms_live bait` -- which order does this venue actually refuse, in a regular
 * session?
 *
 * WHY THIS EXISTS
 *
 * The lifecycle exercise is supposed to provoke one rejection, so that the
 * rejection path has been run against a real venue. Two mechanisms have now
 * been used and neither was ever confirmed:
 *
 *   - A short-sell IOC. Its refusal said "ioc orders are only accepted during
 *     market hours", which means it worked only *outside* them -- and this
 *     account has shorting enabled, so in a regular session it would have
 *     quietly opened a short instead.
 *   - A sub-penny limit price. In the first regular-session run it produced no
 *     identifiable refusal at all.
 *
 * Replacing one unverified mechanism with another is how this happened twice.
 * So this tries each candidate on its own, against the venue, and prints what
 * came back. Nothing is relied on until it is seen refusing.
 *
 * SAFETY
 *
 * Every candidate is designed to be refused. If one is accepted anyway, this
 * cancels it immediately and says so -- an accepted bait is a failed bait and
 * must not be left working.
 */

#include "cli.hpp"
#include "equivalence.hpp"

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "marketscan/live/credentials.hpp"
#include "marketscan/live/endpoints.hpp"
#include "marketscan/live/http.hpp"
#include "marketscan/live/summary_audit.hpp"

namespace marketscan::live {
namespace {

struct Candidate {
    const char* name;
    const char* why;
    std::string body;
};

/// What the venue did with one candidate.
///
/// Three outcomes rather than a bool, because the tally used to say "N of M
/// candidates were refused" with the other M-N lumped together -- and a bait that
/// was accepted and a bait that was never sent because of a rate limit are
/// opposite results. The first means the bait is not a bait; the second means
/// nothing at all was learned about it.
enum class Outcome { Refused, Accepted, RateLimited };

Outcome try_candidate(HttpClient& client, const Candidate& candidate) {
    std::printf("\n  %s\n    %s\n", candidate.name, candidate.why);
    std::fflush(stdout);

    const HttpResponse response = client.post("/v2/orders", candidate.body);

    if (response.rate_limited()) {
        std::printf("    RATE LIMITED -- backing off, not retrying (docs/DATA_POLICY.md)\n");
        return Outcome::RateLimited;
    }

    if (!response.ok()) {
        std::string message = response.body;
        const nlohmann::json parsed = nlohmann::json::parse(response.body, nullptr, false);
        if (!parsed.is_discarded() && parsed.is_object()) {
            message = parsed.value("message", response.body);
        }
        std::printf("    REFUSED  status %d: %s\n", response.status, message.c_str());
        return Outcome::Refused;
    }

    // Accepted. That is a failed bait, and it left a live order behind.
    std::string id;
    const nlohmann::json accepted = nlohmann::json::parse(response.body, nullptr, false);
    if (!accepted.is_discarded() && accepted.is_object()) {
        id = accepted.value("id", std::string{});
    }
    std::printf("    ACCEPTED status %d -- not a bait. Cancelling order %s ... ",
                response.status,
                id.c_str());
    if (!id.empty()) {
        const HttpResponse cancelled = client.del("/v2/orders/" + id);
        std::printf("status %d\n", cancelled.status);
    } else {
        std::printf("no id returned; run `ms_live positions --flatten` to be sure\n");
    }
    return Outcome::Accepted;
}

}  // namespace

int run_bait(int argc, char** argv) {
    const char* symbol = "AAPL";
    for (int i = 0; i + 1 < argc; ++i) {
        if (std::string(argv[i]) == "--symbol") {
            symbol = argv[i + 1];
        }
    }

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

    std::printf("Rejection-bait probe against %s, symbol %s.\n", kPaperTradingHost.data(), symbol);
    std::printf("Each candidate is sent alone. An accepted one is cancelled immediately.\n");

    const std::string sym = symbol;
    std::vector<Candidate> candidates;

    candidates.push_back({"sub-penny limit price",
                          "A limit of 100.001234 on a stock above $1. The venue documents penny "
                          "increments; this is the mechanism currently in the theory, unverified.",
                          R"({"symbol":")" + sym +
                              R"(","qty":"1","side":"buy","type":"limit","time_in_force":"day",)"
                              R"("limit_price":"100.001234"})"});

    candidates.push_back({"unknown symbol",
                          "A ticker no venue lists. Refusal should not depend on the clock, the "
                          "account, or what else is working.",
                          R"({"symbol":"ZZZZQQ","qty":"1","side":"buy","type":"limit",)"
                          R"("time_in_force":"day","limit_price":"1.00"})"});

    candidates.push_back({"zero quantity",
                          "Nothing to trade. Refusal should be structural.",
                          R"({"symbol":")" + sym +
                              R"(","qty":"0","side":"buy","type":"limit",)"
                              R"("time_in_force":"day","limit_price":"1.00"})"});

    candidates.push_back({"fractional quantity on a limit order",
                          "The venue supports fractional shares only for some order types.",
                          R"({"symbol":")" + sym +
                              R"(","qty":"0.5","side":"buy","type":"limit","time_in_force":"day",)"
                              R"("limit_price":"1.00"})"});

    candidates.push_back(
        {"notional far beyond buying power",
         "A million shares. Refused for buying power, which is a real venue "
         "answer -- but one that depends on the account's state, so it is a "
         "worse bait than a structural refusal.",
         R"({"symbol":")" + sym +
             R"(","qty":"1000000","side":"buy","type":"limit","time_in_force":"day",)"
             R"("limit_price":"1000.00"})"});

    std::size_t refused = 0;
    std::size_t accepted = 0;
    std::size_t rate_limited = 0;
    for (const Candidate& candidate : candidates) {
        switch (try_candidate(client, candidate)) {
            case Outcome::Refused:
                ++refused;
                break;
            case Outcome::Accepted:
                ++accepted;
                break;
            case Outcome::RateLimited:
                ++rate_limited;
                break;
        }
        // A pause between attempts. A probe is not a licence to hammer.
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
    }

    std::printf("\n%zu of %zu candidates were refused.\n", refused, candidates.size());
    // The rest of the denominator, named. "3 of 5 refused" used to leave the
    // other two ambiguous, and a bait the venue accepted and a bait that was
    // never sent are opposite outcomes: the first says the bait is no good, the
    // second says nothing was learned about it.
    std::printf(
        "%zu accepted (not baits), %zu never sent (rate limited).\n", accepted, rate_limited);
    std::printf("Prefer a structural refusal -- one that does not depend on the clock, the\n"
                "account's buying power, or what else this run has working.\n");

    SummaryAudit audit;
    audit.check("refused + accepted + rate limited == candidates",
                static_cast<std::uint64_t>(refused + accepted + rate_limited),
                static_cast<std::uint64_t>(candidates.size()));
    std::printf("%s", audit.report().c_str());
    if (audit.exit_code() != 0) {
        return audit.exit_code();
    }
    return refused == 0 ? 1 : 0;
}

}  // namespace marketscan::live
