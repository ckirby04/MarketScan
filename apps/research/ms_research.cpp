// ms_research -- runs a Strategy Form headless and writes its Results.
//
//   ms_research --form form.json --daily data/cache/stooq --out runs/tqqq
//   ms_research --form form.json --csv FIXTURE=prices.csv --out runs/fixture
//
// SPEC_RESEARCH_VIEW.md part R3: the research job without the GUI. The form is
// validated against the contract before anything runs, and the Results the job
// produces are validated against theirs before anything is written -- a Results
// document that fails its own schema is an engine bug, and it is reported as
// one rather than handed to a renderer.
//
// Writes, under --out, the run directory research::write_run lays out -- the
// same one the Research view writes, so a run made here can be opened there:
// form.json (as run), results.json, equity.csv, curves.json, and with --trace,
// trace.json for research/grading.py.

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "marketscan/research/job.hpp"
#include "marketscan/research/json_schema.hpp"

namespace fs = std::filesystem;
using marketscan::research::JobOptions;
using marketscan::research::JsonSchema;
using nlohmann::json;

namespace {

constexpr int kUsage = 1;
constexpr int kInvalidForm = 2;
constexpr int kRefused = 3;
constexpr int kEngineBug = 4;

void usage() {
    std::cout
        << "ms_research -- run a Strategy Form headless\n\n"
           "  --form PATH            the form (contracts/research/strategy_form.schema.json)\n"
           "  --daily DIR            a directory of SYMBOL.msb daily files; repeatable,\n"
           "                         the first holding a symbol answers\n"
           "  --recipes PATH         the catalog's synthetic recipes, a JSON array\n"
           "  --csv SYMBOL=PATH      a date,close CSV for SYMBOL; repeatable\n"
           "  --out DIR              where to write form.json, results.json, equity.csv\n"
           "  --trace                also write trace.json, for research/grading.py\n"
           "  --contracts DIR        the contract schemas (default: contracts/research)\n"
           "  --null-draws N         random-timing alternatives (default 1000)\n"
           "  --paths N              bootstrap paths (default 1000)\n"
           "  --data-snapshot TEXT   recorded in the Results\n"
           "  --fixed-time           record 1970-01-01T00:00:00Z and 0 ms, for fixtures\n"
           "  --machine              one JSON object per line on stdout -- progress, then\n"
           "                         done or refused -- for the job service to read\n";
}

/// --machine output: one JSON object per line, flushed, so a reader on the other
/// end of a pipe sees each event as it happens.
void emit(const json& event) {
    std::cout << event.dump() << "\n" << std::flush;
}

[[nodiscard]] json error_list(const std::vector<marketscan::research::SchemaError>& errors) {
    json out = json::array();
    for (const auto& e : errors) {
        out.push_back(json{{"path", e.instance_path}, {"message", e.message}});
    }
    return out;
}

[[nodiscard]] json read_json(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("cannot open '" + path.string() + "'");
    }
    return json::parse(in);
}

void print_errors(const std::vector<marketscan::research::SchemaError>& errors) {
    for (const auto& e : errors) {
        std::cerr << "  " << (e.instance_path.empty() ? "/" : e.instance_path) << ": " << e.message
                  << "\n";
    }
}

}  // namespace

