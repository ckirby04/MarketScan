/*
 * `ms_live dryrun` -- run a plugin against a recording, with no network.
 *
 * WHY THIS EXISTS
 *
 * A live session is expensive in a way a backtest is not: it needs an open
 * market, it uses the one stream connection the account is allowed, it sends
 * real orders, and it cannot be repeated on demand. Spending one to discover
 * that a parameter was wrong is a bad trade.
 *
 * So this runs the same plugin binary against a recording from a previous
 * session, through the ordinary Engine and the fill simulator, and prints the
 * decision sequence it produces. It answers "will this configuration make the
 * orders I intended" before a venue is involved.
 *
 * It is NOT the equivalence test and makes no claim about live/replay equality.
 * It reuses the same decision extraction so the two report the same shape, and
 * that is the only thing they share.
 */

#include "equivalence.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "marketscan/core/engine.hpp"
#include "marketscan/core/symbol_table.hpp"
#include "marketscan/exec/fill_model.hpp"
#include "marketscan/live/recorded_feed.hpp"
#include "marketscan/strategy/plugin_loader.hpp"
#include "marketscan/strategy/strategy_instance.hpp"

namespace marketscan::live {
namespace {

[[nodiscard]] const char* option_value(int argc, char** argv, const char* name) {
    for (int i = 0; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], name) == 0) {
            return argv[i + 1];
        }
    }
    return nullptr;
}

}  // namespace

int run_dryrun(int argc, char** argv) {
    const char* plugin_arg = option_value(argc, argv, "--plugin");
    const char* recording_arg = option_value(argc, argv, "--recording");
    if (plugin_arg == nullptr || recording_arg == nullptr) {
        std::printf("dryrun needs --plugin DLL and --recording FILE\n");
        return 2;
    }

    const std::filesystem::path plugin = plugin_arg;
    PluginLoader loader({plugin.parent_path()});
    loader.rescan();

    const PluginInfo* info = nullptr;
    for (const PluginInfo& candidate : loader.plugins()) {
        if (candidate.loaded() && candidate.path.filename() == plugin.filename()) {
            info = &candidate;
            break;
        }
    }
    if (info == nullptr) {
        std::printf("no loadable plugin named %s under %s\n",
                    plugin.filename().string().c_str(),
                    plugin.parent_path().string().c_str());
        return 2;
    }

    SymbolTable symbols;
    auto feed = std::make_unique<RecordedFeed>(recording_arg, symbols);
    const RecordedFeedStats stats = feed->stats();

    SpreadCrossingFillModel fill_model{};
    Engine engine;
    engine.add_source(std::move(feed));

    StrategyRuntimeConfig runtime;
    runtime.order_id_base = 1'000'000;

    PluginInstance instance = loader.create(info->id);
    if (!instance.valid()) {
        std::printf("could not instantiate '%s'\n", info->id.c_str());
        return 2;
    }

    StrategyInstance strategy(info->id,
                              info->display_name,
                              std::move(instance),
                              info->required_symbols,
                              runtime,
                              &fill_model,
                              &engine.market(),
                              &symbols,
                              &engine.clock());

    // Any --set NAME=VALUE pairs, so a configuration can be tried without a
    // rebuild. Applied before start(), as the run drivers do.
    for (int i = 0; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], "--set") != 0) {
            continue;
        }
        const std::string pair = argv[i + 1];
        const std::size_t equals = pair.find('=');
        if (equals == std::string::npos) {
            std::printf("--set wants NAME=VALUE, got '%s'\n", pair.c_str());
            return 2;
        }
        const std::string name = pair.substr(0, equals);
        const double value = std::atof(pair.substr(equals + 1).c_str());
        strategy.set_parameter(name.c_str(), value);
        std::printf("set %s = %g\n", name.c_str(), value);
    }

    strategy.resolve_subscriptions();
    engine.add_sink(&strategy);
    strategy.start();
    engine.run();
    strategy.stop();

    const std::vector<Decision> decisions = decisions_of(strategy.broker(), symbols);

    std::printf("\nrecording       %s\n", recording_arg);
    std::printf("events          %llu\n", static_cast<unsigned long long>(stats.events));
    std::printf("strategy        %s\n", info->id.c_str());
    std::printf("decisions       %zu\n", decisions.size());

    if (decisions.empty()) {
        // The same refusal the equivalence test makes, for the same reason: an
        // empty result is not a pass, it is a run that measured nothing.
        std::printf("\nNo decisions. Lower --set period=N, or the recording is too short.\n");
        return 3;
    }

    // What was actually asked for, grouped by the strategy's own client id --
    // which is how the lifecycle exercise labels its three intents.
    std::map<std::uint64_t, std::size_t> by_intent;
    double smallest = decisions.front().quantity;
    double largest = decisions.front().quantity;
    double min_notional = 0.0;
    double max_notional = 0.0;
    bool first = true;
    for (const Decision& decision : decisions) {
        ++by_intent[decision.client_order_id];
        smallest = std::min(smallest, decision.quantity);
        largest = std::max(largest, decision.quantity);
        const double notional = decision.quantity * decision.limit_price;
        if (first || notional < min_notional) {
            min_notional = notional;
        }
        if (first || notional > max_notional) {
            max_notional = notional;
        }
        first = false;
    }

    std::printf("shares/order    %.0f to %.0f\n", smallest, largest);
    std::printf("notional/order  $%.0f to $%.0f\n", min_notional, max_notional);
    std::printf("by client id    ");
    for (const auto& [id, count] : by_intent) {
        std::printf("%llu=%zu  ", static_cast<unsigned long long>(id), count);
    }
    std::printf(
        "\n                (lifecycle_exercise: 1=marketable 2=resting 3=rejection bait)\n");

    const Ledger& ledger = strategy.broker().ledger();
    std::printf("simulated fills %zu blotter rows\n", ledger.blotter().size());

    // What the account went through, because an exercise that exhausts its
    // buying power stops exercising anything. A $100k cash account pays for
    // four $25k orders and then refuses the rest, which is how the first
    // configuration of this was found to be wrong.
    const auto dollars = [](Money m) { return static_cast<double>(m.micros()) / 1e6; };
    std::printf("cash            $%.0f start, $%.0f end%s\n",
                dollars(ledger.starting_cash()),
                dollars(ledger.cash()),
                ledger.cash() < Money{} ? "  <-- NEGATIVE: too big for this account" : "");
    std::printf("equity          $%.0f\n", dollars(ledger.equity()));

    double gross = 0.0;
    for (const Position* position : ledger.open_positions()) {
        gross += std::abs(position->quantity) * position->mark;
    }
    std::printf("open exposure   $%.0f gross at the end of the run\n", gross);

    std::printf("\nFirst ten decisions:\n");
    for (std::size_t i = 0; i < decisions.size() && i < 10; ++i) {
        std::printf("  %s\n", decisions[i].describe().c_str());
    }

    std::printf("\nNo network was touched and no order was sent. This says what the plugin\n"
                "would ask for; it says nothing about what a venue would do with it.\n");
    return 0;
}

}  // namespace marketscan::live
