// The research path's client for the job service, without the network.
//
// docs/specs/SPEC_DESKTOP_CHAT.md section 3. Every test here hands the client a
// fake transport; none opens a connection (SPEC_RESEARCH_VIEW.md section 8).
// What is checked is what the client adds on top of HTTP: which hosts it will
// talk to at all, that the key goes on the wire and nowhere else, and that a
// streamed conversation turn arrives as one event per line however the bytes
// were chunked.

#include <cstdlib>
#include <optional>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include "marketscan/live/endpoints.hpp"  // header only: the broker's hosts, to refuse
#include "marketscan/research/endpoints.hpp"
#include "marketscan/research/service_client.hpp"

using namespace marketscan::research;
using nlohmann::json;

namespace {

constexpr const char* kKey = "test-key-that-must-not-leak";

/// Records what it was asked and answers with a scripted response, streaming
/// `lines` through the handler when one is set.
struct FakeTransport final : ServiceTransport {
    struct Seen {
        std::string method;
        std::string path;
        std::string body;
        std::string authorization;
        long long timeout_seconds = 0;
        bool streamed = false;
    };

    std::vector<Seen>* seen;
    ServiceResponse answer;
    std::vector<std::string> lines;

    explicit FakeTransport(std::vector<Seen>* log,
                           ServiceResponse response = {200, "{}", "", 0, false})
        : seen(log), answer(std::move(response)) {}

    ServiceResponse send(const ServiceRequest& request) override {
        seen->push_back(Seen{std::string(request.method),
                             std::string(request.path),
                             std::string(request.body),
                             std::string(request.authorization),
                             static_cast<long long>(request.timeout.count()),
                             request.on_line != nullptr});
        ServiceResponse out = answer;
        if (request.on_line != nullptr) {
            out.body.clear();
            for (const std::string& line : lines) {
                if (!(*request.on_line)(line)) {
                    out.cancelled = true;
                    break;
                }
            }
        }
        return out;
    }
};

ServiceUrl deployed() {
    return parse_service_url("https://" + std::string(kServiceHost));
}

/// Sets an environment variable for one scope and restores it after.
class ScopedEnv {
public:
    ScopedEnv(const char* name, std::optional<std::string> value) : name_(name) {
#if defined(_MSC_VER)
        char* buffer = nullptr;
        std::size_t length = 0;
        if (_dupenv_s(&buffer, &length, name) == 0 && buffer != nullptr) {
            old_ = std::string(buffer);
            std::free(buffer);
        }
        _putenv_s(name, value ? value->c_str() : "");
#endif
    }

    ~ScopedEnv() {
#if defined(_MSC_VER)
        _putenv_s(name_, old_ ? old_->c_str() : "");
#endif
    }

    ScopedEnv(const ScopedEnv&) = delete;
    ScopedEnv& operator=(const ScopedEnv&) = delete;

private:
    const char* name_;
    std::optional<std::string> old_;
};

}  // namespace

TEST_CASE("a service URL is a scheme, a host and a port") {
    const ServiceUrl a = parse_service_url("https://example.run.app");
    CHECK(a.tls);
    CHECK(a.host == "example.run.app");
    CHECK(a.port == 443);
    const ServiceUrl b = parse_service_url("http://127.0.0.1:8080/");
    CHECK_FALSE(b.tls);
    CHECK(b.host == "127.0.0.1");
    CHECK(b.port == 8080);

    CHECK_THROWS_AS(parse_service_url("example.run.app"), std::invalid_argument);
    CHECK_THROWS_AS(parse_service_url("ftp://example.run.app"), std::invalid_argument);
    CHECK_THROWS_AS(parse_service_url("https://example.run.app/jobs"), std::invalid_argument);
    CHECK_THROWS_AS(parse_service_url("https://user@example.run.app"), std::invalid_argument);
    CHECK_THROWS_AS(parse_service_url("https://example.run.app:http"), std::invalid_argument);
    CHECK_THROWS_AS(parse_service_url("https://example.run.app:70000"), std::invalid_argument);
    CHECK_THROWS_AS(parse_service_url("https://"), std::invalid_argument);
}

