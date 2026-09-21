/*
 * `ms_live probe` -- how many TLS connections can this process have open at
 * once before requests stop completing?
 *
 * WHY THIS EXISTS
 *
 * The first real run of the equivalence test hung. Two threads sat in WinHTTP
 * waits with the process using a quarter of a second of CPU in nine minutes:
 * the market-data socket, the trade-updates socket and a REST request, all
 * TLS, all from one process. Any of the three alone had already worked.
 *
 * Guessing at that is how a workaround gets written for a cause nobody
 * established. So this measures it instead: one REST call at a time, timed,
 * with zero, one and then two long-lived WebSockets open alongside. The
 * numbers say whether concurrency is the problem and at what point it starts.
 *
 * It sends nothing but /v2/clock, which is the cheapest endpoint there is, and
 * it sends them one at a time with a pause. docs/DATA_POLICY.md applies here
 * as everywhere: a probe is not a licence to hammer.
 */

#include "equivalence.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "marketscan/core/clock.hpp"
#include "marketscan/live/credentials.hpp"
#include "marketscan/live/endpoints.hpp"
#include "marketscan/live/http.hpp"
#include "marketscan/live/websocket.hpp"

namespace marketscan::live {
namespace {

constexpr const char* kUserAgent = "MarketScan/0.1 (research tool)";

struct Timing {
    int status = 0;
    double milliseconds = 0.0;
    std::string error;
};

[[nodiscard]] Timing timed_get(HttpClient& client, const char* path) {
    Timing out;
    const auto start = std::chrono::steady_clock::now();
    const HttpResponse response = client.get(path);
    const auto end = std::chrono::steady_clock::now();
    out.milliseconds =
        std::chrono::duration_cast<std::chrono::microseconds>(end - start).count() / 1000.0;
    out.status = response.status;
    out.error = response.error;
    return out;
}

void report(const char* label, HttpClient& client, int calls) {
    std::printf("  %-34s", label);
    std::fflush(stdout);
    for (int i = 0; i < calls; ++i) {
        const Timing timing = timed_get(client, "/v2/clock");
        if (!timing.error.empty()) {
            std::printf(" [%s]", timing.error.c_str());
        } else {
            std::printf(" %.0fms", timing.milliseconds);
        }
        std::fflush(stdout);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
    }
    std::printf("\n");
    std::fflush(stdout);
}

/// A WebSocket held open on a thread of its own, draining whatever arrives.
class HeldSocket {
public:
    HeldSocket(std::string host, std::string path, std::string open_message)
        : host_(std::move(host)), path_(std::move(path)), open_(std::move(open_message)) {}

    ~HeldSocket() { stop(); }

