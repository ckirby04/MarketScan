#ifndef MARKETSCAN_APP_RESEARCH_SESSION_HPP
#define MARKETSCAN_APP_RESEARCH_SESSION_HPP

/*
 * The Research view's engine half: a form, a job on its own thread, and the
 * runs it has written to disk.
 *
 * A sibling of Session, not a part of it (SPEC_RESEARCH_VIEW.md section 5.1).
 * Session replays one run interactively; this runs a whole graded job in
 * batch. The discipline is the same: the GUI posts commands and reads an
 * immutable snapshot published behind a shared_ptr, and nothing on the GUI
 * thread ever waits for a job.
 *
 * Nothing in here includes ImGui.
 */

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "marketscan/research/job.hpp"
#include "marketscan/research/json_schema.hpp"
#include "plain_words.hpp"
#include "research_view.hpp"

namespace marketscan::app {

struct ResearchConfig {
    /// Where strategy_form.schema.json and results.schema.json live. Empty
    /// means look for contracts/research beside the executable and then in each
    /// parent directory, which finds the source tree from a build tree.
    std::filesystem::path contracts_dir;

    /// Where finished runs are written, one directory each, and where Recent
    /// reads them from. Empty means `runs` beside the contracts' repository.
    std::filesystem::path runs_dir;

    /// Bars: a directory of SYMBOL.msb files, and/or date,close CSVs by symbol.
    /// With neither, the committed fixture series is offered as FIXTURE, so the
    /// view works in a fresh clone.
    std::filesystem::path daily_dir;
    std::vector<std::pair<std::string, std::filesystem::path>> csv_sources;

    /// The job's draw counts. JOB.md's 1,000 each unless a test says otherwise.
    int null_draws = 1000;
    int bootstrap_paths = 1000;
};

enum class ResearchState : std::uint8_t {
    /// A form is being filled in.
    Editing = 0,
    Running,
    /// A Results document is showing.
    Done,
    /// The job refused the form or could not run; `job_errors` says why.
    Refused,
};

/// A finished run, immutable once published.
struct ResearchRun {
    nlohmann::json form;
    nlohmann::json results;
    /// The Results read once, when the run was made or opened -- not on every
    /// frame, and not in the middle of one, where a malformed file would throw.
    ResearchView view;
    std::vector<research::JobCurve> curves;
    std::filesystem::path directory;  // empty if it could not be written
};

/// One entry in the Recent list, read from a run directory's results.json.
struct RecentRun {
    std::filesystem::path directory;
    std::string title;
    std::string grade;
    std::string data_grade;
    std::string started_at;
};

struct ResearchSnapshot {
    ResearchState state = ResearchState::Editing;

    /// The form as last set, and what the schema says about it.
    nlohmann::json form;
    std::vector<research::SchemaError> form_errors;
    std::uint64_t form_version = 0;

    /// The binder's refusals, each naming a form path.
    std::vector<research::JobError> job_errors;

    /// Progress while Running.
    std::string stage;
    int done = 0;
    int total = 0;

    std::shared_ptr<const ResearchRun> run;
    std::vector<RecentRun> recent;

    /// Symbols the data source can supply, for the form's asset field.
    std::vector<std::string> symbols;

    /// Whether the assistant can take a question, and if not, why -- said in
    /// the view rather than leaving a dead prompt box.
    bool assistant_available = false;
    std::string assistant_reason;

    /// The Results' identifiers in plain words (contracts/research/plain_words.json).
    /// Never null; empty, so every lookup falls back, if the file could not be read.
    std::shared_ptr<const PlainWords> words = std::make_shared<const PlainWords>();

    /// Set when the view cannot work at all: the contracts were not found.
    std::string setup_error;

    /// Why the last open() did not show its run. Cleared by the next one.
    std::string open_error;
};

class ResearchSession {
public:
    explicit ResearchSession(ResearchConfig config);
    ~ResearchSession();

    ResearchSession(const ResearchSession&) = delete;
    ResearchSession& operator=(const ResearchSession&) = delete;
    ResearchSession(ResearchSession&&) = delete;
    ResearchSession& operator=(ResearchSession&&) = delete;

    /// Replaces the form and validates it against the contract. Immediate:
    /// schema validation is cheap, and the Run button should be right on the
    /// frame the user stops typing.
    void set_form(nlohmann::json form);

    /// The starting point for a form filled in by hand.
    [[nodiscard]] nlohmann::json starter_form() const;

    /// Starts a job on the current form. Ignored while one is running or while
    /// the form does not validate.
    void run();

    /// Stops the running job. No partial Results are kept.
    void cancel();

    /// Back to the form, keeping the last run's form.
    void edit();

    /// Shows a run from disk without re-running it. Ignored while a job is
    /// running: the job would finish and replace it unseen. A run whose files
    /// are missing, malformed, or fail results.schema.json is not shown, and the
    /// snapshot's `open_error` says why.
    void open(const std::filesystem::path& directory);

    [[nodiscard]] std::shared_ptr<const ResearchSnapshot> snapshot() const;

    [[nodiscard]] const ResearchConfig& config() const noexcept { return config_; }

    /// The form schema as a document, for the editor that is drawn from it.
    /// An empty object when the contracts were not found.
    [[nodiscard]] const nlohmann::json& form_schema_json() const noexcept {
        return form_schema_raw_;
    }

    /// Waits for the running job to finish. For tests and for shutdown.
    void wait_idle();

private:
    void publish(const std::function<void(ResearchSnapshot&)>& change);
    void worker_main();
    void execute(const nlohmann::json& form);
    [[nodiscard]] std::vector<RecentRun> scan_recent() const;

    ResearchConfig config_;
    nlohmann::json form_schema_raw_ = nlohmann::json::object();
    std::unique_ptr<research::JsonSchema> form_schema_;
    std::unique_ptr<research::JsonSchema> results_schema_;
    std::unique_ptr<research::BarSource> data_;

    mutable std::mutex snapshot_mutex_;
    std::shared_ptr<const ResearchSnapshot> snapshot_;

    std::mutex job_mutex_;
    std::condition_variable job_signal_;
    std::deque<nlohmann::json> pending_;
    bool busy_ = false;
    std::atomic<bool> cancel_{false};
    std::atomic<bool> quitting_{false};
    std::thread worker_;
};

/// Looks for `relative` beside `start` and then in each parent of it.
[[nodiscard]] std::filesystem::path find_upwards(const std::filesystem::path& start,
                                                 const std::filesystem::path& relative);

}  // namespace marketscan::app

#endif  // MARKETSCAN_APP_RESEARCH_SESSION_HPP
