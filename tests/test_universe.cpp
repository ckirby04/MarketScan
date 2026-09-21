// Security identity, universe membership, and the survivorship guard.
//
// This is the load-bearing file of the project. Two properties are asserted
// here and everything downstream is worth nothing without them:
//
//   1. A strategy is never shown a security that was not an index member that
//      day, and has no API by which to ask about one. Structural, not a rule.
//
//   2. A ticker reused by a different company is two securities. Nothing --
//      no position, no average cost, no price series -- crosses the boundary
//      between them.
//
// The second is the one that would be invisible if it were wrong. A welded
// price series does not crash and does not look odd; it just manufactures a
// return, usually an enormous one, and every statistic computed downstream of
// it inherits the lie.

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "marketscan/core/engine.hpp"
#include "marketscan/data/bar_store.hpp"
#include "marketscan/data/daily_feed.hpp"
#include "marketscan/data/security_master.hpp"
#include "marketscan/data/universe.hpp"
#include "marketscan/exec/fill_model.hpp"
#include "marketscan/strategy/plugin_loader.hpp"
#include "marketscan/strategy/strategy_instance.hpp"

using namespace marketscan;
using Catch::Matchers::ContainsSubstring;

namespace {

[[nodiscard]] Timestamp on(int year, unsigned month, unsigned day) {
    return bar_store::daily_close_stamp(calendar::days_from_civil(year, month, day));
}

[[nodiscard]] constexpr Duration years(double n) {
    return static_cast<Duration>(n * 365.0 * 24 * 3600 * 1'000'000'000LL);
}

class ScratchDir {
public:
    ScratchDir() {
        static int counter = 0;
        path_ = std::filesystem::temp_directory_path() /
                ("marketscan_universe_" + std::to_string(++counter) + "_" +
                 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count() &
                                0xFFFFFF));
        std::filesystem::create_directories(path_);
    }

    ScratchDir(const ScratchDir&) = delete;
    ScratchDir& operator=(const ScratchDir&) = delete;
    ScratchDir(ScratchDir&&) = delete;
    ScratchDir& operator=(ScratchDir&&) = delete;

    ~ScratchDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }

    [[nodiscard]] std::filesystem::path file(const std::string& name) const { return path_ / name; }

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

/// A snapshot file of the shape fja05680/sp500 publishes: a date, then the
/// tickers that were members on it.
void write_snapshots(const std::filesystem::path& path,
                     const std::vector<std::pair<std::string, std::vector<std::string>>>& days) {
    std::ofstream out(path);
    out << "date,tickers\n";
    for (const auto& [date, members] : days) {
        out << date;
        for (const std::string& ticker : members) {
            out << "," << ticker;
        }
        out << "\n";
    }
}

/// Daily bars for one ticker across a date range, at a given price level.
[[nodiscard]] BarSeries
bars_between(const std::string& ticker, int from_year, int to_year, double price) {
    BarSeries out;
    out.ticker = ticker;
    const std::int64_t first = calendar::days_from_civil(from_year, 1, 2);
    const std::int64_t last = calendar::days_from_civil(to_year, 12, 20);
    for (std::int64_t day = first; day <= last; day += 7) {  // weekly, to keep tests quick
        StoredBar bar;
        bar.ts = bar_store::daily_close_stamp(day);
        bar.open = price;
        bar.close = price;
        bar.high = price * 1.01;
        bar.low = price * 0.99;
        bar.volume = 1'000'000.0;
        out.bars.push_back(bar);
    }
    return out;
}

/// Whether `master.resolve(ticker)` compiles.
///
/// Selecting a deleted overload inside a requires-expression makes the
/// requirement unsatisfied rather than making the program ill-formed, which is
/// what lets "this does not compile" be asserted by something that does.
template<typename T>
concept kDatelessResolveCompiles = requires(const T& master) { master.resolve("XYZ"); };

template<typename T>
concept kDatedResolveCompiles = requires(const T& master) { master.resolve("XYZ", Timestamp{}); };

}  // namespace

// --------------------------------------------------------------------------
// A ticker is not an identity
// --------------------------------------------------------------------------

TEST_CASE("resolving a ticker without a date does not compile", "[universe][identity]") {
    // The enforcement the specification asks for, and the reason it is a
    // deleted overload rather than a comment: "which company is XYZ" has no
    // answer without a date, so asking has to be impossible rather than
    // discouraged.
    //
    // Uncommenting the line below is a compile error, which is the test. It
    // cannot be asserted at runtime, because the whole point is that the code
    // does not exist to run.
    //
    //     master.resolve("XYZ");     // error: use of deleted function
    //
    STATIC_REQUIRE(!kDatelessResolveCompiles<SecurityMaster>);

    // And the dated form does, so the assertion above is about the overload
    // being deleted rather than about resolve not existing at all.
    STATIC_REQUIRE(kDatedResolveCompiles<SecurityMaster>);
}

