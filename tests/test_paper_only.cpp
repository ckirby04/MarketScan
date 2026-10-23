// Paper only, and structurally rather than by configuration.
//
// The rule this asserts: there is no value any configuration could take that
// reaches a live-money account, because the hostname that would do it is not in
// this repository at all. A default can be changed by somebody who does not know
// what it protects. An absent string cannot.
//
// WHY THE ASSERTION IS AN ALLOWLIST
//
// The live trading host is the paper host with a prefix removed, so a search for
// the live host also matches the paper host, and a search that excludes the paper
// host first misses anything spelled slightly differently. Neither is a guard.
//
// So this checks the other direction: every host in the broker's domain that
// appears anywhere must be one of a small set that is allowed. An endpoint nobody
// anticipated fails. It also means the forbidden hostname never has to be written
// down -- and a guard that names the thing it forbids has put the thing it
// forbids in the repository.
//
// The source tree is scanned by scripts/check_no_live_endpoints.py, which runs in
// CI before anything is built. This scans the *binaries*, which is the half a
// source grep cannot do: a string can reach a binary through a header, a
// generated file or a dependency without appearing in any source file a reviewer
// would read.

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "marketscan/live/credentials.hpp"
#include "marketscan/live/endpoints.hpp"
#include "marketscan/research/endpoints.hpp"

using namespace marketscan;

namespace {

/// Characters that may appear in a hostname.
[[nodiscard]] bool host_char(unsigned char c) {
    return std::isalnum(c) != 0 || c == '.' || c == '-';
}

/// Every host in the broker's domain found in `blob`.
[[nodiscard]] std::vector<std::string> hosts_in(const std::string& blob) {
    std::vector<std::string> found;
    const std::string needle(live::kBrokerDomain);
    std::size_t at = blob.find(needle);
    while (at != std::string::npos) {
        std::size_t begin = at;
        while (begin > 0 && host_char(static_cast<unsigned char>(blob[begin - 1]))) {
            --begin;
        }
        std::string host = blob.substr(begin, (at + needle.size()) - begin);
        // A leading dot is a fragment, not a host.
        while (!host.empty() && host.front() == '.') {
            host.erase(host.begin());
        }
        found.push_back(std::move(host));
        at = blob.find(needle, at + needle.size());
    }
    return found;
}

[[nodiscard]] std::string read_all(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return {};
    }
    return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

}  // namespace

TEST_CASE("every allowed broker host is a paper or data host", "[live][paper-only]") {
    // The allowlist itself, checked. A guard whose list quietly grew a live host
    // would pass every other assertion in this file.
    REQUIRE(std::size(live::kAllowedHosts) == 3);

    for (const std::string_view host : live::kAllowedHosts) {
        INFO(host);
        CHECK_FALSE(host.empty());
        CHECK(host.find(live::kBrokerDomain) != std::string_view::npos);
        CHECK(live::is_allowed_host(host));

        // Every allowed host is either the paper trading endpoint or a
        // market-data one. Trading anywhere else is what this file exists to
        // prevent, and "anywhere else" includes an endpoint added later.
        const bool paper_trading = host == live::kPaperTradingHost;
        const bool market_data =
            host == live::kMarketDataHost || host == live::kMarketDataStreamHost;
        CHECK((paper_trading || market_data));
    }

    // The trade endpoint carries the paper prefix. If this ever fails, the
    // constant has been repointed and nothing else in the suite would say so.
    CHECK(live::kPaperTradingHost.starts_with("paper-"));

    // The trade-updates stream and the REST endpoint are the same host.
    CHECK(live::kPaperTradeStreamHost == live::kPaperTradingHost);
}

TEST_CASE("an unknown broker host is refused", "[live][paper-only]") {
    // The control for the allowlist: it has to say no to something, or the
    // checks above are a list agreeing with itself.
    //
    // Built from fragments so this file does not itself contain a host that the
    // scanners would then have to make an exception for.
    const std::string live_trading = std::string("api.") + std::string(live::kBrokerDomain);
    CHECK_FALSE(live::is_allowed_host(live_trading));

    CHECK_FALSE(live::is_allowed_host(""));
    CHECK_FALSE(live::is_allowed_host("example.com"));
    CHECK_FALSE(live::is_allowed_host(std::string(live::kBrokerDomain)));
    // A near miss: the right suffix, the wrong host.
    CHECK_FALSE(live::is_allowed_host(std::string("paper.") + std::string(live::kBrokerDomain)));
}

