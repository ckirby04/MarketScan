// Session boundaries and cross-sectional dispatch, end to end through a DLL.
//
// The per-symbol on_bar callback cannot express "rank the universe and act on
// the top decile", because when any one bar arrives the rest of the session
// has not been shown yet. M1 solves that with two things, both of which live
// on the plugin's side of the boundary because the ABI is frozen: a session
// clock in the shim layer that fires on_session_close when the exchange date
// rolls, and a CrossSectionalStrategy base that buffers a session and hands
// the whole thing over at its close.
//
// Everything here is driven through a real loaded DLL. The mechanism being
// tested is the shim layer compiled into the plugin, so a host-side mock of a
// strategy would test the mock and prove nothing.

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "marketscan/core/engine.hpp"
#include "marketscan/core/session_calendar.hpp"
#include "marketscan/core/symbol_table.hpp"
#include "marketscan/data/synthetic_feed.hpp"
#include "marketscan/exec/fill_model.hpp"
#include "marketscan/exec/order.hpp"
#include "marketscan/strategy/plugin_loader.hpp"
#include "marketscan/strategy/strategy_instance.hpp"

using namespace marketscan;

namespace {

/// Theories the committed `theories/` tree is expected to build and load.
///
/// buy_and_hold and sma_crossover. Exact rather than a floor: a floor passes
/// when one of them silently stops loading, which is precisely the failure a
/// test that iterates the loader's output cannot otherwise notice.
constexpr std::size_t kExpectedTheories = 2;

[[nodiscard]] std::filesystem::path plugin_dir() {
    return std::filesystem::path(MARKETSCAN_TEST_PLUGIN_DIR);
}

/// What one plugin published, by name.
class Published {
public:
    explicit Published(const StrategyInstance& theory) {
        for (const PublishedMetric& metric : theory.metrics()) {
            values_.emplace_back(metric.name, metric.value);
        }
    }

    [[nodiscard]] bool has(std::string_view name) const {
        for (const auto& [key, value] : values_) {
            if (key == name) {
                return true;
            }
        }
        return false;
    }