TEST_CASE("a rename keeps one security", "[universe][identity]") {
    SymbolTable symbols;
    SecurityMaster master(symbols);

    const SecurityId fb = master.declare("FB", on(2012, 5, 18), on(2022, 6, 9));

    // Declared, not inferred, and that is the point. From ticker-keyed
    // membership data a rename is indistinguishable from one name leaving and
    // another arriving: the two events look identical and nothing in the
    // source says otherwise. Only something that knows about the company can
    // tell them apart.
    master.add_ticker(fb, "META", on(2022, 6, 9), kForever);
    const SecurityId meta = master.resolve("META", on(2023, 1, 5));

    // A rename is one company with two names, and the id is what says so. A
    // model that made these two securities would break a continuity that
    // genuinely exists: a position held through the change would look like a
    // sale and a purchase.
    REQUIRE(fb == meta);
    REQUIRE(master.size() == 1);

    REQUIRE(master.resolve("FB", on(2015, 1, 5)) == fb);
    REQUIRE(master.resolve("META", on(2023, 1, 5)) == fb);

    // And each name resolves only inside its own period.
    REQUIRE(master.resolve("META", on(2015, 1, 5)) == kInvalidSecurity);
    REQUIRE(master.resolve("FB", on(2023, 1, 5)) == kInvalidSecurity);

    // What it was called on a given day, which is what a blotter prints.
    REQUIRE(master.ticker_at(fb, on(2015, 1, 5)) == "FB");
    REQUIRE(master.ticker_at(fb, on(2023, 1, 5)) == "META");
}

TEST_CASE("a ticker cannot belong to two securities at once", "[universe][identity]") {
    SymbolTable symbols;
    SecurityMaster master(symbols);

    const SecurityId one = master.declare("AAA", on(2000, 1, 1), kForever);
    const SecurityId two = master.declare("BBB", on(2000, 1, 1), kForever);

    // Overlapping periods for one ticker would make resolution ambiguous on
    // every date they shared, and resolution is the whole product. Refused
    // rather than settled by a tie-break.
    REQUIRE_THROWS_AS(master.add_ticker(two, "AAA", on(2005, 1, 1), kForever),
                      std::invalid_argument);

    // After the first one has released the name, it is available.
    REQUIRE_NOTHROW(master.add_ticker(one, "CCC", on(2010, 1, 1), kForever));
    REQUIRE(master.resolve("CCC", on(2012, 1, 1)) == one);
}

TEST_CASE("a reused ticker is two securities", "[universe][identity]") {
    SymbolTable symbols;
    SecurityMaster master(symbols);

    const SecurityId first = master.declare("XYZ", on(1998, 1, 2), on(2003, 6, 30));
    const SecurityId second =
        master.declare("XYZ", on(2011, 4, 1), kForever, "gap of 2832 days after 2003-06-30");

    // The property everything downstream depends on. One string, two
    // companies, two ids -- because treating them as one welds the first
    // company's price history to the second's and manufactures a return that
    // never existed.
    REQUIRE(first != second);
    REQUIRE(master.size() == 2);
    REQUIRE(master.incarnations("XYZ") == 2);

    REQUIRE(master.resolve("XYZ", on(2000, 1, 5)) == first);
    REQUIRE(master.resolve("XYZ", on(2015, 1, 5)) == second);

    // The dead years in between belong to neither.
    REQUIRE(master.resolve("XYZ", on(2007, 1, 5)) == kInvalidSecurity);

    // Both display as XYZ. The internal key that keeps them apart is an
    // implementation detail and no strategy ever sees it.
    REQUIRE(master.ticker_at(first, on(2000, 1, 5)) == "XYZ");
    REQUIRE(master.ticker_at(second, on(2015, 1, 5)) == "XYZ");
    REQUIRE(master.find(second)->split_reason.find("gap") != std::string::npos);
}

TEST_CASE("an unknown ticker resolves to nothing", "[universe][identity]") {
    SymbolTable symbols;
    SecurityMaster master(symbols);
    master.declare("AAA", on(2000, 1, 1), kForever);

    REQUIRE(master.resolve("BBB", on(2010, 1, 1)) == kInvalidSecurity);
    REQUIRE(master.find(kInvalidSecurity) == nullptr);
    REQUIRE(master.find(999) == nullptr);
}

// --------------------------------------------------------------------------
// The split heuristic, and its audit trail
// --------------------------------------------------------------------------