int main(int argc, char** argv) {
    fs::path form_path;
    std::vector<fs::path> daily;
    fs::path recipes;
    fs::path out_dir;
    fs::path contracts = "contracts/research";
    std::vector<std::pair<std::string, fs::path>> csvs;
    bool trace = false;
    bool fixed_time = false;
    bool machine = false;
    JobOptions options;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const auto value = [&]() -> std::string {
            if (i + 1 >= argc) {
                std::cerr << arg << " needs a value\n";
                std::exit(kUsage);
            }
            return argv[++i];
        };
        if (arg == "-h" || arg == "--help") {
            usage();
            return 0;
        } else if (arg == "--form") {
            form_path = value();
        } else if (arg == "--daily") {
            daily.emplace_back(value());
        } else if (arg == "--recipes") {
            recipes = value();
        } else if (arg == "--csv") {
            const std::string spec = value();
            const auto eq = spec.find('=');
            if (eq == std::string::npos || eq == 0) {
                std::cerr << "--csv takes SYMBOL=PATH\n";
                return kUsage;
            }
            csvs.emplace_back(spec.substr(0, eq), spec.substr(eq + 1));
        } else if (arg == "--out") {
            out_dir = value();
        } else if (arg == "--trace") {
            trace = true;
        } else if (arg == "--contracts") {
            contracts = value();
        } else if (arg == "--null-draws") {
            options.null_draws = std::stoi(value());
        } else if (arg == "--paths") {
            options.bootstrap_paths = std::stoi(value());
        } else if (arg == "--data-snapshot") {
            options.data_snapshot = value();
        } else if (arg == "--fixed-time") {
            fixed_time = true;
        } else if (arg == "--machine") {
            machine = true;
        } else {
            std::cerr << "unknown argument " << arg << "\n";
            usage();
            return kUsage;
        }
    }
    if (form_path.empty() || out_dir.empty() || (daily.empty() && csvs.empty())) {
        usage();
        return kUsage;
    }

    try {
        const JsonSchema form_schema = JsonSchema::load(contracts / "strategy_form.schema.json");
        const JsonSchema results_schema = JsonSchema::load(contracts / "results.schema.json");

        const json form = read_json(form_path);
        if (const auto errors = form_schema.validate(form); !errors.empty()) {
            if (machine) {
                emit({{"event", "refused"},
                      {"kind", "invalid_form"},
                      {"errors", error_list(errors)}});
            }
            std::cerr << "the form is not valid:\n";
            print_errors(errors);
            return kInvalidForm;
        }

        if (!recipes.empty()) {
            options.recipes = read_json(recipes);
            if (!options.recipes.is_array()) {
                std::cerr << recipes.string() << " is not a JSON array of recipes\n";
                return kUsage;
            }
        }

        std::unique_ptr<marketscan::research::BarSource> data;
        if (!daily.empty()) {
            data = std::make_unique<marketscan::research::DirectoryBarSource>(daily);
        } else {
            auto memory = std::make_unique<marketscan::research::MemoryBarSource>();
            for (const auto& [symbol, path] : csvs) {
                memory->add(marketscan::research::load_close_csv(path, symbol));
            }
            data = std::move(memory);
        }

        const auto began = std::chrono::steady_clock::now();
        options.started_at =
            fixed_time ? "1970-01-01T00:00:00Z" : marketscan::research::utc_timestamp();
        if (machine) {
            options.progress = [](std::string_view stage, int done, int total) {
                emit({{"event", "progress"}, {"stage", stage}, {"done", done}, {"total", total}});
                return true;
            };
        }
        auto outcome = marketscan::research::run_job(form, *data, options);
        if (!outcome.ok()) {
            if (machine) {
                json errors = json::array();
                for (const auto& e : outcome.errors) {
                    errors.push_back(json{{"path", e.path}, {"message", e.message}});
                }
                emit({{"event", "refused"}, {"kind", "refused"}, {"errors", errors}});
            }
            std::cerr << "the job refused this form:\n";
            for (const auto& e : outcome.errors) {
                std::cerr << "  " << (e.path.empty() ? "(data)" : e.path) << ": " << e.message
                          << "\n";
            }
            return kRefused;
        }
        if (!fixed_time) {
            outcome.results["run"]["duration_ms"] =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - began)
                    .count();
        }
        if (const auto errors = results_schema.validate(outcome.results); !errors.empty()) {
            if (machine) {
                emit(
                    {{"event", "refused"}, {"kind", "engine_bug"}, {"errors", error_list(errors)}});
            }
            std::cerr << "ENGINE BUG: the Results document fails results.schema.json:\n";
            print_errors(errors);
            return kEngineBug;
        }

        marketscan::research::write_run(out_dir, form, outcome, trace);

        const json& verdict = outcome.results["verdict"];
        if (machine) {
            emit({{"event", "done"},
                  {"grade", verdict["grade"]},
                  {"run_id", outcome.results["run"]["run_id"]}});
            return 0;
        }
        std::cout << "grade      " << verdict["grade"].get<std::string>() << " ("
                  << verdict["data_grade"].get<std::string>() << " data)\n";
        for (const json& gate : verdict["gates"]) {
            std::cout << "  " << gate["status"].get<std::string>() << "  "
                      << gate["id"].get<std::string>() << ": " << gate["summary"].get<std::string>()
                      << "\n";
        }
        std::cout << "wrote " << out_dir.string() << "\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "ms_research: " << error.what() << "\n";
        return kUsage;
    }
}
