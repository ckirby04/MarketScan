/*
 * The driver for `ms_live equivalence`.
 *
 * Runs one plugin binary against the paper venue while recording the market
 * data, then runs the same file again against that recording, and compares the
 * decisions. See equivalence.hpp for what is compared and what a pass does and
 * does not establish.
 */

#include "equivalence.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "marketscan/core/clock.hpp"
#include "marketscan/core/engine.hpp"
#include "marketscan/core/symbol_table.hpp"
#include "marketscan/exec/fill_model.hpp"
#include "marketscan/live/credentials.hpp"
#include "marketscan/live/endpoints.hpp"
#include "marketscan/live/live_broker.hpp"
#include "marketscan/live/live_feed.hpp"
#include "marketscan/live/order_map.hpp"
#include "marketscan/live/recorded_broker.hpp"
#include "marketscan/live/recorded_feed.hpp"
#include "marketscan/live/recorder.hpp"
#include "marketscan/strategy/plugin_loader.hpp"
#include "marketscan/strategy/strategy_instance.hpp"

namespace marketscan::live {
namespace {

constexpr const char* kUserAgent = "MarketScan/0.1 (research tool)";

/// Says where the run has got to, and flushes.
///
/// Redirected output is block buffered, so a run that stops in one of these
/// phases otherwise prints nothing at all about which one -- which is how the
/// first real session of this command cost ten minutes of guessing.
void phase(const char* what) {
    std::printf("[%s]\n", what);
    std::fflush(stdout);
}

[[nodiscard]] const char* option_value(int argc, char** argv, const char* name) {
    for (int i = 0; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], name) == 0) {
            return argv[i + 1];
        }
    }
    return nullptr;
}

[[nodiscard]] bool has_flag(int argc, char** argv, const char* name) {
    for (int i = 0; i < argc; ++i) {
        if (std::strcmp(argv[i], name) == 0) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] std::vector<std::string> split_commas(const std::string& text) {
    std::vector<std::string> out;
    std::string current;
    for (const char c : text) {
        if (c == ',') {
            if (!current.empty()) {
                out.push_back(current);
            }
            current.clear();
        } else {
            current.push_back(c);
        }
    }
    if (!current.empty()) {
        out.push_back(current);
    }
    return out;
}

/// One loaded plugin, with the loader that owns it.
struct LoadedPlugin {
    std::unique_ptr<PluginLoader> loader;
    const PluginInfo* info = nullptr;
    std::string error;

    [[nodiscard]] bool ok() const noexcept { return info != nullptr; }
};

/// Loads the plugin file at `path`. The loader scans directories, so the
/// file's parent is the root and the plugin is picked out by filename.
[[nodiscard]] LoadedPlugin load_plugin(const std::filesystem::path& plugin) {
    LoadedPlugin out;
    out.loader =
        std::make_unique<PluginLoader>(std::vector<std::filesystem::path>{plugin.parent_path()});
    out.loader->rescan();

    for (const PluginInfo& info : out.loader->plugins()) {
        if (info.loaded() && info.path.filename() == plugin.filename()) {
            out.info = &info;
            return out;
        }
    }

    out.error = "no loadable plugin named " + plugin.filename().string() + " under " +
                plugin.parent_path().string();
    for (const PluginInfo& info : out.loader->plugins()) {
        if (!info.error.empty()) {
            out.error += "; " + info.path.filename().string() + ": " + info.error;
        }
    }
    return out;
}

}  // namespace