TEST_CASE("a long gap splits and a short one does not", "[universe][identity]") {
    const ScratchDir dir;
    // KEEP leaves for nine months and comes back. SPLT leaves for eight years.
    write_snapshots(dir.file("history.csv"),
                    {
                        {"2000-01-03", {"KEEP", "SPLT", "STAY"}},
                        {"2001-01-03", {"STAY"}},
                        {"2001-10-03", {"KEEP", "STAY"}},
                        {"2009-01-05", {"KEEP", "SPLT", "STAY"}},
                        {"2010-01-05", {"KEEP", "SPLT", "STAY"}},
                    });

    SymbolTable symbols;
    SecurityMaster master(symbols);
    const SnapshotMembershipSource source(dir.file("history.csv"));
    const MembershipHistory history = build_membership(source, master);

    // STAY never left: no gap, no decision to make.
    REQUIRE(master.incarnations("STAY") == 1);

    // KEEP's gap is well under two years.
    REQUIRE(master.incarnations("KEEP") == 1);

    // SPLT's is eight, so it is a different company now.
    REQUIRE(master.incarnations("SPLT") == 2);

    // Every gap is recorded, split or not. A decision that nearly went the
    // other way is exactly the one worth being able to look up.
    REQUIRE(history.decisions.size() >= 2);
    bool saw_split = false;
    bool saw_keep = false;
    for (const IdentityDecision& decision : history.decisions) {
        saw_split = saw_split || (decision.ticker == "SPLT" && decision.split);
        saw_keep = saw_keep || (decision.ticker == "KEEP" && !decision.split);
    }
    REQUIRE(saw_split);
    REQUIRE(saw_keep);
}

TEST_CASE("an override beats the threshold, both ways", "[universe][identity]") {
    const ScratchDir dir;
    write_snapshots(dir.file("history.csv"),
                    {
                        {"2000-01-03", {"AAA", "BBB", "ANCHOR"}},
                        {"2001-01-03", {"ANCHOR"}},
                        {"2001-06-03", {"AAA", "ANCHOR"}},         // 5 months: normally kept
                        {"2010-01-05", {"AAA", "BBB", "ANCHOR"}},  // 9 years: normally split
                        {"2011-01-05", {"AAA", "BBB", "ANCHOR"}},
                    });

    std::ofstream overrides(dir.file("overrides.csv"));
    overrides << "ticker,action,resumed_on,note\n";
    overrides << "AAA,split,2001-06-03,known reverse merger\n";
    overrides << "BBB,keep,2010-01-05,same company, index reshuffle\n";
    overrides.close();

    SymbolTable symbols;
    SecurityMaster master(symbols);
    const IdentityRules rules = load_overrides(dir.file("overrides.csv"));
    REQUIRE(rules.overrides.size() == 2);

    const SnapshotMembershipSource source(dir.file("history.csv"));
    const MembershipHistory history = build_membership(source, master, rules);

    // A short gap forced to split, and a long one forced to stay.
    REQUIRE(master.incarnations("AAA") >= 2);
    REQUIRE(master.incarnations("BBB") == 1);

    bool override_recorded = false;
    for (const IdentityDecision& decision : history.decisions) {
        override_recorded = override_recorded || decision.rule == "override";
    }
    REQUIRE(override_recorded);
}

TEST_CASE("the decision report records every call", "[universe][identity]") {
    const ScratchDir dir;
    write_snapshots(dir.file("history.csv"),
                    {
                        {"2000-01-03", {"GONE", "HERE"}},
                        {"2001-01-03", {"HERE"}},
                        {"2009-01-05", {"GONE", "HERE"}},
                        {"2010-01-05", {"GONE", "HERE"}},
                    });

    SymbolTable symbols;
    SecurityMaster master(symbols);
    const SnapshotMembershipSource source(dir.file("history.csv"));
    const MembershipHistory history = build_membership(source, master);

    write_decision_report(
        dir.file("IDENTITY_DECISIONS.md"), history, master, IdentityRules{}, source.describe());

    std::ifstream report(dir.file("IDENTITY_DECISIONS.md"));
    const std::string text((std::istreambuf_iterator<char>(report)),
                           std::istreambuf_iterator<char>());

    // The threshold is arguable; the audit trail is what makes it reviewable
    // rather than magic. So the report has to carry the rule that was applied,
    // the gap it was applied to, and what came out.
    REQUIRE_THAT(text, ContainsSubstring("Split threshold"));
    REQUIRE_THAT(text, ContainsSubstring("GONE"));
    REQUIRE_THAT(text, ContainsSubstring("Over-splitting is the safe direction"));
    REQUIRE_THAT(text, ContainsSubstring("When CRSP arrives"));
}

// --------------------------------------------------------------------------
// Universe membership
// --------------------------------------------------------------------------

TEST_CASE("membership is a function of the date", "[universe]") {
    Universe universe;
    universe.add(1, on(2010, 1, 1), on(2015, 1, 1));
    universe.add(2, on(2012, 1, 1), kForever);
    universe.build();

    REQUIRE(universe.members_on(on(2011, 6, 1)) == std::vector<SecurityId>{1});
    REQUIRE(universe.members_on(on(2013, 6, 1)) == std::vector<SecurityId>{1, 2});
    REQUIRE(universe.members_on(on(2016, 6, 1)) == std::vector<SecurityId>{2});
    REQUIRE(universe.members_on(on(2000, 6, 1)).empty());

    REQUIRE(universe.is_member(1, on(2011, 6, 1)));
    REQUIRE_FALSE(universe.is_member(1, on(2016, 6, 1)));
    REQUIRE(universe.membership_ends(1, on(2011, 6, 1)) == on(2015, 1, 1));
    REQUIRE(universe.membership_ends(2, on(2013, 6, 1)) == kForever);
}