TEST_CASE("the client talks only to the service, and only over TLS beyond this machine") {
    std::vector<FakeTransport::Seen> seen;
    const auto make = [&](const std::string& url) {
        return ServiceClient(parse_service_url(url), kKey, std::make_unique<FakeTransport>(&seen));
    };

    CHECK_NOTHROW(make("https://" + std::string(kServiceHost)));
    CHECK_NOTHROW(make("http://127.0.0.1:8080"));
    CHECK_NOTHROW(make("http://localhost:8080"));

    // The control: it has to say no to something, and to a broker in particular.
    CHECK_THROWS_AS(make("https://example.com"), std::invalid_argument);
    CHECK_THROWS_AS(make("https://" + std::string(marketscan::live::kPaperTradingHost)),
                    std::invalid_argument);
    CHECK_THROWS_AS(make("https://" + std::string(marketscan::live::kMarketDataHost)),
                    std::invalid_argument);
    // A near miss of the service's own host.
    CHECK_THROWS_AS(make("https://x" + std::string(kServiceHost)), std::invalid_argument);
    // Plain HTTP to the deployed service would put the key on the wire in clear.
    CHECK_THROWS_AS(make("http://" + std::string(kServiceHost)), std::invalid_argument);
    CHECK(seen.empty());  // refused before anything was sent
}

TEST_CASE("the key goes on the wire as a bearer token and nowhere else") {
    std::vector<FakeTransport::Seen> seen;
    CHECK_THROWS_AS(ServiceClient(deployed(), "  \n", std::make_unique<FakeTransport>(&seen)),
                    std::invalid_argument);

    ServiceClient client(
        deployed(), std::string(kKey) + "\n", std::make_unique<FakeTransport>(&seen));
    const ServiceResponse got = client.get("/conversations");
    const ServiceResponse posted = client.post("/conversations", json::object());
    REQUIRE(seen.size() == 2);
    CHECK(seen[0].method == "GET");
    CHECK(seen[0].path == "/conversations");
    CHECK(seen[0].authorization == std::string("Bearer ") + kKey);  // trimmed
    CHECK(seen[1].method == "POST");
    CHECK(seen[1].body == "{}");
    CHECK(seen[1].timeout_seconds == 30);
    for (const ServiceResponse& response : {got, posted}) {
        CHECK(response.body.find(kKey) == std::string::npos);
        CHECK(response.error.find(kKey) == std::string::npos);
    }

    // A refusal names the host, never the key.
    try {
        ServiceClient refused(
            parse_service_url("https://example.com"), kKey, std::make_unique<FakeTransport>(&seen));
        FAIL("example.com was not refused");
    } catch (const std::invalid_argument& error) {
        CHECK(std::string(error.what()).find(kKey) == std::string::npos);
        CHECK(std::string(error.what()).find("example.com") != std::string::npos);
    }
}

TEST_CASE("a streamed turn arrives as one event per line") {
    std::vector<FakeTransport::Seen> seen;
    auto transport = std::make_unique<FakeTransport>(&seen);
    transport->lines = {R"({"event":"activity","text":"Checking SPY"})",
                        "",
                        R"({"event":"text","delta":"Hello"})",
                        R"({"event":"end"})"};
    ServiceClient client(deployed(), kKey, std::move(transport));

    std::vector<std::string> events;
    const ServiceResponse response =
        client.stream("/conversations/abc/messages", json{{"text", "hi"}}, [&](const json& event) {
            events.push_back(event.at("event").get<std::string>());
            return true;
        });
    CHECK(response.ok());
    CHECK_FALSE(response.cancelled);
    CHECK(events == std::vector<std::string>{"activity", "text", "end"});  // the blank line skipped
    REQUIRE(seen.size() == 1);
    CHECK(seen[0].streamed);
    CHECK(seen[0].timeout_seconds == ServiceClient::kTurnTimeout.count());
    CHECK(json::parse(seen[0].body) == json{{"text", "hi"}});
}

