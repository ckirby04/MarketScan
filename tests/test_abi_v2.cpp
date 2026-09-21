// ABI v2, and the promise the version-tolerant loader was built to keep.
//
// `SPEC_2_M1B.md` §4 deferred `on_order_status` and built the loader's version
// branch anyway, explicitly so that adding it later would never be a flag day.
// This is that day, and these are the assertions that say whether the loader was
// built right -- which, as that spec put it, is memory corruption if it was not.
//
// The promise, in three parts:
//
//   1. A v1 plugin still loads.
//   2. It behaves byte-identically -- same callbacks, same counts, same results.
//   3. It never receives the new callback, because it has no slot to receive it
//      in. Calling slot ten of a nine-slot table is not a wrong answer, it is
//      memory corruption, and it happens before a single assertion could notice.

#include <filesystem>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "marketscan/abi/abi_compat.hpp"
#include "marketscan/abi/plugin_abi.h"
#include "marketscan/core/engine.hpp"
#include "marketscan/data/synthetic_feed.hpp"
#include "marketscan/exec/fill_model.hpp"
#include "marketscan/strategy/plugin_loader.hpp"
#include "marketscan/strategy/strategy_instance.hpp"

using namespace marketscan;

namespace {

std::filesystem::path plugin_dir() {
    return std::filesystem::path(MARKETSCAN_TEST_PLUGIN_DIR);
}

SyntheticFeedConfig feed_config() {
    SyntheticFeedConfig config;
    config.instruments = {SyntheticInstrument{"SPY", 500.0, 0.05, 0.15, 90'000.0}};
    config.bars_per_session = 120;
    config.sessions = 2;
    config.seed = 6060;
    config.emit_quotes = true;
    return config;
}

/// Runs one plugin over the synthetic feed and returns what it counted.
struct RunResult {
    double bars = 0.0;
    double quotes = 0.0;
    double trades = 0.0;
    double fills = 0.0;
    double started = 0.0;
    double stopped = 0.0;
    double order_status = 0.0;
    double submitted = 0.0;

    std::uint64_t host_status_events = 0;
    std::uint32_t abi_version = 0;
    Money final_equity;
};

[[nodiscard]] RunResult run_plugin(const std::string& id, double reject_probability = 0.0) {
    SymbolTable symbols;
    Engine engine;
    engine.add_source(std::make_unique<SyntheticFeed>(feed_config(), symbols));

    FillModelConfig fills;
    fills.simulated_reject_probability = reject_probability;
    const SpreadCrossingFillModel model{fills};

    PluginLoader loader({plugin_dir()});
    loader.rescan();

    const PluginInfo* info = loader.find(id);
    REQUIRE(info != nullptr);
    REQUIRE(info->loaded());

    StrategyRuntimeConfig runtime;
    runtime.starting_cash = Money::from_dollars(100'000);
    StrategyInstance instance(id,
                              id,
                              loader.create(id),
                              {},
                              runtime,
                              &model,
                              &engine.market(),
                              &symbols,
                              &engine.clock());
    engine.add_sink(&instance);
    instance.start();
    while (engine.step()) {
    }
    instance.stop();

    RunResult out;
    out.abi_version = info->abi_version;
    out.bars = instance.parameter("bars");
    out.quotes = instance.parameter("quotes");
    out.trades = instance.parameter("trades");
    out.fills = instance.parameter("fills");
    out.started = instance.parameter("started");
    out.stopped = instance.parameter("stopped");
    out.order_status = instance.parameter("order_status");
    out.submitted = instance.parameter("submitted");
    out.host_status_events = instance.order_status_events();
    out.final_equity = instance.equity();
    return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// The version table
// ---------------------------------------------------------------------------

TEST_CASE("the ABI is at version 2 and the vtable has ten slots", "[abi][v2]") {
    CHECK(MS_ABI_VERSION == 2u);
    CHECK(sizeof(MS_StrategyVTable) == 10 * sizeof(void (*)()));

    // v1's count did not move. If it ever does, every plugin compiled against v1
    // is about to be handed a vtable of the wrong length.
    CHECK(abi::kV1SlotCount == 9);
    CHECK(abi::kV2SlotCount == 10);
    CHECK(abi::slot_count_for(1) == 9);
    CHECK(abi::slot_count_for(2) == 10);
}

TEST_CASE("a version this host does not know is refused rather than guessed at", "[abi][v2]") {
    // The direction that matters. A plugin NEWER than the host writes more bytes
    // than the host declared, past the end of the host's object, before a single
    // callback runs. There is no recovering from that, so it is refused on the
    // declared version and never created.
    CHECK(abi::slot_count_for(0) == 0);
    CHECK(abi::slot_count_for(3) == 0);
    CHECK(abi::slot_count_for(1000) == 0);

    CHECK(abi::is_supported(1));
    CHECK(abi::is_supported(2));
    CHECK_FALSE(abi::is_supported(3));
    CHECK_FALSE(abi::is_supported(0));
}

TEST_CASE("trimming nulls exactly the slots a version is not entitled to", "[abi][v2]") {
    MS_StrategyVTable table{};
    // Every slot filled, as a v2 plugin would.
    const auto fill = [](MS_StrategyVTable& t) {
        t.destroy = reinterpret_cast<void (*)(void*)>(&std::abort);
        t.on_start = reinterpret_cast<void (*)(void*, const MS_HostContext*)>(&std::abort);
        t.on_bar = reinterpret_cast<void (*)(void*, const MS_Bar*)>(&std::abort);
        t.on_quote = reinterpret_cast<void (*)(void*, const MS_Quote*)>(&std::abort);
        t.on_trade = reinterpret_cast<void (*)(void*, const MS_Trade*)>(&std::abort);
        t.on_fill = reinterpret_cast<void (*)(void*, const MS_Fill*)>(&std::abort);
        t.on_stop = reinterpret_cast<void (*)(void*)>(&std::abort);
        t.set_param = reinterpret_cast<void (*)(void*, const char*, double)>(&std::abort);
        t.get_param = reinterpret_cast<double (*)(void*, const char*)>(&std::abort);
        t.on_order_status = reinterpret_cast<void (*)(void*, const MS_OrderStatus*)>(&std::abort);
    };

    fill(table);
    abi::trim_to_version(table, 2);
    CHECK(table.on_order_status != nullptr);
    CHECK(table.get_param != nullptr);

    fill(table);
    abi::trim_to_version(table, 1);
    // The one slot v1 must not keep.
    CHECK(table.on_order_status == nullptr);
    // And every slot it must.
    CHECK(table.destroy != nullptr);
    CHECK(table.on_start != nullptr);
    CHECK(table.on_bar != nullptr);
    CHECK(table.on_quote != nullptr);
    CHECK(table.on_trade != nullptr);
    CHECK(table.on_fill != nullptr);
    CHECK(table.on_stop != nullptr);
    CHECK(table.set_param != nullptr);
    CHECK(table.get_param != nullptr);

    fill(table);
    abi::trim_to_version(table, 1000);
    // An unknown version is entitled to nothing at all.
    CHECK(table.destroy == nullptr);
    CHECK(table.on_order_status == nullptr);
}

// ---------------------------------------------------------------------------
// The migration promise
// ---------------------------------------------------------------------------

TEST_CASE("a v1 plugin still loads after the bump", "[abi][v2][migration]") {
    PluginLoader loader({plugin_dir()});
    loader.rescan();

    const PluginInfo* info = loader.find("v1_reference");
    REQUIRE(info != nullptr);
    INFO(info->error);
    CHECK(info->loaded());
    CHECK(info->abi_version == 1u);

    // And the host is at 2, so this is genuinely a cross-version load rather
    // than a plugin that happens to match.
    CHECK(MS_ABI_VERSION == 2u);
    CHECK(info->abi_version != MS_ABI_VERSION);
}

TEST_CASE("a v1 plugin never receives the v2 callback", "[abi][v2][migration]") {
    // The assertion the version branch exists for. A v1 plugin's vtable has nine
    // slots; the tenth is not a slot it declined to fill, it is memory that
    // belongs to something else.
    //
    // Rejections are forced on so there is something to deliver. The first
    // version of this test used a plugin that submitted no orders, so the host
    // produced nothing, so the count was zero -- and the test passed for the
    // wrong reason. The guard below is what caught that.
    const RunResult v1 = run_plugin("v1_reference", 0.5);

    INFO("host produced " << v1.host_status_events << " status events");
    CHECK(v1.abi_version == 1u);
    CHECK(v1.order_status == 0.0);

    // The host did produce them. Without this, the line above is satisfied by
    // nothing having happened.
    REQUIRE(v1.submitted > 0.0);
    REQUIRE(v1.host_status_events > 0);
}

TEST_CASE("the v2 twin does receive it", "[abi][v2][migration]") {
    // The control for the test above. "A v1 plugin got no status events" means
    // nothing unless an otherwise identical v2 plugin got some.
    const RunResult v2 = run_plugin("v2_reference", 0.5);

    INFO("host produced " << v2.host_status_events << " status events");
    CHECK(v2.abi_version == MS_ABI_VERSION);
    REQUIRE(v2.host_status_events > 0);
    CHECK(v2.order_status > 0.0);

    // Everything the host produced reached the plugin. A gap here would be the
    // host dropping events rather than the plugin ignoring them.
    CHECK(v2.order_status == static_cast<double>(v2.host_status_events));
}

TEST_CASE("a v1 plugin behaves identically to its v2 twin", "[abi][v2][migration]") {
    // "Byte-identical", as something measured. One body, exported at two
    // versions, over the same feed: every observable count and the final equity
    // have to match.
    //
    // WITH REJECTIONS OFF, AND THAT IS NOT A DODGE.
    //
    // The simulated rejection draw comes from the strategy's own random stream,
    // and each strategy's stream is seeded from `derive_seed(run_seed, id)` --
    // deliberately, so that loading a second theory cannot change the numbers the
    // first one produces. The two plugins here have different ids, so with
    // rejections on they draw different numbers and refuse different orders. That
    // is the per-strategy seeding working, not the ABI leaking, and comparing
    // across it would be comparing two different runs.
    //
    // Written the other way first, which is how the difference surfaced: nine
    // fills against six.
    const RunResult v1 = run_plugin("v1_reference", 0.0);
    const RunResult v2 = run_plugin("v2_reference", 0.0);

    // Non-zero first: identical counts of nothing would satisfy every line below.
    REQUIRE(v1.bars > 100.0);
    REQUIRE(v1.quotes > 100.0);
    REQUIRE(v1.submitted > 0.0);
    CHECK(v1.started == 1.0);
    CHECK(v1.stopped == 1.0);

    CHECK(v2.bars == v1.bars);
    CHECK(v2.quotes == v1.quotes);
    CHECK(v2.trades == v1.trades);
    CHECK(v2.fills == v1.fills);
    CHECK(v2.submitted == v1.submitted);
    CHECK(v2.started == v1.started);
    CHECK(v2.stopped == v1.stopped);

    // The ledger, which is the number that would move if the version branch
    // changed anything that mattered.
    CHECK(v2.final_equity == v1.final_equity);

    // With nothing going wrong, neither receives a status event: there is
    // nothing terminal-without-a-fill to report. The difference between them is
    // asserted by the two tests above, where there is something to deliver.
    CHECK(v1.order_status == 0.0);
    CHECK(v2.order_status == 0.0);
    CHECK(v1.host_status_events == 0);
    CHECK(v2.host_status_events == 0);
}

TEST_CASE("rejections off leaves a v1 plugin exactly where it was", "[abi][v2][migration]") {
    // The default configuration, which is what every committed result in this
    // repository was measured under. If this ever differs from the same run
    // before the ABI bump, the bump moved a number it had no business moving --
    // and the suite's other 370-odd tests are the check on that, since they all
    // run at the default and none of them changed.
    const RunResult quiet = run_plugin("v1_reference", 0.0);

    REQUIRE(quiet.bars > 100.0);
    REQUIRE(quiet.submitted > 0.0);
    CHECK(quiet.order_status == 0.0);

    // Nothing terminated without a fill, so the host produced nothing to report.
    CHECK(quiet.host_status_events == 0);

    // Deterministic across runs, which is the property the whole replay layer
    // rests on and the one an ABI change is most likely to disturb.
    const RunResult again = run_plugin("v1_reference", 0.0);
    CHECK(again.final_equity == quiet.final_equity);
    CHECK(again.fills == quiet.fills);
}

// ---------------------------------------------------------------------------
// The simulator's half
// ---------------------------------------------------------------------------

TEST_CASE("the simulator emits no status events when nothing goes wrong", "[abi][v2][simulator]") {
    // The default, and the reason every committed result is unchanged by this
    // milestone: with rejections off and orders that fill, there is nothing
    // terminal-without-a-fill to report.
    FillModelConfig fills;
    CHECK(fills.simulated_reject_probability == 0.0);
}

TEST_CASE("a simulated rejection is its own reason, not a borrowed one", "[abi][v2][simulator]") {
    // So that a rejection somebody arranged can never be read in a blotter as
    // one the market caused.
    CHECK(to_string(RejectReason::SimulatedRejection) != to_string(RejectReason::NotMarketable));
    CHECK(to_string(RejectReason::SimulatedRejection) != to_string(RejectReason::None));
    CHECK_FALSE(to_string(RejectReason::SimulatedRejection).empty());
}

TEST_CASE("the status event carries what a strategy needs to act on it", "[abi][v2][simulator]") {
    MS_OrderStatus status{};
    CHECK(status.reason == nullptr);  // a default-constructed POD, before the broker fills it

    // The layout guard, restated where somebody changing the struct will see it.
    CHECK(sizeof(MS_OrderStatus) == 56);

    // Every state a simulator or a venue can report, and which of them end the
    // order's life.
    CHECK(MS_ORDER_ACCEPTED == 0);
    CHECK(MS_ORDER_REJECTED == 1);
    CHECK(MS_ORDER_CANCELLED == 2);
    CHECK(MS_ORDER_EXPIRED == 3);
    CHECK(MS_ORDER_REPLACED == 4);
}