TEST_CASE("members come back in id order", "[universe][determinism]") {
    // Ascending by id, and a vector rather than a set, because this reaches
    // ranking and therefore order-emitting code. A decile computed from a
    // differently-ordered list is a different decile.
    Universe universe;
    for (SecurityId id : {7U, 3U, 9U, 1U, 5U}) {
        universe.add(id, on(2010, 1, 1), kForever);
    }
    universe.build();

    const std::vector<SecurityId>& members = universe.members_on(on(2012, 1, 1));
    REQUIRE(members == std::vector<SecurityId>{1, 3, 5, 7, 9});
    REQUIRE(std::is_sorted(members.begin(), members.end()));
}

TEST_CASE("a universe is read-only once built", "[universe]") {
    Universe universe;
    universe.add(1, on(2010, 1, 1), kForever);
    universe.build();
    // A universe that could change mid-replay would make the run depend on
    // when the question was asked.
    REQUIRE_THROWS_AS(universe.add(2, on(2011, 1, 1), kForever), std::logic_error);
}

// --------------------------------------------------------------------------
// The structural guard
// --------------------------------------------------------------------------

namespace {

/// One replay of a daily cache through one theory, with an optional universe.
struct GuardRun {
    std::uint64_t withheld = 0;
    std::uint64_t forced_exits = 0;
    std::vector<std::pair<SymbolId, Timestamp>> seen;
    Money final_cash;
    std::size_t trades = 0;
};

/// Records which securities it was shown, and when.
class Watcher final : public IEventSink {
public:
    void on_bar(const MS_Bar& bar) override { seen.emplace_back(bar.symbol, bar.ts); }

    std::vector<std::pair<SymbolId, Timestamp>> seen;
};

}  // namespace

// The test the specification names: X is a member only during 2015, the replay
// covers 2013 to 2017, and the strategy must have observed X on no date
// outside 2015.
TEST_CASE("a strategy never sees a security outside its membership", "[universe][guard]") {
    const ScratchDir dir;
    bar_store::write(dir.file("ALWAYS.msb"), bars_between("ALWAYS", 2013, 2017, 100.0));
    bar_store::write(dir.file("X.msb"), bars_between("X", 2013, 2017, 50.0));

    SymbolTable symbols;
    DailyFeedOptions options;
    options.synthesize_quotes = false;

    Engine engine;
    engine.add_source(std::make_unique<DailyBarFeed>(dir.path(), symbols, options));

    const SymbolId always = symbols.find("ALWAYS");
    const SymbolId x = symbols.find("X");
    REQUIRE(always != kInvalidSymbol);
    REQUIRE(x != kInvalidSymbol);

    Universe universe;
    universe.add(always, on(2013, 1, 1), kForever);
    universe.add(x, on(2015, 1, 1), on(2016, 1, 1));
    universe.build();

    PluginLoader loader({std::filesystem::path(MARKETSCAN_TEST_PLUGIN_DIR)});
    loader.rescan();

    const SpreadCrossingFillModel fill_model;
    StrategyRuntimeConfig config;
    config.starting_cash = Money::from_dollars(1'000'000);

    StrategyInstance theory("watcher",
                            "Watcher",
                            loader.create("session_watcher"),
                            {},
                            config,
                            &fill_model,
                            &engine.market(),
                            &symbols,
                            &engine.clock());
    theory.set_universe(&universe, nullptr);

    // A second sink with no universe, to prove the data really did contain
    // the bars the strategy was not shown.
    Watcher unfiltered;
    engine.add_sink(&theory);
    engine.add_sink(&unfiltered);
    theory.start();
    engine.run();
    theory.stop();

    // The data has X throughout 2013-2017.
    std::size_t x_in_data = 0;
    for (const auto& [symbol, ts] : unfiltered.seen) {
        x_in_data += symbol == x ? 1 : 0;
    }
    REQUIRE(x_in_data > 200);

    // And the strategy was shown it on no date outside 2015.
    INFO("withheld " << theory.events_withheld());
    REQUIRE(theory.events_withheld() > 0);

    // Every withheld event is an X bar outside 2015 -- ALWAYS was a member
    // throughout, so nothing of its was withheld.
    std::size_t x_outside_2015 = 0;
    for (const auto& [symbol, ts] : unfiltered.seen) {
        if (symbol != x) {
            continue;
        }
        if (ts < on(2015, 1, 1) || ts >= on(2016, 1, 1)) {
            ++x_outside_2015;
        }
    }
    REQUIRE(theory.events_withheld() == x_outside_2015);
}

