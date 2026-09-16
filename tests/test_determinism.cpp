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
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "marketscan/core/engine.hpp"
#include "marketscan/core/random.hpp"
#include "marketscan/data/synthetic_feed.hpp"
#include "marketscan/run/backtest.hpp"
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

// --------------------------------------------------------------------------
// The order log
// --------------------------------------------------------------------------
//
// The contract as the specification states it: run the same replay twice, hash
// the serialized order stream, assert equality. Until M4 there were no orders
// to hash; now two theories trade, one of them twenty times, so this covers the
// whole path -- feed, engine, strategy, fill model, ledger.

namespace {

/// Hashes every fill of every theory, in order, field by field.
///
/// Field by field rather than by hashing raw bytes: a struct carries alignment
/// padding that belongs to no field, and folding it in would make the digest
/// depend on whatever was previously in that memory.
[[nodiscard]] std::string digest_orders(const run::BacktestResult& result) {
    Digest digest;
    digest.absorb_u64(result.events);

    for (const run::TheoryResult& theory : result.theories) {
        digest.absorb_bytes(theory.id);
        digest.absorb_money(theory.final_equity);
        digest.absorb_money(theory.cash);
        digest.absorb_money(theory.realized_pnl);
        digest.absorb_money(theory.commission);
        digest.absorb_u64(theory.trades);
        digest.absorb_u64(theory.rejected);

        for (const TradeRecord& trade : theory.blotter) {
            digest.absorb_i64(trade.ts);
            digest.absorb_u64(trade.order_id);
            digest.absorb_u64(trade.client_order_id);
            digest.absorb_u32(trade.symbol);
            digest.absorb_u32(static_cast<std::uint32_t>(trade.side));
            digest.absorb_double(trade.quantity);
            digest.absorb_double(trade.reference_price);
            digest.absorb_double(trade.fill_price);
            digest.absorb_money(trade.slippage);
            digest.absorb_money(trade.commission);
            digest.absorb_money(trade.cash_delta);
            digest.absorb_money(trade.realized_pnl);
        }
    }
    return digest.to_hex();
}

[[nodiscard]] run::BacktestConfig backtest_config(std::uint64_t seed = 0xC0FF'EE00'1234'5678ULL) {
    run::BacktestConfig config;
    config.data_file = std::filesystem::path(MARKETSCAN_TEST_DATA_DIR) / "synthetic_spy_1min.csv";
    config.theory_roots = {std::filesystem::path(MARKETSCAN_THEORY_DIR)};
    config.seed = seed;
    config.starting_cash = Money::from_dollars(100'000);
    return config;
}

}  // namespace

TEST_CASE("the same replay produces the same order log twice", "[determinism]") {
    const run::BacktestResult first = run::run_backtest(backtest_config());
    const run::BacktestResult second = run::run_backtest(backtest_config());

    REQUIRE(first.ok());
    REQUIRE_FALSE(first.theories.empty());

    // Not a vacuous pass: something actually traded.
    std::size_t total_trades = 0;
    for (const run::TheoryResult& theory : first.theories) {
        total_trades += theory.trades;
    }
    INFO("theories: " << first.theories.size() << ", trades: " << total_trades);
    REQUIRE(total_trades > 0);

    REQUIRE(digest_orders(first) == digest_orders(second));
}

TEST_CASE("a third and fourth run agree with the first", "[determinism]") {
    // Three more, because a bug that only shows up sometimes is the one worth
    // catching, and two runs agreeing can be luck.
    const std::string expected = digest_orders(run::run_backtest(backtest_config()));
    for (int attempt = 0; attempt < 3; ++attempt) {
        INFO("attempt " << attempt);
        REQUIRE(digest_orders(run::run_backtest(backtest_config())) == expected);
    }
}

TEST_CASE("every theory's ledger balances after a full replay", "[determinism][ledger]") {
    const run::BacktestResult result = run::run_backtest(backtest_config());
    REQUIRE(result.ok());

    for (const run::TheoryResult& theory : result.theories) {
        INFO("theory " << theory.id);
        REQUIRE(theory.invariant_held);
        // A cash account never goes negative.
        REQUIRE(theory.cash >= Money{});
    }
}

// The calibration claim, asserted rather than left in a README: the optimistic
// model must produce a *better* result than the realistic one, and the gap must
// grow with how often a theory trades. If these ever fail, the fill simulator
// has stopped charging for the spread.
TEST_CASE("optimistic fills flatter every theory, most where it trades most",
          "[determinism][fills]") {
    run::BacktestConfig optimistic = backtest_config();
    optimistic.optimistic_fills = true;

    const run::BacktestResult honest = run::run_backtest(backtest_config());
    const run::BacktestResult wishful = run::run_backtest(optimistic);

    REQUIRE(honest.ok());
    REQUIRE(wishful.ok());

    Money biggest_gap;
    std::size_t busiest_trades = 0;
    std::string busiest;

    for (const run::TheoryResult& theory : honest.theories) {
        const run::TheoryResult* other = wishful.find(theory.id);
        REQUIRE(other != nullptr);
        INFO("theory " << theory.id);

        // Same decisions, cheaper fills: never worse off.
        REQUIRE(other->final_equity >= theory.final_equity);
        REQUIRE(other->slippage < theory.slippage);

        const Money gap = other->final_equity - theory.final_equity;
        if (theory.trades > busiest_trades) {
            busiest_trades = theory.trades;
            busiest = theory.id;
            biggest_gap = gap;
        }
    }

    // The theory that trades most is the one the assumption flatters most.
    for (const run::TheoryResult& theory : honest.theories) {
        if (theory.id == busiest) {
            continue;
        }
        const Money gap = wishful.find(theory.id)->final_equity - theory.final_equity;
        INFO(busiest << " (" << busiest_trades << " trades) vs " << theory.id << " ("
                     << theory.trades << " trades)");
        REQUIRE(biggest_gap > gap);
    }
}

// --------------------------------------------------------------------------
// One theory must not be able to hear another
// --------------------------------------------------------------------------
//
// Every theory in a run shares the engine, the clock, the feed, the symbol
// table and the fill model. None of that is supposed to be a channel between
// them -- each theory has its own ledger, its own order flow and its own
// random stream -- but "supposed to" is not a test. So: a theory's results
// must be identical whether it ran alone or alongside something else.
//
// The failure mode this guards against is quiet. Shared state between
// theories does not crash and does not make a run irreproducible; every
// number still comes back the same on a rerun. It just makes every result
// depend on which other DLLs happened to be in the folder, which turns a
// comparison between two theories into a comparison of nothing.
//
// Neither reference theory draws random numbers, so the per-strategy RNG
// stream is not what this exercises -- [determinism][rng] covers that, and
// derive_seed is the reason a theory that did draw would pass here too.

namespace {

/// A directory holding exactly the theories a test wants to run.
class SoloTheoryDir {
public:
    explicit SoloTheoryDir(const std::string& dll_name) {
        static int counter = 0;
        path_ = std::filesystem::temp_directory_path() /
                ("marketscan_solo_" + std::to_string(++counter) + "_" + dll_name);
        std::filesystem::create_directories(path_);
        std::filesystem::copy_file(std::filesystem::path(MARKETSCAN_THEORY_DIR) / dll_name,
                                   path_ / dll_name,
                                   std::filesystem::copy_options::overwrite_existing);
    }

