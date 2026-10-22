#include "research_session.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <fstream>
#include <optional>
#include <sstream>

#include "headless.hpp"
#include "marketscan/data/bar_store.hpp"
#include "marketscan/util/logging.hpp"

namespace marketscan::app {

namespace fs = std::filesystem;
using nlohmann::json;

namespace {

[[nodiscard]] json read_json(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("cannot open '" + path.string() + "'");
    }
    return json::parse(in);
}

/// A data source over several others: a daily directory, then CSVs.
class CombinedSource final : public research::BarSource {
public:
    void add(std::unique_ptr<research::BarSource> source) { sources_.push_back(std::move(source)); }

    [[nodiscard]] std::optional<BarSeries> load(std::string_view symbol) const override {
        for (const auto& source : sources_) {
            if (auto series = source->load(symbol)) {
                return series;
            }
        }
        return std::nullopt;
    }

private:
    std::vector<std::unique_ptr<research::BarSource>> sources_;
};

}  // namespace

fs::path find_upwards(const fs::path& start, const fs::path& relative) {
    std::error_code error;
    fs::path dir = fs::absolute(start, error);
    while (!dir.empty()) {
        if (fs::exists(dir / relative, error)) {
            return dir / relative;
        }
        const fs::path parent = dir.parent_path();
        if (parent == dir) {
            break;
        }
        dir = parent;
    }
    return {};
}

ResearchSession::ResearchSession(ResearchConfig config) : config_(std::move(config)) {
    auto initial = std::make_shared<ResearchSnapshot>();
    initial->assistant_available = false;
    initial->assistant_reason =
        "The assistant is not connected yet (SPEC_RESEARCH_VIEW.md part R2). Fill in a form "
        "by hand; everything after the form works without it.";

    if (config_.contracts_dir.empty()) {
        config_.contracts_dir = find_upwards(executable_directory(), "contracts/research");
    }
    if (config_.contracts_dir.empty()) {
        initial->setup_error = "contracts/research was not found beside the executable or in "
                               "any directory above it.";
    } else {
        try {
            form_schema_raw_ = read_json(config_.contracts_dir / "strategy_form.schema.json");
            form_schema_ = std::make_unique<research::JsonSchema>(
                research::JsonSchema::compile(form_schema_raw_));
            results_schema_ = std::make_unique<research::JsonSchema>(
                research::JsonSchema::load(config_.contracts_dir / "results.schema.json"));
        } catch (const std::exception& error) {
            initial->setup_error = error.what();
        }
        // Not fatal: without it the view shows identifiers as they are.
        try {
            initial->words = std::make_shared<const PlainWords>(
                PlainWords::load(config_.contracts_dir / "plain_words.json"));
        } catch (const std::exception& error) {
            log::warn("research", error.what());
        }
    }
    const fs::path repo = config_.contracts_dir.empty()
                              ? fs::path{}
                              : config_.contracts_dir.parent_path().parent_path();
    if (config_.runs_dir.empty() && !repo.empty()) {
        config_.runs_dir = repo / "runs";
    }

    // The data, and which symbols it can offer.
    auto data = std::make_unique<CombinedSource>();
    std::error_code error;
    if (!config_.daily_dir.empty() && fs::is_directory(config_.daily_dir, error)) {
        data->add(std::make_unique<research::DirectoryBarSource>(config_.daily_dir));
        try {
            for (const fs::path& file : bar_store::list(config_.daily_dir)) {
                initial->symbols.push_back(file.stem().string());
            }
        } catch (const std::exception& failure) {
            log::warn("research", failure.what());
        }
    }
    if (config_.csv_sources.empty() && !repo.empty()) {
        const fs::path fixture = repo / "data" / "samples" / "research" / "drawdown_prices.csv";
        if (fs::exists(fixture, error)) {
            config_.csv_sources.emplace_back("FIXTURE", fixture);
        }
    }
    auto memory = std::make_unique<research::MemoryBarSource>();
    for (const auto& [symbol, path] : config_.csv_sources) {
        try {
            memory->add(research::load_close_csv(path, symbol));
            initial->symbols.push_back(symbol);
        } catch (const std::exception& failure) {
            log::warn("research", failure.what());
        }
    }
    data->add(std::move(memory));
    data_ = std::move(data);

    initial->recent = scan_recent();
    snapshot_ = initial;
    set_form(starter_form());

    worker_ = std::thread([this] { worker_main(); });
}

