#ifndef MARKETSCAN_LIVE_ENDPOINTS_HPP
#define MARKETSCAN_LIVE_ENDPOINTS_HPP

/*
 * Every broker hostname this project knows, and there are only paper ones.
 *
 * PAPER ONLY, STRUCTURALLY
 *
 * Not "paper by default". Not "paper unless a flag is set". The live-money
 * trading host does not appear anywhere in this repository -- not in a constant,
 * not in a comment, not in a config example, not behind an `if`. There is no
 * value any configuration could take that would reach it, because the string is
 * not here to be reached.
 *
 * A default can be changed by someone who does not know what it protects. An
 * absent string cannot.
 *
 * HOW THAT IS ENFORCED
 *
 * `tests/test_paper_only.cpp` and `scripts/check_no_live_endpoints.py` scan the
 * source tree and the built binaries for every occurrence of the broker's domain
 * and assert that each one is part of a host on the allowlist below. The test
 * fails on an unknown host rather than on a known-bad one, which is the
 * direction that survives somebody inventing a new endpoint.
 *
 * THE SUBTLETY THAT MAKES A NAIVE GREP WRONG
 *
 * The live trading host is the paper host with a prefix removed. A search for
 * the live hostname therefore matches the paper hostname as a substring, and a
 * search that excludes the paper hostname first will miss a live URL that
 * happens to be spelled differently. So the check is not "is the forbidden
 * string present" -- it is "is every host that appears one we allow". That also
 * means the forbidden string never has to be written down, here or anywhere,
 * which is why it is not.
 */

#include <string_view>

namespace marketscan::live {

/// The domain every broker host in this project belongs to.
inline constexpr std::string_view kBrokerDomain = "alpaca.markets";

/// Paper trading REST. Orders, positions, account.
inline constexpr std::string_view kPaperTradingHost = "paper-api.alpaca.markets";

/// Paper trade updates: fills, partials, rejections, cancels.
inline constexpr std::string_view kPaperTradeStreamHost = "paper-api.alpaca.markets";

/// Historical bars and quotes.
inline constexpr std::string_view kMarketDataHost = "data.alpaca.markets";

/// Streaming market data. IEX on the free tier.
inline constexpr std::string_view kMarketDataStreamHost = "stream.data.alpaca.markets";

/// Every host that may appear in this repository.
///
/// The guard checks membership in this list. A host not on it fails the build,
/// including one nobody has thought of yet.
inline constexpr std::string_view kAllowedHosts[] = {
    kPaperTradingHost,
    kMarketDataHost,
    kMarketDataStreamHost,
};

/// Whether `host` is one this project is allowed to contact.
[[nodiscard]] constexpr bool is_allowed_host(std::string_view host) noexcept {
    for (const std::string_view allowed : kAllowedHosts) {
        if (host == allowed) {
            return true;
        }
    }
    return false;
}

}  // namespace marketscan::live

#endif  // MARKETSCAN_LIVE_ENDPOINTS_HPP
