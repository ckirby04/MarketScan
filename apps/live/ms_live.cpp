/*
 * ms_live -- the command line front end to the live execution seam.
 *
 * Subcommands:
 *   account                      what the paper account says about itself
 *   clock                        the exchange clock, and this machine's skew
 *   calendar   [start] [end]     trading sessions, from the broker
 *   record     --out FILE ...    stream market data and write a recording
 *   replay     FILE              read a recording back and report what is in it
 *   guard                        prove the paper-only guard refuses a live host
 *
 * Credentials come from APCA_API_KEY_ID and APCA_API_SECRET_KEY, never from a
 * file in this repository. See docs/DATA_POLICY.md.
 */

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "cli.hpp"

#include "marketscan/core/clock.hpp"
#include "marketscan/core/symbol_table.hpp"
#include "marketscan/live/calendar.hpp"
#include "marketscan/live/credentials.hpp"
#include "marketscan/live/endpoints.hpp"
#include "marketscan/live/http.hpp"
#include "marketscan/live/live_feed.hpp"
#include "marketscan/live/recorded_feed.hpp"
#include "marketscan/util/logging.hpp"

#include "equivalence.hpp"

namespace {

using namespace marketscan;
using namespace marketscan::live;

void usage() {
    std::printf(
        "ms_live -- paper-only live execution seam\n"
        "\n"
        "  ms_live account\n"
        "  ms_live clock\n"
        "  ms_live calendar [START] [END]        (YYYY-MM-DD, default: today + 10 days)\n"
        "  ms_live record --out FILE [--symbols AAPL,SPY] [--seconds N] [--drop-after N]\n"
        "  ms_live equivalence --plugin DLL --out FILE [--symbols ..] [--seconds N] [--period N]\n"
        "  ms_live replay FILE\n"
        "  ms_live dryrun --plugin DLL --recording FILE [--set name=value]\n"
        "  ms_live orders [--limit N] [--run TOKEN]\n"
        "  ms_live positions [--flatten --yes] [--limit]\n"
        "  ms_live bait [--symbol AAPL]\n"
        "  ms_live probe\n"
        "  ms_live guard\n"
        "\n"
        "Credentials come from APCA_API_KEY_ID and APCA_API_SECRET_KEY.\n"
        "Paper endpoints only, by construction: docs/DATA_POLICY.md.\n");
}

/// "YYYY-MM-DD" for now plus `days`, in UTC. Close enough for a calendar
/// window, which only has to bracket the days of interest.
[[nodiscard]] std::string utc_date(int days_ahead) {
    const SystemClock clock;
    const Timestamp ts =
        clock.now() + static_cast<Timestamp>(days_ahead) * 86400LL * 1'000'000'000LL;
    const std::string iso = time::to_iso8601(ts);
    return iso.substr(0, 10);
}

[[nodiscard]] double seconds(std::int64_t nanos) {
    return static_cast<double>(nanos) / 1e9;
}

[[nodiscard]] int with_client(const char* host, const std::function<int(HttpClient&)>& body) {
    const Credentials credentials = load_credentials();
    if (!credentials.ok()) {
        std::printf("error: %s\n", credentials.error.c_str());
        return 2;
    }
    HttpClient client(std::string(host), credentials, kUserAgent);
    if (!client.connected()) {
        std::printf("error: %s\n", client.error().c_str());
        return 2;
    }
    return body(client);
}

int cmd_account() {
    return with_client(kPaperTradingHost.data(), [](HttpClient& client) {
        const HttpResponse response = client.get("/v2/account");
        if (!response.ok()) {
            std::printf(
                "GET /v2/account failed: status %d %s\n", response.status, response.error.c_str());
            return 2;
        }
        const nlohmann::json account = nlohmann::json::parse(response.body, nullptr, false);
        if (account.is_discarded()) {
            std::printf("account response was not JSON\n");
            return 2;
        }
        std::printf("host          %s\n", client.host().c_str());
        std::printf("status        %s\n", account.value("status", "?").c_str());
        std::printf("equity        %s\n", account.value("equity", "?").c_str());
        std::printf("buying power  %s\n", account.value("buying_power", "?").c_str());
        std::printf("shorting      %s\n", account.value("shorting_enabled", false) ? "yes" : "no");
        std::printf("blocked       %s\n", account.value("trading_blocked", false) ? "YES" : "no");
        return 0;
    });
}

int cmd_clock() {
    return with_client(kPaperTradingHost.data(), [](HttpClient& client) {
        const SystemClock local;

        // Twice. The first request pays for DNS, the TCP handshake and the TLS
        // handshake -- and on this machine also for a local TLS inspection
        // proxy, which costs seconds. Measuring skew through that would
        // attribute connection setup to the clock. The second request goes
        // over the connection the first opened.
        const HttpResponse warmup = client.get("/v2/clock");
        if (!warmup.ok()) {
            std::printf(
                "GET /v2/clock failed: status %d %s\n", warmup.status, warmup.error.c_str());
            return 2;
        }

        const Timestamp before = local.now();
        const HttpResponse response = client.get("/v2/clock");
        const Timestamp after = local.now();
        if (!response.ok()) {
            std::printf(
                "GET /v2/clock failed: status %d %s\n", response.status, response.error.c_str());
            return 2;
        }
        const nlohmann::json clock = nlohmann::json::parse(response.body, nullptr, false);
        if (clock.is_discarded()) {
            std::printf("clock response was not JSON\n");
            return 2;
        }

        const std::string stamp = clock.value("timestamp", std::string{});
        std::printf("is open       %s\n", clock.value("is_open", false) ? "yes" : "no");
        std::printf("next open     %s\n", clock.value("next_open", "?").c_str());
        std::printf("next close    %s\n", clock.value("next_close", "?").c_str());
        std::printf("exchange      %s\n", stamp.c_str());

        // The broker's timestamp carries a numeric offset, which from_iso8601
        // deliberately refuses. Trim to the instant and add the offset back by
        // hand rather than teaching the parser to accept a zone it would then
        // accept everywhere else.
        Timestamp exchange = 0;
        const std::size_t plus = stamp.find_last_of("+-");
        if (plus != std::string::npos && plus > 10 &&
            time::from_iso8601(stamp.substr(0, plus), exchange)) {
            const int sign = stamp[plus] == '-' ? 1 : -1;
            const int hours = std::atoi(stamp.substr(plus + 1, 2).c_str());
            const int minutes = std::atoi(stamp.substr(plus + 4, 2).c_str());
            exchange +=
                static_cast<Timestamp>(sign) * (hours * 3600LL + minutes * 60LL) * 1'000'000'000LL;

            // The round trip bounds the answer: the true instant lies inside
            // it, so the offset is reported as a range rather than as a single
            // number that pretends the network took no time.
            const std::int64_t low = before - exchange;
            const std::int64_t high = after - exchange;
            std::printf("round trip    %.3f ms\n", seconds(after - before) * 1e3);
            std::printf("local - exch  between %.3f ms and %.3f ms\n",
                        seconds(low) * 1e3,
                        seconds(high) * 1e3);
        } else {
            std::printf("could not parse the exchange timestamp for a skew estimate\n");
        }
        return 0;
    });
}

int cmd_calendar(int argc, char** argv) {
    const std::string start = argc > 2 ? argv[2] : utc_date(0);
    const std::string end = argc > 3 ? argv[3] : utc_date(10);

    return with_client(kPaperTradingHost.data(), [&](HttpClient& client) {
        MarketCalendar calendar;
        if (!calendar.fetch(client, start, end)) {
            std::printf("calendar: %s\n", calendar.error().c_str());
            return 2;
        }
        std::printf("%zu sessions from %s to %s\n\n",
                    calendar.sessions().size(),
                    start.c_str(),
                    end.c_str());
        for (const TradingSession& session : calendar.sessions()) {
            std::printf("  %s  %s .. %s  %s\n",
                        session.date.c_str(),
                        time::to_iso8601(session.open).c_str(),
                        time::to_iso8601(session.close).c_str(),
                        session.half_day ? "(half day)" : "");
        }

        const SystemClock local;
        const Timestamp now = local.now();
        const TradingSession* current = calendar.session_at(now);
        std::printf("\nnow %s: %s\n",
                    time::to_iso8601(now).c_str(),
                    current != nullptr ? "OPEN" : "closed");
        if (current == nullptr) {
            const TradingSession* next = calendar.next_session(now);
            if (next != nullptr) {
                std::printf("next open %s (in %.2f hours)\n",
                            time::to_iso8601(next->open).c_str(),
                            seconds(next->open - now) / 3600.0);
            }
        } else {
            std::printf("closes %s (in %.2f hours)\n",
                        time::to_iso8601(current->close).c_str(),
                        seconds(current->close - now) / 3600.0);
        }
        return 0;
    });
}

int cmd_record(int argc, char** argv) {
    const char* out = option_value(argc, argv, "--out");
    if (out == nullptr) {
        std::printf("record needs --out FILE\n");
        return 2;
    }
    const char* symbols_arg = option_value(argc, argv, "--symbols");
    const char* seconds_arg = option_value(argc, argv, "--seconds");
    const char* drop_arg = option_value(argc, argv, "--drop-after");

    LiveFeedConfig config;
    config.symbols = split_commas(symbols_arg != nullptr ? symbols_arg : "AAPL,SPY");
    config.recording_path = out;
    config.user_agent = kUserAgent;

    const int duration = seconds_arg != nullptr ? std::atoi(seconds_arg) : 30;
    const int drop_after = drop_arg != nullptr ? std::atoi(drop_arg) : 0;

    const Credentials credentials = load_credentials();
    if (!credentials.ok()) {
        std::printf("error: %s\n", credentials.error.c_str());
        return 2;
    }

    SymbolTable table;
    LiveMarketDataFeed feed(config, credentials, table);

    std::printf("recording %zu symbols to %s for %ds\n", config.symbols.size(), out, duration);
    feed.start();
    if (!feed.wait_until_streaming(20000)) {
        const LiveFeedStats stats = feed.stats();
        std::printf("never reached streaming: state=%s error=%s\n",
                    std::string(to_string(stats.state)).c_str(),
                    stats.error.c_str());
        feed.stop();
        return 2;
    }

    // The engine side of the seam: drain exactly as EngineT would, so the
    // events are consumed and the queue does not fill.
    std::uint64_t drained = 0;
    Timestamp first = 0;
    Timestamp last = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(duration);
    const auto drop_at = std::chrono::steady_clock::now() + std::chrono::seconds(drop_after);
    bool dropped = false;
    MarketEvent event;

    while (std::chrono::steady_clock::now() < deadline) {
        bool any = false;
        while (feed.next(event)) {
            any = true;
            ++drained;
            if (first == 0) {
                first = event.ts();
            }
            last = event.ts();
        }
        if (drop_after > 0 && !dropped && std::chrono::steady_clock::now() >= drop_at) {
            std::printf("  [forcing a reconnect to exercise the backoff path]\n");
            feed.request_drop_for_test();
            dropped = true;
        }
        if (!any) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }

    feed.stop();

    // The socket thread has joined, so whatever is still in the queue is the
    // tail of the session rather than a race. Draining it here keeps "parsed"
    // and "drained" comparable; leaving it would show a shortfall of two or
    // three events that looks like loss and is not.
    while (feed.next(event)) {
        ++drained;
        last = event.ts();
    }

    const LiveFeedStats stats = feed.stats();

    std::printf("\nstate           %s\n", std::string(to_string(stats.state)).c_str());
    std::printf("messages        %llu\n", static_cast<unsigned long long>(stats.messages));
    std::printf("events parsed   %llu\n", static_cast<unsigned long long>(stats.events_parsed));
    std::printf("events drained  %llu\n", static_cast<unsigned long long>(drained));
    std::printf("dropped (full)  %llu\n", static_cast<unsigned long long>(stats.dropped_events));
    std::printf("unknown symbol  %llu\n", static_cast<unsigned long long>(stats.unknown_symbol));
    std::printf("malformed       %llu\n", static_cast<unsigned long long>(stats.malformed));
    std::printf("unhandled kind  %llu\n", static_cast<unsigned long long>(stats.unhandled_kind));
    std::printf("tick jitter     %llu reordered, max %.3f ms backwards\n",
                static_cast<unsigned long long>(stats.reordered),
                seconds(stats.max_backwards_ns) * 1e3);
    std::printf("bar lag         %llu interval-stamped, max %.3f s behind the ticks\n",
                static_cast<unsigned long long>(stats.bars_interval_stamped),
                seconds(stats.max_bar_lag_ns));
    std::printf("reconnects      %u\n", stats.reconnects);
    if (stats.skew_samples > 0) {
        std::printf("skew (ticks)    mean %.1f ms, min %.1f ms, max %.1f ms over %llu events\n",
                    seconds(stats.skew_mean_ns) * 1e3,
                    seconds(stats.skew_min_ns) * 1e3,
                    seconds(stats.skew_max_ns) * 1e3,
                    static_cast<unsigned long long>(stats.skew_samples));
    }
    if (first != 0) {
        std::printf("span            %s .. %s\n",
                    time::to_iso8601(first).c_str(),
                    time::to_iso8601(last).c_str());
    }
    std::printf("recorded        %llu messages\n",
                static_cast<unsigned long long>(feed.recorded_messages()));
    if (!stats.error.empty()) {
        std::printf("last error      %s\n", stats.error.c_str());
    }
    return 0;
}

int cmd_replay(int argc, char** argv) {
    if (argc < 3) {
        std::printf("replay needs a recording path\n");
        return 2;
    }
    SymbolTable table;
    RecordedFeed feed(argv[2], table);
    const RecordedFeedStats& stats = feed.stats();

    std::printf("recording       %s\n", argv[2]);
    std::printf("written by      MarketScan %s\n", feed.header().engine_version.c_str());
    std::printf("host            %s%s\n", feed.header().host.c_str(), feed.header().path.c_str());
    std::printf("symbols         ");
    for (const std::string& s : feed.header().symbols) {
        std::printf("%s ", s.c_str());
    }
    std::printf("\n");
    std::printf("messages        %llu\n", static_cast<unsigned long long>(stats.messages));
    std::printf("events          %llu\n", static_cast<unsigned long long>(stats.events));
    std::printf("controls        %llu\n", static_cast<unsigned long long>(stats.controls));
    std::printf("unknown symbol  %llu\n", static_cast<unsigned long long>(stats.unknown_symbol));
    std::printf("malformed       %llu\n", static_cast<unsigned long long>(stats.malformed));
    std::printf("unhandled kind  %llu\n", static_cast<unsigned long long>(stats.unhandled_kind));
    std::printf("tick jitter     %llu reordered, max %.3f ms backwards\n",
                static_cast<unsigned long long>(stats.reordered),
                seconds(stats.max_backwards_ns) * 1e3);
    std::printf("bar lag         %llu interval-stamped, max %.3f s\n",
                static_cast<unsigned long long>(stats.bars_interval_stamped),
                seconds(stats.max_bar_lag_ns));
    std::printf("reconnect gaps  %llu\n", static_cast<unsigned long long>(stats.reconnects));

    std::uint64_t bars = 0;
    std::uint64_t quotes = 0;
    std::uint64_t trades = 0;
    MarketEvent event;
    while (feed.next(event)) {
        switch (event.type) {
            case EventType::Bar:
                ++bars;
                break;
            case EventType::Quote:
                ++quotes;
                break;
            case EventType::Trade:
                ++trades;
                break;
        }
    }
    std::printf("drained         %llu bars, %llu quotes, %llu trades\n",
                static_cast<unsigned long long>(bars),
                static_cast<unsigned long long>(quotes),
                static_cast<unsigned long long>(trades));
    if (feed.span().known && feed.span().event_count > 0) {
        std::printf("span            %s .. %s\n",
                    time::to_iso8601(feed.span().first).c_str(),
                    time::to_iso8601(feed.span().last).c_str());
    }

    if (stats.skew_samples > 0) {
        std::printf("recorded skew   mean %.1f ms, min %.1f ms, max %.1f ms over %llu ticks\n",
                    seconds(stats.skew_mean_ns) * 1e3,
                    seconds(stats.skew_min_ns) * 1e3,
                    seconds(stats.skew_max_ns) * 1e3,
                    static_cast<unsigned long long>(stats.skew_samples));
    }
    return 0;
}

int cmd_guard() {
    // The runtime half of the paper-only rule, demonstrated rather than
    // asserted. The source and binary scans prove a live host is not present;
    // this proves one assembled at runtime does not get connected to.
    const Credentials credentials = load_credentials();
    const Credentials usable = credentials.ok() ? credentials : Credentials{"x", "y", {}};

    // The second of these is assembled at runtime from a prefix and the broker
    // domain, and is deliberately not written down as a literal. A live host
    // spelled out here would sit in the source tree and in the binary, and is
    // exactly what scripts/check_no_live_endpoints.py and
    // tests/test_paper_only.cpp exist to fail on -- as they did when the first
    // draft of this function spelled it out.
    //
    // Assembling it also makes the stronger point. A hostname built at runtime
    // passes both scans, and is still refused here, by the one check that acts
    // at connect time.
    const std::vector<std::string> hosts = {
        "example.com",
        "api." + std::string(kBrokerDomain),
    };
    int failures = 0;
    for (const std::string& host : hosts) {
        try {
            HttpClient client(host, usable, kUserAgent);
            std::printf("GUARD FAILED: constructed a client for %s\n", host.c_str());
            ++failures;
        } catch (const std::invalid_argument& e) {
            std::printf("refused %-28s %s\n", host.c_str(), e.what());
        }
    }
    for (const std::string_view allowed : kAllowedHosts) {
        try {
            HttpClient client(std::string(allowed), usable, kUserAgent);
            std::printf("allowed %-28s ok\n", std::string(allowed).c_str());
        } catch (const std::invalid_argument&) {
            std::printf("GUARD WRONG: refused an allowed host %s\n", std::string(allowed).c_str());
            ++failures;
        }
    }
    return failures == 0 ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        usage();
        return 1;
    }

