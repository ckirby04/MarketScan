// The desktop chat, against what the job service actually sends.
//
// docs/specs/SPEC_DESKTOP_CHAT.md section 4. data/samples/research/chat/ is one
// conversation recorded from the service running locally: every turn's NDJSON
// as streamed, and the conversation as the service showed it after each step.
// The first half replays those turns through the chat's rules and requires them
// to reassemble into the service's own record. The second runs the whole
// session against a fake transport that serves the recording, so the worker's
// order of calls -- ask, answer, Run, follow the run, fetch its Results, have it
// analysed once -- is tested without a network (SPEC_RESEARCH_VIEW.md section 8).

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include "chat.hpp"
#include "marketscan/research/job.hpp"
#include "marketscan/research/service_client.hpp"
#include "research_session.hpp"

using namespace marketscan;
using namespace marketscan::app;
using nlohmann::json;

namespace {

const std::filesystem::path kRepo = MARKETSCAN_SOURCE_DIR;
const std::filesystem::path kChat = kRepo / "data" / "samples" / "research" / "chat";
const std::filesystem::path kFixtures = kRepo / "data" / "samples" / "research";

json read_json(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    REQUIRE(in.good());
    return json::parse(in);
}

std::vector<std::string> read_lines(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    REQUIRE(in.good());
    std::vector<std::string> out;
    for (std::string line; std::getline(in, line);) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        out.push_back(line);
    }
    return out;
}

std::vector<json> read_events(const std::filesystem::path& path) {
    std::vector<json> out;
    for (const std::string& line : read_lines(path)) {
        if (!line.empty()) {
            out.push_back(json::parse(line));
        }
    }
    return out;
}

/// The text of every item of `kind`, in order.
std::vector<std::string> texts(const std::vector<ChatItem>& items, const std::string& kind) {
    std::vector<std::string> out;
    for (const ChatItem& item : items) {
        if (item.kind == kind) {
            out.push_back(item.text);
        }
    }
    return out;
}

std::vector<std::string> texts(const json& conversation, const std::string& kind) {
    std::vector<std::string> out;
    for (const json& item : conversation["display"]) {
        if (item["kind"] == kind) {
            out.push_back(item.value("text", ""));
        }
    }
    return out;
}

std::vector<std::string> kinds(const std::vector<ChatItem>& items) {
    std::vector<std::string> out;
    for (const ChatItem& item : items) {
        out.push_back(item.kind);
    }
    return out;
}

std::vector<std::string> kinds(const json& conversation) {
    std::vector<std::string> out;
    for (const json& item : conversation["display"]) {
        out.push_back(item["kind"].get<std::string>());
    }
    return out;
}

const json& after(const char* step) {
    static std::map<std::string, json> cache;
    auto it = cache.find(step);
    if (it == cache.end()) {
        it = cache
                 .emplace(step,
                          read_json(kChat / (std::string("conversation_after_") + step + ".json")))
                 .first;
    }
    return it->second;
}

}  // namespace

// --- The recorded turns, through the chat's rules -----------------------------------

TEST_CASE("the first turn: the message shows at once, then the service's copy and its questions") {
    ChatView view;
    add_sent(view, "the question as typed");
    view.busy = true;
    REQUIRE(view.items.size() == 1);
    CHECK(view.items.front().sent);

    for (const json& event : read_events(kChat / "turn_1_question.ndjson")) {
        apply_event(view, event, TurnKind::Assistant);
    }
    close_live(view);

    // The typed copy is gone: one user item, the service's.
    CHECK(kinds(view.items) == kinds(after("turn_1_question")));
    for (const ChatItem& item : view.items) {
        CHECK_FALSE(item.sent);
        CHECK_FALSE(item.live);
    }
    CHECK(texts(view.items, "user") == texts(after("turn_1_question"), "user"));
    const ChatItem& questions = view.items.back();
    REQUIRE(questions.kind == "questions");
    REQUIRE_FALSE(questions.questions.empty());
    for (const ChatQuestion& q : questions.questions) {
        CHECK_FALSE(q.question.empty());
        CHECK(q.options.size() >= 2);
    }
    CHECK_FALSE(view.ready);  // the form event said not yet
}

TEST_CASE("the second turn: streamed text reassembles into the service's record") {
    ChatView view;
    load_conversation(view, after("turn_1_question"));
    add_sent(view, "the answer as typed");
    const auto events = read_events(kChat / "turn_2_answer.ndjson");
    std::size_t deltas = 0;
    for (const json& event : events) {
        deltas += event["event"] == "text" ? 1 : 0;
        apply_event(view, event, TurnKind::Assistant);
    }
    close_live(view);

    REQUIRE(deltas > 10);  // a real stream, many pieces
    // Every delta, in order, makes exactly the text the service kept.
    CHECK(texts(view.items, "assistant") == texts(after("turn_2_answer"), "assistant"));
    CHECK(kinds(view.items) == kinds(after("turn_2_answer")));
    CHECK(view.ready);  // the form event carried ready: true
    CHECK(view.activity.empty());

    // And the reload that follows a turn makes it the service's record exactly.
    load_conversation(view, after("turn_2_answer"));
    CHECK(view.ready);
    CHECK(view.items[1].answered);  // the questions, answered
    CHECK(run_ids(view).empty());
}