    [[nodiscard]] bool start() {
        thread_ = std::thread([this] { pump(); });
        for (int i = 0; i < 200 && !ready_.load() && !failed_.load(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        return ready_.load();
    }

    void stop() {
        stopping_.store(true);
        {
            const std::lock_guard<std::mutex> lock(socket_mutex_);
            if (active_ != nullptr) {
                active_->request_close();
            }
        }
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    [[nodiscard]] std::uint64_t messages() const { return messages_.load(); }

private:
    void pump() {
        WebSocket socket;
        if (!socket.connect(host_, path_, kUserAgent)) {
            failed_.store(true);
            return;
        }
        if (!open_.empty() && !socket.send(open_)) {
            failed_.store(true);
            return;
        }
        {
            const std::lock_guard<std::mutex> lock(socket_mutex_);
            active_ = &socket;
        }
        ready_.store(true);
        while (!stopping_.load()) {
            const SocketMessage message = socket.receive(250);
            if (message.closed) {
                break;
            }
            if (message.ok) {
                messages_.fetch_add(1);
            }
        }
        {
            const std::lock_guard<std::mutex> lock(socket_mutex_);
            active_ = nullptr;
        }
        socket.close();
    }

    std::string host_;
    std::string path_;
    std::string open_;
    std::mutex socket_mutex_;
    WebSocket* active_ = nullptr;
    std::thread thread_;
    std::atomic<bool> stopping_{false};
    std::atomic<bool> ready_{false};
    std::atomic<bool> failed_{false};
    std::atomic<std::uint64_t> messages_{0};
};

}  // namespace

int run_probe(int argc, char** argv) {
    (void)argc;
    (void)argv;

    const Credentials credentials = load_credentials();
    if (!credentials.ok()) {
        std::printf("error: %s\n", credentials.error.c_str());
        return 2;
    }

    HttpClient client(std::string(kPaperTradingHost), credentials, kUserAgent);
    if (!client.connected()) {
        std::printf("error: %s\n", client.error().c_str());
        return 2;
    }

    std::printf("TLS concurrency probe -- one REST call at a time, timed.\n");
    std::printf("Each row adds one long-lived socket and repeats the same request.\n\n");

    report("0 sockets open", client, 4);

    // The market data stream. Authenticates with the same credentials and then
    // subscribes to nothing, so it stays open and nearly silent.
    {
        std::string auth = R"({"action":"auth","key":")" + credentials.key_id + R"(","secret":")" +
                           credentials.secret + R"("})";
        HeldSocket data(
            std::string(kMarketDataStreamHost), std::string(kMarketDataStreamPath), auth);
        if (!data.start()) {
            std::printf("  could not open the market data socket\n");
            return 2;
        }
        report("1 socket (market data)", client, 4);

        std::string listen = R"({"action":"authenticate","data":{"key_id":")" + credentials.key_id +
                             R"(","secret_key":")" + credentials.secret + R"("}})";
        HeldSocket updates(
            std::string(kPaperTradingHost), std::string(kPaperTradeStreamPath), listen);
        if (!updates.start()) {
            std::printf("  could not open the trade-updates socket\n");
            return 2;
        }
        report("2 sockets (+ trade updates)", client, 6);

        // A POST, with both sockets open. No earlier probe exercised one:
        // every REST call this project had made until the first equivalence
        // run was a GET, and the first thing that run did differently was POST
        // an order.
        //
        // The body is deliberately incomplete, so the venue refuses it and no
        // order is created. What is measured is whether a POST completes at
        // all, not what the venue thinks of it.
        std::printf("\n  POST /v2/orders, 2 sockets open (invalid body; nothing is placed)\n");
        std::fflush(stdout);
        {
            const auto start = std::chrono::steady_clock::now();
            const HttpResponse response = client.post("/v2/orders", R"({"probe":"no symbol"})");
            const auto end = std::chrono::steady_clock::now();
            const double ms =
                std::chrono::duration_cast<std::chrono::microseconds>(end - start).count() / 1000.0;
            std::printf("    status %d in %.0fms%s%s\n",
                        response.status,
                        ms,
                        response.error.empty() ? "" : ", error: ",
                        response.error.c_str());
            std::fflush(stdout);
        }

        std::printf("\n  market data messages seen  %llu\n",
                    static_cast<unsigned long long>(data.messages()));
        std::printf("  trade update messages seen %llu\n",
                    static_cast<unsigned long long>(updates.messages()));

        // Closing a *silent* socket is the operation that hung before the
        // receive timeout was fixed. Timed, so a regression shows as a number
        // rather than as a run that never ends.
        std::printf("  closing both sockets");
        std::fflush(stdout);
        const auto start = std::chrono::steady_clock::now();
        updates.stop();
        data.stop();
        const auto end = std::chrono::steady_clock::now();
        std::printf(" ... %.0fms\n",
                    std::chrono::duration_cast<std::chrono::microseconds>(end - start).count() /
                        1000.0);
        std::fflush(stdout);
    }

    report("0 sockets again", client, 4);

    std::printf("\nA row whose times jump by orders of magnitude, or whose calls never\n"
                "return, is the answer: this process cannot hold that many TLS connections\n"
                "at once on this machine. See docs/EXECUTION.md.\n");
    return 0;
}

}  // namespace marketscan::live