TEST_CASE("the host context offers no way to ask about a non-member", "[universe][guard]") {
    SymbolTable symbols;
    const SymbolId member = symbols.intern("IN");
    const SymbolId outsider = symbols.intern("OUT");

    Universe universe;
    universe.add(member, on(2010, 1, 1), kForever);
    universe.build();

    Engine engine;
    const SpreadCrossingFillModel fill_model;
    StrategyRuntimeConfig config;

    PluginLoader loader({std::filesystem::path(MARKETSCAN_TEST_PLUGIN_DIR)});
    loader.rescan();

    StrategyInstance theory("probe",
                            "Probe",
                            loader.create("session_watcher"),
                            {},
                            config,
                            &fill_model,
                            &engine.market(),
                            &symbols,
                            &engine.clock());
    theory.set_universe(&universe, nullptr);
    engine.clock().reset(on(2012, 6, 1));

    // Filtering is not a policy applied at a checkpoint; this instance is the
    // only implementation of the host context a plugin has, so there is no
    // second route to ask.
    HostServices& host = theory;

    REQUIRE(host.symbol_id("IN") == member);
    REQUIRE(host.symbol_id("OUT") == kInvalidSymbol);
    REQUIRE(host.position(outsider) == 0.0);
    REQUIRE(host.symbol_count() == 1);  // members today, not everything in the file
}

TEST_CASE("without a universe nothing is filtered", "[universe][guard]") {
    // --no-universe, and the intraday case. The guard has to be absent rather
    // than merely permissive, or every existing result would move.
    SymbolTable symbols;
    const SymbolId a = symbols.intern("A");

    Engine engine;
    const SpreadCrossingFillModel fill_model;
    StrategyRuntimeConfig config;

    PluginLoader loader({std::filesystem::path(MARKETSCAN_TEST_PLUGIN_DIR)});
    loader.rescan();

    StrategyInstance theory("open",
                            "Open",
                            loader.create("session_watcher"),
                            {},
                            config,
                            &fill_model,
                            &engine.market(),
                            &symbols,
                            &engine.clock());

    REQUIRE(theory.universe() == nullptr);
    HostServices& host = theory;
    REQUIRE(host.symbol_id("A") == a);
    REQUIRE(host.symbol_count() == 1);
    REQUIRE(theory.events_withheld() == 0);
}

// --------------------------------------------------------------------------
// Removal from the universe
// --------------------------------------------------------------------------

