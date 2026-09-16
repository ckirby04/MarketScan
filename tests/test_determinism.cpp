// The determinism contract.
//
// Running the same input twice must produce the same stream, byte for byte.
// Everything else in MarketScan -- the ledger, the metrics, the comparison
// between two theories -- is only meaningful if this holds, so a failure here
// is always a real bug and this file is never quarantined.
//
// At this milestone the stream being hashed is the event stream. When orders
// exist it becomes the order log as well.

#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "marketscan/core/engine.hpp"
#include "marketscan/core/random.hpp"
#include "marketscan/data/synthetic_feed.hpp"
#include "marketscan/util/digest.hpp"

using namespace marketscan;

namespace {

/// Hashes every event the engine dispatches, in dispatch order.
class DigestSink final : public IEventSink {
public:
    void on_event_complete(const MarketEvent& event) override {
        digest_.absorb(event);
        ++count_;
    }

    [[nodiscard]] std::string hex() const { return digest_.to_hex(); }

    [[nodiscard]] std::uint64_t count() const noexcept { return count_; }

    void clear() noexcept {
        digest_ = Digest{};
        count_ = 0;
    }

private:
    Digest digest_;
    std::uint64_t count_ = 0;
};

SyntheticFeedConfig replay_config(std::uint64_t seed = 0xABCD'1234) {
    SyntheticFeedConfig config;
    config.instruments = {
        SyntheticInstrument{"AAA", 100.0, 0.05, 0.20, 50'000.0},
        SyntheticInstrument{"BBB", 250.0, 0.00, 0.30, 10'000.0},
    };
    config.bars_per_session = 120;
    config.sessions = 3;
    config.seed = seed;
    return config;
}

/// Builds a fresh engine over a fresh feed and returns the digest of the run.
std::string run_once(std::uint64_t seed) {
    SymbolTable symbols;
    Engine engine;
    DigestSink sink;

    engine.add_source(std::make_unique<SyntheticFeed>(replay_config(seed), symbols));
    engine.add_sink(&sink);
    engine.run();

    REQUIRE(sink.count() > 0);
    return sink.hex();
}

}  // namespace

// --------------------------------------------------------------------------
// The generator underneath everything
// --------------------------------------------------------------------------

TEST_CASE("the same seed gives the same numbers", "[determinism][rng]") {
    DeterministicRng a(42);
    DeterministicRng b(42);

    bool identical = true;
    for (int i = 0; i < 10'000; ++i) {
        if (a.next_u64() != b.next_u64()) {
            identical = false;
            break;
        }
    }
    REQUIRE(identical);
}

TEST_CASE("different seeds give different numbers", "[determinism][rng]") {
    DeterministicRng a(1);
    DeterministicRng b(2);
    REQUIRE(a.next_u64() != b.next_u64());
}

// Seed 0 has to work. xoshiro cannot be started from an all-zero state, which
// is exactly why the seed goes through SplitMix64 first.
TEST_CASE("a zero seed is usable", "[determinism][rng]") {
    DeterministicRng rng(0);
    REQUIRE(rng.next_u64() != 0);
    REQUIRE(rng.next_u64() != rng.next_u64());
}

TEST_CASE("uniform stays inside its interval", "[determinism][rng]") {
    DeterministicRng rng(7);
    bool in_range = true;
    for (int i = 0; i < 100'000; ++i) {
        const double u = rng.uniform();
        if (!(u >= 0.0 && u < 1.0)) {
            in_range = false;
            break;
        }
    }
    REQUIRE(in_range);
}

// uniform_positive exists so that std::log never sees a zero.
TEST_CASE("uniform_positive excludes zero", "[determinism][rng]") {
    DeterministicRng rng(11);
    bool positive = true;
    for (int i = 0; i < 100'000; ++i) {
        const double u = rng.uniform_positive();
        if (!(u > 0.0 && u <= 1.0)) {
            positive = false;
            break;
        }
    }
    REQUIRE(positive);
}

TEST_CASE("normal draws look standard normal", "[determinism][rng]") {
    DeterministicRng rng(2'024);
    constexpr int kDraws = 200'000;

    double sum = 0.0;
    double sum_sq = 0.0;
    for (int i = 0; i < kDraws; ++i) {
        const double z = rng.normal();
        sum += z;
        sum_sq += z * z;
    }
    const double mean = sum / kDraws;
    const double variance = sum_sq / kDraws - mean * mean;

    // Loose enough not to be flaky, tight enough to catch a scale error.
    REQUIRE(std::fabs(mean) < 0.02);
    REQUIRE(std::fabs(variance - 1.0) < 0.02);
}

TEST_CASE("reseeding restores the stream", "[determinism][rng]") {
    DeterministicRng rng(5);
    const std::uint64_t first = rng.next_u64();
    for (int i = 0; i < 100; ++i) {
        (void)rng.next_u64();
    }
    rng.reseed(5);
    REQUIRE(rng.next_u64() == first);
}

// Each strategy draws from its own stream, so enabling an unrelated theory
// cannot change the numbers another one sees.
TEST_CASE("derived seeds are independent per name", "[determinism][rng]") {
    constexpr std::uint64_t run_seed = 0xDEAD'BEEF;

    REQUIRE(derive_seed(run_seed, "alpha") != derive_seed(run_seed, "beta"));
    REQUIRE(derive_seed(run_seed, "alpha") == derive_seed(run_seed, "alpha"));
    REQUIRE(derive_seed(1, "alpha") != derive_seed(2, "alpha"));

    DeterministicRng alpha(derive_seed(run_seed, "alpha"));
    DeterministicRng beta(derive_seed(run_seed, "beta"));
    REQUIRE(alpha.next_u64() != beta.next_u64());
}

// --------------------------------------------------------------------------
// The digest itself
// --------------------------------------------------------------------------

TEST_CASE("the digest separates values a memcmp would confuse", "[determinism][digest]") {
    // Same fields, different types.
    MS_Bar bar{};
    bar.ts = 1;
    bar.symbol = 1;
    bar.close = 100.0;

    MS_Quote quote{};
    quote.ts = 1;
    quote.symbol = 1;
    quote.bid_px = 100.0;

    Digest bar_digest;
    Digest quote_digest;
    bar_digest.absorb(MarketEvent::from(bar));
    quote_digest.absorb(MarketEvent::from(quote));
    REQUIRE(bar_digest.value() != quote_digest.value());
}

TEST_CASE("the digest notices a one-bit price change", "[determinism][digest]") {
    MS_Bar a{};
    a.close = 100.0;
    MS_Bar b = a;
    b.close = std::nextafter(100.0, 101.0);

    Digest first;
    Digest second;
    first.absorb(a);
    second.absorb(b);
    REQUIRE(first.value() != second.value());
}

TEST_CASE("concatenation is unambiguous", "[determinism][digest]") {
    Digest a;
    a.absorb_bytes("ab");
    a.absorb_bytes("c");

    Digest b;
    b.absorb_bytes("a");
    b.absorb_bytes("bc");

    REQUIRE(a.value() != b.value());
}

TEST_CASE("the two zeros hash the same", "[determinism][digest]") {
    Digest positive;
    Digest negative;
    positive.absorb_double(0.0);
    negative.absorb_double(-0.0);
    REQUIRE(positive.value() == negative.value());
}

// --------------------------------------------------------------------------
// The contract
// --------------------------------------------------------------------------

TEST_CASE("the same replay run twice produces the same stream", "[determinism]") {
    const std::string first = run_once(0xABCD'1234);
    const std::string second = run_once(0xABCD'1234);
    REQUIRE(first == second);
}

TEST_CASE("a different seed produces a different stream", "[determinism]") {
    REQUIRE(run_once(1) != run_once(2));
}

// Rewinding must give the same answer as building the engine again. This is
// what makes scrubbing the transport backwards trustworthy: the run the user
// scrubs into is the run they were already watching.
TEST_CASE("rewinding reproduces the run exactly", "[determinism]") {
    SymbolTable symbols;
    Engine engine;
    DigestSink sink;

    engine.add_source(std::make_unique<SyntheticFeed>(replay_config(), symbols));
    engine.add_sink(&sink);

    engine.run();
    const std::string first_pass = sink.hex();
    const std::uint64_t first_count = sink.count();

    REQUIRE(engine.reset());

    // The engine does not reset its sinks -- it does not own them -- so the
    // caller clears its own state before the second pass. That is exactly what
    // the transport does when it rebuilds strategies for a backwards scrub.
    sink.clear();
    engine.run();

    REQUIRE(sink.count() == first_count);
    REQUIRE(sink.hex() == first_pass);
}

TEST_CASE("event count is stable across runs", "[determinism]") {
    SymbolTable symbols;
    Engine engine;
    DigestSink sink;
    engine.add_source(std::make_unique<SyntheticFeed>(replay_config(), symbols));
    engine.add_sink(&sink);
    engine.run();

    // 3 sessions * 120 bars * 2 instruments * (quote + bar)
    REQUIRE(sink.count() == 3 * 120 * 2 * 2);
    REQUIRE(engine.stats().events_dispatched == sink.count());
}
