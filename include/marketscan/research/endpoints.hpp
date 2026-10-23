#ifndef MARKETSCAN_RESEARCH_ENDPOINTS_HPP
#define MARKETSCAN_RESEARCH_ENDPOINTS_HPP

/*
 * Every host the research path may contact, and there is one service.
 *
 * The desktop Research view talks to the job service (service/, on Cloud Run)
 * for its conversation and the runs it starts -- docs/specs/SPEC_DESKTOP_CHAT.md.
 * This is that path's allowlist, and it is a different list from the broker's
 * (include/marketscan/live/endpoints.hpp) on purpose: SPEC_RESEARCH_VIEW.md
 * section 2 keeps the research path from ever reaching a broker, and section
 * 5.3 asks for two lists that are disjoint, each enforced by its own client.
 * tests/test_paper_only.cpp asserts they share no host and that nothing here is
 * in the broker's domain; research::ServiceClient refuses any host not below.
 *
 * Loopback is on the list so the service can be run on this machine
 * (service/README.md, "Running it locally"). It is the only place plain HTTP is
 * allowed; everything else must be TLS.
 */

#include <string_view>

namespace marketscan::research {

/// The job service as deployed by service/deploy.ps1: the Cloud Run service
/// `marketscan-research` in us-central1. Stable while the service keeps its name
/// and region; a custom domain would replace it here and in the host test.
inline constexpr std::string_view kServiceHost = "marketscan-research-7xbngotfiq-uc.a.run.app";

/// This machine, for a service run locally.
inline constexpr std::string_view kLoopbackHosts[] = {"127.0.0.1", "localhost"};

/// Every host the research path may contact.
inline constexpr std::string_view kServiceHosts[] = {kServiceHost, "127.0.0.1", "localhost"};

[[nodiscard]] constexpr bool is_loopback_host(std::string_view host) noexcept {
    for (const std::string_view loopback : kLoopbackHosts) {
        if (host == loopback) {
            return true;
        }
    }
    return false;
}

/// Whether the research path may contact `host`.
[[nodiscard]] constexpr bool is_service_host(std::string_view host) noexcept {
    for (const std::string_view allowed : kServiceHosts) {
        if (host == allowed) {
            return true;
        }
    }
    return false;
}

}  // namespace marketscan::research

#endif  // MARKETSCAN_RESEARCH_ENDPOINTS_HPP