int run_equivalence(int argc, char** argv) {
    const char* plugin_arg = option_value(argc, argv, "--plugin");
    const char* out_arg = option_value(argc, argv, "--out");
    const char* symbols_arg = option_value(argc, argv, "--symbols");
    const char* seconds_arg = option_value(argc, argv, "--seconds");
    const char* period_arg = option_value(argc, argv, "--period");
    const bool keep = has_flag(argc, argv, "--keep");
    const bool extended = has_flag(argc, argv, "--extended-hours");
    const char* drop_arg = option_value(argc, argv, "--drop-after");

    if (plugin_arg == nullptr || out_arg == nullptr) {
        std::printf("equivalence needs --plugin DLL and --out RECORDING\n");
        return 2;
    }

    const std::filesystem::path plugin = plugin_arg;
    const std::filesystem::path recording = out_arg;
    const std::vector<std::string> tickers =
        split_commas(symbols_arg != nullptr ? symbols_arg : "AAPL,SPY");
    const int duration = seconds_arg != nullptr ? std::atoi(seconds_arg) : 120;
    const double period = period_arg != nullptr ? std::atof(period_arg) : 200.0;
    const int drop_after = drop_arg != nullptr ? std::atoi(drop_arg) : 0;

    const Credentials credentials = load_credentials();
    if (!credentials.ok()) {
        std::printf("error: %s\n", credentials.error.c_str());
        return 2;
    }

    std::vector<Decision> live_decisions;
    std::uint64_t live_events = 0;

    // Per symbol, because a vacuous run needs to say *why* it was vacuous. The
    // first real session ran five symbols in extended hours and four of them
    // produced no events at all -- a fact the report did not mention, so it
    // read as "the strategy is broken" rather than "you asked for four silent
    // instruments".
    std::vector<std::uint64_t> per_symbol(tickers.size(), 0);
    LiveBrokerStats broker_stats;
    Reconciliation reconciled;
    std::string strategy_id;

    /// Measured before the file is deleted, because the last run reported an
    /// estimate and could not check it.
    std::uintmax_t recording_bytes = 0;
    std::uint64_t recorded_lines = 0;

    // --- The live half ----------------------------------------------------
    {
        const LoadedPlugin loaded = load_plugin(plugin);
        if (!loaded.ok()) {
            std::printf("error: %s\n", loaded.error.c_str());
            return 2;
        }
        strategy_id = loaded.info->id;

        SymbolTable symbols;

        // One recorder, shared by the feed and the broker, so market data,
        // orders out and answers back land in one file in one arrival order.
        // That interleaving is the whole point: a recording of only one
        // direction is what made the last equivalence run wrong.
        const std::string run_token = make_run_token();

        RecordingHeader header;
        header.host = std::string(kMarketDataStreamHost);
        header.path = std::string(kMarketDataStreamPath);
        header.feed = "iex";
        header.symbols = tickers;
        header.run_token = run_token;
        {
            const SystemClock started;
            header.started = started.now();
        }
        auto recorder = std::make_shared<StreamRecorder>(recording, header);

        LiveFeedConfig feed_config;
        feed_config.symbols = tickers;
        feed_config.recorder = recorder;
        feed_config.user_agent = kUserAgent;
        LiveMarketDataFeed feed(feed_config, credentials, symbols);

        LiveBrokerConfig broker_config;
        broker_config.user_agent = kUserAgent;
        broker_config.allow_extended_hours = extended;
        broker_config.run_token = run_token;
        broker_config.recorder = recorder;
        LiveBroker broker(broker_config, credentials, &symbols);

        // The strategy needs a fill model and a market view because the
        // constructor takes them; live, the broker never consults the model.
        // Passing one rather than null keeps the two constructions identical,
        // which is the whole point of not having a live code path.
        SpreadCrossingFillModel fill_model{};
        MarketView market;
        SystemClock clock;

        StrategyRuntimeConfig runtime;
        runtime.broker = &broker;
        runtime.order_id_base = 1'000'000;

        PluginInstance instance = loaded.loader->create(loaded.info->id);
        if (!instance.valid()) {
            std::printf("error: could not instantiate '%s'\n", loaded.info->id.c_str());
            return 2;
        }

        StrategyInstance strategy(loaded.info->id,
                                  loaded.info->display_name,
                                  std::move(instance),
                                  loaded.info->required_symbols,
                                  runtime,
                                  &fill_model,
                                  &market,
                                  &symbols,
                                  &clock);
        strategy.set_parameter("period", period);

        std::printf("live: %s against %s for %ds, symbols",
                    loaded.info->id.c_str(),
                    std::string(broker.venue()).c_str(),
                    duration);
        for (const std::string& ticker : tickers) {
            std::printf(" %s", ticker.c_str());
        }
        std::printf("\n");

        phase("starting the trade-updates stream");
        if (!broker.start()) {
            std::printf("error: the trade-updates stream would not start\n");
            return 2;
        }
        phase("starting the market data feed");
        feed.start();
        if (!feed.wait_until_streaming(20000)) {
            std::printf("error: the market data feed never reached streaming: %s\n",
                        feed.stats().error.c_str());
            feed.stop();
            broker.stop();
            return 2;
        }

        // Symbols are interned by the feed's constructor, so this resolves to
        // the same ids the replay half will assign from the recording header.
        strategy.resolve_subscriptions();
        strategy.start();
        phase("streaming");

        // The engine loop, written out rather than hidden, because this loop
        // is what the comparison is about. Same shape as EngineT::step: pull an
        // event, update the book, dispatch it, then let the broker act.
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(duration);
        const auto drop_at = std::chrono::steady_clock::now() + std::chrono::seconds(drop_after);
        bool dropped = false;
        MarketEvent event;
        while (std::chrono::steady_clock::now() < deadline) {
            // The reconnect exercise. Dropping the trade-updates connection
            // while orders are live is the case that matters: the venue keeps
            // working them, this process stops hearing about them, and the only
            // way back is the client order id -- see OrderMap.
            if (drop_after > 0 && !dropped && std::chrono::steady_clock::now() >= drop_at) {
                phase("dropping the trade-updates connection on purpose");
                broker.request_drop_for_test();
                dropped = true;
            }
            bool any = false;
            while (feed.next(event)) {
                any = true;
                ++live_events;
                const std::string_view name = symbols.name(event.symbol());
                for (std::size_t i = 0; i < tickers.size(); ++i) {
                    if (tickers[i] == name) {
                        ++per_symbol[i];
                        break;
                    }
                }
                market.apply(event);
                switch (event.type) {
                    case EventType::Bar:
                        strategy.on_bar(event.bar);
                        break;
                    case EventType::Quote:
                        strategy.on_quote(event.quote);
                        break;
                    case EventType::Trade:
                        strategy.on_trade(event.trade);
                        break;
                }
                strategy.on_event_complete(event);
            }
            if (!any) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }

        phase("stopping the feed");
        feed.stop();

        // The tail. The socket thread has joined, so whatever is still queued
        // is the end of the session rather than a race -- and it has to go
        // through the strategy, because the replay half will see it.
        //
        // Without this the two halves saw different numbers of events: a real
        // run reported 609 live against 610 replayed. The decisions happened to
        // match, which is worse than if they had not: the next undrained event
        // to trigger an order would have been reported as a divergence that was
        // really a harness bug.
        while (feed.next(event)) {
            ++live_events;
            const std::string_view tail_name = symbols.name(event.symbol());
            for (std::size_t i = 0; i < tickers.size(); ++i) {
                if (tickers[i] == tail_name) {
                    ++per_symbol[i];
                    break;
                }
            }
            market.apply(event);
            switch (event.type) {
                case EventType::Bar:
                    strategy.on_bar(event.bar);
                    break;
                case EventType::Quote:
                    strategy.on_quote(event.quote);
                    break;
                case EventType::Trade:
                    strategy.on_trade(event.trade);
                    break;
            }
            strategy.on_event_complete(event);
        }

        phase("stopping the strategy");
        strategy.stop();

        // Leave nothing working at the venue, then give it a moment to answer
        // and drain what it said.
        phase("cancelling anything still working");
        broker.cancel_all_open(clock.now());
        std::this_thread::sleep_for(std::chrono::milliseconds(2000));
        broker.on_market_event(kInvalidSymbol, clock.now());

        phase("reconciling against the venue");
        reconciled = broker.reconcile();
        broker_stats = broker.stats();
        live_decisions = decisions_of(broker, symbols);

        phase("stopping the trade-updates stream");
        broker.stop();

        // Closed here, after both writers are done, and measured before
        // anything deletes it.
        recorder->close();
        recorded_lines = recorder->messages();
        std::error_code size_error;
        recording_bytes = std::filesystem::file_size(recording, size_error);
        if (size_error) {
            recording_bytes = 0;
        }
    }

    // --- The replay half --------------------------------------------------
    std::vector<Decision> replay_decisions;
    std::uint64_t replay_events = 0;
    bool replayed_venue = false;
    CorrespondenceFailure correspondence;
    std::size_t submissions_matched = 0;
    std::size_t submissions_recorded = 0;
    std::size_t responses_delivered = 0;
    std::size_t responses_recorded = 0;

    {
        // The same plugin file. Not a rebuild and not a variant: the binary
        // the live half just ran, loaded again.
        const LoadedPlugin loaded = load_plugin(plugin);
        if (!loaded.ok()) {
            std::printf("error: %s\n", loaded.error.c_str());
            return 2;
        }

        phase("replaying the recording");
        SymbolTable symbols;

        // Read once, to find out what this file can answer for. A recording with
        // a venue side replays the venue; one without falls back to the fill
        // simulator. Decided by the file's contents, never by a flag -- and
        // nothing in the engine or the strategy can tell which it got.
        const RecordingContents contents = read_recording_contents(recording);

        auto feed = std::make_unique<RecordedFeed>(recording, symbols);
        replay_events = feed->stats().events;

        ReplayPosition position;
        feed->set_position_sink(&position);

        SpreadCrossingFillModel fill_model{};
        Engine engine;
        engine.add_source(std::move(feed));

        StrategyRuntimeConfig runtime;
        runtime.order_id_base = 1'000'000;

        std::unique_ptr<RecordedBroker> venue;
        if (contents.has_venue_side()) {
            venue = std::make_unique<RecordedBroker>(contents.submissions,
                                                     contents.responses,
                                                     &symbols,
                                                     runtime.starting_cash,
                                                     runtime.order_id_base,
                                                     contents.header.run_token);
            venue->set_position_source(&position);
            runtime.broker = venue.get();
        }

        PluginInstance instance = loaded.loader->create(loaded.info->id);
        if (!instance.valid()) {
            std::printf("error: could not instantiate '%s' for the replay\n",
                        loaded.info->id.c_str());
            return 2;
        }

        StrategyInstance strategy(loaded.info->id,
                                  loaded.info->display_name,
                                  std::move(instance),
                                  loaded.info->required_symbols,
                                  runtime,
                                  &fill_model,
                                  &engine.market(),
                                  &symbols,
                                  &engine.clock());
        strategy.set_parameter("period", period);
        strategy.resolve_subscriptions();
        engine.add_sink(&strategy);
        strategy.start();
        engine.run();

        if (venue) {
            // A session ends by cancelling everything, and those answers arrive
            // after the last market event. Without this they would never be
            // applied and the two ledgers would differ for no good reason.
            venue->drain_remaining(engine.now());
        }
        strategy.stop();

        replay_decisions = decisions_of(strategy.broker(), symbols);
        if (venue) {
            replayed_venue = true;
            correspondence = venue->failure();
            submissions_matched = venue->submissions_matched();
            submissions_recorded = venue->submissions_recorded();
            responses_delivered = venue->responses_delivered();
            responses_recorded = venue->responses_recorded();
        }
    }

    // --- The comparison ---------------------------------------------------

    std::printf("\nstrategy         %s\n", strategy_id.c_str());
    std::printf("live events      %llu\n", static_cast<unsigned long long>(live_events));
    std::printf("replay events    %llu\n", static_cast<unsigned long long>(replay_events));
    std::printf("live decisions   %zu\n", live_decisions.size());
    std::printf("replay decisions %zu\n", replay_decisions.size());
    std::printf("venue            %llu submitted, %llu accepted, %llu filled, %llu partial, "
                "%llu cancelled, %llu expired, %llu rejected, %llu lost cancel races\n",
                static_cast<unsigned long long>(broker_stats.submitted),
                static_cast<unsigned long long>(broker_stats.accepted),
                static_cast<unsigned long long>(broker_stats.filled),
                static_cast<unsigned long long>(broker_stats.partial_fills),
                static_cast<unsigned long long>(broker_stats.cancelled),
                static_cast<unsigned long long>(broker_stats.expired),
                static_cast<unsigned long long>(broker_stats.rejected),
                static_cast<unsigned long long>(broker_stats.lost_cancel_races));
    if (broker_stats.reconnects > 0) {
        std::printf("                 %u trade-stream reconnect(s), %llu order(s) reclaimed, "
                    "%llu stale update(s) dropped\n",
                    broker_stats.reconnects,
                    static_cast<unsigned long long>(broker_stats.reclaimed_after_reconnect),
                    static_cast<unsigned long long>(broker_stats.stale_updates));
        if (broker_stats.reclaimed_after_reconnect == 0) {
            // A reconnect that reclaimed nothing exercised the reconnect and
            // not the reclaim. Said plainly, because the two are easy to
            // conflate and only one of them is the hard part.
            std::printf("                 (nothing was live at the drop, so the reclaim path "
                        "ran empty)\n");
        }
    }
    if (broker_stats.foreign_updates > 0) {
        std::printf("                 %llu update(s) for orders this run did not submit\n",
                    static_cast<unsigned long long>(broker_stats.foreign_updates));
    }
    if (reconciled.checked) {
        std::printf("reconciliation   %s\n",
                    reconciled.agreed() ? "positions agree with the venue" : "DIVERGED");
        // Named here as well as logged. "DIVERGED -- see the log above" was
        // useless the first time it printed, because nothing had initialised
        // the log and there was no log above.
        for (const Divergence& divergence : reconciled.divergences) {
            std::printf("                 %s: this run says %s, the venue says %s\n",
                        divergence.what.c_str(),
                        divergence.ours.c_str(),
                        divergence.theirs.c_str());
        }
    } else if (!reconciled.error.empty()) {
        std::printf("reconciliation   could not be checked: %s\n", reconciled.error.c_str());
    }

    const auto cleanup = [&] {
        if (keep) {
            std::printf("recording        kept at %s -- market data, never commit it\n",
                        recording.string().c_str());
            return;
        }
        // The recording is real market data under a subscriber agreement.
        // Deleted by default; --keep exists for debugging a divergence, and
        // .gitignore still stops it reaching a commit.
        std::error_code ec;
        std::filesystem::remove(recording, ec);
        std::printf("recording        deleted (market data; see docs/DATA_POLICY.md)\n");
    };

    std::printf("recording       %llu lines, %llu bytes (%.2f MB)\n",
                static_cast<unsigned long long>(recorded_lines),
                static_cast<unsigned long long>(recording_bytes),
                static_cast<double>(recording_bytes) / (1024.0 * 1024.0));
    if (replayed_venue) {
        std::printf("venue side      replayed from the recording: %zu of %zu submissions "
                    "matched, %zu of %zu responses delivered\n",
                    submissions_matched,
                    submissions_recorded,
                    responses_delivered,
                    responses_recorded);
    } else {
        std::printf("venue side      NOT in the recording -- replay used the fill simulator, so "
                    "refusals and partials did not reproduce\n");
    }

    std::printf("events per sym  ");
    std::size_t silent = 0;
    for (std::size_t i = 0; i < tickers.size(); ++i) {
        std::printf(" %s=%llu", tickers[i].c_str(), static_cast<unsigned long long>(per_symbol[i]));
        silent += per_symbol[i] == 0 ? 1 : 0;
    }
    std::printf("\n");
    if (silent > 0) {
        std::printf("                 %zu of %zu symbols produced nothing at all\n",
                    silent,
                    tickers.size());
    }

    if (live_decisions.empty()) {
        // Two empty sequences compare equal, and that pass would prove
        // nothing. This is the vacuous case, and it is reported as a failure
        // of the test rather than as a result.
        cleanup();
        std::printf("\nVACUOUS: the strategy made no decisions, so there is nothing to "
                    "compare.\n");
        if (silent == tickers.size()) {
            std::printf("Every symbol was silent: the market is shut, or these instruments do "
                        "not trade on this feed.\n");
        } else if (silent > 0) {
            std::printf("Some symbols were silent -- see the line above. A strategy pointed at "
                        "one of those sees nothing, which is what happened the first time this "
                        "ran.\n");
        } else {
            std::printf("Data arrived but no order followed. Run for longer, or lower "
                        "--period.\n");
        }
        return 3;
    }

    // The correspondence assertion fires before the decision comparison does,
    // because it knows *which* order first disagreed and the comparison only
    // knows that two lists differ.
    if (correspondence.happened) {
        cleanup();
        std::printf("\nDIVERGED: replay stopped corresponding to the recording.\n\n%s\n",
                    correspondence.describe().c_str());
        std::printf("\nReported where it happened rather than fed forward. The comparison is "
                    "not relaxed to make it pass.\n");
        return 1;
    }

    const DecisionDiff diff = compare(live_decisions, replay_decisions);
    cleanup();

    if (diff.equal) {
        std::printf("\nEQUIVALENT: %zu decisions identical across live and replay.\n",
                    live_decisions.size());
        return 0;
    }

    std::printf("\nDIVERGED: %s\n", diff.what.c_str());
    std::printf("  live   %s\n", diff.live.c_str());
    std::printf("  replay %s\n", diff.replayed.c_str());
    std::printf(
        "\nThe smallest differing decision, reported rather than absorbed. The comparison is "
        "not relaxed to make it pass.\n");
    return 1;
}

}  // namespace marketscan::live