TEST_CASE("the analysis turn: its text belongs to the run it analyses") {
    ChatView view;
    load_conversation(view, after("run"));
    const std::vector<std::string> runs = run_ids(view);
    REQUIRE(runs.size() == 1);
    CHECK_FALSE(analysed(view, runs.front()));

    for (const json& event : read_events(kChat / "turn_3_analysis.ndjson")) {
        apply_event(view, event, TurnKind::Analysis, runs.front());
    }
    close_live(view);
    REQUIRE(view.items.back().kind == "analysis");
    CHECK(view.items.back().job_id == runs.front());
    CHECK(texts(view.items, "analysis") == texts(after("turn_3_analysis"), "analysis"));

    load_conversation(view, after("turn_3_analysis"));
    CHECK(analysed(view, runs.front()));
}

TEST_CASE("events the chat does not know, and errors, are handled") {
    ChatView view;
    apply_event(view, json{{"event", "something_new"}, {"x", 1}}, TurnKind::Assistant);
    CHECK(view.items.empty());
    apply_event(view, json{{"event", "text"}, {"delta", "Hel"}}, TurnKind::Assistant);
    apply_event(view, json{{"event", "text"}, {"delta", "lo"}}, TurnKind::Assistant);
    apply_event(
        view, json{{"event", "error"}, {"message", "the model refused"}}, TurnKind::Assistant);
    REQUIRE(view.items.size() == 2);
    CHECK(view.items[0].text == "Hello");
    CHECK_FALSE(view.items[0].live);  // the error closed it
    CHECK(view.items[1].kind == "error");
    CHECK(view.items[1].text == "the model refused");
}

// --- The whole session, against a fake service ------------------------------------

namespace {

/// Serves the recording: each call answered as the service answered it.
struct ServiceState {
    std::mutex mutex;
    std::vector<std::string> calls;  // "METHOD path"
    int messages = 0;
    std::string conversation_step;  // which recorded record GET returns
    int polls = 0;
    int refuse_status = 0;  // when set, every call answers this
    bool unreachable = false;
};

json curves_document() {
    research::MemoryBarSource data;
    data.add(research::load_close_csv(kFixtures / "drawdown_prices.csv", "FIXTURE"));
    research::JobOptions options;
    options.null_draws = 5;
    options.bootstrap_paths = 2;
    const auto outcome = research::run_job(read_json(kFixtures / "job_form.json"), data, options);
    REQUIRE(outcome.ok());
    json out = json::array();
    for (const research::JobCurve& c : outcome.curves) {
        out.push_back(json{{"id", c.id},
                           {"label", c.label},
                           {"is_benchmark", c.is_benchmark},
                           {"times", c.times},
                           {"equity", c.equity},
                           {"normalised", c.normalised},
                           {"drawdown", c.drawdown}});
    }
    return out;
}

struct FakeService final : research::ServiceTransport {
    std::shared_ptr<ServiceState> state;
    json curves;

    explicit FakeService(std::shared_ptr<ServiceState> s)
        : state(std::move(s)), curves(curves_document()) {}