    SoloTheoryDir(const SoloTheoryDir&) = delete;
    SoloTheoryDir& operator=(const SoloTheoryDir&) = delete;

    ~SoloTheoryDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

/// Everything about one theory's trading that must not depend on its company.
///
/// Benchmark-relative statistics are deliberately left out: information ratio
/// and beta are measured against buy_and_hold, so a run without it genuinely
/// has no answer for them. What must not move is what the theory did.
[[nodiscard]] std::string digest_theory(const run::TheoryResult& theory) {
    Digest digest;
    digest.absorb_bytes(theory.id);
    digest.absorb_money(theory.final_equity);
    digest.absorb_money(theory.cash);
    digest.absorb_money(theory.realized_pnl);
    digest.absorb_money(theory.unrealized_pnl);
    digest.absorb_money(theory.commission);
    digest.absorb_money(theory.slippage);
    digest.absorb_u64(theory.trades);
    digest.absorb_u64(theory.rejected);

    for (const TradeRecord& trade : theory.blotter) {
        digest.absorb_i64(trade.ts);
        digest.absorb_u32(trade.symbol);
        digest.absorb_u32(static_cast<std::uint32_t>(trade.side));
        digest.absorb_double(trade.quantity);
        digest.absorb_double(trade.fill_price);
        digest.absorb_money(trade.realized_pnl);
    }

    for (const double level : theory.equity_curve) {
        digest.absorb_double(level);
    }
    return digest.to_hex();
}

}  // namespace

TEST_CASE("a theory gets the same result alone as it does in company", "[determinism]") {
    const run::BacktestResult together = run::run_backtest(backtest_config());
    REQUIRE(together.ok());
    REQUIRE(together.theories.size() >= 2);

    const SoloTheoryDir solo("sma_crossover.dll");
    run::BacktestConfig alone_config = backtest_config();
    alone_config.theory_roots = {solo.path()};
    const run::BacktestResult alone = run::run_backtest(alone_config);

    REQUIRE(alone.ok());
    REQUIRE(alone.theories.size() == 1);

    const run::TheoryResult* in_company = together.find("sma_crossover");
    REQUIRE(in_company != nullptr);

    // Not a vacuous pass: it has to have traded for there to be anything to
    // disagree about.
    INFO("trades: " << in_company->trades);
    REQUIRE(in_company->trades > 0);

    REQUIRE(digest_theory(alone.theories.front()) == digest_theory(*in_company));
}