ResearchSession::~ResearchSession() {
    cancel_ = true;
    {
        // Under the lock the worker waits with, so the wakeup cannot fall between
        // its check of the condition and its going to sleep.
        const std::lock_guard lock(job_mutex_);
        quitting_ = true;
    }
    job_signal_.notify_all();
    if (worker_.joinable()) {
        worker_.join();
    }
}

std::shared_ptr<const ResearchSnapshot> ResearchSession::snapshot() const {
    const std::lock_guard lock(snapshot_mutex_);
    return snapshot_;
}

void ResearchSession::publish(const std::function<void(ResearchSnapshot&)>& change) {
    const std::lock_guard lock(snapshot_mutex_);
    auto next = std::make_shared<ResearchSnapshot>(*snapshot_);
    change(*next);
    snapshot_ = std::move(next);
}

json ResearchSession::starter_form() const {
    json form;
    try {
        form = read_json(config_.contracts_dir / "examples" / "starter_form.json");
    } catch (const std::exception&) {
        return json::object();
    }
    const auto snap = snapshot();
    if (snap && !snap->symbols.empty()) {
        const std::string symbol = snap->symbols.front();
        form["universe"]["assets"][0]["symbol"] = symbol;
        std::string id = symbol;
        std::transform(id.begin(), id.end(), id.begin(), [](unsigned char c) {
            return static_cast<char>(c == '.' || c == '-' ? '_' : std::tolower(c));
        });
        form["universe"]["assets"][0]["security_id"] = id;
        form["benchmarks"] = json::array({symbol});
    }
    return form;
}

void ResearchSession::set_form(json form) {
    std::vector<research::SchemaError> errors;
    if (form_schema_) {
        errors = form_schema_->validate(form);
    }
    publish([&](ResearchSnapshot& s) {
        s.form = std::move(form);
        s.form_errors = std::move(errors);
        ++s.form_version;
        if (s.state == ResearchState::Refused) {
            s.state = ResearchState::Editing;
            s.job_errors.clear();
        }
    });
}

void ResearchSession::run() {
    const auto snap = snapshot();
    if (!form_schema_ || !snap->form_errors.empty()) {
        return;
    }
    const std::lock_guard lock(job_mutex_);
    if (busy_) {
        return;
    }
    busy_ = true;
    cancel_ = false;
    // Running is published before the job is queued, and under the job lock, so
    // nothing the worker publishes about this job can be overwritten by it.
    publish([](ResearchSnapshot& s) {
        s.state = ResearchState::Running;
        s.job_errors.clear();
        s.stage = "starting";
        s.done = 0;
        s.total = 0;
    });
    pending_.push_back(snap->form);
    job_signal_.notify_one();
}

void ResearchSession::cancel() {
    cancel_ = true;
}

void ResearchSession::edit() {
    publish([](ResearchSnapshot& s) {
        if (s.state != ResearchState::Running) {
            s.state = ResearchState::Editing;
        }
    });
}

void ResearchSession::open(const fs::path& directory) {
    {
        const std::lock_guard lock(job_mutex_);
        if (busy_) {
            return;
        }
    }
    std::string failure;
    auto run = std::make_shared<ResearchRun>();
    std::vector<research::SchemaError> form_errors;
    try {
        run->form = read_json(directory / "form.json");
        run->results = read_json(directory / "results.json");
        if (results_schema_) {
            if (const auto errors = results_schema_->validate(run->results); !errors.empty()) {
                throw std::runtime_error("results.json fails results.schema.json at '" +
                                         errors.front().instance_path +
                                         "': " + errors.front().message);
            }
        }
        run->view = read_results(run->results);
        run->curves = research::read_curves(directory / "curves.json");
        run->directory = directory;
        if (form_schema_) {
            form_errors = form_schema_->validate(run->form);
        }
    } catch (const std::exception& error) {
        failure = "Could not open " + directory.filename().string() + ": " + error.what();
    }
    publish([&](ResearchSnapshot& s) {
        s.open_error = failure;
        if (!failure.empty()) {
            return;
        }
        s.form = run->form;
        s.form_errors = std::move(form_errors);
        ++s.form_version;
        s.run = run;
        s.state = ResearchState::Done;
    });
}