    research::ServiceResponse send(const research::ServiceRequest& request) override {
        const std::string method(request.method);
        const std::string path(request.path);
        std::lock_guard lock(state->mutex);
        state->calls.push_back(method + " " + path);
        research::ServiceResponse r;
        if (state->unreachable) {
            r.error = "WinHttpSendRequest failed (code 12029)";
            return r;
        }
        if (state->refuse_status != 0) {
            r.status = state->refuse_status;
            r.body = R"({"detail":"invalid key"})";
            return r;
        }
        const json conversation_id = after("turn_1_question")["id"];
        const std::string id = conversation_id.get<std::string>();
        const std::string job = after("run")["runs"][0].get<std::string>();
        r.status = 200;
        if (method == "POST" && path == "/conversations") {
            r.status = 201;
            r.body = json{{"id", id}}.dump();
        } else if (method == "GET" && path == "/conversations") {
            r.body = json::array({{{"id", id},
                                   {"title", after("turn_1_question")["title"]},
                                   {"updated_at", 1.0}}})
                         .dump();
        } else if (method == "GET" && path == "/conversations/" + id) {
            r.body = after(state->conversation_step.c_str()).dump();
        } else if (method == "POST" && path == "/conversations/" + id + "/messages") {
            ++state->messages;
            const char* turn = state->messages == 1 ? "turn_1_question" : "turn_2_answer";
            stream(kChat / (std::string(turn) + ".ndjson"), request, r);
            state->conversation_step = turn;
        } else if (method == "POST" && path == "/conversations/" + id + "/run") {
            r.status = 202;
            r.body = read_json(kChat / "run_response.json").dump();
            state->conversation_step = "run";
        } else if (method == "GET" && path == "/jobs/" + job) {
            const json statuses = read_json(kChat / "job_statuses.json");
            r.body = statuses[std::min<std::size_t>(state->polls++, statuses.size() - 1)].dump();
        } else if (method == "GET" && path == "/jobs/" + job + "/results") {
            r.body = read_json(kFixtures / "job_results.json").dump();
        } else if (method == "GET" && path == "/jobs/" + job + "/curves") {
            r.body = curves.dump();
        } else if (method == "GET" && path == "/jobs/" + job + "/form") {
            r.body = read_json(kFixtures / "job_form.json").dump();
        } else if (method == "POST" && path == "/conversations/" + id + "/results") {
            REQUIRE(json::parse(request.body) == json{{"job_id", job}});
            stream(kChat / "turn_3_analysis.ndjson", request, r);
            state->conversation_step = "turn_3_analysis";
        } else {
            r.status = 404;
            r.body = R"({"detail":"not found"})";
        }
        return r;
    }

    static void stream(const std::filesystem::path& file,
                       const research::ServiceRequest& request,
                       research::ServiceResponse& r) {
        REQUIRE(request.on_line != nullptr);
        for (const std::string& line : read_lines(file)) {
            if (!(*request.on_line)(line)) {
                r.cancelled = true;
                return;
            }
        }
    }
};

ResearchConfig chat_config(const std::shared_ptr<ServiceState>& state) {
    ResearchConfig config;
    config.contracts_dir = kRepo / "contracts" / "research";
    config.runs_dir = std::filesystem::temp_directory_path() / "ms_research_chat_runs";
    config.csv_sources = {{"FIXTURE", kFixtures / "drawdown_prices.csv"}};
    config.poll_interval = std::chrono::milliseconds(1);
    config.connect_service = [state] {
        research::ServiceSetup setup;
        setup.client = std::make_unique<research::ServiceClient>(
            research::parse_service_url("http://127.0.0.1:8080"),
            "test-key",
            std::make_unique<FakeService>(state));
        return setup;
    };
    return config;
}

std::size_t count(const std::vector<std::string>& calls, const std::string& call) {
    return static_cast<std::size_t>(std::count(calls.begin(), calls.end(), call));
}

}  // namespace

TEST_CASE("a conversation, end to end: ask, answer, run, Results, analysis") {
    auto state = std::make_shared<ServiceState>();
    ResearchSession session(chat_config(state));
    REQUIRE(session.snapshot()->chat.available);
    const std::string id = after("turn_1_question")["id"].get<std::string>();
    const std::string job = after("run")["runs"][0].get<std::string>();

    // Ask. The new conversation is made on the service with the first message.
    session.chat_send("Does buying QQQ after a 25% drop beat holding it?");
    CHECK(session.snapshot()->chat.open.items.size() >= 1);  // shown before any call returns
    session.wait_chat_idle();
    auto snap = session.snapshot();
    CHECK(snap->chat.open.id == id);
    CHECK(kinds(snap->chat.open.items) == kinds(after("turn_1_question")));
    CHECK_FALSE(snap->chat.open.busy);
    CHECK(snap->chat.open.problem.empty());
    REQUIRE(snap->chat.conversations.size() == 1);
    CHECK(snap->chat.conversations.front().id == id);

    // Run is ignored until the assistant has set the backtest up.
    session.chat_run();
    session.wait_chat_idle();
    CHECK(count(state->calls, "POST /conversations/" + id + "/run") == 0);

    // Answer.
    session.chat_send("the first option");
    session.wait_chat_idle();
    snap = session.snapshot();
    CHECK(kinds(snap->chat.open.items) == kinds(after("turn_2_answer")));
    CHECK(snap->chat.open.ready);

    // Run: the worker follows the run, fetches its Results, and has it analysed.
    session.chat_run();
    session.wait_chat_idle();
    snap = session.snapshot();
    CHECK(kinds(snap->chat.open.items) == kinds(after("turn_3_analysis")));
    CHECK(analysed(snap->chat.open, job));
    const ChatRun* run = snap->chat.run(job);
    REQUIRE(run != nullptr);
    CHECK(run->state == "done");
    CHECK(run->problem.empty());
    REQUIRE(run->run);
    // Read by the same reader as a local run's Results.
    CHECK(run->run->view.grade ==
          read_json(kFixtures / "job_results.json")["verdict"]["grade"].get<std::string>());
    CHECK(run->run->curves.size() == 2);
    CHECK(run->run->form == read_json(kFixtures / "job_form.json"));

    // Analysed once, and the Results fetched once, however many times it was polled.
    CHECK(count(state->calls, "POST /conversations/" + id + "/results") == 1);
    CHECK(count(state->calls, "GET /jobs/" + job + "/results") == 1);
    CHECK(count(state->calls, "POST /conversations") == 1);  // made once, with the first message

    // Reopened in the same session: the settled run is neither polled for its
    // Results again nor analysed again.
    session.chat_open(id);
    session.wait_chat_idle();
    CHECK(kinds(session.snapshot()->chat.open.items) == kinds(after("turn_3_analysis")));
    CHECK(count(state->calls, "GET /jobs/" + job + "/results") == 1);
    CHECK(count(state->calls, "POST /conversations/" + id + "/results") == 1);
}