TEST_CASE("a held security that leaves the universe is liquidated", "[universe][guard][exit]") {
    const ScratchDir dir;
    bar_store::write(dir.file("LEAVER.msb"), bars_between("LEAVER", 2013, 2017, 80.0));

    SymbolTable symbols;
    Engine engine;
    engine.add_source(std::make_unique<DailyBarFeed>(dir.path(), symbols));

    const SymbolId leaver = symbols.find("LEAVER");
    REQUIRE(leaver != kInvalidSymbol);

    Universe universe;
    universe.add(leaver, on(2013, 1, 1), on(2015, 7, 1));
    universe.build();

    PluginLoader loader({std::filesystem::path(MARKETSCAN_THEORY_DIR)});
    loader.rescan();

    const SpreadCrossingFillModel fill_model;
    StrategyRuntimeConfig config;
    config.starting_cash = Money::from_dollars(1'000'000);

    StrategyInstance theory("holder",
                            "Holder",
                            loader.create("buy_and_hold"),
                            {},
                            config,
                            &fill_model,
                            &engine.market(),
                            &symbols,
                            &engine.clock());
    theory.set_universe(&universe, nullptr);
    engine.add_sink(&theory);
    theory.start();
    engine.run();
    theory.stop();

    // Bought while a member, sold when it stopped being one, and flat after.
    REQUIRE(theory.forced_exits() == 1);
    REQUIRE(theory.ledger().position(leaver) == Catch::Approx(0.0));
    REQUIRE(theory.ledger().check_invariant());

    // The proceeds are in cash rather than having evaporated.
    REQUIRE(theory.ledger().cash() > Money::from_dollars(900'000));

    // The forced exit is marked in the blotter as something the engine did,
    // not something the strategy decided.
    bool marked = false;
    for (const TradeRecord& trade : theory.ledger().blotter()) {
        marked = marked || trade.client_order_id == kForcedExitClientId;
    }
    REQUIRE(marked);
}

TEST_CASE("nothing is liquidated while membership holds", "[universe][guard][exit]") {
    const ScratchDir dir;
    bar_store::write(dir.file("STAYER.msb"), bars_between("STAYER", 2013, 2017, 80.0));

    SymbolTable symbols;
    Engine engine;
    engine.add_source(std::make_unique<DailyBarFeed>(dir.path(), symbols));
    const SymbolId stayer = symbols.find("STAYER");

    Universe universe;
    universe.add(stayer, on(2013, 1, 1), kForever);
    universe.build();

    PluginLoader loader({std::filesystem::path(MARKETSCAN_THEORY_DIR)});
    loader.rescan();

    const SpreadCrossingFillModel fill_model;
    StrategyRuntimeConfig config;
    config.starting_cash = Money::from_dollars(1'000'000);

    StrategyInstance theory("holder",
                            "Holder",
                            loader.create("buy_and_hold"),
                            {},
                            config,
                            &fill_model,
                            &engine.market(),
                            &symbols,
                            &engine.clock());
    theory.set_universe(&universe, nullptr);
    engine.add_sink(&theory);
    theory.start();
    engine.run();
    theory.stop();

    REQUIRE(theory.forced_exits() == 0);
    REQUIRE(theory.ledger().position(stayer) > 0.0);
}

// --------------------------------------------------------------------------
// Identity and the ledger, together
// --------------------------------------------------------------------------

TEST_CASE("nothing crosses the boundary between two incarnations of a ticker",
          "[universe][identity][guard]") {
    // The failure this whole model exists to prevent. XYZ trades at $10 until
    // 2003 and a different company trades at $400 under the same string from
    // 2011. Welded together, that is a fortyfold return that never happened.
    const ScratchDir dir;

    SymbolTable symbols;
    SecurityMaster master(symbols);
    const SecurityId first = master.declare("XYZ", on(1998, 1, 1), on(2003, 7, 1));
    const SecurityId second = master.declare("XYZ", on(2011, 1, 1), kForever, "gap of 3106 days");
    REQUIRE(first != second);

    // Each incarnation gets its own file, under the internal key the master
    // interned it as -- which is what keeps the two price series apart on disk
    // as well as in memory.
    BarSeries early = bars_between("XYZ", 1998, 2002, 10.0);
    BarSeries late = bars_between("XYZ", 2011, 2015, 400.0);
    early.ticker = std::string(symbols.name(first));
    late.ticker = std::string(symbols.name(second));
    bar_store::write(dir.file(early.ticker + ".msb"), early);
    bar_store::write(dir.file(late.ticker + ".msb"), late);

    SymbolTable replay_symbols;
    DailyFeedOptions options;
    options.synthesize_quotes = false;
    DailyBarFeed feed(dir.path(), replay_symbols, options);

    // Two distinct instruments in the replay, not one.
    REQUIRE(feed.symbols().size() == 2);

    // And no price series crosses: the early one never exceeds $11 and the
    // late one never drops below $390. A welded series would run between them.
    MarketEvent event;
    double early_max = 0.0;
    double late_min = 1e9;
    while (feed.next(event)) {
        if (event.type != EventType::Bar) {
            continue;
        }
        if (event.bar.ts < on(2005, 1, 1)) {
            early_max = std::max(early_max, event.bar.close);
        } else {
            late_min = std::min(late_min, event.bar.close);
        }
    }
    REQUIRE(early_max < 11.0);
    REQUIRE(late_min > 390.0);

    // Resolution by date sends each era to the right one.
    REQUIRE(master.resolve("XYZ", on(2000, 1, 1)) == first);
    REQUIRE(master.resolve("XYZ", on(2013, 1, 1)) == second);
}

TEST_CASE("a position in one incarnation is not a position in the other",
          "[universe][identity][guard]") {
    SymbolTable symbols;
    SecurityMaster master(symbols);
    const SecurityId first = master.declare("XYZ", on(1998, 1, 1), on(2003, 7, 1));
    const SecurityId second = master.declare("XYZ", on(2011, 1, 1), kForever, "reused");

    Ledger ledger(Money::from_dollars(100'000));
    ledger.apply_fill(on(2000, 1, 1), 1, 0, first, MS_SIDE_BUY, 100.0, 10.0, 10.0, Money{});

    // The whole point of a permanent id: a strategy holding the 1998 company
    // owns nothing of the 2011 one, and no ledger lookup can confuse them.
    REQUIRE(ledger.position(first) == Catch::Approx(100.0));
    REQUIRE(ledger.position(second) == Catch::Approx(0.0));
    REQUIRE(ledger.check_invariant());
}

TEST_CASE("the feed resolves each bar's identity by its own date", "[universe][identity][feed]") {
    // Without this, a file's ticker is interned once and every bar in it gets
    // the same id -- correct until the string has been reused, and then it
    // welds one company's price history to another's.
    const ScratchDir dir;

    SymbolTable symbols;
    SecurityMaster master(symbols);
    const SecurityId early = master.declare("XYZ", on(1998, 1, 1), on(2003, 7, 1));
    const SecurityId late = master.declare("XYZ", on(2011, 1, 1), kForever, "reused");

    // ONE file, under ONE ticker, spanning both eras and the dead years
    // between them -- which is exactly the shape a vendor download has.
    BarSeries whole = bars_between("XYZ", 1998, 2015, 10.0);
    bar_store::write(dir.file("XYZ.msb"), whole);

    DailyFeedOptions options;
    options.synthesize_quotes = false;
    options.master = &master;
    DailyBarFeed feed(dir.path(), symbols, options);

    std::size_t to_early = 0;
    std::size_t to_late = 0;
    MarketEvent event;
    while (feed.next(event)) {
        if (event.type != EventType::Bar) {
            continue;
        }
        if (event.bar.symbol == early) {
            ++to_early;
            REQUIRE(event.bar.ts < on(2003, 7, 1));
        } else if (event.bar.symbol == late) {
            ++to_late;
            REQUIRE(event.bar.ts >= on(2011, 1, 1));
        } else {
            FAIL("a bar was assigned to neither incarnation");
        }
    }

    // One file, two instruments, and the split fell where the dates say it
    // should rather than where the filename does.
    REQUIRE(to_early > 200);
    REQUIRE(to_late > 200);

    // The eight dead years belong to neither company, so those bars are
    // dropped and counted rather than silently attached to one of them.
    REQUIRE(feed.unidentified_bars() > 300);
    REQUIRE(to_early + to_late + feed.unidentified_bars() == whole.bars.size());
}

TEST_CASE("without a master the feed interns once, as it always did",
          "[universe][identity][feed]") {
    // The compatibility floor: every daily replay before M3 assigned one id per
    // file, and a run with no identity layer still does.
    const ScratchDir dir;
    bar_store::write(dir.file("XYZ.msb"), bars_between("XYZ", 1998, 2015, 10.0));

    SymbolTable symbols;
    DailyFeedOptions options;
    options.synthesize_quotes = false;
    DailyBarFeed feed(dir.path(), symbols, options);

    REQUIRE(feed.symbols().size() == 1);
    REQUIRE(feed.unidentified_bars() == 0);
}

// --------------------------------------------------------------------------
// Measurement over a whole history
// --------------------------------------------------------------------------

TEST_CASE("measurement overrules the threshold where the data can",
          "[universe][identity][measure]") {
    const ScratchDir dir;

    // KEPTUP left the index for eight years and kept trading throughout.
    // GONE left for eight years and stopped trading. The threshold splits both;
    // only the second deserves it.
    write_snapshots(dir.file("history.csv"),
                    {
                        {"2000-01-03", {"KEPTUP", "GONE", "ANCHOR"}},
                        {"2001-01-03", {"ANCHOR"}},
                        {"2009-01-05", {"KEPTUP", "GONE", "ANCHOR"}},
                        {"2010-01-05", {"KEPTUP", "GONE", "ANCHOR"}},
                    });

    const ScratchDir prices;
    bar_store::write(prices.file("KEPTUP.msb"), bars_between("KEPTUP", 1999, 2010, 50.0));

    BarSeries broken;
    broken.ticker = "GONE";
    for (const BarSeries& piece :
         {bars_between("GONE", 1999, 2000, 20.0), bars_between("GONE", 2009, 2010, 80.0)}) {
        broken.bars.insert(broken.bars.end(), piece.bars.begin(), piece.bars.end());
    }
    bar_store::write(prices.file("GONE.msb"), broken);
    // ANCHOR has no cached series at all.

    SymbolTable symbols;
    SecurityMaster master(symbols);
    const SnapshotMembershipSource source(dir.file("history.csv"));
    MembershipHistory history = build_membership(source, master);

    // The weekly fixtures are about a fifth of the weekdays they span, so the
    // thresholds have to be relaxed to match. What is being tested is the
    // wiring and the ordering, not the exact numbers -- those are pinned in
    // test_identity_probe.cpp against series with a bar on every weekday.
    IdentityProbeConfig probe;
    probe.continuous_coverage = 0.15;
    probe.flank_coverage = 0.10;
    probe.max_hole_days = 10;
    measure_identities(history, prices.path(), probe);

    const IdentityDecision* kept = nullptr;
    const IdentityDecision* gone = nullptr;
    for (const IdentityDecision& decision : history.decisions) {
        if (decision.ticker == "KEPTUP") {
            kept = &decision;
        } else if (decision.ticker == "GONE") {
            gone = &decision;
        }
    }
    REQUIRE(kept != nullptr);
    REQUIRE(gone != nullptr);

    // The threshold split both; the data agrees with one of them.
    REQUIRE(kept->split);
    REQUIRE(gone->split);
    REQUIRE(kept->measured.verdict == IdentityVerdict::Continuous);
    REQUIRE(gone->measured.verdict == IdentityVerdict::Discontinuous);

    // Which makes KEPTUP a disagreement and GONE a confirmation.
    REQUIRE(kept->disagrees());
    REQUIRE_FALSE(gone->disagrees());

    // Only measurements reach the override file, and the note states what was
    // observed rather than what is believed.
    const std::size_t rows =
        write_measured_overrides(dir.file("overrides.csv"), history, probe, "test");
    REQUIRE(rows == 2);

    std::ifstream file(dir.file("overrides.csv"));
    const std::string text((std::istreambuf_iterator<char>(file)),
                           std::istreambuf_iterator<char>());
    REQUIRE_THAT(text, ContainsSubstring("KEPTUP,keep,"));
    REQUIRE_THAT(text, ContainsSubstring("GONE,split,"));
    REQUIRE_THAT(text, ContainsSubstring("coverage "));

    // And the file round-trips: what was written is what load_overrides reads.
    const IdentityRules reloaded = load_overrides(dir.file("overrides.csv"));
    REQUIRE(reloaded.overrides.size() == 2);
}

TEST_CASE("a ticker with no cached prices is never split by measurement",
          "[universe][identity][measure]") {
    const ScratchDir dir;
    write_snapshots(dir.file("history.csv"),
                    {
                        {"2000-01-03", {"NODATA", "ANCHOR"}},
                        {"2001-01-03", {"ANCHOR"}},
                        {"2009-01-05", {"NODATA", "ANCHOR"}},
                        {"2010-01-05", {"NODATA", "ANCHOR"}},
                    });

    const ScratchDir empty_cache;

    SymbolTable symbols;
    SecurityMaster master(symbols);
    const SnapshotMembershipSource source(dir.file("history.csv"));
    MembershipHistory history = build_membership(source, master);
    measure_identities(history, empty_cache.path());

    // The property the whole design turns on: a vendor that does not carry a
    // ticker is a coverage limit, not a delisting. Nothing here may become a
    // verdict, and nothing here may reach the override file.
    for (const IdentityDecision& decision : history.decisions) {
        INFO(decision.ticker << ": " << decision.measured.reason);
        REQUIRE_FALSE(decision.measured.resolved());
    }
    REQUIRE(write_measured_overrides(dir.file("overrides.csv"), history, {}, "test") == 0);
}

// --------------------------------------------------------------------------
// Calibration
// --------------------------------------------------------------------------
//
// An uncalibrated instrument's readings are not evidence. data/identity_
// calibration.csv holds cases whose status is known with high confidence, and
// this runs the classifier against the real membership history and the real
// price cache to check it gets them right.
//
// It SKIPs when either is absent, which is the normal state in CI and on a
// fresh clone: neither is committed, because one is market data and the other
// is fetched. The skip message says exactly what to run. A test that silently
// passed when it had measured nothing would be worse than no test.

TEST_CASE("the classifier agrees with the cases we are sure of",
          "[universe][identity][calibration]") {
    const std::filesystem::path history_path("data/cache/universe/sp500_history.csv");
    const std::filesystem::path price_cache("data/cache/stooq");
    const std::filesystem::path calibration_path("data/identity_calibration.csv");

    // Gated on the history only. Whether there is anything to measure is decided
    // at the bottom, on the count of cases actually verified -- not here, on
    // whether a directory exists.
    //
    // That distinction is not pedantry. `data/cache/stooq` existing and being
    // empty used to satisfy this check, after which the test measured nothing,
    // asserted nothing, and reported **green**: "0 verified, 10
    // coverage-limited". A skipped test at least announces itself. A vacuous pass
    // does not, and this is the test whose whole job is to say whether the
    // identity classifier has ever been right about anything.
    if (!std::filesystem::exists(history_path)) {
        SKIP("no membership history; run scripts/fetch_sp500.py");
    }

    // ticker -> expected verdict.
    std::vector<std::pair<std::string, IdentityVerdict>> expected;
    {
        std::ifstream file(calibration_path);
        REQUIRE(file);
        std::string line;
        while (std::getline(file, line)) {
            if (line.empty() || line[0] == '#' || line.rfind("ticker,", 0) == 0) {
                continue;
            }
            const std::size_t first = line.find(',');
            if (first == std::string::npos) {
                continue;
            }
            const std::size_t second = line.find(',', first + 1);
            const std::string ticker = line.substr(0, first);
            const std::string want = line.substr(first + 1, second - first - 1);
            expected.emplace_back(ticker,
                                  want == "continuous" ? IdentityVerdict::Continuous
                                                       : IdentityVerdict::Discontinuous);
        }
    }
    REQUIRE(expected.size() == 10);

    SymbolTable symbols;
    SecurityMaster master(symbols);
    const SnapshotMembershipSource source(history_path);
    MembershipHistory history = build_membership(source, master);
    measure_identities(history, price_cache);

    std::size_t checked = 0;
    std::size_t coverage_limited = 0;
    for (const auto& [ticker, want] : expected) {
        for (const IdentityDecision& decision : history.decisions) {
            if (decision.ticker != ticker) {
                continue;
            }
            INFO(ticker << ": expected " << to_string(want) << ", measured "
                        << to_string(decision.measured.verdict) << " -- "
                        << decision.measured.reason);
            if (!decision.measured.resolved()) {
                // A coverage limit, not a wrong answer. Recorded, not failed.
                ++coverage_limited;
                break;
            }
            // The assertion. A wrong verdict on a case we are sure of means the
            // classifier is wrong, and nothing it says about the other 43 gaps
            // can be trusted until it is fixed.
            REQUIRE(decision.measured.verdict == want);
            ++checked;
            break;
        }
    }

    // Nothing verified is not a pass.
    //
    // Skipped, loudly, with the count in the reason, so it lands in the CI skip
    // enumeration and has to be allowlisted deliberately rather than scrolling
    // past as "N passed". See scripts/check_skips.py and tests/allowed_skips.txt.
    if (checked == 0) {
        std::ostringstream reason;
        reason << "0 of " << expected.size()
               << " calibration cases could be verified: no price data for any of them ("
               << coverage_limited << " coverage-limited). The identity classifier has never "
               << "been checked against a real price series. This needs delisted price "
               << "history, which no permitted source carries -- see docs/DATA_POLICY.md.";
        SKIP(reason.str());
    }

    WARN("calibration: " << checked << " verified, " << coverage_limited << " coverage-limited, of "
                         << expected.size());
}
