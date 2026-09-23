/*
 * What became of a submission, and why one sentinel was not enough.
 *
 * `submit` used to return 0 both for "the broker refused this order" and for
 * "the order was never sent because nothing is wired up". Those are not the
 * same fact: the first is information a strategy should act on, the second is a
 * bug in the harness. A caller that cannot tell them apart will treat a wiring
 * mistake as a market event, which is the same shape as a test that passes
 * without measuring anything.
 *
 * `IStrategy` is header-only, so this instantiates one directly and hands it a
 * host table built by hand. No DLL, no engine, no network.
 */

#include <catch2/catch_test_macros.hpp>

#include <cstdint>

#include "marketscan/strategy/istrategy.hpp"

namespace {

/// The smallest strategy there is. It exists to be submitted through.
class Probe final : public marketscan::IStrategy {
public:
    static constexpr const char* kDescription = "Test probe.";
    static constexpr MS_ParamDesc kParams[] = {
        marketscan::param_double("unused", "Nothing reads this.", 0.0, 1.0, 0.0),
    };
    static constexpr uint32_t kParamCount = 1;

    using marketscan::IStrategy::attach_host;
    using marketscan::IStrategy::submit;
    using marketscan::IStrategy::submit_limit;
    using marketscan::IStrategy::submit_market;
    using marketscan::IStrategy::SubmitOutcome;
    using marketscan::IStrategy::SubmitResult;
};

/// A host whose submit_order answers with whatever it was told to.
struct StubHost {
    std::uint64_t answer = 0;
    std::uint64_t calls = 0;
    MS_HostContext table{};

    explicit StubHost(std::uint64_t reply) : answer(reply) {
        table.host = this;
        table.submit_order = [](void* self, const MS_OrderRequest*) -> std::uint64_t {
            auto* me = static_cast<StubHost*>(self);
            ++me->calls;
            return me->answer;
        };
    }
};

MS_OrderRequest an_order() {
    MS_OrderRequest request{};
    request.symbol = 1;
    request.side = MS_SIDE_BUY;
    request.type = MS_ORD_LIMIT;
    request.tif = MS_TIF_DAY;
    request.quantity = 10.0;
    request.limit_price = 100.0;
    return request;
}

}  // namespace

TEST_CASE("an order nobody could send is not an order that was refused", "[strategy][submit]") {
    // No host attached at all. The old API answered 0, indistinguishable from a
    // venue refusal.
    Probe probe;
    const auto result = probe.submit_limit(1, MS_SIDE_BUY, 10.0, 100.0);

    REQUIRE(result.not_sent());
    REQUIRE_FALSE(result.refused());
    REQUIRE_FALSE(result.sent());
    REQUIRE(result.id == 0);
}

TEST_CASE("a host with no submit_order is also not-sent", "[strategy][submit]") {
    // A host table exists but the slot is null, which is legal: the ABI says
    // any callback may be null. Still not an answer about the order.
    Probe probe;
    MS_HostContext empty{};
    probe.attach_host(&empty);

    const auto result = probe.submit_limit(1, MS_SIDE_BUY, 10.0, 100.0);
    REQUIRE(result.not_sent());
    REQUIRE_FALSE(result.refused());
}

TEST_CASE("a broker refusal is reported as a refusal", "[strategy][submit]") {
    // The case that matters. Reacting to this is what a live strategy has to
    // do, and it is why the ABI grew on_order_status.
    StubHost host(0);
    Probe probe;
    probe.attach_host(&host.table);

    const auto result = probe.submit(an_order());

    REQUIRE(host.calls == 1);
    REQUIRE(result.refused());
    REQUIRE_FALSE(result.not_sent());
    REQUIRE_FALSE(result.sent());
    REQUIRE(result.id == 0);
}

TEST_CASE("an accepted order carries its id", "[strategy][submit]") {
    StubHost host(4242);
    Probe probe;
    probe.attach_host(&host.table);

    const auto result = probe.submit(an_order());

    REQUIRE(result.sent());
    REQUIRE_FALSE(result.refused());
    REQUIRE_FALSE(result.not_sent());
    REQUIRE(result.id == 4242);
}

TEST_CASE("the result still converts to a bare id", "[strategy][submit]") {
    // Every existing call site assigns this to a uint64_t and compares it
    // against 0. That has to keep meaning what it meant, or this change would
    // have been a rewrite of every theory rather than an addition.
    StubHost host(77);
    Probe probe;
    probe.attach_host(&host.table);

    const std::uint64_t id = probe.submit(an_order());
    REQUIRE(id == 77);

    StubHost refusing(0);
    Probe other;
    other.attach_host(&refusing.table);
    const std::uint64_t none = other.submit(an_order());
    REQUIRE(none == 0);
}

TEST_CASE("the three outcomes are mutually exclusive", "[strategy][submit]") {
    // Asserted rather than assumed, because a predicate set where two can be
    // true at once is a predicate set that will be read wrongly.
    using Result = Probe::SubmitResult;
    using Outcome = Probe::SubmitOutcome;

    const Result sent{1, Outcome::Sent};
    const Result refused{0, Outcome::RefusedByBroker};
    const Result absent{0, Outcome::NotSent};

    for (const Result& r : {sent, refused, absent}) {
        const int count = (r.sent() ? 1 : 0) + (r.refused() ? 1 : 0) + (r.not_sent() ? 1 : 0);
        REQUIRE(count == 1);
    }
}