TEST_CASE("a conversation reopened later shows its run without analysing it again") {
    // A fresh session knows nothing but what the service says: the record lists
    // the run as analysed, and that alone must stop a second analysis.
    auto state = std::make_shared<ServiceState>();
    state->conversation_step = "turn_3_analysis";
    state->polls = 1;  // the job is already done
    ResearchSession session(chat_config(state));
    const std::string id = after("turn_1_question")["id"].get<std::string>();
    const std::string job = after("run")["runs"][0].get<std::string>();

    session.chat_open(id);
    session.wait_chat_idle();
    const auto snap = session.snapshot();
    CHECK(kinds(snap->chat.open.items) == kinds(after("turn_3_analysis")));
    const ChatRun* run = snap->chat.run(job);
    REQUIRE(run != nullptr);
    CHECK(run->run);  // its Results fetched, to draw
    CHECK(count(state->calls, "POST /conversations/" + id + "/results") == 0);
}

TEST_CASE("a refused key and an unreachable service are said, and the message is not kept") {
    auto state = std::make_shared<ServiceState>();
    state->refuse_status = 401;
    {
        ResearchSession session(chat_config(state));
        session.chat_send("hello");
        session.wait_chat_idle();
        const auto snap = session.snapshot();
        CHECK(snap->chat.open.items.empty());  // the typed message withdrawn
        CHECK_FALSE(snap->chat.open.busy);
        CHECK(snap->chat.open.problem.find("MS_SERVICE_KEY") != std::string::npos);
        CHECK(snap->chat.open.problem.find("test-key") == std::string::npos);
    }
    state->refuse_status = 0;
    state->unreachable = true;
    {
        ResearchSession session(chat_config(state));
        session.chat_send("hello");
        session.wait_chat_idle();
        CHECK(session.snapshot()->chat.open.problem.find("Couldn't reach the job service") !=
              std::string::npos);
    }
}

TEST_CASE("without a connection the chat is off, says why, and sends nothing") {
    ResearchConfig config;
    config.contracts_dir = kRepo / "contracts" / "research";
    config.csv_sources = {{"FIXTURE", kFixtures / "drawdown_prices.csv"}};
    ResearchSession session(config);
    const auto snap = session.snapshot();
    CHECK_FALSE(snap->chat.available);
    CHECK_FALSE(snap->chat.reason.empty());
    session.chat_send("hello");
    session.wait_chat_idle();
    CHECK(session.snapshot()->chat.open.items.empty());
}

// --- The real service ---------------------------------------------------------------

// Hidden ([.]): not run by ctest, because it needs a service and costs a model
// call. Run it by hand against a service on this machine:
//
//   MS_SERVICE_URL=http://127.0.0.1:8080 MS_SERVICE_KEY=dev test_research_chat "[network]"
TEST_CASE("the real service answers a question", "[.][network]") {
    ResearchConfig config;
    config.contracts_dir = kRepo / "contracts" / "research";
    config.csv_sources = {{"FIXTURE", kFixtures / "drawdown_prices.csv"}};
    config.connect_service = [] { return research::connect_from_environment("MarketScan test"); };
    ResearchSession session(config);
    const auto first = session.snapshot();
    INFO(first->chat.reason);
    REQUIRE(first->chat.available);

    session.chat_refresh();
    session.chat_send("Does buying SPY after a 20% drop from its all-time high beat holding it?");
    session.wait_chat_idle();
    const auto snap = session.snapshot();
    INFO(snap->chat.open.problem);
    CHECK(snap->chat.open.problem.empty());
    CHECK_FALSE(snap->chat.open.id.empty());
    REQUIRE(snap->chat.open.items.size() >= 2);
    CHECK(snap->chat.open.items.front().kind == "user");
    CHECK_FALSE(snap->chat.open.items.front().sent);  // the service's copy
    CHECK_FALSE(snap->chat.conversations.empty());
}