TEST_CASE("a stream can be cancelled, and a line that is not JSON ends it without its content") {
    std::vector<FakeTransport::Seen> seen;
    auto stopping = std::make_unique<FakeTransport>(&seen);
    stopping->lines = {R"({"event":"text","delta":"a"})", R"({"event":"text","delta":"b"})"};
    ServiceClient a(deployed(), kKey, std::move(stopping));
    int received = 0;
    const ServiceResponse stopped = a.stream("/x", json::object(), [&](const json&) {
        ++received;
        return false;
    });
    CHECK(stopped.cancelled);
    CHECK(received == 1);

    auto broken = std::make_unique<FakeTransport>(&seen);
    broken->lines = {
        R"({"event":"text","delta":"a"})", "secret-looking garbage", R"({"event":"end"})"};
    ServiceClient b(deployed(), kKey, std::move(broken));
    received = 0;
    const ServiceResponse failed = b.stream("/x", json::object(), [&](const json&) {
        ++received;
        return true;
    });
    CHECK(received == 1);
    CHECK_FALSE(failed.ok());
    CHECK(failed.error == "line 2 of the stream is not a JSON object");
}

TEST_CASE("lines are delivered whole however the bytes were chunked") {
    std::vector<std::string> got;
    const LineHandler collect = [&](std::string_view line) {
        got.emplace_back(line);
        return true;
    };
    std::string pending;
    for (const char* chunk : {"{\"a\":1}\r\n{\"b", "\":2}\n", "\n{\"c\":3", "}\n{\"d\":"}) {
        pending += chunk;
        CHECK(detail::deliver_lines(pending, collect));
    }
    CHECK(got == std::vector<std::string>{R"({"a":1})", R"({"b":2})", "", R"({"c":3})"});
    CHECK(pending == R"({"d":)");  // the incomplete tail waits for the next chunk

    // Stopping keeps what follows the line it stopped on.
    got.clear();
    std::string rest = "one\ntwo\nthree\n";
    CHECK_FALSE(detail::deliver_lines(rest, [&](std::string_view line) {
        got.emplace_back(line);
        return line != "one";
    }));
    CHECK(got == std::vector<std::string>{"one"});
    CHECK(rest == "two\nthree\n");
}

TEST_CASE("the environment gives a client, or one sentence on why not") {
    std::vector<FakeTransport::Seen> seen;
    std::optional<ServiceUrl> asked;
    const auto fake = [&](const ServiceUrl& url) -> std::unique_ptr<ServiceTransport> {
        asked = url;
        return std::make_unique<FakeTransport>(&seen);
    };

    {
        ScopedEnv key("MS_SERVICE_KEY", std::nullopt);
        ScopedEnv url("MS_SERVICE_URL", std::nullopt);
        const ServiceSetup none = connect_from_environment("test", fake);
        CHECK_FALSE(none.client);
        CHECK(none.reason.find("MS_SERVICE_KEY") != std::string::npos);
    }
    {
        ScopedEnv key("MS_SERVICE_KEY", std::string(kKey));
        ScopedEnv url("MS_SERVICE_URL", std::nullopt);
        const ServiceSetup deployed_setup = connect_from_environment("test", fake);
        REQUIRE(deployed_setup.client);
        CHECK(deployed_setup.reason.empty());
        REQUIRE(asked);
        CHECK(asked->host == kServiceHost);  // the default is the deployed service
        CHECK(asked->tls);
    }
    {
        ScopedEnv key("MS_SERVICE_KEY", std::string(kKey));
        ScopedEnv url("MS_SERVICE_URL", std::string("https://example.com"));
        const ServiceSetup refused = connect_from_environment("test", fake);
        CHECK_FALSE(refused.client);
        CHECK(refused.reason.find("MS_SERVICE_URL") != std::string::npos);
        CHECK(refused.reason.find(kKey) == std::string::npos);
    }
}
