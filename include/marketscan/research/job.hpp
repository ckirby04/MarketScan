#ifndef MARKETSCAN_RESEARCH_JOB_HPP
#define MARKETSCAN_RESEARCH_JOB_HPP

/*
 * The research job: a validated Strategy Form in, a Results document out.
 *
 * Specification: contracts/research/JOB.md, which says how every number is
 * computed. The research layer's research/grading.py implements the same
 * specification from the job's trace -- the runs' equity curves and fills --
 * and the two must agree on the Results.
 *
 * Deterministic: the same form over the same bars gives the same Results, byte
 * for byte, given the same options. Run timing is an option rather than read
 * from a clock for exactly that reason.
 */

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "marketscan/data/bar_store.hpp"

namespace marketscan::research {

/// Where a job's bars come from, by ticker.
class BarSource {
public:
    BarSource() = default;
    BarSource(const BarSource&) = delete;
    BarSource& operator=(const BarSource&) = delete;
    BarSource(BarSource&&) = delete;
    BarSource& operator=(BarSource&&) = delete;
    virtual ~BarSource() = default;

    [[nodiscard]] virtual std::optional<BarSeries> load(std::string_view symbol) const = 0;

    /// CATALOG.md's quality tier. Until the catalog exists, everything is
    /// `exercise_only`: nothing here can vouch for delisting coverage or
    /// point-in-time adjustments.
    [[nodiscard]] virtual std::string quality_tier(std::string_view symbol) const;
};

/// `<directory>/<SYMBOL>.msb`, the layout of the daily cache.
class DirectoryBarSource final : public BarSource {
public:
    explicit DirectoryBarSource(std::filesystem::path directory);
    [[nodiscard]] std::optional<BarSeries> load(std::string_view symbol) const override;

private:
    std::filesystem::path directory_;
};

/// Reads a `date,close` CSV with a header row into daily bars stamped at each
/// day's close, as a daily file is. Open, high and low are set to the close.
/// Throws std::runtime_error naming the file and line on anything malformed.
///
/// For fixtures: data/samples/research/drawdown_prices.csv is in this layout,
/// and it is what the research layer reads too.
[[nodiscard]] BarSeries load_close_csv(const std::filesystem::path& path, std::string ticker);

/// Series held in memory, by ticker. For tests and fixtures.
class MemoryBarSource final : public BarSource {
public:
    void add(BarSeries series);
    [[nodiscard]] std::optional<BarSeries> load(std::string_view symbol) const override;

private:
    std::map<std::string, BarSeries, std::less<>> series_;
};

struct JobOptions {
    /// JOB.md sections 6 and 9 say 1,000 of each. Fewer only for tests.
    int null_draws = 1000;
    int bootstrap_paths = 1000;

    // Recorded in the Results' `run` block and nowhere else.
    std::string started_at = "1970-01-01T00:00:00Z";
    std::int64_t duration_ms = 0;
    std::string data_snapshot = "unspecified";
    std::string engine_version;  // empty means this build's version
    std::string equity_uri = "equity.csv";
};

struct JobError {
    /// The form path the error is about, or empty for one about the data.
    std::string path;
    std::string message;
};

struct JobOutcome {
    std::vector<JobError> errors;

    /// A Results document (results.schema.json). Null when errors is not empty.
    nlohmann::json results;

    /// What research/grading.py needs to regrade the job independently. Null
    /// when errors is not empty.
    nlohmann::json trace;

    /// Daily equity, "date,strategy,<benchmark>..." with a header.
    std::string equity_csv;

    [[nodiscard]] bool ok() const noexcept { return errors.empty(); }
};

[[nodiscard]] JobOutcome
run_job(const nlohmann::json& form, const BarSource& data, const JobOptions& options = {});

/// JOB.md section 6: the form without `prompt` and `provenance`, keys sorted,
/// no whitespace. Its SHA-256 names the form and seeds the job.
[[nodiscard]] std::string canonical_form(const nlohmann::json& form);

/// JOB.md section 6's generator.
class SplitMix64 {
public:
    explicit SplitMix64(std::uint64_t seed) noexcept : state_(seed) {}

    std::uint64_t next() noexcept {
        state_ += 0x9E3779B97F4A7C15ULL;
        std::uint64_t z = state_;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        return z ^ (z >> 31);
    }

    /// `next() % n`. Modulo bias is part of the specification, not an
    /// oversight: both implementations must draw the same numbers.
    std::uint64_t draw(std::uint64_t n) noexcept { return next() % n; }

private:
    std::uint64_t state_;
};

/// JOB.md section 6's percentile rule. `values` need not be sorted.
[[nodiscard]] double percentile(std::vector<double> values, double q);

/// Eigenvalues of a symmetric matrix, by cyclic Jacobi rotation. Exposed for
/// the tests.
[[nodiscard]] std::vector<double> symmetric_eigenvalues(std::vector<std::vector<double>> matrix);

}  // namespace marketscan::research

#endif  // MARKETSCAN_RESEARCH_JOB_HPP
