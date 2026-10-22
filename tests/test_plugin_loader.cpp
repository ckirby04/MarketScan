#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "temp_paths.hpp"
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "marketscan/core/engine.hpp"
#include "marketscan/data/synthetic_feed.hpp"
#include "marketscan/strategy/plugin_loader.hpp"
#include "marketscan/strategy/registry.hpp"
#include "marketscan/strategy/strategy_instance.hpp"

using namespace marketscan;
using Catch::Matchers::ContainsSubstring;

namespace {

std::filesystem::path theory_dir() {
    return std::filesystem::path(MARKETSCAN_THEORY_DIR);
}

std::filesystem::path fixture_dir() {
    return std::filesystem::path(MARKETSCAN_TEST_PLUGIN_DIR);
}

/// A directory of its own, so a test can put exactly the DLLs it wants in it.
class ScratchDir {
public:
    ScratchDir() {
        path_ = test::unique_temp_path("marketscan_plugins_");
        std::filesystem::create_directories(path_);
    }

    ScratchDir(const ScratchDir&) = delete;
    ScratchDir& operator=(const ScratchDir&) = delete;

    ~ScratchDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }

    /// Copies a fixture DLL in under a chosen name.
    std::filesystem::path put(const std::string& fixture, const std::string& as) const {
        const std::filesystem::path target = path_ / as;
        std::filesystem::copy_file(
            fixture_dir() / fixture, target, std::filesystem::copy_options::overwrite_existing);
        return target;
    }