void ResearchSession::wait_idle() {
    for (;;) {
        {
            const std::lock_guard lock(job_mutex_);
            if (!busy_) {
                return;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

void ResearchSession::worker_main() {
    for (;;) {
        json form;
        {
            std::unique_lock lock(job_mutex_);
            job_signal_.wait(lock, [this] { return quitting_ || !pending_.empty(); });
            if (quitting_) {
                return;
            }
            form = std::move(pending_.front());
            pending_.pop_front();
        }
        try {
            execute(form);
        } catch (const std::exception& error) {
            // Nothing may leave this thread: an exception here ends the program.
            publish([&](ResearchSnapshot& s) {
                s.state = ResearchState::Refused;
                s.job_errors = {{"", std::string("the job failed: ") + error.what()}};
            });
        }
        const std::lock_guard lock(job_mutex_);
        busy_ = false;
    }
}

void ResearchSession::execute(const json& form) {
    research::JobOptions options;
    options.null_draws = config_.null_draws;
    options.bootstrap_paths = config_.bootstrap_paths;
    options.started_at = research::utc_timestamp();
    options.data_snapshot = config_.daily_dir.empty() ? "csv" : config_.daily_dir.string();
    options.progress = [this](std::string_view stage, int done, int total) {
        publish([&](ResearchSnapshot& s) {
            s.stage = std::string(stage);
            s.done = done;
            s.total = total;
        });
        return !cancel_.load();
    };

    const auto began = std::chrono::steady_clock::now();
    research::JobOutcome outcome = research::run_job(form, *data_, options);

    if (outcome.cancelled) {
        publish([](ResearchSnapshot& s) { s.state = ResearchState::Editing; });
        return;
    }
    if (!outcome.ok()) {
        publish([&](ResearchSnapshot& s) {
            s.state = ResearchState::Refused;
            s.job_errors = std::move(outcome.errors);
        });
        return;
    }

    outcome.results["run"]["duration_ms"] = std::chrono::duration_cast<std::chrono::milliseconds>(
                                                std::chrono::steady_clock::now() - began)
                                                .count();
    if (results_schema_) {
        if (const auto errors = results_schema_->validate(outcome.results); !errors.empty()) {
            publish([&](ResearchSnapshot& s) {
                s.state = ResearchState::Refused;
                s.job_errors = {{"",
                                 "ENGINE BUG: the Results fail results.schema.json at " +
                                     errors.front().instance_path + ": " + errors.front().message}};
            });
            return;
        }
    }

    auto run = std::make_shared<ResearchRun>();
    run->form = form;
    run->results = outcome.results;
    run->view = read_results(run->results);
    run->curves = outcome.curves;

    // R5: the run on disk, in the layout ms_research writes, so Recent can bring
    // it back without re-running.
    if (!config_.runs_dir.empty()) {
        try {
            std::string stamp = options.started_at;
            std::replace(stamp.begin(), stamp.end(), ':', '-');
            const fs::path directory =
                config_.runs_dir / (stamp + "_" + run->results["run"]["run_id"].get<std::string>());
            research::write_run(directory, form, outcome);
            run->directory = directory;
        } catch (const std::exception& error) {
            log::warn("research", std::string("could not save the run: ") + error.what());
        }
    }

    auto recent = scan_recent();
    publish([&](ResearchSnapshot& s) {
        s.run = run;
        s.state = ResearchState::Done;
        s.recent = std::move(recent);
    });
}

std::vector<RecentRun> ResearchSession::scan_recent() const {
    std::vector<RecentRun> out;
    std::error_code error;
    if (config_.runs_dir.empty() || !fs::is_directory(config_.runs_dir, error)) {
        return out;
    }
    // Non-throwing throughout: this runs on the worker thread, where an escaping
    // filesystem_error would end the program.
    for (fs::directory_iterator it(config_.runs_dir, error), end; !error && it != end;
         it.increment(error)) {
        const fs::path directory = it->path();
        if (!it->is_directory(error) || !fs::exists(directory / "results.json", error) ||
            !fs::exists(directory / "curves.json", error)) {
            continue;
        }
        try {
            const json results = read_json(directory / "results.json");
            const json form = read_json(directory / "form.json");
            RecentRun row;
            row.directory = directory;
            row.grade = results.at("verdict").at("grade").get<std::string>();
            row.data_grade = results.at("verdict").at("data_grade").get<std::string>();
            row.started_at = results.at("run").at("started_at").get<std::string>();
            const std::string symbol =
                form.at("universe").at("assets").at(0).at("symbol").get<std::string>();
            row.title = symbol + " " + form.at("template").get<std::string>() + ", " +
                        form.at("date_range").at("start").get<std::string>().substr(0, 4) + "-" +
                        form.at("date_range").at("end").get<std::string>().substr(0, 4);
            out.push_back(std::move(row));
        } catch (const std::exception&) {
            continue;  // a half-written or foreign directory is not a run
        }
    }
    std::sort(out.begin(), out.end(), [](const RecentRun& a, const RecentRun& b) {
        return a.started_at > b.started_at;
    });
    return out;
}

}  // namespace marketscan::app
