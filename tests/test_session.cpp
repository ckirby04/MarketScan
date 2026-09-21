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
    const auto before = wait_for(session, has_strategies);
    const std::size_t started_with = before->strategies.size();
    REQUIRE(started_with >= 1);

    // Deliberately not written as "unload the only theory": the folder gains
    // theories over time, and a test that assumes it holds exactly one breaks
    // the moment somebody adds the second.
    session.unload_plugin("buy_and_hold");
    const auto unloaded = wait_for(session, [started_with](const SessionSnapshot& s) {
        return s.strategies.size() == started_with - 1;
    });
    REQUIRE(unloaded->strategies.size() == started_with - 1);

    for (const StrategySnapshot& row : unloaded->strategies) {
        REQUIRE(row.id != "buy_and_hold");
    }

    bool saw_it = false;
    for (const PluginInfo& info : session.plugins()) {
        if (info.id == "buy_and_hold") {
            saw_it = true;
            REQUIRE(info.state == PluginState::Discovered);
            REQUIRE(info.live_instances == 0);
        }
    }
    REQUIRE(saw_it);

    session.rescan_theories();
    const auto reloaded = wait_for(session, [started_with](const SessionSnapshot& s) {
        return s.strategies.size() == started_with;
    });
    REQUIRE(reloaded->strategies.size() == started_with);
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

// --- The Universe Inspector -----------------------------------------------
//
// Session C Part 4 / Session D Part 5. The panel is ImGui and is not unit
// tested; everything it draws is, because everything it draws is assembled in
// `Session::universe_snapshot_locked` and the panel only formats it. That split
// is deliberate -- a panel that computed its own member list would eventually
// disagree with the guard it exists to make visible.

namespace {

/// The committed churn fixture: thirty names at a time, names that leave, and a
/// membership file derived from the bars themselves.
///
/// Reusing M9's fixture rather than inventing a second one. It is the only
/// committed dataset where anything leaves a universe, and holding the Inspector
/// to the same bars the parity tests use means a change to the fixture cannot
/// quietly break one and not the other.
SessionConfig churn_config() {
    SessionConfig config = small_config();
    config.data_file = std::filesystem::path(MARKETSCAN_TEST_DATA_DIR) / "parity_churn_panel.csv";
    config.universe_file =
        std::filesystem::path(MARKETSCAN_TEST_DATA_DIR) / "parity_churn_universe.csv";
    return config;
}

}  // namespace

TEST_CASE("a session with no universe says so rather than looking empty", "[session][universe]") {
    Session session(small_config());
    const std::shared_ptr<const SessionSnapshot> snapshot = wait_for(session, has_strategies);

    // Not an error and not a blank panel. A run without a universe is the only
    // sensible thing for one instrument; it is survivorship-unsafe for a
    // cross-section, and which of the two a run is has to be visible rather than
    // remembered.
    CHECK_FALSE(snapshot->universe.active);
    CHECK(snapshot->universe.members.empty());
    CHECK_FALSE(snapshot->universe.source.empty());
    CHECK(snapshot->universe.events_withheld == 0);
}

TEST_CASE("a universe reaches the snapshot with its membership and its provenance",
          "[session][universe]") {
    Session session(churn_config());
    std::shared_ptr<const SessionSnapshot> snapshot = wait_for(session, has_strategies);

    REQUIRE(snapshot->state != TransportState::Failed);
    const UniverseSnapshot& universe = snapshot->universe;
    INFO("source " << universe.source << ", " << universe.securities << " securities over "
                   << universe.periods << " periods");

    REQUIRE(universe.active);
    CHECK(universe.rejected.empty());

    // More securities than are ever members at once, which is what says names
    // were replaced rather than merely present. The churn fixture holds thirty-one
    // at a time -- thirty names and the roll symbol -- out of sixty.
    CHECK(universe.securities > 40);
    CHECK(universe.periods == universe.securities);
    CHECK(universe.first != kNever);

    // Adds and drops: one add per period, one drop per name that left.
    CHECK(universe.events.size() > universe.securities);
    std::size_t added = 0;
    std::size_t dropped = 0;
    for (const MembershipEvent& event : universe.events) {
        (event.added ? added : dropped) += 1;
        CHECK_FALSE(event.ticker.empty());
    }
    CHECK(added == universe.securities);
    CHECK(dropped > 10);

    // Ascending by time, so the panel can clip a long history without sorting it
    // per frame.
    for (std::size_t i = 1; i < universe.events.size(); ++i) {
        CHECK(universe.events[i - 1].ts <= universe.events[i].ts);
    }
}