TEST_CASE("the research hosts and the broker hosts are two lists that share nothing",
          "[live][paper-only][research]") {
    // SPEC_RESEARCH_VIEW.md 5.3: the research path's allowlist is its own list,
    // enforced by its own client, and disjoint from the broker's. So neither
    // client can be pointed at the other's hosts, and a research host can never
    // be a way to a broker.
    for (const std::string_view host : research::kServiceHosts) {
        INFO(host);
        CHECK_FALSE(live::is_allowed_host(host));
        CHECK(host.find(live::kBrokerDomain) == std::string_view::npos);
    }
    for (const std::string_view host : live::kAllowedHosts) {
        INFO(host);
        CHECK_FALSE(research::is_service_host(host));
    }
    // The control: each list does accept its own.
    CHECK(research::is_service_host(research::kServiceHost));
    CHECK(live::is_allowed_host(live::kPaperTradingHost));
}

TEST_CASE("no built binary contains a broker host outside the allowlist", "[live][paper-only]") {
    // The half a source grep cannot do.
    const std::filesystem::path bin_dir = std::filesystem::path(MARKETSCAN_RUNTIME_DIR);
    REQUIRE(std::filesystem::exists(bin_dir));

    std::size_t scanned = 0;
    std::vector<std::string> offending;

    for (const std::filesystem::directory_entry& entry :
         std::filesystem::recursive_directory_iterator(bin_dir)) {
        if (!entry.is_regular_file()) {
            continue;
        }
        const std::string extension = entry.path().extension().string();
        if (extension != ".exe" && extension != ".dll") {
            continue;
        }
        ++scanned;
        for (const std::string& host : hosts_in(read_all(entry.path()))) {
            // Two different questions, and conflating them is how this first
            // failed.
            //
            // `is_allowed_host` answers "may this be connected to", and the apex
            // domain is not a host anything connects to. But the domain constant
            // the guard is built from lands in every binary that links the guard,
            // so the apex is a string that may legitimately *appear*.
            //
            // Permitting it costs nothing: the live trading host carries a
            // prefix, so it is a different string and still fails.
            if (host == live::kBrokerDomain) {
                continue;
            }
            if (!live::is_allowed_host(host)) {
                offending.push_back(entry.path().filename().string() + ": " + host);
            }
        }
    }

    // Non-empty, because "no binary contained a bad host" is trivially true of no
    // binaries -- which is exactly what a wrong path here would produce.
    INFO("scanned " << scanned << " binaries in " << bin_dir.string());
    REQUIRE(scanned > 5);

    if (!offending.empty()) {
        std::string message = "a broker host outside the paper allowlist is in a built binary:";
        for (const std::string& line : offending) {
            message += "\n  " + line;
        }
        FAIL(message);
    }
}

TEST_CASE("credentials come from the environment and nowhere else", "[live][credentials]") {
    // The variables are the broker's own names, so a key already exported for
    // their tooling works here without being copied into anything.
    CHECK(std::string(live::kKeyIdVariable) == "APCA_API_KEY_ID");
    CHECK(std::string(live::kSecretVariable) == "APCA_API_SECRET_KEY");

    const live::Credentials loaded = live::load_credentials();
    if (loaded.ok()) {
        // A machine with credentials exported. Nothing here prints them.
        CHECK_FALSE(loaded.key_id.empty());
        CHECK_FALSE(loaded.secret.empty());
        SUCCEED("credentials are present in the environment");
    } else {
        // The ordinary case, including CI. The diagnostic has to name what is
        // missing rather than falling back to anything.
        INFO(loaded.error);
        CHECK(loaded.error.find(live::kKeyIdVariable) != std::string::npos);
        CHECK(loaded.key_id.empty());
        CHECK(loaded.secret.empty());
    }
}

TEST_CASE("a redacted credential is not a credential", "[live][credentials]") {
    CHECK(live::redact("") == "(unset)");
    CHECK(live::redact("abc") == "****");
    CHECK(live::redact("abcd") == "****");

    const std::string key = "PKTESTTESTTESTTESTTE";
    const std::string masked = live::redact(key);
    INFO(masked);
    CHECK(masked.size() == key.size());
    CHECK(masked.substr(0, 4) == "PKTE");
    // Everything after the fourth character is gone, not merely shortened.
    CHECK(masked.find(key.substr(4)) == std::string::npos);
    CHECK(std::count(masked.begin(), masked.end(), '*') == static_cast<long>(key.size() - 4));
}