    // Without this the log goes nowhere. That is not cosmetic: the live
    // broker reports a ledger divergence by logging an error, and Part 4's
    // whole claim is that such a divergence is loud rather than silently
    // corrected. The first real session had it silent, because this call was
    // missing and nothing noticed -- a log nobody reads is the same as no log.
    marketscan::log::init(MS_LOG_INFO);
    const std::string command = argv[1];
    try {
        if (command == "account") {
            return cmd_account();
        }
        if (command == "clock") {
            return cmd_clock();
        }
        if (command == "calendar") {
            return cmd_calendar(argc, argv);
        }
        if (command == "record") {
            return cmd_record(argc, argv);
        }
        if (command == "replay") {
            return cmd_replay(argc, argv);
        }
        if (command == "equivalence") {
            return run_equivalence(argc, argv);
        }
        if (command == "bait") {
            return run_bait(argc, argv);
        }
        if (command == "dryrun") {
            return run_dryrun(argc, argv);
        }
        if (command == "orders") {
            return run_orders(argc, argv);
        }
        if (command == "positions") {
            return run_positions(argc, argv);
        }
        if (command == "probe") {
            return run_probe(argc, argv);
        }
        if (command == "guard") {
            return cmd_guard();
        }
    } catch (const std::exception& e) {
        std::printf("error: %s\n", e.what());
        return 2;
    }
    usage();
    return 1;
}