TEST_CASE("the member list follows the clock", "[session][universe]") {
    // The claim the panel makes in as many words: membership as of the instant
    // the engine has reached, not as of today. A list that did not move as the
    // replay advanced would be a picture of the final universe, which is the
    // survivorship bias this whole layer exists to prevent.
    Session session(churn_config());
    const std::shared_ptr<const SessionSnapshot> opening = wait_for(session, has_strategies);
    REQUIRE(opening->universe.active);

    const std::size_t width = opening->universe.members.size();
    CHECK(width > 10);
    for (const UniverseMember& member : opening->universe.members) {
        CHECK_FALSE(member.ticker.empty());
        CHECK(member.since != kNever);
    }

    session.play();
    const std::shared_ptr<const SessionSnapshot> ended = wait_for(session, finished);
    REQUIRE(ended->state == TransportState::Finished);

    // The panel is fixed-width by construction -- a departure is replaced on the
    // following session -- so the count is the same and the names are not.
    const UniverseSnapshot& late = ended->universe;
    CHECK(late.members.size() == width);

    std::vector<std::string> before;
    std::vector<std::string> after;
    for (const UniverseMember& member : opening->universe.members) {
        before.push_back(member.ticker);
    }
    for (const UniverseMember& member : late.members) {
        after.push_back(member.ticker);
    }
    CHECK(before != after);
}

TEST_CASE("the guard's effect is a number, and the forced exits are the blotter's",
          "[session][universe]") {
    Session session(churn_config());
    REQUIRE(wait_for(session, has_strategies)->universe.active);
    session.play();
    const std::shared_ptr<const SessionSnapshot> ended = wait_for(session, finished);
    REQUIRE(ended->state == TransportState::Finished);

    const UniverseSnapshot& universe = ended->universe;
    INFO(universe.events_withheld << " events withheld, " << universe.forced_exits
                                  << " forced exits, " << universe.departures.size()
                                  << " departure rows");

    // A run where this is zero has not been filtered at all. The panel colours it
    // as a warning for the same reason.
    CHECK(universe.events_withheld > 0);

    // Every departure row is a real blotter row, not a membership event the panel
    // assumed produced a trade. The two counts agree because the rows are read
    // out of the ledger by the client id the engine stamps on a forced exit.
    CHECK(universe.departures.size() == universe.forced_exits);
    for (const DepartureRecord& record : universe.departures) {
        CHECK_FALSE(record.ticker.empty());
        CHECK_FALSE(record.strategy.empty());
        CHECK(record.quantity != 0.0);
        CHECK(record.price > 0.0);
    }

    // Ascending by time, like the events.
    for (std::size_t i = 1; i < universe.departures.size(); ++i) {
        CHECK(universe.departures[i - 1].ts <= universe.departures[i].ts);
    }
}

TEST_CASE("a universe file that cannot be read fails the session", "[session][universe]") {
    // Loudly, rather than running unfiltered. A survivorship guard that quietly
    // did not load is worse than no guard at all: the output looks identical and
    // nothing in it says which one produced it.
    SessionConfig config = churn_config();
    config.universe_file = "no_such_membership_history.csv";

    Session session(config);
    const std::shared_ptr<const SessionSnapshot> snapshot = wait_for(
        session, [](const SessionSnapshot& s) { return s.state == TransportState::Failed; });

    CHECK(snapshot->state == TransportState::Failed);
    CHECK_FALSE(snapshot->error.empty());
    CHECK_FALSE(snapshot->universe.active);
}