    /// The published value, or a sentinel a test will notice.
    [[nodiscard]] double operator[](std::string_view name) const {
        for (const auto& [key, value] : values_) {
            if (key == name) {
                return value;
            }
        }
        return -1.0;
    }

private:
    std::vector<std::pair<std::string, double>> values_;
};

/// One replay of `sessions` days over `instruments` symbols, through one
/// plugin, returning what it published.
///
/// Starts at 13:30 UTC, which is 09:30 in New York on a daylight-saving date:
/// a real US equity session, entirely inside one exchange day, and not inside
/// one UTC day for the whole run.
struct Replay {
    Published metrics;
    std::size_t symbol_count = 0;
    std::uint64_t events = 0;
};

[[nodiscard]] Replay replay(std::string_view plugin_id,
                            std::uint32_t sessions,
                            std::vector<SyntheticInstrument> instruments,
                            Timestamp start,
                            std::uint32_t bars_per_session = 30) {
    SymbolTable symbols;
    Engine engine;

    SyntheticFeedConfig feed;
    feed.instruments = std::move(instruments);
    feed.bars_per_session = bars_per_session;
    feed.sessions = sessions;
    feed.start = start;
    feed.seed = 4242;
    engine.add_source(std::make_unique<SyntheticFeed>(feed, symbols));

    PluginLoader loader({plugin_dir()});
    loader.rescan();

    const SpreadCrossingFillModel fill_model;
    StrategyRuntimeConfig config;
    config.starting_cash = Money::from_dollars(100'000);

    StrategyInstance theory(std::string(plugin_id),
                            std::string(plugin_id),
                            loader.create(plugin_id),
                            {},
                            config,
                            &fill_model,
                            &engine.market(),
                            &symbols,
                            &engine.clock());
    engine.add_sink(&theory);
    theory.start();
    const std::uint64_t events = engine.run();
    theory.stop();

    return Replay{Published(theory), symbols.count(), events};
}

[[nodiscard]] Timestamp at(const char* iso) {
    Timestamp ts = 0;
    REQUIRE(time::from_iso8601(iso, ts));
    return ts;
}

}  // namespace

// --------------------------------------------------------------------------
// on_session_close
// --------------------------------------------------------------------------

TEST_CASE("a session close fires once per session", "[session][plugin]") {
    // Three sessions in, three closes out. The third is the one that only
    // exists because the stop shim flushes it: nothing arrives after the last
    // bar to roll the date, so without that flush a daily strategy would
    // silently never act on the final day of every backtest it ever ran.
    const Replay run = replay("session_watcher",
                              3,
                              {SyntheticInstrument{"SPY", 500.0, 0.05, 0.15, 100'000.0}},
                              at("2024-03-18T13:30:00Z"));

    INFO("bars " << run.metrics["bars"]);
    REQUIRE(run.metrics["bars"] == 90);
    REQUIRE(run.metrics["closes"] == 3);

    // Two of them fired during the run; the third fired at stop.
    REQUIRE(run.metrics["closes_at_last_bar"] == 2);
    REQUIRE(run.metrics["closes_at_stop"] == 3);
}

TEST_CASE("a session close never arrives before the session is over", "[session][plugin]") {
    const Replay run = replay("session_watcher",
                              4,
                              {SyntheticInstrument{"SPY", 500.0, 0.05, 0.15, 100'000.0}},
                              at("2024-03-18T13:30:00Z"));

    REQUIRE(run.metrics["closes"] == 4);
    REQUIRE(run.metrics["closes_out_of_order"] == 0);

    // Each close came after a whole session's worth of bars, not part of one.
    REQUIRE(run.metrics["bars_at_close"] == 120);
}

TEST_CASE("the close is dated to the session that ended", "[session][plugin]") {
    const Replay run = replay("session_watcher",
                              2,
                              {SyntheticInstrument{"SPY", 500.0, 0.05, 0.15, 100'000.0}},
                              at("2024-03-18T13:30:00Z"));

    // The last close is the one for 2024-03-19, and session_day of its
    // exclusive end lands on the following day -- which is what makes it an
    // end rather than a start.
    const double expected =
        static_cast<double>(session_day(ExchangeZone::UsEastern, at("2024-03-20T12:00:00Z")));
    REQUIRE(run.metrics["close_day"] == expected);
}

TEST_CASE("an evening bar does not open a new session", "[session][plugin]") {
    // 23:00 UTC is 19:00 in New York: the same exchange day as the afternoon
    // before it, and the next UTC day. A UTC-based boundary would report one
    // extra session here, and this is the test that would catch it.
    const Replay evening = replay("session_watcher",
                                  1,
                                  {SyntheticInstrument{"SPY", 500.0, 0.05, 0.15, 100'000.0}},
                                  at("2024-06-10T19:00:00Z"),
                                  360);

    // Six hours of one-minute bars from 15:00 to 21:00 New York time. One
    // exchange day, two UTC days, one close.
    REQUIRE(evening.metrics["bars"] == 360);
    REQUIRE(evening.metrics["closes"] == 1);
}

TEST_CASE("session detection survives a daylight-saving transition", "[session][plugin][dst]") {
    // Four sessions from Friday 2024-03-08, which steps over the spring
    // transition on Sunday the 10th: the 11th is a 23-hour day in New York and
    // its bars are an hour earlier in UTC than the week before.
    const Replay run = replay("session_watcher",
                              4,
                              {SyntheticInstrument{"SPY", 500.0, 0.05, 0.15, 100'000.0}},
                              at("2024-03-08T14:30:00Z"));

    // Four calendar days, four sessions, regardless of the clock change.
    REQUIRE(run.metrics["closes"] == 4);
    REQUIRE(run.metrics["closes_out_of_order"] == 0);
}

// --------------------------------------------------------------------------
// CrossSectionalStrategy
// --------------------------------------------------------------------------

TEST_CASE("a cross-section holds every symbol that traded", "[session][plugin][xsection]") {
    const Replay run = replay("cross_section_probe",
                              3,
                              {SyntheticInstrument{"AAA", 100.0, 0.05, 0.20, 50'000.0},
                               SyntheticInstrument{"BBB", 250.0, 0.03, 0.18, 70'000.0},
                               SyntheticInstrument{"CCC", 40.0, 0.07, 0.30, 20'000.0}},
                              at("2024-03-18T13:30:00Z"));

    REQUIRE(run.symbol_count == 3);
    REQUIRE(run.metrics["sections"] == 3);

    // Every session hands over all three names, once each.
    REQUIRE(run.metrics["last_count"] == 3);
    REQUIRE(run.metrics["total_bars"] == 9);
}

TEST_CASE("a cross-section arrives in symbol order and only at the close",
          "[session][plugin][xsection][determinism]") {
    const Replay run = replay("cross_section_probe",
                              3,
                              {SyntheticInstrument{"AAA", 100.0, 0.05, 0.20, 50'000.0},
                               SyntheticInstrument{"BBB", 250.0, 0.03, 0.18, 70'000.0},
                               SyntheticInstrument{"CCC", 40.0, 0.07, 0.30, 20'000.0}},
                              at("2024-03-18T13:30:00Z"));

    // Checked inside the plugin, because "the bars were sorted" is not a
    // number a test can read off a metric. What comes back is the count of
    // violations.
    REQUIRE(run.metrics["out_of_order"] == 0);

    // No bar in a cross-section belongs to a session later than the one being
    // closed.
    REQUIRE(run.metrics["from_the_future"] == 0);

    // The last bar of the run arrived while only two sections had been
    // delivered, so nothing was handed over mid-session.
    REQUIRE(run.metrics["sections_at_last_bar"] == 2);
}

TEST_CASE("find agrees with iteration", "[session][plugin][xsection]") {
    const Replay run = replay("cross_section_probe",
                              2,
                              {SyntheticInstrument{"AAA", 100.0, 0.05, 0.20, 50'000.0},
                               SyntheticInstrument{"BBB", 250.0, 0.03, 0.18, 70'000.0},
                               SyntheticInstrument{"CCC", 40.0, 0.07, 0.30, 20'000.0},
                               SyntheticInstrument{"DDD", 15.0, 0.02, 0.40, 10'000.0}},
                              at("2024-03-18T13:30:00Z"));

    REQUIRE(run.metrics["last_count"] == 4);
    REQUIRE(run.metrics["found_by_lookup"] == 4);
    REQUIRE(run.metrics["missing_lookup_is_null"] == 1.0);
}

TEST_CASE("the cross-sectional path replays exactly", "[session][plugin][xsection][determinism]") {
    const auto once = [] {
        return replay("cross_section_probe",
                      3,
                      {SyntheticInstrument{"AAA", 100.0, 0.05, 0.20, 50'000.0},
                       SyntheticInstrument{"BBB", 250.0, 0.03, 0.18, 70'000.0}},
                      at("2024-03-18T13:30:00Z"));
    };

    const Replay first = once();
    const Replay second = once();

    REQUIRE(first.events == second.events);
    for (const char* name : {"sections", "total_bars", "bars_seen", "out_of_order"}) {
        INFO(name);
        REQUIRE(first.metrics[name] == second.metrics[name]);
    }
    REQUIRE(first.metrics["sections"] == 3);
}

// --------------------------------------------------------------------------
// The compatibility promise
// --------------------------------------------------------------------------

TEST_CASE("a theory that ignores sessions is unaffected", "[session][plugin][abi]") {
    // buy_and_hold and sma_crossover were written before on_session_close
    // existed and do not override it. The shim layer still runs its session
    // clock for them, so this is the test that the bookkeeping is invisible to
    // a strategy that did not ask for it.
    PluginLoader loader({std::filesystem::path(MARKETSCAN_THEORY_DIR)});
    loader.rescan();

    // Both counts asserted, and exactly rather than as a floor.
    //
    // This loop discovers its inputs from a directory. An empty directory, a
    // failed build, or a rename would make it iterate nothing and pass. A floor
    // of two would also pass if a third theory silently stopped loading.
    REQUIRE_FALSE(loader.plugins().empty());
    REQUIRE(loader.plugins().size() == kExpectedTheories);

    std::size_t loaded = 0;
    for (const PluginInfo& info : loader.plugins()) {
        if (info.state == PluginState::Loaded) {
            ++loaded;
            INFO(info.id);
            REQUIRE(info.abi_version == MS_ABI_VERSION);
        }
    }
    REQUIRE(loaded == kExpectedTheories);
}

// --------------------------------------------------------------------------
// A strategy that has to cope with a pending order
// --------------------------------------------------------------------------
//
// Against an instant fill simulator these tests are unwritable: every order is
// filled by the time submit returns, so "an order is outstanding" is a state
// that never exists and the bug it causes cannot be reproduced. That is the
// gap M1b closes, and this is where it is demonstrated from the strategy's
// side of the boundary.

namespace {

/// One replay through order_lifecycle with a configurable fill model.
[[nodiscard]] Published trade_replay(int mode,
                                     const FillModelConfig& fill_config,
                                     std::uint32_t bars_per_session = 40,
                                     int size = 10) {
    SymbolTable symbols;
    Engine engine;

    SyntheticFeedConfig feed;
    feed.instruments = {SyntheticInstrument{"SPY", 100.0, 0.0, 0.10, 1'000.0}};
    feed.bars_per_session = bars_per_session;
    feed.sessions = 1;
    feed.start = at("2024-03-18T13:30:00Z");
    feed.seed = 31337;
    feed.quote_size = 1'000.0;
    engine.add_source(std::make_unique<SyntheticFeed>(feed, symbols));

    PluginLoader loader({plugin_dir()});
    loader.rescan();

    const SpreadCrossingFillModel fill_model(fill_config);
    StrategyRuntimeConfig config;
    config.starting_cash = Money::from_dollars(10'000'000);

    StrategyInstance theory("order_lifecycle",
                            "Order Lifecycle",
                            loader.create("order_lifecycle"),
                            {},
                            config,
                            &fill_model,
                            &engine.market(),
                            &symbols,
                            &engine.clock());
    theory.set_parameter("mode", static_cast<double>(mode));
    theory.set_parameter("size", static_cast<double>(size));

    engine.add_sink(&theory);
    theory.start();
    engine.run();
    theory.stop();

    return Published(theory);
}

/// Latency long enough that an order is in flight across several bars.
[[nodiscard]] FillModelConfig slow_fills() {
    FillModelConfig config;
    config.submit_latency = 5 * time::kMinute;  // bars are one minute apart
    return config;
}

}  // namespace

// The bug, reproduced. This is what it looks like when it is not guarded
// against, and the test exists so the guarded case below is not measuring
// nothing.
TEST_CASE("an unguarded strategy submits again before its order fills", "[session][plugin][exec]") {
    const Published run = trade_replay(1 /* unguarded */, slow_fills());

    REQUIRE(run["bars"] == 40);

    // One order per bar, and several alive at once the whole way through.
    REQUIRE(run["submits"] == 40);
    REQUIRE(run["peak_outstanding"] > 1);

    // It ends up holding far more than one order's worth, having decided the
    // same thing once and acted on it five times.
    REQUIRE(run["position"] > 100);
}

TEST_CASE("a guarded strategy waits for the fill it is owed", "[session][plugin][exec]") {
    const Published run = trade_replay(0 /* guarded */, slow_fills());

    REQUIRE(run["bars"] == 40);

    // Never more than one order alive, because has_pending_order() answered
    // the question the position could not.
    REQUIRE(run["peak_outstanding"] == 1);

    // Still traded -- a strategy that simply stopped submitting would also
    // have a peak of one.
    REQUIRE(run["submits"] > 1);
    REQUIRE(run["final_position"] == run["filled_quantity"]);

    // Every order either filled or was still crossing the wire when the tape
    // ran out. Nothing went missing in between, which is the accounting the
    // wrapper-side tracking exists to make checkable at all.
    REQUIRE(run["fills"] + run["final_outstanding"] == run["submits"]);

    // And it submitted materially less often than the unguarded version, which
    // is the entire difference between the two.
    const Published unguarded = trade_replay(1, slow_fills());
    REQUIRE(run["submits"] < unguarded["submits"]);
}

TEST_CASE("with no latency the guard costs nothing", "[session][plugin][exec]") {
    // The compatibility statement, from the strategy's side: an instant
    // simulator never leaves an order outstanding, so the guard never fires
    // and a guarded strategy behaves exactly like an unguarded one.
    const FillModelConfig instant;
    const Published guarded = trade_replay(0, instant);
    const Published unguarded = trade_replay(1, instant);

    REQUIRE(guarded["submits"] == 40);
    REQUIRE(guarded["submits"] == unguarded["submits"]);
    REQUIRE(guarded["final_position"] == unguarded["final_position"]);
    REQUIRE(guarded["peak_outstanding"] == 0);
}

TEST_CASE("a strategy can cancel an order that has not filled", "[session][plugin][exec]") {
    const Published run = trade_replay(2 /* cancel in flight */, slow_fills());

    REQUIRE(run["submits"] == 1);
    REQUIRE(run["cancels"] == 1);

    // It read a non-zero age off a live order, which is only possible if it
    // genuinely observed one pending.
    REQUIRE(run["saw_order_age"] == 1.0);

    // The cancel beat the fill, so nothing traded.
    REQUIRE(run["fills"] == -1.0);  // never published: on_fill never ran
    REQUIRE(run["position"] == 0.0);
}

TEST_CASE("a strategy sees each partial as its own fill", "[session][plugin][exec]") {
    FillModelConfig config;
    config.max_participation = 0.02;  // 2% of the displayed size per event
    config.impact_coefficient_bps = 0.0;

    // One order far larger than the touch, so it has to arrive in pieces.
    const Published run = trade_replay(0 /* guarded */, config, 40, 500);

    REQUIRE(run["submits"] >= 1);

    // Several fills against the very first order id. A model that emitted one
    // fill and adjusted the quantity afterwards would report one.
    REQUIRE(run["fills_on_first_order"] > 1);

    // They add up to what was asked for, and the position matches.
    REQUIRE(run["filled_quantity"] == run["final_position"]);
}

TEST_CASE("latency and partial fills replay exactly", "[session][plugin][exec][determinism]") {
    FillModelConfig config;
    config.submit_latency = 3 * time::kMinute;
    config.latency_jitter = 4 * time::kMinute;
    config.max_participation = 0.05;
    config.impact_coefficient_bps = 0.0;

    const Published first = trade_replay(0, config, 60, 200);
    const Published second = trade_replay(0, config, 60, 200);

    // Not vacuous: jitter was on, partials happened, and orders were in
    // flight. A run where nothing traded would replay identically too.
    REQUIRE(first["submits"] > 1);
    REQUIRE(first["fills"] > first["submits"]);

    for (const char* name :
         {"bars", "submits", "fills", "filled_quantity", "final_position", "peak_outstanding"}) {
        INFO(name);
        REQUIRE(first[name] == second[name]);
    }
}
