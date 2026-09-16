// The engine on its own thread.
//
// Session is the only genuinely concurrent code in the project: a worker
// thread runs the replay while the GUI thread reads published snapshots. That
// is exactly the kind of code that should not be reachable only by launching a
// window and looking at it, so it is driven directly here.
//
// Every wait below is bounded. A test that hangs is worse than one that fails,
// because CI reports it as a timeout with no information in it.

#include <chrono>
#include <filesystem>
#include <thread>

#include <catch2/catch_test_macros.hpp>

#include "session.hpp"

using namespace marketscan;
using namespace marketscan::app;

namespace {

std::filesystem::path theory_dir() {
    return std::filesystem::path(MARKETSCAN_THEORY_DIR);
}

SessionConfig small_config() {
    SessionConfig config;
    config.bars_per_session = 60;
    config.sessions = 1;
    config.seed = 4242;
    config.starting_cash = Money::from_dollars(100'000);
    config.theory_roots = {theory_dir()};
    return config;
}

/// Polls until `predicate` holds or the deadline passes. Returns the last
/// snapshot either way, so a failing test can say what it actually saw.
template<typename Predicate>
std::shared_ptr<const SessionSnapshot>
wait_for(Session& session,
         Predicate predicate,
         std::chrono::milliseconds timeout = std::chrono::seconds(20)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::shared_ptr<const SessionSnapshot> snapshot = session.snapshot();
    while (std::chrono::steady_clock::now() < deadline) {
        snapshot = session.snapshot();
        if (predicate(*snapshot)) {
            return snapshot;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return snapshot;
}

bool finished(const SessionSnapshot& snapshot) {
    return snapshot.state == TransportState::Finished;
}

bool has_strategies(const SessionSnapshot& snapshot) {
    return !snapshot.strategies.empty();
}

}  // namespace

TEST_CASE("a session publishes a snapshot before it is asked to do anything", "[session]") {
    Session session(small_config());

    // Never null, from the very first frame. The GUI draws before the worker
    // has necessarily done anything, and a null here would be a crash on
    // startup rather than an empty window.
    REQUIRE(session.snapshot() != nullptr);

    const auto snapshot = wait_for(session, has_strategies);
    REQUIRE_FALSE(snapshot->strategies.empty());
    REQUIRE(snapshot->strategies.front().id == "buy_and_hold");
    REQUIRE(snapshot->strategies.front().is_benchmark);
}

TEST_CASE("playing runs the replay to the end", "[session]") {
    Session session(small_config());
    wait_for(session, has_strategies);

    session.play();
    const auto snapshot = wait_for(session, finished);

    REQUIRE(snapshot->state == TransportState::Finished);
    REQUIRE(snapshot->events > 0);
    // 60 bars, one instrument, a quote and a bar each.
    REQUIRE(snapshot->events == 120);
    REQUIRE(snapshot->progress > 0.99);

    const StrategySnapshot& row = snapshot->strategies.front();
    REQUIRE(row.trades == 1);
    REQUIRE(row.equity_curve.size() > 1);
    REQUIRE(row.slippage > Money{});
}

TEST_CASE("stepping advances exactly one event", "[session]") {
    Session session(small_config());
    wait_for(session, has_strategies);

    session.step_event();
    auto snapshot = wait_for(session, [](const SessionSnapshot& s) { return s.events >= 1; });
    REQUIRE(snapshot->events == 1);
    REQUIRE(snapshot->state == TransportState::Paused);

    session.step_event();
    snapshot = wait_for(session, [](const SessionSnapshot& s) { return s.events >= 2; });
    REQUIRE(snapshot->events == 2);
}

// Quotes outnumber bars one for one in this feed, so stepping events would
// stop on a quote. "Step one bar" has to mean a bar.
TEST_CASE("stepping one bar lands on a bar", "[session]") {
    Session session(small_config());
    wait_for(session, has_strategies);

    session.step_bar();
    const auto snapshot = wait_for(session, [](const SessionSnapshot& s) { return s.events >= 2; });

    // The quote comes first, then the bar: two events for one bar.
    REQUIRE(snapshot->events == 2);
    REQUIRE(snapshot->state == TransportState::Paused);
}

TEST_CASE("pausing stops the replay where it is", "[session]") {
    SessionConfig config = small_config();
    config.bars_per_session = 5'000;
    Session session(config);
    wait_for(session, has_strategies);

    // Throttled, so there is a replay in progress to pause. Unthrottled, ten
    // thousand events are gone before a command can reach the worker, and the
    // run would be Finished rather than Paused -- which is the correct state
    // for a finished run, and not what this test is about.
    //
    // At 10000x, the 5000 minutes of data take ~30 seconds, and the first
    // hundred events arrive in a fraction of one.
    session.set_speed(10'000.0);
    session.play();

    // Generous, because this is the one test in the file whose subject is wall
    // clock pacing and therefore the one a loaded machine can disturb. The
    // hundred events it waits for are one percent of the run, so there is no
    // risk of the replay finishing first -- only of the worker being starved.
    const auto started = wait_for(
        session,
        [](const SessionSnapshot& s) {
            return s.events > 100 || s.state == TransportState::Finished;
        },
        std::chrono::seconds(60));

    if (started->state == TransportState::Finished) {
        SKIP("the machine was too loaded to observe a replay in progress");
    }
    session.pause();

    const auto paused = wait_for(
        session, [](const SessionSnapshot& s) { return s.state == TransportState::Paused; });
    REQUIRE(paused->state == TransportState::Paused);

    const std::uint64_t at_pause = paused->events;
    REQUIRE(at_pause > 100);
    REQUIRE(at_pause < 10'000);  // genuinely partway through, not finished

    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    REQUIRE(session.snapshot()->events == at_pause);
}

// Scrubbing backwards rebuilds the run and replays it. The whole reason that
// is acceptable is that it is deterministic: the run a user scrubs into is the
// run they were already watching.
TEST_CASE("seeking reproduces the same state as playing there", "[session][determinism]") {
    SessionConfig config = small_config();

    Timestamp midpoint = 0;
    Money equity_when_played;
    std::size_t trades_when_played = 0;

    {
        Session session(config);
        wait_for(session, has_strategies);
        session.play();
        const auto done = wait_for(session, finished);
        REQUIRE(done->state == TransportState::Finished);

        // Halfway through the dataset by time.
        midpoint = done->first_ts + (done->last_ts - done->first_ts) / 2;
    }

    {
        Session session(config);
        wait_for(session, has_strategies);
        session.seek(midpoint);
        const auto sought = wait_for(session, [midpoint](const SessionSnapshot& s) {
            return s.state == TransportState::Paused && s.clock >= midpoint;
        });
        REQUIRE(sought->state == TransportState::Paused);
        REQUIRE(sought->clock <= midpoint);
        equity_when_played = sought->strategies.front().equity;
        trades_when_played = sought->strategies.front().trades;
    }

    // Seeking to the same point again gives the same answer.
    {
        Session session(config);
        wait_for(session, has_strategies);
        session.seek(midpoint);
        const auto sought = wait_for(session, [midpoint](const SessionSnapshot& s) {
            return s.state == TransportState::Paused && s.clock >= midpoint;
        });
        REQUIRE(sought->strategies.front().equity == equity_when_played);
        REQUIRE(sought->strategies.front().trades == trades_when_played);
    }
}

TEST_CASE("restarting returns to the beginning", "[session]") {
    Session session(small_config());
    wait_for(session, has_strategies);

    session.play();
    wait_for(session, finished);

    session.restart();
    const auto restarted =
        wait_for(session, [](const SessionSnapshot& s) { return s.events == 0; });

    REQUIRE(restarted->events == 0);
    REQUIRE(restarted->state == TransportState::Ready);
    REQUIRE(restarted->strategies.front().trades == 0);
    REQUIRE(restarted->strategies.front().equity == restarted->strategies.front().starting_cash);
}

TEST_CASE("the optimistic fill model can be swapped in at runtime", "[session][fills]") {
    Session session(small_config());
    wait_for(session, has_strategies);

    session.play();
    const auto honest = wait_for(session, finished);
    const Money realistic_slippage = honest->strategies.front().slippage;
    REQUIRE(honest->fill_model_realistic);
    REQUIRE(realistic_slippage > Money{});

    session.set_optimistic_fills(true);
    wait_for(session, [](const SessionSnapshot& s) { return !s.fill_model_realistic; });
    session.play();
    const auto wishful = wait_for(session, finished);

    REQUIRE_FALSE(wishful->fill_model_realistic);
    // The optimistic model fills at the last trade price, so there is no
    // spread to pay and the slippage column collapses. The difference between
    // the two runs is what a naive backtest awards itself.
    REQUIRE(wishful->strategies.front().slippage < realistic_slippage);
    REQUIRE(wishful->strategies.front().equity > honest->strategies.front().equity);
}

TEST_CASE("rescanning reloads the theories and rebuilds the run", "[session]") {
    Session session(small_config());
    wait_for(session, has_strategies);

    session.play();
    wait_for(session, finished);

    session.rescan_theories();
    const auto rescanned =
        wait_for(session, [](const SessionSnapshot& s) { return s.events == 0; });

    REQUIRE(rescanned->events == 0);
    REQUIRE_FALSE(rescanned->strategies.empty());
    REQUIRE(session.plugins().size() >= 1);
}

// Unloading while the run holds instances has to work: the loader refuses to
// free a module anything is using, so the session must drop its strategies
// first.
TEST_CASE("a theory can be unloaded and loaded again", "[session][plugin]") {
    Session session(small_config());
    wait_for(session, has_strategies);

    session.unload_plugin("buy_and_hold");
    const auto unloaded =
        wait_for(session, [](const SessionSnapshot& s) { return s.strategies.empty(); });
    REQUIRE(unloaded->strategies.empty());

    for (const PluginInfo& info : session.plugins()) {
        if (info.id == "buy_and_hold") {
            REQUIRE(info.state == PluginState::Discovered);
            REQUIRE(info.live_instances == 0);
        }
    }

    session.rescan_theories();
    const auto reloaded = wait_for(session, has_strategies);
    REQUIRE_FALSE(reloaded->strategies.empty());
}

TEST_CASE("a parameter can be changed while the session is live", "[session][plugin]") {
    Session session(small_config());
    auto snapshot = wait_for(session, [](const SessionSnapshot& s) {
        return !s.strategies.empty() && !s.strategies.front().params.empty();
    });

    REQUIRE_FALSE(snapshot->strategies.front().params.empty());
    REQUIRE(snapshot->strategies.front().param_values.front() == 0.0);  // symbol_index

    session.set_parameter("buy_and_hold", "cash_reserve_pct", 7.5);
    snapshot = wait_for(session, [](const SessionSnapshot& s) {
        return !s.strategies.empty() && s.strategies.front().param_values.size() > 1 &&
               s.strategies.front().param_values[1] == 7.5;
    });
    REQUIRE(snapshot->strategies.front().param_values[1] == 7.5);
}

TEST_CASE("a missing data file fails visibly rather than silently", "[session]") {
    SessionConfig config = small_config();
    config.data_file = "does/not/exist.csv";

    Session session(config);
    const auto snapshot = wait_for(
        session, [](const SessionSnapshot& s) { return s.state == TransportState::Failed; });

    REQUIRE(snapshot->state == TransportState::Failed);
    REQUIRE_FALSE(snapshot->error.empty());
}

TEST_CASE("a session destroyed while running shuts down cleanly", "[session]") {
    // The worker owns plugin instances and an engine; tearing that down from
    // under a running replay is the shutdown path the window close button
    // takes, so it gets exercised here rather than only by a person clicking.
    SessionConfig config = small_config();
    config.bars_per_session = 100'000;

    {
        Session session(config);
        wait_for(session, has_strategies);
        session.play();
        wait_for(session, [](const SessionSnapshot& s) { return s.events > 10; });
    }
    SUCCEED("session destroyed mid-replay without hanging or crashing");
}

// --------------------------------------------------------------------------
// The USB port
// --------------------------------------------------------------------------

// The headline claim, tested rather than asserted: a compiled strategy the
// host has never seen, dropped into the folder and picked up on demand, with
// no recompile of the host and no change to host code.
//
// The DLL used is the one built from theories/_template -- a theory the
// application does not normally load, and which names nothing in the host.
TEST_CASE("a new theory dropped into the folder appears on rescan",
          "[session][plugin][hotreload]") {
    const std::filesystem::path template_dll{MARKETSCAN_TEMPLATE_DLL};
    if (!std::filesystem::exists(template_dll)) {
        SKIP("the template theory was not built; nothing to drop in");
    }

    // A folder of its own, so this cannot disturb another test's theories.
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        ("marketscan_usb_" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count() & 0xFFFFFF));
    std::filesystem::create_directories(root);

    std::error_code ec;
    std::filesystem::copy_file(theory_dir() / "buy_and_hold.dll",
                               root / "buy_and_hold.dll",
                               std::filesystem::copy_options::overwrite_existing,
                               ec);
    REQUIRE_FALSE(ec);

    SessionConfig config = small_config();
    config.theory_roots = {root};

    {
        Session session(config);

        const auto before = wait_for(session, has_strategies);
        REQUIRE(before->strategies.size() == 1);
        REQUIRE(before->strategies.front().id == "buy_and_hold");

        // The moment that matters: a DLL the running process has never heard
        // of arrives on disk.
        std::filesystem::copy_file(template_dll,
                                   root / "my_theory.dll",
                                   std::filesystem::copy_options::overwrite_existing,
                                   ec);
        REQUIRE_FALSE(ec);

        // The loader notices without being told, and raises the badge the
        // plugin manager shows. It does not act on it: reloading a strategy
        // out from under a running replay would invalidate everything on
        // screen without saying so.
        REQUIRE(session.theories_changed_on_disk());

        session.rescan_theories();
        const auto after =
            wait_for(session, [](const SessionSnapshot& s) { return s.strategies.size() == 2; });

        REQUIRE(after->strategies.size() == 2);

        bool found = false;
        for (const StrategySnapshot& row : after->strategies) {
            if (row.id == "my_theory") {
                found = true;
                REQUIRE(row.display_name == "My Theory");
                // Its parameters came across the boundary too, which is what
                // lets the GUI build controls for a theory it knows nothing
                // about.
                REQUIRE(row.params.size() == 3);
                REQUIRE(row.params[0].name == "lookback");
            }
        }
        REQUIRE(found);

        // And it runs, through the same engine as everything else.
        session.play();
        const auto done = wait_for(session, finished);
        REQUIRE(done->state == TransportState::Finished);
        REQUIRE(done->strategies.size() == 2);
        for (const StrategySnapshot& row : done->strategies) {
            REQUIRE(row.equity_curve.size() > 1);
        }
    }

    std::filesystem::remove_all(root, ec);
}