    std::filesystem::path put_theory(const std::string& name) const {
        const std::filesystem::path target = path_ / name;
        std::filesystem::copy_file(
            theory_dir() / name, target, std::filesystem::copy_options::overwrite_existing);
        return target;
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

const PluginInfo* entry_for(const PluginLoader& loader, std::string_view filename) {
    for (const PluginInfo& info : loader.plugins()) {
        if (info.path.filename().string() == filename) {
            return &info;
        }
    }
    return nullptr;
}

}  // namespace

// --------------------------------------------------------------------------
// The happy path
// --------------------------------------------------------------------------

TEST_CASE("the reference theory is discovered and loaded", "[plugin]") {
    PluginLoader loader({theory_dir()});
    REQUIRE(loader.rescan() >= 1);

    const PluginInfo* info = loader.find("buy_and_hold");
    REQUIRE(info != nullptr);
    REQUIRE(info->state == PluginState::Loaded);
    REQUIRE(info->error.empty());
    REQUIRE(info->abi_version == MS_ABI_VERSION);
    REQUIRE(info->display_name == "Buy and Hold");
    REQUIRE(info->author == "Clark Kirby");
    REQUIRE_FALSE(info->description.empty());
}

// The GUI renders sliders from this without the host knowing anything about
// the theory, so the whole array has to survive the trip across the boundary.
TEST_CASE("parameter metadata crosses the boundary intact", "[plugin][abi]") {
    PluginLoader loader({theory_dir()});
    loader.rescan();

    const PluginInfo* info = loader.find("buy_and_hold");
    REQUIRE(info != nullptr);
    REQUIRE(info->params.size() == 2);

    REQUIRE(info->params[0].name == "symbol_index");
    REQUIRE(info->params[0].type == MS_PARAM_INT);
    REQUIRE(info->params[0].default_value == 0.0);
    REQUIRE_FALSE(info->params[0].description.empty());

    REQUIRE(info->params[1].name == "cash_reserve_pct");
    REQUIRE(info->params[1].type == MS_PARAM_DOUBLE);
    REQUIRE(info->params[1].default_value == 1.0);
}

TEST_CASE("an instance can be created and its parameters read back", "[plugin]") {
    PluginLoader loader({theory_dir()});
    loader.rescan();

    PluginInstance instance = loader.create("buy_and_hold");
    REQUIRE(instance.valid());
    REQUIRE(loader.find("buy_and_hold")->live_instances == 1);

    REQUIRE(instance.get_param("cash_reserve_pct") == 1.0);
    instance.set_param("cash_reserve_pct", 5.0);
    REQUIRE(instance.get_param("cash_reserve_pct") == 5.0);

    // An unknown parameter is not a crash; the binding simply does not match.
    instance.set_param("no_such_parameter", 1.0);
    REQUIRE(instance.get_param("no_such_parameter") == 0.0);

    instance.reset();
    REQUIRE_FALSE(instance.valid());
    REQUIRE(loader.find("buy_and_hold")->live_instances == 0);
}

// --------------------------------------------------------------------------
// Refusing, rather than crashing
// --------------------------------------------------------------------------

// The dangerous failure: every export resolves, so without the version check
// the host would read a descriptor whose fields are at the wrong offsets --
// and, worse, would hand ms_plugin_create a vtable smaller than the one the
// plugin is about to write into it.
TEST_CASE("a plugin built against a newer ABI is refused", "[plugin][abi]") {
    const ScratchDir dir;
    dir.put("test_plugin_bad_abi.dll", "test_plugin_bad_abi.dll");

    PluginLoader loader({dir.path()});
    REQUIRE(loader.rescan() == 0);

    const PluginInfo* info = entry_for(loader, "test_plugin_bad_abi.dll");
    REQUIRE(info != nullptr);
    REQUIRE(info->state == PluginState::Failed);

    // The message has to say which way round it is. "Version mismatch" sends
    // the reader off to rebuild a theory that is already newer than the host.
    REQUIRE_THAT(info->error, ContainsSubstring("newer"));
    REQUIRE_THAT(info->error, ContainsSubstring("corrupt memory"));
    REQUIRE_THAT(info->error, ContainsSubstring("Update MarketScan"));
}

TEST_CASE("a DLL that is not a theory is refused", "[plugin]") {
    const ScratchDir dir;
    dir.put("test_plugin_no_exports.dll", "test_plugin_no_exports.dll");

    PluginLoader loader({dir.path()});
    REQUIRE(loader.rescan() == 0);

    const PluginInfo* info = entry_for(loader, "test_plugin_no_exports.dll");
    REQUIRE(info != nullptr);
    REQUIRE(info->state == PluginState::Failed);
    REQUIRE_THAT(info->error, ContainsSubstring("missing export"));
    REQUIRE_THAT(info->error, ContainsSubstring("MARKETSCAN_EXPORT_STRATEGY"));
}

TEST_CASE("a file that is not a DLL at all is refused", "[plugin]") {
    const ScratchDir dir;
    {
        std::ofstream out(dir.path() / "garbage.dll", std::ios::binary);
        out << "this is not a portable executable";
    }

    PluginLoader loader({dir.path()});
    REQUIRE(loader.rescan() == 0);

    const PluginInfo* info = entry_for(loader, "garbage.dll");
    REQUIRE(info != nullptr);
    REQUIRE(info->state == PluginState::Failed);
    REQUIRE_THAT(info->error, ContainsSubstring("LoadLibrary failed"));
}

TEST_CASE("a plugin whose id is blank is refused", "[plugin]") {
    const ScratchDir dir;
    dir.put("test_plugin_empty_id.dll", "test_plugin_empty_id.dll");

    PluginLoader loader({dir.path()});
    REQUIRE(loader.rescan() == 0);
    REQUIRE_THAT(entry_for(loader, "test_plugin_empty_id.dll")->error,
                 ContainsSubstring("empty id"));
}

// The plugin loads and describes itself perfectly, then refuses to construct.
// That is what the export macro does when a theory's constructor throws.
TEST_CASE("a constructor that refuses is reported, not fatal", "[plugin]") {
    const ScratchDir dir;
    dir.put("test_plugin_null_create.dll", "test_plugin_null_create.dll");

    PluginLoader loader({dir.path()});
    REQUIRE(loader.rescan() == 1);
    REQUIRE(loader.find("null_create")->state == PluginState::Loaded);

    const PluginInstance instance = loader.create("null_create");
    REQUIRE_FALSE(instance.valid());

    const PluginInfo* info = loader.find("null_create");
    REQUIRE(info->state == PluginState::Failed);
    REQUIRE_THAT(info->error, ContainsSubstring("returned null"));
    REQUIRE(info->live_instances == 0);
}

TEST_CASE("creating from an unknown or unloaded plugin yields nothing", "[plugin]") {
    PluginLoader loader({theory_dir()});
    loader.rescan();
    REQUIRE_FALSE(loader.create("no_such_theory").valid());
}

// One bad plugin must not stop the others loading. This is the scenario that
// matters in practice: a folder with a stale DLL in it should still work.
TEST_CASE("a bad plugin does not take the good ones with it", "[plugin]") {
    const ScratchDir dir;
    dir.put_theory("buy_and_hold.dll");
    dir.put("test_plugin_bad_abi.dll", "test_plugin_bad_abi.dll");
    dir.put("test_plugin_no_exports.dll", "test_plugin_no_exports.dll");
    {
        std::ofstream out(dir.path() / "garbage.dll", std::ios::binary);
        out << "nonsense";
    }

    PluginLoader loader({dir.path()});
    REQUIRE(loader.rescan() == 1);
    REQUIRE(loader.find("buy_and_hold") != nullptr);
    REQUIRE(loader.find("buy_and_hold")->state == PluginState::Loaded);
    REQUIRE(loader.plugins().size() == 4);  // every failure is still listed
}

TEST_CASE("an empty or missing theories directory is not an error", "[plugin]") {
    const ScratchDir empty;
    PluginLoader loader({empty.path(), "definitely/not/a/real/path"});
    REQUIRE(loader.rescan() == 0);
    REQUIRE(loader.plugins().empty());
}

// --------------------------------------------------------------------------
// Shadow copying, which is what makes hot reload possible
// --------------------------------------------------------------------------

// Windows locks a loaded DLL. If the loader opened the original, an author
// rebuilding their theory would get a link error from their own build.
TEST_CASE("the original file stays writable while the plugin is loaded", "[plugin][hotreload]") {
    const ScratchDir dir;
    const std::filesystem::path original = dir.put_theory("buy_and_hold.dll");

    PluginLoader loader({dir.path()});
    REQUIRE(loader.rescan() == 1);
    REQUIRE(loader.find("buy_and_hold")->state == PluginState::Loaded);

    // Loaded from a copy elsewhere, not from this file.
    REQUIRE(loader.find("buy_and_hold")->shadow_path != original);
    REQUIRE(std::filesystem::exists(loader.find("buy_and_hold")->shadow_path));

    // The thing a rebuild does: overwrite the original in place.
    std::error_code ec;
    std::filesystem::remove(original, ec);
    REQUIRE_FALSE(ec);

    std::filesystem::copy_file(theory_dir() / "buy_and_hold.dll", original, ec);
    REQUIRE_FALSE(ec);
}

TEST_CASE("a rebuilt plugin is noticed and picked up on rescan", "[plugin][hotreload]") {
    const ScratchDir dir;
    const std::filesystem::path original = dir.put_theory("buy_and_hold.dll");

    PluginLoader loader({dir.path()});
    REQUIRE(loader.rescan() == 1);
    REQUIRE_FALSE(loader.changes_available());

    // Touch it the way a rebuild would.
    std::filesystem::last_write_time(
        original, std::filesystem::last_write_time(original) + std::chrono::seconds(10));

    REQUIRE(loader.changes_available());
    REQUIRE(loader.rescan() == 1);
    REQUIRE_FALSE(loader.changes_available());
    REQUIRE(loader.find("buy_and_hold")->state == PluginState::Loaded);
}

TEST_CASE("a plugin that disappears is dropped on rescan", "[plugin]") {
    const ScratchDir dir;
    const std::filesystem::path original = dir.put_theory("buy_and_hold.dll");

    PluginLoader loader({dir.path()});
    REQUIRE(loader.rescan() == 1);

    std::error_code ec;
    std::filesystem::remove(original, ec);
    REQUIRE(loader.changes_available());
    REQUIRE(loader.rescan() == 0);
    REQUIRE(loader.plugins().empty());
}

// --------------------------------------------------------------------------
// Unloading
// --------------------------------------------------------------------------

// Freeing a module out from under a live object turns its next call into a
// jump into unmapped memory.
TEST_CASE("a plugin with live instances refuses to unload", "[plugin]") {
    PluginLoader loader({theory_dir()});
    loader.rescan();

    PluginInstance instance = loader.create("buy_and_hold");
    REQUIRE(instance.valid());

    REQUIRE_FALSE(loader.unload("buy_and_hold"));
    REQUIRE_THAT(loader.find("buy_and_hold")->error, ContainsSubstring("instance(s) still live"));
    REQUIRE(loader.find("buy_and_hold")->state == PluginState::Loaded);

    instance.reset();
    REQUIRE(loader.unload("buy_and_hold"));
    REQUIRE(loader.find("buy_and_hold")->state == PluginState::Discovered);
}

TEST_CASE("unload then reload works", "[plugin][hotreload]") {
    const ScratchDir dir;
    dir.put_theory("buy_and_hold.dll");

    PluginLoader loader({dir.path()});
    REQUIRE(loader.rescan() == 1);
    REQUIRE(loader.reload("buy_and_hold"));
    REQUIRE(loader.find("buy_and_hold")->state == PluginState::Loaded);

    // And it is still usable afterwards.
    REQUIRE(loader.create("buy_and_hold").valid());
}

TEST_CASE("moving an instance does not destroy it twice", "[plugin]") {
    PluginLoader loader({theory_dir()});
    loader.rescan();

    PluginInstance first = loader.create("buy_and_hold");
    REQUIRE(first.valid());
    REQUIRE(loader.find("buy_and_hold")->live_instances == 1);

    PluginInstance second = std::move(first);
    REQUIRE(second.valid());
    REQUIRE_FALSE(first.valid());  // NOLINT(bugprone-use-after-move)
    REQUIRE(loader.find("buy_and_hold")->live_instances == 1);

    second.reset();
    REQUIRE(loader.find("buy_and_hold")->live_instances == 0);
}

// --------------------------------------------------------------------------
// End to end: a plugin trades
// --------------------------------------------------------------------------

// The milestone's actual claim. A DLL discovered on disk, loaded at runtime,
// given its own account, driven by the same engine as everything else -- and
// it buys something.
TEST_CASE("buy_and_hold loads from disk and trades", "[plugin][e2e]") {
    SymbolTable symbols;
    Engine engine;

    SyntheticFeedConfig feed_config;
    feed_config.instruments = {SyntheticInstrument{"SPY", 500.0, 0.05, 0.15, 100'000.0}};
    feed_config.bars_per_session = 100;
    feed_config.sessions = 2;
    feed_config.seed = 31'337;
    engine.add_source(std::make_unique<SyntheticFeed>(feed_config, symbols));

    PluginLoader loader({theory_dir()});
    REQUIRE(loader.rescan() >= 1);

    PluginInstance plugin = loader.create("buy_and_hold");
    REQUIRE(plugin.valid());

    const SpreadCrossingFillModel fill_model;
    StrategyRuntimeConfig runtime;
    runtime.starting_cash = Money::from_dollars(100'000);

    StrategyInstance strategy("buy_and_hold",
                              "Buy and Hold",
                              std::move(plugin),
                              {},
                              runtime,
                              &fill_model,
                              &engine.market(),
                              &symbols,
                              &engine.clock());

    engine.add_sink(&strategy);
    strategy.start();
    engine.run();
    strategy.stop();

    const Ledger& ledger = strategy.ledger();

    // It bought, once, and held.
    REQUIRE(ledger.blotter().size() == 1);
    REQUIRE(ledger.blotter().front().side == MS_SIDE_BUY);
    REQUIRE(ledger.position(symbols.find("SPY")) > 0.0);

    // It paid the ask rather than the mid, so slippage is a real cost.
    REQUIRE(ledger.blotter().front().slippage > Money{});
    REQUIRE(ledger.blotter().front().commission > Money{});

    // It did not overdraw: this is a cash account.
    REQUIRE(ledger.cash() >= Money{});
    REQUIRE(ledger.check_invariant());

    // The equity curve was sampled, and the metrics it published arrived.
    REQUIRE(ledger.equity_curve().size() > 1);
    REQUIRE_FALSE(strategy.metrics().empty());

    bool published_shares = false;
    for (const PublishedMetric& metric : strategy.metrics()) {
        if (metric.name == "shares_held" && metric.value > 0.0) {
            published_shares = true;
        }
    }
    REQUIRE(published_shares);
}

// Two instances of one plugin, each with its own account, neither able to see
// the other. This is what "its own virtual account" has to mean.
TEST_CASE("two instances of one theory are independent", "[plugin][e2e]") {
    SymbolTable symbols;
    Engine engine;

    SyntheticFeedConfig feed_config;
    feed_config.instruments = {SyntheticInstrument{"SPY", 500.0, 0.05, 0.15, 100'000.0}};
    feed_config.bars_per_session = 60;
    feed_config.sessions = 1;
    engine.add_source(std::make_unique<SyntheticFeed>(feed_config, symbols));

    PluginLoader loader({theory_dir()});
    loader.rescan();

    const SpreadCrossingFillModel fill_model;

    StrategyRuntimeConfig rich;
    rich.starting_cash = Money::from_dollars(1'000'000);
    rich.order_id_base = 1'000'000;

    StrategyRuntimeConfig poor;
    poor.starting_cash = Money::from_dollars(10'000);
    poor.order_id_base = 2'000'000;

    StrategyInstance a("rich",
                       "Rich",
                       loader.create("buy_and_hold"),
                       {},
                       rich,
                       &fill_model,
                       &engine.market(),
                       &symbols,
                       &engine.clock());
    StrategyInstance b("poor",
                       "Poor",
                       loader.create("buy_and_hold"),
                       {},
                       poor,
                       &fill_model,
                       &engine.market(),
                       &symbols,
                       &engine.clock());

    engine.add_sink(&a);
    engine.add_sink(&b);
    a.start();
    b.start();
    engine.run();

    REQUIRE(a.ledger().position(symbols.find("SPY")) > b.ledger().position(symbols.find("SPY")));
    REQUIRE(a.ledger().check_invariant());
    REQUIRE(b.ledger().check_invariant());

    // Order ids do not collide, so a merged blotter is unambiguous.
    REQUIRE(a.broker().orders().front().id >= 1'000'000);
    REQUIRE(b.broker().orders().front().id >= 2'000'000);
}

// --------------------------------------------------------------------------
// Built-in strategies take the same path
// --------------------------------------------------------------------------

namespace {

/// Compiled into this test binary rather than loaded from a DLL. Everything
/// downstream of PluginInstance treats it identically to one that was.
class CountingTheory final : public marketscan::IStrategy {
public:
    static constexpr const char* kDescription = "Counts bars. Registered, not loaded.";

    static constexpr MS_ParamDesc kParams[] = {
        marketscan::param_int("stride", "Bars between logs.", 1.0, 100.0, 10.0),
    };
    static constexpr uint32_t kParamCount = 1;

    CountingTheory() { bind_param("stride", &stride_); }

    void on_bar(const MS_Bar& bar) override {
        ++bars_;
        publish_metric("bars", static_cast<double>(bars_));
        if (bars_ == 1) {
            // Trades, so that this proves the whole path and not just dispatch.
            submit_market(bar.symbol, MS_SIDE_BUY, 10.0);
        }
    }

private:
    int stride_ = 10;
    long long bars_ = 0;
};

}  // namespace

MARKETSCAN_REGISTER_STRATEGY(CountingTheory, "counting", "Counting Theory", "test")

TEST_CASE("a built-in strategy is registered and described", "[plugin][registry]") {
    const StrategyRegistry& registry = StrategyRegistry::instance();

    const BuiltinStrategy* entry = registry.find("counting");
    REQUIRE(entry != nullptr);
    REQUIRE(entry->display_name == "Counting Theory");
    REQUIRE(entry->author == "test");
    REQUIRE(entry->param_count == 1);
    REQUIRE(std::string(entry->params[0].name) == "stride");
    REQUIRE(registry.find("not_registered") == nullptr);
}

TEST_CASE("the registry keeps a stable order", "[plugin][registry][determinism]") {
    const auto& entries = StrategyRegistry::instance().entries();
    // Sorted by id, not by whatever order static initialisation happened to
    // run in, which the standard leaves undefined across translation units.
    REQUIRE(std::is_sorted(
        entries.begin(), entries.end(), [](const BuiltinStrategy& a, const BuiltinStrategy& b) {
            return a.id < b.id;
        }));
}

TEST_CASE("a duplicate registration is refused", "[plugin][registry]") {
    BuiltinStrategy duplicate;
    duplicate.id = "counting";
    duplicate.create = StrategyRegistry::instance().find("counting")->create;
    REQUIRE_FALSE(StrategyRegistry::instance().add(std::move(duplicate)));

    BuiltinStrategy nameless;
    nameless.create = StrategyRegistry::instance().find("counting")->create;
    REQUIRE_FALSE(StrategyRegistry::instance().add(std::move(nameless)));
}

// The claim this milestone rests on, stated the other way round: a strategy
// that never went near a DLL runs through exactly the same engine, ledger and
// fill model as one that did.
TEST_CASE("a built-in strategy runs through the same engine", "[plugin][registry][e2e]") {
    SymbolTable symbols;
    Engine engine;

    SyntheticFeedConfig feed_config;
    feed_config.instruments = {SyntheticInstrument{"SPY", 400.0, 0.0, 0.15, 100'000.0}};
    feed_config.bars_per_session = 50;
    feed_config.sessions = 1;
    engine.add_source(std::make_unique<SyntheticFeed>(feed_config, symbols));

    PluginInstance builtin = StrategyRegistry::instance().create("counting");
    REQUIRE(builtin.valid());
    REQUIRE(builtin.get_param("stride") == 10.0);

    const SpreadCrossingFillModel fill_model;
    StrategyRuntimeConfig runtime;
    runtime.starting_cash = Money::from_dollars(100'000);

    StrategyInstance strategy("counting",
                              "Counting Theory",
                              std::move(builtin),
                              {},
                              runtime,
                              &fill_model,
                              &engine.market(),
                              &symbols,
                              &engine.clock());

    engine.add_sink(&strategy);
    strategy.start();
    engine.run();
    strategy.stop();

    REQUIRE(strategy.ledger().blotter().size() == 1);
    REQUIRE(strategy.ledger().position(symbols.find("SPY")) == 10.0);
    REQUIRE(strategy.ledger().check_invariant());
    // It paid the ask, exactly as a plugin would have.
    REQUIRE(strategy.ledger().blotter().front().slippage > Money{});
}

TEST_CASE("creating an unregistered built-in yields nothing", "[plugin][registry]") {
    REQUIRE_FALSE(StrategyRegistry::instance().create("no_such_builtin").valid());
}
