// The research job. Specification: contracts/research/JOB.md; every section
// below names the section it implements.

#include "marketscan/research/job.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <limits>
#include <numeric>
#include <set>
#include <sstream>
#include <stdexcept>
#include <utility>

#include "marketscan/core/session_calendar.hpp"
#include "marketscan/research/json_schema.hpp"
#include "marketscan/research/rule_run.hpp"
#include "marketscan/util/sha256.hpp"
#include "marketscan/util/version.hpp"

namespace marketscan::research {

using nlohmann::json;

// --- Data sources -----------------------------------------------------------

std::string BarSource::quality_tier(std::string_view) const {
    return "exercise_only";
}

DirectoryBarSource::DirectoryBarSource(std::filesystem::path directory)
    : directories_{std::move(directory)} {}

DirectoryBarSource::DirectoryBarSource(std::vector<std::filesystem::path> directories)
    : directories_(std::move(directories)) {}

std::optional<BarSeries> DirectoryBarSource::load(std::string_view symbol) const {
    for (const std::filesystem::path& directory : directories_) {
        const std::filesystem::path path =
            directory / (std::string(symbol) + std::string(bar_store::kExtension));
        if (std::filesystem::exists(path)) {
            return bar_store::read(path);
        }
    }
    return std::nullopt;
}

BarSeries load_close_csv(const std::filesystem::path& path, std::string ticker) {
    std::ifstream in(path);
    if (!in) {
        throw std::runtime_error("cannot open '" + path.string() + "'");
    }
    BarSeries series;
    series.ticker = std::move(ticker);
    std::string line;
    std::size_t number = 0;
    while (std::getline(in, line)) {
        ++number;
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (number == 1 || line.empty()) {
            continue;
        }
        const auto fail = [&](const char* what) {
            return std::runtime_error(path.string() + ":" + std::to_string(number) + ": " + what);
        };
        const auto comma = line.find(',');
        if (comma != 10 || !is_valid_date(line.substr(0, 10))) {
            throw fail("expected YYYY-MM-DD,close");
        }
        char* end = nullptr;
        const double close = std::strtod(line.c_str() + comma + 1, &end);
        if (end == line.c_str() + comma + 1 || *end != '\0' || !(close > 0.0)) {
            throw fail("the close is not a positive number");
        }
        const std::string date = line.substr(0, 10);
        StoredBar bar;
        bar.ts = bar_store::daily_close_stamp(
            calendar::days_from_civil(std::stoll(date.substr(0, 4)),
                                      static_cast<unsigned>(std::stoul(date.substr(5, 2))),
                                      static_cast<unsigned>(std::stoul(date.substr(8, 2)))));
        if (!series.bars.empty() && bar.ts <= series.bars.back().ts) {
            throw fail("dates must ascend");
        }
        bar.open = bar.high = bar.low = bar.close = close;
        bar.volume = 1'000'000.0;
        series.bars.push_back(bar);
    }
    return series;
}

void MemoryBarSource::add(BarSeries series) {
    std::string ticker = series.ticker;
    series_.insert_or_assign(std::move(ticker), std::move(series));
}

std::optional<BarSeries> MemoryBarSource::load(std::string_view symbol) const {
    const auto it = series_.find(symbol);
    if (it == series_.end()) {
        return std::nullopt;
    }
    return it->second;
}

// --- Run directories --------------------------------------------------------

std::string utc_timestamp() {
    const std::time_t now = std::time(nullptr);
    std::tm parts{};
#ifdef _WIN32
    gmtime_s(&parts, &now);
#else
    gmtime_r(&now, &parts);
#endif
    char buffer[32];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &parts);
    return buffer;
}

namespace {

void write_file(const std::filesystem::path& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        throw std::runtime_error("cannot write '" + path.string() + "'");
    }
    out << text;
}

}  // namespace

void write_run(const std::filesystem::path& directory,
               const json& form,
               const JobOutcome& outcome,
               bool with_trace) {
    std::filesystem::create_directories(directory);
    write_file(directory / "form.json", form.dump(2) + "\n");
    write_file(directory / "results.json", outcome.results.dump(2) + "\n");
    write_file(directory / "equity.csv", outcome.equity_csv);
    json curves = json::array();
    for (const JobCurve& curve : outcome.curves) {
        curves.push_back(json{{"id", curve.id},
                              {"label", curve.label},
                              {"is_benchmark", curve.is_benchmark},
                              {"times", curve.times},
                              {"equity", curve.equity},
                              {"normalised", curve.normalised},
                              {"drawdown", curve.drawdown}});
    }
    write_file(directory / "curves.json", curves.dump() + "\n");
    if (with_trace) {
        write_file(directory / "trace.json", outcome.trace.dump() + "\n");
    }
}

std::vector<JobCurve> read_curves(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("cannot open '" + path.string() + "'");
    }
    std::vector<JobCurve> out;
    for (const json& c : json::parse(in)) {
        JobCurve curve;
        curve.id = c.at("id").get<std::string>();
        curve.label = c.at("label").get<std::string>();
        curve.is_benchmark = c.at("is_benchmark").get<bool>();
        curve.times = c.at("times").get<std::vector<double>>();
        curve.equity = c.at("equity").get<std::vector<double>>();
        curve.normalised = c.at("normalised").get<std::vector<double>>();
        curve.drawdown = c.at("drawdown").get<std::vector<double>>();
        out.push_back(std::move(curve));
    }
    return out;
}

// --- Shared pieces ----------------------------------------------------------

std::string canonical_form(const json& form) {
    json copy = form;
    if (copy.is_object()) {
        copy.erase("prompt");
        copy.erase("provenance");
    }
    // nlohmann's object is a std::map, so dump() is already key-sorted, and
    // with no indent it has no whitespace.
    return copy.dump();
}

double percentile(std::vector<double> values, double q) {
    values.erase(
        std::remove_if(values.begin(), values.end(), [](double v) { return std::isnan(v); }),
        values.end());
    if (values.empty()) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    std::sort(values.begin(), values.end());
    const double h = static_cast<double>(values.size() - 1) * q;
    const auto l = static_cast<std::size_t>(std::floor(h));
    if (l + 1 >= values.size()) {
        return values[l];
    }
    return values[l] + (h - static_cast<double>(l)) * (values[l + 1] - values[l]);
}

std::vector<double> symmetric_eigenvalues(std::vector<std::vector<double>> a) {
    const std::size_t n = a.size();
    for (int sweep = 0; sweep < 100; ++sweep) {
        double off = 0.0;
        for (std::size_t i = 0; i < n; ++i) {
            for (std::size_t j = i + 1; j < n; ++j) {
                off += a[i][j] * a[i][j];
            }
        }
        if (off < 1e-22) {
            break;
        }
        for (std::size_t p = 0; p < n; ++p) {
            for (std::size_t q = p + 1; q < n; ++q) {
                if (std::abs(a[p][q]) < 1e-300) {
                    continue;
                }
                const double theta = (a[q][q] - a[p][p]) / (2.0 * a[p][q]);
                const double t = (theta >= 0.0 ? 1.0 : -1.0) /
                                 (std::abs(theta) + std::sqrt(theta * theta + 1.0));
                const double c = 1.0 / std::sqrt(t * t + 1.0);
                const double s = t * c;
                for (std::size_t k = 0; k < n; ++k) {
                    const double akp = a[k][p];
                    const double akq = a[k][q];
                    a[k][p] = c * akp - s * akq;
                    a[k][q] = s * akp + c * akq;
                }
                for (std::size_t k = 0; k < n; ++k) {
                    const double apk = a[p][k];
                    const double aqk = a[q][k];
                    a[p][k] = c * apk - s * aqk;
                    a[q][k] = s * apk + c * aqk;
                }
            }
        }
    }
    std::vector<double> out(n);
    for (std::size_t i = 0; i < n; ++i) {
        out[i] = a[i][i];
    }
    return out;
}

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr int kMaxPoints = 500;
constexpr double kReservePct = 2.0;

[[nodiscard]] std::int64_t parse_day(const std::string& iso) {
    return calendar::days_from_civil(std::stoll(iso.substr(0, 4)),
                                     static_cast<unsigned>(std::stoul(iso.substr(5, 2))),
                                     static_cast<unsigned>(std::stoul(iso.substr(8, 2))));
}

[[nodiscard]] std::string iso_date(std::int64_t day) {
    const auto civil = calendar::civil_from_days(day);
    char buffer[16];
    std::snprintf(buffer,
                  sizeof(buffer),
                  "%04lld-%02u-%02u",
                  static_cast<long long>(civil.year),
                  civil.month,
                  civil.day);
    return buffer;
}

[[nodiscard]] json num(double value) {
    return std::isnan(value) ? json(nullptr) : json(value);
}

[[nodiscard]] double as_double(const json& value) {
    return value.is_null() ? kNaN : value.get<double>();
}

// --- 1. Binding ---------------------------------------------------------------

/// A strategy parameter's possible values.
struct Param {
    std::string path;
    std::vector<double> values;
    bool integral = false;
};

struct Series {
    std::string symbol;
    BarSeries bars;
    std::vector<std::int64_t> days;
    std::vector<double> closes;
    std::size_t s = 0;  // first decision bar
    std::size_t o = 0;  // first out-of-sample bar; e + 1 without a split
    std::size_t e = 0;  // last bar
    std::string quality;
    // Bars on days before this are synthetic (section 1a); the lowest day
    // when none are.
    std::int64_t real_from_day = std::numeric_limits<std::int64_t>::min();

    [[nodiscard]] bool synthetic_at(std::size_t t) const { return days[t] < real_from_day; }
};

struct Bound {
    Series asset;
    std::vector<Series> benchmarks;  // [0] is primary
    bool has_split = false;
    std::int64_t start_day = 0;

    RuleRunConfig base;
    std::vector<Param> params;       // every strategy parameter, fixed or swept
    std::vector<std::size_t> swept;  // indices into params, sorted by path
    json strategy;

    std::string metric;
    int min_trades = 3;
    std::string security_id;

    // The recipe the asset's history was extended with, as {recipe_id,
    // validation}; null when it was not.
    json synthetic = nullptr;
};

[[nodiscard]] std::string lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return text;
}

[[nodiscard]] std::string upper(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
        return static_cast<char>(std::toupper(c));
    });
    return text;
}

class Binder {
public:
    Binder(const json& form, const BarSource& data, const json& recipes)
        : form_(form), data_(data), recipes_(recipes) {}

    std::optional<Bound> bind(std::vector<JobError>& errors) {
        Bound bound;
        refuse_unsupported();
        bind_params(bound);
        bind_series(bound);
        if (!errors_.empty()) {
            errors = std::move(errors_);
            return std::nullopt;
        }
        std::size_t points = 1;
        for (const std::size_t index : bound.swept) {
            points *= bound.params[index].values.size();
            if (points > static_cast<std::size_t>(kMaxPoints)) {
                errors.push_back(
                    {"/strategy",
                     "the sweep has more than " + std::to_string(kMaxPoints) + " points"});
                return std::nullopt;
            }
        }
        return bound;
    }

private:
    void refuse(std::string path, std::string message) {
        errors_.push_back({std::move(path), std::move(message)});
    }

    void refuse_unsupported() {
        const json& f = form_;
        if (f.at("template") != "drawdown_entry") {
            refuse("/template", "only the drawdown_entry template is built");
        }
        const json& assets = f.at("universe").at("assets");
        if (assets.size() != 1) {
            refuse("/universe/assets", "the drawdown_entry template trades exactly one asset");
        }
        if (f.at("universe").at("price_adjustment") == "total_return") {
            refuse("/universe/price_adjustment",
                   "the bar store holds no dividends, so total return cannot be honoured; use "
                   "split_only");
        }
        if (f.at("cash").at("yield") != "none") {
            refuse("/cash/yield", "there is no rate series for cash to earn; use none");
        }
        if (f.at("execution").at("fill") == "next_open") {
            refuse("/execution/fill",
                   "a daily feed has no opening quote to fill against; use next_close");
        }
        if (f.at("robustness").contains("walk_forward")) {
            refuse("/robustness/walk_forward", "walk-forward testing is not built yet");
        }
    }

    void add_param(Bound& bound, const std::string& path, const json& value, bool integral) {
        Param param;
        param.path = path;
        param.integral = integral;
        if (value.is_number()) {
            param.values = {value.get<double>()};
        } else if (value.contains("values")) {
            for (const json& v : value.at("values")) {
                param.values.push_back(v.get<double>());
            }
        } else {
            const json& sweep = value.at("sweep");
            const double min = sweep.at("min").get<double>();
            const double max = sweep.at("max").get<double>();
            const double step = sweep.at("step").get<double>();
            if (max < min) {
                refuse(path + "/sweep", "max is below min");
                return;
            }
            // Counted in double first: a tiny step makes the count too large
            // for an integer, and the cast would be undefined.
            const double count = std::floor((max - min) / step + 1e-9) + 1.0;
            if (!(count <= static_cast<double>(kMaxPoints))) {
                refuse(path + "/sweep", "more than " + std::to_string(kMaxPoints) + " values");
                return;
            }
            const auto n = static_cast<long long>(count);
            for (long long i = 0; i < n; ++i) {
                param.values.push_back(min + static_cast<double>(i) * step);
            }
        }
        if (integral) {
            for (const double v : param.values) {
                if (v != std::floor(v) || v < 1.0) {
                    refuse(path, "must be a whole number of bars, at least 1");
                    return;
                }
            }
        }
        bound.params.push_back(std::move(param));
    }

    void bind_params(Bound& bound) {
        const json& strategy = form_.at("strategy");
        bound.strategy = strategy;
        const json& reference = strategy.at("reference");
        if (reference.at("type") == "rolling_high") {
            add_param(bound, "/strategy/reference/window_days", reference.at("window_days"), true);
        }
        const json& entry = strategy.at("entry");
        if (entry.at("style") == "single") {
            add_param(bound, "/strategy/entry/threshold_pct", entry.at("threshold_pct"), false);
        } else {
            const json& tranches = entry.at("tranches");
            if (tranches.size() > static_cast<std::size_t>(kMaxTranches)) {
                refuse("/strategy/entry/tranches",
                       "at most " + std::to_string(kMaxTranches) + " tranches");
            }
            for (std::size_t k = 0; k < tranches.size(); ++k) {
                add_param(bound,
                          "/strategy/entry/tranches/" + std::to_string(k) + "/drawdown_pct",
                          tranches[k].at("drawdown_pct"),
                          false);
            }
        }
        std::set<std::string> seen;
        const json& rules = strategy.at("exit").at("rules");
        for (std::size_t i = 0; i < rules.size(); ++i) {
            const std::string type = rules[i].at("type").get<std::string>();
            const std::string at = "/strategy/exit/rules/" + std::to_string(i);
            if (!seen.insert(type).second) {
                refuse(at, "a second " + type + " rule; each exit may appear once");
                continue;
            }
            if (type == "time_stop") {
                add_param(bound, at + "/days", rules[i].at("days"), true);
            } else if (type != "recover_to_high") {
                add_param(bound, at + "/pct", rules[i].at("pct"), false);
            }
        }
        for (std::size_t i = 0; i < bound.params.size(); ++i) {
            if (bound.params[i].values.size() > 1) {
                bound.swept.push_back(i);
            }
        }
        std::sort(bound.swept.begin(), bound.swept.end(), [&](std::size_t a, std::size_t b) {
            return bound.params[a].path < bound.params[b].path;
        });

        const json& objective = form_.at("objective");
        bound.metric = objective.at("metric").get<std::string>();
        bound.min_trades =
            objective.contains("min_trades") ? objective.at("min_trades").get<int>() : 3;
    }

    std::optional<BarSeries> fetch(const std::string& symbol, const std::string& path) {
        std::optional<BarSeries> loaded;
        try {
            loaded = data_.load(symbol);
        } catch (const std::exception& error) {
            refuse(path, symbol + ": " + error.what());
            return std::nullopt;
        }
        if (!loaded) {
            refuse(path, "no data for " + symbol);
        }
        return loaded;
    }

    /// Section 1a: the asset's bars, with the synthetic history the form asks
    /// for prepended. Returns the first real bar's day, or nothing (with the
    /// reasons refused) if the extension cannot be honoured.
    std::optional<std::int64_t> extend(BarSeries& bars, const json& ext, Bound& bound) {
        const std::string at = "/universe/assets/0/synthetic_extension";
        const std::string id = ext.at("recipe_id").get<std::string>();
        const json* recipe = nullptr;
        for (const json& candidate : recipes_) {
            if (candidate.value("record", "") == "synthetic_recipe" &&
                candidate.value("recipe_id", "") == id) {
                recipe = &candidate;
            }
        }
        if (recipe == nullptr) {
            refuse(at + "/recipe_id", "no synthetic recipe named " + id + " in the catalog");
            return std::nullopt;
        }
        const std::size_t before = errors_.size();
        const json& params = recipe->at("params");
        if (recipe->value("method", "") != "daily_leverage") {
            refuse(at + "/recipe_id", id + " is not a daily_leverage recipe");
        }
        if (!recipe->contains("validation")) {
            refuse(at + "/recipe_id",
                   id + " has no validation, and a recipe without one cannot be used");
        }
        if (recipe->value("target_security_id", "") != bound.security_id) {
            refuse(at + "/recipe_id",
                   id + " extends " + recipe->value("target_security_id", "") + ", not " +
                       bound.security_id);
        }
        const std::string underlying = ext.at("underlying").get<std::string>();
        if (lower(underlying) != recipe->value("underlying_security_id", "")) {
            refuse(at + "/underlying",
                   "recipe " + id + " is built from " +
                       recipe->value("underlying_security_id", "") + ", not " + underlying);
        }
        // The engine checks the form still matches the recipe it was copied
        // from (the schema's promise): a changed fee is a different recipe,
        // and its validation would not describe it.
        for (const char* key : {"leverage", "annual_fee_pct", "borrow_spread_pct"}) {
            if (ext.at(key).get<double>() != params.at(key).get<double>()) {
                refuse(at + "/" + key,
                       std::string("does not match recipe ") + id + ", which has " +
                           params.at(key).dump());
            }
        }
        if (errors_.size() != before || bars.bars.empty()) {
            return std::nullopt;
        }

        const bool total_return =
            params.value("underlying_adjustment", "split_only") == "total_return";
        const auto underlying_bars =
            fetch(upper(underlying) + (total_return ? ".TR" : ""), at + "/underlying");
        const auto rate =
            fetch(upper(params.at("cash_rate_series_id").get<std::string>()), at + "/recipe_id");
        if (!underlying_bars || !rate) {
            return std::nullopt;
        }
        const std::int64_t first_real = day_of(bars.bars.front().ts);
        SyntheticBars synthetic;
        try {
            synthetic = extend_history(*underlying_bars,
                                       *rate,
                                       first_real,
                                       bars.bars.front().close,
                                       {params.at("leverage").get<double>(),
                                        params.at("annual_fee_pct").get<double>(),
                                        params.at("borrow_spread_pct").get<double>()});
        } catch (const std::exception& error) {
            refuse(at, error.what());
            return std::nullopt;
        }
        std::vector<StoredBar> joined;
        joined.reserve(synthetic.days.size() + bars.bars.size());
        for (std::size_t i = 0; i < synthetic.days.size(); ++i) {
            StoredBar bar;
            bar.ts = bar_store::daily_close_stamp(synthetic.days[i]);
            bar.open = bar.high = bar.low = bar.close = synthetic.closes[i];
            // A synthetic bar has no volume of its own; this is load_close_csv's.
            bar.volume = 1'000'000.0;
            joined.push_back(bar);
        }
        joined.insert(joined.end(), bars.bars.begin(), bars.bars.end());
        bars.bars = std::move(joined);
        bound.synthetic = json{{"recipe_id", id}, {"validation", recipe->at("validation")}};
        return first_real;
    }

    std::optional<Series> load_series(const std::string& symbol,
                                      const std::string& path,
                                      std::int64_t start_day,
                                      std::int64_t end_day,
                                      int warmup_days) {
        auto loaded = fetch(symbol, path);
        if (!loaded) {
            return std::nullopt;
        }
        return cut(*loaded, symbol, path, start_day, end_day, warmup_days);
    }

    std::optional<Series> cut(const BarSeries& loaded_series,
                              const std::string& symbol,
                              const std::string& path,
                              std::int64_t start_day,
                              std::int64_t end_day,
                              int warmup_days) {
        const BarSeries* loaded = &loaded_series;
        std::vector<StoredBar> bars;
        for (const StoredBar& bar : loaded->bars) {
            if (day_of(bar.ts) <= end_day) {
                bars.push_back(bar);
            }
        }
        const auto first = std::find_if(bars.begin(), bars.end(), [&](const StoredBar& bar) {
            return day_of(bar.ts) >= start_day;
        });
        if (first == bars.end()) {
            refuse(path, symbol + " has no bar between the start and end dates");
            return std::nullopt;
        }
        auto from = static_cast<std::size_t>(first - bars.begin());
        if (warmup_days > 0 && from > static_cast<std::size_t>(warmup_days)) {
            bars.erase(bars.begin(),
                       bars.begin() + static_cast<std::ptrdiff_t>(from) - warmup_days);
            from = static_cast<std::size_t>(warmup_days);
        }
        Series series;
        series.symbol = symbol;
        series.bars.ticker = symbol;
        series.bars.interval = loaded->interval;
        series.bars.bars = std::move(bars);
        for (const StoredBar& bar : series.bars.bars) {
            series.days.push_back(day_of(bar.ts));
            series.closes.push_back(bar.close);
        }
        series.s = from;
        series.e = series.bars.size() - 1;
        series.quality = data_.quality_tier(symbol);
        return series;
    }

    static void place_split(Series& series, std::optional<std::int64_t> split_day) {
        series.o = series.e + 1;
        if (!split_day) {
            return;
        }
        for (std::size_t t = series.s; t <= series.e; ++t) {
            if (series.days[t] >= *split_day) {
                series.o = t;
                return;
            }
        }
    }

    void bind_series(Bound& bound) {
        const json& range = form_.at("date_range");
        bound.start_day = parse_day(range.at("start").get<std::string>());
        const std::int64_t end_day = parse_day(range.at("end").get<std::string>());
        const int warmup = range.contains("warmup_days") ? range.at("warmup_days").get<int>() : 0;

        std::optional<std::int64_t> split;
        const json& oos = form_.at("robustness").at("oos_split");
        if (!oos.is_null()) {
            split = parse_day(oos.get<std::string>());
        }
        bound.has_split = split.has_value();

        const json& asset = form_.at("universe").at("assets").at(0);
        bound.security_id = asset.at("security_id").get<std::string>();
        const std::string symbol = asset.at("symbol").get<std::string>();
        auto asset_bars = fetch(symbol, "/universe/assets/0/symbol");
        std::optional<std::int64_t> real_from;
        if (asset_bars && asset.contains("synthetic_extension")) {
            real_from = extend(*asset_bars, asset.at("synthetic_extension"), bound);
            if (!real_from) {
                // Refused already. Going on with the real bars alone would
                // only add refusals that follow from this one (a split before
                // the shortened history, say), and bury it.
                asset_bars.reset();
            }
        }
        std::optional<Series> series;
        if (asset_bars) {
            series = cut(
                *asset_bars, symbol, "/universe/assets/0/symbol", bound.start_day, end_day, warmup);
        }
        if (series && real_from) {
            series->real_from_day = *real_from;
        }
        if (series) {
            place_split(*series, split);
            if (split && (series->o <= series->s || series->o > series->e)) {
                refuse("/robustness/oos_split", "the split leaves an empty window");
            }
            bound.asset = std::move(*series);
        }

        const json& benchmarks = form_.at("benchmarks");
        if (benchmarks.empty()) {
            refuse("/benchmarks", "at least one benchmark is needed to judge against");
        }
        for (std::size_t i = 0; i < benchmarks.size(); ++i) {
            const std::string name = benchmarks[i].get<std::string>();
            const std::string at = "/benchmarks/" + std::to_string(i);
            // A benchmark that names the asset is the asset's own series,
            // synthetic history included: holding it is the comparison.
            std::optional<Series> bench;
            if (name == symbol) {
                if (!asset_bars) {
                    continue;  // the asset's own refusal says why
                }
                bench = cut(*asset_bars, name, at, bound.start_day, end_day, warmup);
                if (bench && real_from) {
                    bench->real_from_day = *real_from;
                }
            } else {
                bench = load_series(name, at, bound.start_day, end_day, warmup);
            }
            if (bench) {
                place_split(*bench, split);
                if (split && (bench->o <= bench->s || bench->o > bench->e)) {
                    refuse("/benchmarks/" + std::to_string(i),
                           bench->symbol + " has no bars on one side of the split");
                    continue;
                }
                bound.benchmarks.push_back(std::move(*bench));
            }
        }

        const json& costs = form_.at("costs");
        bound.base.start_day = bound.start_day;
        bound.base.starting_cash =
            Money::from_double(form_.at("capital").at("initial").get<double>());
        bound.base.spread_bps = 2.0 * costs.at("slippage_bps").get<double>();
        bound.base.commission_per_order = costs.at("commission_per_trade").get<double>();
        bound.base.reserve_pct = kReservePct;
    }

    const json& form_;
    const BarSource& data_;
    const json& recipes_;
    std::vector<JobError> errors_;
};

/// Section 2's enumeration: last swept dimension fastest.
[[nodiscard]] std::vector<std::vector<std::size_t>> enumerate_points(const Bound& bound) {
    std::vector<std::vector<std::size_t>> points{{}};
    for (const std::size_t dim : bound.swept) {
        std::vector<std::vector<std::size_t>> next;
        for (const auto& prefix : points) {
            for (std::size_t v = 0; v < bound.params[dim].values.size(); ++v) {
                auto point = prefix;
                point.push_back(v);
                next.push_back(std::move(point));
            }
        }
        points = std::move(next);
    }
    return points;
}

[[nodiscard]] RuleParams rule_for(const Bound& bound, const std::vector<std::size_t>& point) {
    std::map<std::string, double> value;
    for (const Param& param : bound.params) {
        value[param.path] = param.values.front();
    }
    for (std::size_t d = 0; d < bound.swept.size(); ++d) {
        const Param& param = bound.params[bound.swept[d]];
        value[param.path] = param.values[point[d]];
    }

    const json& strategy = bound.strategy;
    RuleParams rule;
    const std::string reference = strategy.at("reference").at("type").get<std::string>();
    rule.reference = reference == "rolling_high"      ? ReferenceKind::RollingHigh
                     : reference == "since_last_exit" ? ReferenceKind::SinceLastExit
                                                      : ReferenceKind::AllTimeHigh;
    if (rule.reference == ReferenceKind::RollingHigh) {
        rule.window_days = static_cast<int>(value.at("/strategy/reference/window_days"));
    }
    const json& entry = strategy.at("entry");
    rule.tranches.clear();
    if (entry.at("style") == "single") {
        rule.tranches.push_back(
            Tranche{value.at("/strategy/entry/threshold_pct"), entry.value("position_pct", 100.0)});
    } else {
        const json& tranches = entry.at("tranches");
        for (std::size_t k = 0; k < tranches.size(); ++k) {
            rule.tranches.push_back(
                Tranche{value.at("/strategy/entry/tranches/" + std::to_string(k) + "/drawdown_pct"),
                        tranches[k].at("fraction_pct").get<double>()});
        }
    }
    rule.recover_to_high = false;
    const json& rules = strategy.at("exit").at("rules");
    for (std::size_t i = 0; i < rules.size(); ++i) {
        const std::string type = rules[i].at("type").get<std::string>();
        const std::string at = "/strategy/exit/rules/" + std::to_string(i);
        if (type == "recover_to_high") {
            rule.recover_to_high = true;
        } else if (type == "take_profit") {
            rule.take_profit_pct = value.at(at + "/pct");
        } else if (type == "stop_loss") {
            rule.stop_loss_pct = value.at(at + "/pct");
        } else if (type == "trailing_stop") {
            rule.trailing_stop_pct = value.at(at + "/pct");
        } else if (type == "time_stop") {
            rule.time_stop_days = static_cast<int>(value.at(at + "/days"));
        }
    }
    rule.reentry = strategy.at("reentry").get<bool>();
    return rule;
}

// --- 4. Analysis of one run -----------------------------------------------------

struct Trade {
    std::size_t first_buy_bar = 0;
    std::optional<std::size_t> sell_bar;
    double cost = 0.0;
    double quantity = 0.0;
    double bought_value = 0.0;  // sum price * quantity, for the entry price
    double value = 0.0;
    double exit_price = 0.0;
    ExitReason reason = ExitReason::None;

    [[nodiscard]] double return_pct() const { return 100.0 * (value / cost - 1.0); }
};

struct Analysis {
    std::vector<double> equity;
    std::vector<char> exposure;
    std::vector<Trade> trades;
    std::vector<RuleFill> fills;
};

[[nodiscard]] Analysis analyse(const RuleRun& run, const Series& series) {
    Analysis out;
    out.equity = run.equity;
    out.fills = run.fills;
    out.exposure.assign(series.closes.size(), 0);
    std::optional<Trade> open;
    for (const RuleFill& fill : run.fills) {
        if (fill.side == MS_SIDE_BUY) {
            if (!open) {
                open = Trade{};
                open->first_buy_bar = fill.bar;
            }
            open->cost += fill.price * fill.quantity + fill.commission.to_double();
            open->quantity += fill.quantity;
            open->bought_value += fill.price * fill.quantity;
        } else if (open) {
            open->sell_bar = fill.bar;
            open->value = fill.price * fill.quantity - fill.commission.to_double();
            open->exit_price = fill.price;
            open->reason = static_cast<ExitReason>(fill.tag & kTagPayload);
            out.trades.push_back(*open);
            open.reset();
        }
    }
    if (open) {
        open->value = series.closes[series.e] * open->quantity;
        open->exit_price = series.closes[series.e];
        out.trades.push_back(*open);
    }
    for (const Trade& trade : out.trades) {
        const std::size_t last = trade.sell_bar.value_or(series.e);
        for (std::size_t t = trade.first_buy_bar + 1; t <= last; ++t) {
            out.exposure[t] = 1;
        }
    }
    return out;
}

struct Window {
    std::size_t a = 0;
    std::size_t b = 0;
    bool present = false;
};

struct Windows {
    Window full;
    Window in_sample;
    Window out_of_sample;
};

[[nodiscard]] Windows windows_of(const Series& series, bool has_split) {
    Windows w;
    w.full = {series.s, series.e, true};
    if (has_split) {
        w.in_sample = {series.s, series.o - 1, true};
        w.out_of_sample = {series.o, series.e, true};
    }
    return w;
}

struct Metrics {
    double total_return_pct = kNaN;
    double cagr_pct = kNaN;
    double volatility_pct = kNaN;
    double sharpe = kNaN;
    double sortino = kNaN;
    double max_drawdown_pct = kNaN;
    double max_drawdown_days = kNaN;
    double calmar = kNaN;
    double ulcer_index = kNaN;
    double time_in_market_pct = kNaN;
    long long n_entries = 0;
    double win_rate_pct = kNaN;
    double avg_trade_return_pct = kNaN;
    double worst_trade_pct = kNaN;

    [[nodiscard]] double get(const std::string& name) const {
        if (name == "cagr") {
            return cagr_pct;
        }
        if (name == "sharpe") {
            return sharpe;
        }
        if (name == "sortino") {
            return sortino;
        }
        if (name == "calmar") {
            return calmar;
        }
        if (name == "max_drawdown") {
            return max_drawdown_pct;
        }
        if (name == "ulcer_index") {
            return ulcer_index;
        }
        return total_return_pct;  // "total_return"
    }
};

/// Section 4. `levels` and `days` are the whole series; the window picks.
[[nodiscard]] Metrics metrics_of(const std::vector<double>& equity,
                                 const std::vector<std::int64_t>& days,
                                 const std::vector<char>& exposure,
                                 const std::vector<Trade>& trades,
                                 const Series& series,
                                 const Window& w) {
    Metrics m;
    for (const Trade& trade : trades) {
        if (trade.first_buy_bar >= w.a && trade.first_buy_bar <= w.b) {
            ++m.n_entries;
        }
    }
    const std::size_t from = w.a > series.s ? w.a - 1 : w.a;
    if (w.b <= from) {
        return m;
    }
    const std::size_t count = w.b - from;  // returns
    const auto level = [&](std::size_t i) { return equity[from + i]; };
    const double first = level(0);
    const double last = level(count);

    m.total_return_pct = 100.0 * (last / first - 1.0);
    const auto calendar_days = static_cast<double>(days[w.b] - days[from]);
    if (calendar_days > 0.0) {
        m.cagr_pct = 100.0 * (std::pow(last / first, 365.25 / calendar_days) - 1.0);
    }

    std::vector<double> r(count);
    double sum = 0.0;
    for (std::size_t i = 1; i <= count; ++i) {
        r[i - 1] = level(i) / level(i - 1) - 1.0;
        sum += r[i - 1];
    }
    const double mean = sum / static_cast<double>(count);
    if (count >= 2) {
        double squares = 0.0;
        for (const double x : r) {
            squares += (x - mean) * (x - mean);
        }
        const double sd = std::sqrt(squares / static_cast<double>(count - 1));
        m.volatility_pct = 100.0 * sd * std::sqrt(252.0);
        if (sd != 0.0) {
            m.sharpe = mean / sd * std::sqrt(252.0);
        }
    }
    double downside = 0.0;
    for (const double x : r) {
        const double d = std::min(x, 0.0);
        downside += d * d;
    }
    const double dd = std::sqrt(downside / static_cast<double>(count));
    if (dd != 0.0) {
        m.sortino = mean / dd * std::sqrt(252.0);
    }

    double peak = first;
    std::int64_t peak_day = days[from];
    bool underwater = false;
    std::int64_t longest = 0;
    double worst = 0.0;
    double ulcer = 0.0;
    for (std::size_t i = 0; i <= count; ++i) {
        const double x = level(i);
        const std::int64_t day = days[from + i];
        if (x >= peak) {
            if (underwater) {
                longest = std::max(longest, day - peak_day);
                underwater = false;
            }
            peak = x;
            peak_day = day;
        } else {
            underwater = true;
        }
        const double drawdown = 100.0 * (x / peak - 1.0);
        worst = std::min(worst, drawdown);
        ulcer += drawdown * drawdown;
    }
    if (underwater) {
        longest = std::max(longest, days[w.b] - peak_day);
    }
    m.max_drawdown_pct = worst;
    m.max_drawdown_days = static_cast<double>(longest);
    if (worst != 0.0 && !std::isnan(m.cagr_pct)) {
        m.calmar = m.cagr_pct / std::abs(worst);
    }
    m.ulcer_index = std::sqrt(ulcer / static_cast<double>(count + 1));

    long long exposed = 0;
    for (std::size_t t = from + 1; t <= w.b; ++t) {
        exposed += exposure[t] != 0 ? 1 : 0;
    }
    m.time_in_market_pct = 100.0 * static_cast<double>(exposed) / static_cast<double>(count);

    double wins = 0.0;
    double total = 0.0;
    long long closed = 0;
    double worst_trade = std::numeric_limits<double>::infinity();
    for (const Trade& trade : trades) {
        if (!trade.sell_bar || *trade.sell_bar < w.a || *trade.sell_bar > w.b) {
            continue;
        }
        const double ret = trade.return_pct();
        ++closed;
        wins += ret > 0.0 ? 1.0 : 0.0;
        total += ret;
        worst_trade = std::min(worst_trade, ret);
    }
    if (closed > 0) {
        m.win_rate_pct = 100.0 * wins / static_cast<double>(closed);
        m.avg_trade_return_pct = total / static_cast<double>(closed);
        m.worst_trade_pct = worst_trade;
    }
    return m;
}

[[nodiscard]] json metrics_json(const Metrics& m) {
    return json{{"total_return_pct", num(m.total_return_pct)},
                {"cagr_pct", num(m.cagr_pct)},
                {"volatility_pct", num(m.volatility_pct)},
                {"sharpe", num(m.sharpe)},
                {"sortino", num(m.sortino)},
                {"max_drawdown_pct", num(m.max_drawdown_pct)},
                {"max_drawdown_days", num(m.max_drawdown_days)},
                {"calmar", num(m.calmar)},
                {"ulcer_index", num(m.ulcer_index)},
                {"time_in_market_pct", num(m.time_in_market_pct)},
                {"n_entries", m.n_entries},
                {"win_rate_pct", num(m.win_rate_pct)},
                {"avg_trade_return_pct", num(m.avg_trade_return_pct)},
                {"worst_trade_pct", num(m.worst_trade_pct)}};
}

struct Measured {
    Analysis analysis;
    Metrics full;
    Metrics in_sample;
    Metrics out_of_sample;
};

[[nodiscard]] Measured measure(const RuleRun& run, const Series& series, bool has_split) {
    Measured out;
    out.analysis = analyse(run, series);
    const Windows w = windows_of(series, has_split);
    const auto of = [&](const Window& window) {
        return metrics_of(out.analysis.equity,
                          series.days,
                          out.analysis.exposure,
                          out.analysis.trades,
                          series,
                          window);
    };
    out.full = of(w.full);
    if (has_split) {
        out.in_sample = of(w.in_sample);
        out.out_of_sample = of(w.out_of_sample);
    }
    return out;
}

[[nodiscard]] json windows_json(const Measured& m, const Series& series, bool has_split) {
    const Windows w = windows_of(series, has_split);
    const auto one = [&](const Window& window, const Metrics& metrics) {
        return json{{"from", iso_date(series.days[window.a])},
                    {"to", iso_date(series.days[window.b])},
                    {"metrics", metrics_json(metrics)}};
    };
    json out = json::object();
    out["full"] = one(w.full, m.full);
    if (has_split) {
        out["in_sample"] = one(w.in_sample, m.in_sample);
        out["out_of_sample"] = one(w.out_of_sample, m.out_of_sample);
    }
    return out;
}

// --- 5. Selection ---------------------------------------------------------------

/// Whether `a` is better than `b` for `metric`. A null is worse than any number.
[[nodiscard]] bool better(const std::string& metric, double a, double b) {
    if (std::isnan(a)) {
        return false;
    }
    if (std::isnan(b)) {
        return true;
    }
    return metric == "ulcer_index" ? a < b : a > b;
}

// --- 6. The null ------------------------------------------------------------------

/// Section 6's frictionless curve for an exposure series, as levels over `full`.
[[nodiscard]] std::vector<double> timing_curve(const Series& series,
                                               const std::vector<char>& exposure) {
    std::vector<double> v(series.closes.size(), 1.0);
    for (std::size_t t = series.s + 1; t <= series.e; ++t) {
        v[t] = exposure[t] != 0 ? v[t - 1] * (1.0 + (series.closes[t] / series.closes[t - 1] - 1.0))
                                : v[t - 1];
    }
    return v;
}

[[nodiscard]] double
timing_metric(const Series& series, const std::vector<char>& exposure, const std::string& metric) {
    const std::vector<double> curve = timing_curve(series, exposure);
    const Window full{series.s, series.e, true};
    return metrics_of(curve, series.days, exposure, {}, series, full).get(metric);
}

[[nodiscard]] std::vector<std::size_t> holding_lengths(const Analysis& analysis,
                                                       const Series& series) {
    std::vector<std::size_t> lengths;
    for (const Trade& trade : analysis.trades) {
        const std::size_t last = trade.sell_bar.value_or(series.e);
        lengths.push_back(last - trade.first_buy_bar);
    }
    return lengths;
}

struct NullResult {
    std::vector<double> values;
    double strategy = kNaN;
    double percentile_rank = 0.0;
    double p_value = 1.0;
};

[[nodiscard]] NullResult random_timing(const Series& series,
                                       const Analysis& selected,
                                       const std::string& metric,
                                       int draws,
                                       SplitMix64& rng) {
    NullResult out;
    out.strategy = timing_metric(series, selected.exposure, metric);
    const std::vector<std::size_t> lengths = holding_lengths(selected, series);
    if (lengths.empty()) {
        return out;
    }
    const std::size_t bars = series.e - series.s;
    const std::size_t held = std::accumulate(lengths.begin(), lengths.end(), std::size_t{0});
    for (int n = 0; n < draws; ++n) {
        std::vector<std::size_t> order = lengths;
        for (std::size_t i = order.size() - 1; i > 0; --i) {
            std::swap(order[i], order[rng.draw(i + 1)]);
        }
        std::vector<std::size_t> gaps(order.size());
        for (std::size_t& g : gaps) {
            g = rng.draw(bars - held + 1);
        }
        std::sort(gaps.begin(), gaps.end());
        std::vector<char> exposure(series.closes.size(), 0);
        std::size_t before = 0;
        for (std::size_t j = 0; j < order.size(); ++j) {
            const std::size_t start = series.s + gaps[j] + before;
            for (std::size_t t = start + 1; t <= start + order[j]; ++t) {
                exposure[t] = 1;
            }
            before += order[j];
        }
        out.values.push_back(timing_metric(series, exposure, metric));
    }
    double below = 0.0;
    double equal = 0.0;
    double at_or_above = 0.0;
    for (const double v : out.values) {
        // Null metrics compare as worse than any number, the same rule as
        // selection, so they count as below a numeric strategy figure.
        const bool v_better = better(metric, v, out.strategy);
        const bool s_better = better(metric, out.strategy, v);
        if (!v_better && !s_better) {
            equal += 1.0;
            at_or_above += 1.0;
        } else if (s_better) {
            below += 1.0;
        } else {
            at_or_above += 1.0;
        }
    }
    const auto count = static_cast<double>(out.values.size());
    out.percentile_rank = 100.0 * (below + 0.5 * equal) / count;
    out.p_value = (1.0 + at_or_above) / (1.0 + count);
    return out;
}

// --- 7. Effective N -------------------------------------------------------------

[[nodiscard]] double effective_n(const std::vector<Measured>& points, const Series& series) {
    std::vector<std::vector<double>> returns;
    for (const Measured& point : points) {
        std::vector<double> r;
        for (std::size_t t = series.s + 1; t <= series.e; ++t) {
            r.push_back(point.analysis.equity[t] / point.analysis.equity[t - 1] - 1.0);
        }
        double sum = 0.0;
        for (const double x : r) {
            sum += x;
        }
        const double mean = sum / static_cast<double>(r.size());
        double squares = 0.0;
        for (double& x : r) {
            x -= mean;
            squares += x * x;
        }
        if (squares > 0.0) {
            const double norm = std::sqrt(squares);
            for (double& x : r) {
                x /= norm;
            }
            returns.push_back(std::move(r));
        }
    }
    const std::size_t n = returns.size();
    if (n <= 1) {
        return 1.0;
    }
    std::vector<std::vector<double>> corr(n, std::vector<double>(n, 0.0));
    for (std::size_t i = 0; i < n; ++i) {
        for (std::size_t j = i; j < n; ++j) {
            double dot = 0.0;
            for (std::size_t t = 0; t < returns[i].size(); ++t) {
                dot += returns[i][t] * returns[j][t];
            }
            corr[i][j] = corr[j][i] = dot;
        }
    }
    std::vector<double> eig = symmetric_eigenvalues(std::move(corr));
    std::sort(eig.begin(), eig.end(), std::greater<>());
    double total = 0.0;
    for (const double v : eig) {
        total += v;
    }
    double running = 0.0;
    for (std::size_t k = 0; k < eig.size(); ++k) {
        running += eig[k];
        if (running >= 0.95 * total) {
            return static_cast<double>(k + 1);
        }
    }
    return static_cast<double>(eig.size());
}

// --- 8. Plateau -----------------------------------------------------------------

[[nodiscard]] double edge(const std::string& metric, double point, double benchmark) {
    if (std::isnan(point) || std::isnan(benchmark)) {
        return kNaN;
    }
    return metric == "ulcer_index" ? benchmark - point : point - benchmark;
}

[[nodiscard]] double plateau_ratio(const Bound& bound,
                                   const std::vector<std::vector<std::size_t>>& grid,
                                   const std::vector<double>& objective,
                                   std::size_t selected,
                                   double benchmark_objective) {
    const double own = edge(bound.metric, objective[selected], benchmark_objective);
    if (std::isnan(own) || own <= 0.0) {
        return kNaN;
    }
    std::vector<double> neighbours;
    for (std::size_t i = 0; i < grid.size(); ++i) {
        std::size_t differing = 0;
        bool one_step = true;
        for (std::size_t d = 0; d < grid[i].size(); ++d) {
            if (grid[i][d] != grid[selected][d]) {
                ++differing;
                const auto delta =
                    static_cast<long long>(grid[i][d]) - static_cast<long long>(grid[selected][d]);
                one_step = one_step && (delta == 1 || delta == -1);
            }
        }
        if (differing == 1 && one_step) {
            const double e = edge(bound.metric, objective[i], benchmark_objective);
            if (std::isnan(e)) {
                return kNaN;
            }
            neighbours.push_back(e);
        }
    }
    if (neighbours.empty()) {
        return kNaN;
    }
    std::sort(neighbours.begin(), neighbours.end());
    const std::size_t n = neighbours.size();
    const double median =
        n % 2 == 1 ? neighbours[n / 2] : (neighbours[n / 2 - 1] + neighbours[n / 2]) / 2.0;
    return median / own;
}

// --- 9. The outcome range -------------------------------------------------------

[[nodiscard]] BarSeries resample(const Series& series, SplitMix64& rng) {
    std::vector<double> returns;
    for (std::size_t t = series.s + 1; t <= series.e; ++t) {
        returns.push_back(series.closes[t] / series.closes[t - 1] - 1.0);
    }
    BarSeries path = series.bars;
    if (returns.empty()) {
        return path;
    }
    std::size_t j = rng.draw(returns.size());
    double close = series.closes[series.s];
    for (std::size_t t = series.s + 1; t <= series.e; ++t) {
        if (t > series.s + 1) {
            if (rng.draw(63) == 0) {
                j = rng.draw(returns.size());
            } else {
                j = (j + 1) % returns.size();
            }
        }
        close = close * (1.0 + returns[j]);
        StoredBar& bar = path.bars[t];
        bar.open = bar.high = bar.low = bar.close = close;
    }
    return path;
}

// --- Assembly ---------------------------------------------------------------------

[[nodiscard]] std::string gate_number(double value) {
    if (std::isnan(value)) {
        return "not measured";
    }
    std::ostringstream text;
    text.precision(4);
    text << value;
    return text.str();
}

struct Gate {
    std::string id;
    std::string status;  // pass | fail | not_applicable
    json measured = json::object();
    json threshold = json::object();
    json comparison = json::object();
    std::string summary;
};

[[nodiscard]] json gate_json(const Gate& g) {
    return json{{"id", g.id},
                {"status", g.status},
                {"measured", g.measured},
                {"threshold", g.threshold},
                {"comparison", g.comparison},
                {"summary", g.summary}};
}

[[nodiscard]] json fills_json(const std::vector<RuleFill>& fills) {
    json out = json::array();
    for (const RuleFill& f : fills) {
        out.push_back(json{{"bar", f.bar},
                           {"side", f.side == MS_SIDE_BUY ? "buy" : "sell"},
                           {"quantity", f.quantity},
                           {"price", f.price},
                           {"tag", f.tag},
                           {"commission", f.commission.to_double()}});
    }
    return out;
}

[[nodiscard]] json series_trace(const Series& series) {
    json dates = json::array();
    for (const std::int64_t day : series.days) {
        dates.push_back(iso_date(day));
    }
    return json{{"symbol", series.symbol},
                {"dates", dates},
                {"closes", series.closes},
                {"s", series.s},
                {"o", series.o},
                {"e", series.e}};
}

}  // namespace

// --- 1a. Synthetic history ----------------------------------------------------

SyntheticBars extend_history(const BarSeries& underlying,
                             const BarSeries& rate,
                             std::int64_t first_real_day,
                             double first_real_close,
                             const SyntheticParams& p) {
    const std::vector<StoredBar>& u = underlying.bars;
    std::size_t anchor = u.size();
    for (std::size_t k = 0; k < u.size(); ++k) {
        if (day_of(u[k].ts) == first_real_day) {
            anchor = k;
            break;
        }
    }
    if (anchor == u.size()) {
        throw std::runtime_error("the underlying has no bar on " + iso_date(first_real_day) +
                                 ", where the real history starts");
    }
    std::vector<std::int64_t> rate_days;
    for (const StoredBar& bar : rate.bars) {
        rate_days.push_back(day_of(bar.ts));
    }
    SyntheticBars out;
    double s = first_real_close;
    for (std::size_t k = anchor; k > 0; --k) {
        const std::int64_t previous = day_of(u[k - 1].ts);
        // The rate observed on or before the day the interval starts.
        const auto after = std::upper_bound(rate_days.begin(), rate_days.end(), previous);
        if (after == rate_days.begin()) {
            break;
        }
        const double rate_pct =
            rate.bars[static_cast<std::size_t>(after - rate_days.begin()) - 1].close;
        // One operation at a time, in research/synthetic.py's order.
        const double cost =
            (p.annual_fee_pct + (p.leverage - 1.0) * (rate_pct + p.borrow_spread_pct)) / 100.0 /
            252.0;
        const double r = p.leverage * (u[k].close / u[k - 1].close - 1.0) - cost;
        if (!(1.0 + r > 0.0)) {
            throw std::runtime_error("the recipe takes the price to zero on " +
                                     iso_date(day_of(u[k].ts)));
        }
        s = s / (1.0 + r);
        out.days.push_back(previous);
        out.closes.push_back(s);
    }
    std::reverse(out.days.begin(), out.days.end());
    std::reverse(out.closes.begin(), out.closes.end());
    return out;
}

JobOutcome run_job(const json& form, const BarSource& data, const JobOptions& options) {
    JobOutcome outcome;
    if (!drawdown_entry_registered()) {
        outcome.errors.push_back({"", "the drawdown_entry strategy is not registered"});
        return outcome;
    }

    // --- 1. Bind --------------------------------------------------------------
    Binder binder(form, data, options.recipes);
    std::optional<Bound> maybe = binder.bind(outcome.errors);
    if (!maybe) {
        return outcome;
    }
    const Bound& bound = *maybe;
    const Series& asset = bound.asset;
    const bool split = bound.has_split;
    const std::string canonical = canonical_form(form);
    const std::string form_hash = sha256_hex(canonical);
    std::uint64_t seed = 0;
    {
        const auto digest = sha256(canonical);
        for (int i = 7; i >= 0; --i) {
            seed = (seed << 8) | digest[static_cast<std::size_t>(i)];
        }
    }
    SplitMix64 rng(seed);

    // Every run is checked by the engine's own instruments before it is graded:
    // a ledger that did not balance, or a fill priced off something its decision
    // had already seen, is an engine bug, and a grade built on it would be a
    // statement about the bug.
    const auto run = [&](const Series& series, const RuleParams& rule) {
        RuleRunConfig config = bound.base;
        config.series = series.bars;
        config.rule = rule;
        RuleRun result = run_rule(config);
        if (result.ok() && !result.invariant_held) {
            result.error = "ENGINE BUG: the ledger's accounting identity did not hold";
        }
        for (const RuleFill& fill : result.fills) {
            if (result.ok() && !fill.honest) {
                result.error = "ENGINE BUG: a fill was priced no later than its decision";
            }
        }
        return result;
    };

    // --- 2. The sweep ---------------------------------------------------------
    const auto grid = enumerate_points(bound);
    std::vector<Measured> points;
    const auto report = [&](std::string_view stage, int done, int total) {
        if (options.progress && !options.progress(stage, done, total)) {
            outcome.errors.push_back({"", "cancelled"});
            outcome.cancelled = true;
            return false;
        }
        return true;
    };
    const auto grid_size = static_cast<int>(grid.size());
    for (const auto& point : grid) {
        if (!report("sweep", static_cast<int>(points.size()), grid_size)) {
            return outcome;
        }
        const RuleRun result = run(asset, rule_for(bound, point));
        if (!result.ok()) {
            outcome.errors.push_back({"/strategy", result.error});
            return outcome;
        }
        points.push_back(measure(result, asset, split));
    }

    std::vector<Measured> benchmarks;
    for (const Series& series : bound.benchmarks) {
        const RuleRun result = run(series, RuleParams::buy_and_hold());
        if (!result.ok()) {
            outcome.errors.push_back({"/benchmarks", result.error});
            return outcome;
        }
        benchmarks.push_back(measure(result, series, split));
    }
    const Measured& primary = benchmarks.front();

    // --- 5. Selection ---------------------------------------------------------
    const auto ranking_window = [&](const Measured& m) -> const Metrics& {
        return split ? m.in_sample : m.full;
    };
    const auto judged_window = [&](const Measured& m) -> const Metrics& {
        return split ? m.out_of_sample : m.full;
    };
    std::vector<double> objective;
    for (const Measured& m : points) {
        objective.push_back(ranking_window(m).get(bound.metric));
    }
    std::vector<bool> eligible;
    bool any_eligible = false;
    for (const Measured& m : points) {
        eligible.push_back(ranking_window(m).n_entries >= bound.min_trades);
        any_eligible = any_eligible || eligible.back();
    }
    std::size_t selected = 0;
    bool chosen = false;
    for (std::size_t i = 0; i < points.size(); ++i) {
        if (any_eligible && !eligible[i]) {
            continue;
        }
        if (!chosen || better(bound.metric, objective[i], objective[selected])) {
            selected = i;
            chosen = true;
        }
    }
    const Measured& best = points[selected];

    // --- 6, 7, 8 --------------------------------------------------------------
    if (!report("random timing", 0, 1)) {
        return outcome;
    }
    const NullResult null =
        random_timing(asset, best.analysis, bound.metric, options.null_draws, rng);
    const bool swept = !bound.swept.empty();
    const double n_eff = swept ? effective_n(points, asset) : kNaN;
    const double p_corrected = swept ? 1.0 - std::pow(1.0 - null.p_value, n_eff) : kNaN;
    const double benchmark_ranking = ranking_window(primary).get(bound.metric);
    const double plateau =
        swept ? plateau_ratio(bound, grid, objective, selected, benchmark_ranking) : kNaN;

    // --- 9. The outcome range -------------------------------------------------
    const RuleParams best_rule = rule_for(bound, grid[selected]);
    std::vector<double> path_strategy_cagr;
    std::vector<double> path_benchmark_cagr;
    std::vector<double> path_strategy_drawdown;
    json trace_paths = json::array();
    json first_path_closes = json::array();
    for (int n = 0; n < options.bootstrap_paths; ++n) {
        if (!report("bootstrap", n, options.bootstrap_paths)) {
            return outcome;
        }
        Series path = asset;
        path.bars = resample(asset, rng);
        for (std::size_t t = 0; t < path.closes.size(); ++t) {
            path.closes[t] = path.bars.bars[t].close;
        }
        if (n == 0) {
            first_path_closes = path.closes;
        }
        const RuleRun strategy_run = run(path, best_rule);
        const RuleRun bench_run = run(path, RuleParams::buy_and_hold());
        if (!strategy_run.ok() || !bench_run.ok()) {
            outcome.errors.push_back({"", "a bootstrap path could not be run"});
            return outcome;
        }
        const Measured s = measure(strategy_run, path, false);
        const Measured b = measure(bench_run, path, false);
        path_strategy_cagr.push_back(s.full.cagr_pct);
        path_benchmark_cagr.push_back(b.full.cagr_pct);
        path_strategy_drawdown.push_back(s.full.max_drawdown_pct);
        trace_paths.push_back(
            json{{"strategy", {strategy_run.equity[asset.s], strategy_run.equity[asset.e]}},
                 {"benchmark", {bench_run.equity[asset.s], bench_run.equity[asset.e]}},
                 {"strategy_max_drawdown_pct", num(s.full.max_drawdown_pct)}});
    }
    double beats = 0.0;
    double ruined = 0.0;
    for (std::size_t i = 0; i < path_strategy_cagr.size(); ++i) {
        beats += better("cagr", path_strategy_cagr[i], path_benchmark_cagr[i]) ? 1.0 : 0.0;
        ruined += path_strategy_drawdown[i] < -90.0 ? 1.0 : 0.0;
    }
    const auto path_count = static_cast<double>(std::max(options.bootstrap_paths, 1));
    const double prob_beats = beats / path_count;
    const double share_ruined = ruined / path_count;

    // --- 10. Gates and the grade -------------------------------------------------
    std::vector<Gate> gates;
    {
        Gate g{"evidence"};
        const long long entries = best.full.n_entries;
        g.measured["n_entries"] = entries;
        g.threshold["n_entries"] = 5;
        g.comparison["n_entries"] = ">=";
        bool pass = entries >= 5;
        g.summary = std::to_string(entries) + " entries (needs at least 5)";
        if (split) {
            const long long oos = best.out_of_sample.n_entries;
            g.measured["n_entries_oos"] = oos;
            g.threshold["n_entries_oos"] = 2;
            g.comparison["n_entries_oos"] = ">=";
            pass = pass && oos >= 2;
            g.summary += ", " + std::to_string(oos) + " out of sample (needs at least 2)";
        }
        g.summary += ".";
        g.status = pass ? "pass" : "fail";
        gates.push_back(g);
    }
    {
        Gate g{"oos_benchmark"};
        const double mine = judged_window(best).get(bound.metric);
        const double theirs = judged_window(primary).get(bound.metric);
        g.measured["strategy"] = num(mine);
        g.measured["benchmark"] = num(theirs);
        g.threshold["strategy"] = num(theirs);
        g.comparison["strategy"] = bound.metric == "ulcer_index" ? "<" : ">";
        g.status = better(bound.metric, mine, theirs) ? "pass" : "fail";
        g.summary = std::string(split ? "Out of sample, " : "Over the full range, ") +
                    bound.metric + " " + gate_number(mine) + " against the benchmark's " +
                    gate_number(theirs) + ".";
        gates.push_back(g);
    }
    {
        Gate g{"random_timing"};
        g.measured["strategy_percentile"] = null.percentile_rank;
        g.threshold["strategy_percentile"] = 95;
        g.comparison["strategy_percentile"] = ">=";
        g.status = null.percentile_rank >= 95.0 ? "pass" : "fail";
        g.summary = "Timing ranks at the " + gate_number(null.percentile_rank) +
                    "th percentile of random entries with the same holding periods (needs 95).";
        gates.push_back(g);
    }
    {
        Gate g{"multiple_testing"};
        if (!swept) {
            g.status = "not_applicable";
            g.summary = "Nothing was swept.";
        } else {
            g.measured["p_value_corrected"] = num(p_corrected);
            g.threshold["p_value_corrected"] = 0.05;
            g.comparison["p_value_corrected"] = "<";
            g.status = p_corrected < 0.05 ? "pass" : "fail";
            g.summary = "Corrected p-value " + gate_number(p_corrected) + " over " +
                        gate_number(n_eff) + " effective variants (needs below 0.05).";
        }
        gates.push_back(g);
    }
    {
        Gate g{"plateau"};
        if (!swept) {
            g.status = "not_applicable";
            g.summary = "Nothing was swept.";
        } else {
            g.measured["plateau_ratio"] = num(plateau);
            g.threshold["plateau_ratio"] = 0.5;
            g.comparison["plateau_ratio"] = ">=";
            g.status = !std::isnan(plateau) && plateau >= 0.5 ? "pass" : "fail";
            g.summary = std::isnan(plateau)
                            ? std::string("No plateau to measure: the selected point has no edge "
                                          "over the benchmark in sample, or no neighbours.")
                            : "Neighbouring parameters keep " + gate_number(plateau) +
                                  " of the selected edge (needs 0.5).";
        }
        gates.push_back(g);
    }
    {
        Gate g{"survival"};
        const double worst = best.full.max_drawdown_pct;
        g.measured["max_drawdown_pct"] = num(worst);
        g.measured["share_paths_below_minus_90_pct"] = share_ruined;
        g.threshold["max_drawdown_pct"] = -90;
        g.threshold["share_paths_below_minus_90_pct"] = 0.05;
        g.comparison["max_drawdown_pct"] = ">";
        g.comparison["share_paths_below_minus_90_pct"] = "<";
        g.status = (worst > -90.0 && share_ruined < 0.05) ? "pass" : "fail";
        g.summary = "Worst drawdown " + gate_number(worst) + "%, and " +
                    gate_number(100.0 * share_ruined) + "% of resampled paths below -90%.";
        gates.push_back(g);
    }

    static const std::map<std::string, std::string> kFailsTo = {
        {"evidence", "insufficient_evidence"},
        {"oos_benchmark", "not_supported"},
        {"random_timing", "fragile"},
        {"multiple_testing", "fragile"},
        {"plateau", "fragile"},
        {"survival", "fragile"}};
    std::string grade = "supported";
    json deciding = nullptr;
    for (const Gate& g : gates) {
        if (g.status == "fail") {
            grade = kFailsTo.at(g.id);
            deciding = g.id;
            break;
        }
    }
    bool research_grade = asset.quality == "research";
    for (const Series& series : bound.benchmarks) {
        research_grade = research_grade && series.quality == "research";
    }

    // --- The Results document -----------------------------------------------------
    json results;
    results["results_version"] = "0.1";
    results["run"] = {
        {"run_id", form_hash.substr(0, 16)},
        {"form_sha256", form_hash},
        {"engine_version",
         options.engine_version.empty() ? std::string(version::kString) : options.engine_version},
        {"criteria_version", "0.1"},
        {"data_snapshot", options.data_snapshot},
        {"started_at", options.started_at},
        {"duration_ms", options.duration_ms}};

    json gate_list = json::array();
    for (const Gate& g : gates) {
        gate_list.push_back(gate_json(g));
    }
    const json outcome_range = {{"cagr_p5_pct", num(percentile(path_strategy_cagr, 0.05))},
                                {"cagr_p50_pct", num(percentile(path_strategy_cagr, 0.50))},
                                {"cagr_p95_pct", num(percentile(path_strategy_cagr, 0.95))}};
    results["verdict"] = {
        {"grade", grade},
        {"data_grade", research_grade ? "research" : "exercise_only"},
        {"deciding_gate", deciding},
        {"headline",
         {{"strategy_oos_cagr_pct", num(judged_window(best).cagr_pct)},
          {"benchmark_oos_cagr_pct", num(judged_window(primary).cagr_pct)},
          {"strategy_oos_max_drawdown_pct", num(judged_window(best).max_drawdown_pct)},
          {"benchmark_oos_max_drawdown_pct", num(judged_window(primary).max_drawdown_pct)},
          {"outcome_range", outcome_range}}},
        {"gates", gate_list}};

    json selected_params = json::object();
    for (std::size_t d = 0; d < bound.swept.size(); ++d) {
        const Param& param = bound.params[bound.swept[d]];
        selected_params[param.path] = param.values[grid[selected][d]];
    }

    json bench_list = json::array();
    for (std::size_t i = 0; i < benchmarks.size(); ++i) {
        const Series& series = bound.benchmarks[i];
        const std::string id = series.symbol == asset.symbol ? bound.security_id : series.symbol;
        bench_list.push_back(json{{"symbol", series.symbol},
                                  {"security_id", id},
                                  {"windows", windows_json(benchmarks[i], series, split)}});
    }

    json trades = json::array();
    bool open_at_end = false;
    for (const Trade& trade : best.analysis.trades) {
        const std::size_t last = trade.sell_bar.value_or(asset.e);
        const double entry_price = trade.bought_value / trade.quantity;
        double worst = 0.0;
        for (std::size_t t = trade.first_buy_bar; t <= last; ++t) {
            worst = std::min(worst, 100.0 * (asset.closes[t] / entry_price - 1.0));
        }
        open_at_end = open_at_end || !trade.sell_bar;
        trades.push_back(json{
            {"entry_date", iso_date(asset.days[trade.first_buy_bar])},
            {"entry_price", entry_price},
            {"exit_date", trade.sell_bar ? json(iso_date(asset.days[*trade.sell_bar])) : json()},
            {"exit_price", trade.exit_price},
            {"return_pct", trade.return_pct()},
            {"holding_days", asset.days[last] - asset.days[trade.first_buy_bar]},
            {"max_adverse_pct", worst},
            {"exit_reason",
             trade.sell_bar ? std::string(to_string(trade.reason)) : std::string("open")},
            {"window", split && trade.first_buy_bar >= asset.o ? "out_of_sample" : "in_sample"},
            // A trade is held in date order, so it touched synthetic history
            // exactly when it was entered on it.
            {"synthetic", asset.synthetic_at(trade.first_buy_bar)}});
    }

    json surface = nullptr;
    bool any_excluded = false;
    bool at_edge = false;
    if (swept) {
        json swept_paths = json::array();
        for (const std::size_t index : bound.swept) {
            swept_paths.push_back(bound.params[index].path);
        }
        json surface_points = json::array();
        for (std::size_t i = 0; i < points.size(); ++i) {
            json params = json::object();
            for (std::size_t d = 0; d < bound.swept.size(); ++d) {
                const Param& param = bound.params[bound.swept[d]];
                params[param.path] = param.values[grid[i][d]];
            }
            const bool excluded = any_eligible && !eligible[i];
            any_excluded = any_excluded || excluded;
            surface_points.push_back(
                json{{"params", params},
                     {"in_sample_metric", num(objective[i])},
                     {"out_of_sample_metric",
                      num(split ? points[i].out_of_sample.get(bound.metric) : kNaN)},
                     {"n_entries", ranking_window(points[i]).n_entries},
                     {"excluded", excluded}});
        }
        for (std::size_t d = 0; d < bound.swept.size(); ++d) {
            const std::size_t size = bound.params[bound.swept[d]].values.size();
            at_edge = at_edge || grid[selected][d] == 0 || grid[selected][d] + 1 == size;
        }
        surface = json{{"swept", swept_paths},
                       {"metric", bound.metric},
                       {"points", surface_points},
                       {"selected_index", selected},
                       {"plateau_ratio", num(plateau)},
                       {"effective_n", n_eff}};
        // Reported unless the form asked not to. The gates still use it: the
        // surface is what multiple_testing and plateau measure, whether or not
        // it is shown.
        if (!form.at("robustness").value("parameter_surface", true)) {
            surface = nullptr;
        }
    }

    // The synthetic part of the bars used, with the recipe's validation, so
    // the analysis can say in numbers how far to trust those years.
    json synthetic_used = nullptr;
    std::size_t synthetic_decisions = 0;
    if (!bound.synthetic.is_null() && asset.synthetic_at(0)) {
        std::size_t last = 0;
        while (last + 1 < asset.days.size() && asset.synthetic_at(last + 1)) {
            ++last;
        }
        const json& validation = bound.synthetic.at("validation");
        synthetic_used =
            json{{"recipe_id", bound.synthetic.at("recipe_id")},
                 {"from", iso_date(asset.days.front())},
                 {"to", iso_date(asset.days[last])},
                 {"daily_return_correlation", validation.at("daily_return_correlation")},
                 {"annualized_tracking_difference_pct",
                  validation.at("annualized_tracking_difference_pct")}};
        for (std::size_t t = asset.s; t <= asset.e; ++t) {
            synthetic_decisions += asset.synthetic_at(t) ? 1 : 0;
        }
    }
    json data_list = json::array();
    data_list.push_back(json{
        {"security_id", bound.security_id},
        {"series_id",
         synthetic_used.is_null()
             ? asset.symbol
             : asset.symbol + "+synthetic:" + bound.synthetic.at("recipe_id").get<std::string>()},
        {"quality_tier", asset.quality},
        {"used_from", iso_date(asset.days.front())},
        {"used_to", iso_date(asset.days.back())},
        {"synthetic", synthetic_used}});
    for (const Series& series : bound.benchmarks) {
        if (series.symbol == asset.symbol) {
            continue;
        }
        data_list.push_back(json{{"security_id", series.symbol},
                                 {"series_id", series.symbol},
                                 {"quality_tier", series.quality},
                                 {"used_from", iso_date(series.days.front())},
                                 {"used_to", iso_date(series.days.back())},
                                 {"synthetic", nullptr}});
    }

    results["stats"] = {
        {"selected", {{"params", selected_params}, {"windows", windows_json(best, asset, split)}}},
        {"benchmarks", bench_list},
        {"trades", trades},
        {"parameter_surface", surface},
        {"regimes", json::array()},
        {"random_timing",
         {{"n_simulations", static_cast<long long>(null.values.size())},
          {"metric", bound.metric},
          {"null_p5", num(null.values.empty() ? kNaN : percentile(null.values, 0.05))},
          {"null_p50", num(null.values.empty() ? kNaN : percentile(null.values, 0.50))},
          {"null_p95", num(null.values.empty() ? kNaN : percentile(null.values, 0.95))},
          {"strategy_percentile", null.percentile_rank},
          {"p_value", null.p_value},
          {"p_value_corrected", num(p_corrected)}}},
        {"bootstrap",
         {{"method", "stationary_block"},
          {"mean_block_days", 63},
          {"n_paths", options.bootstrap_paths},
          {"strategy", outcome_range},
          {"benchmark",
           {{"cagr_p5_pct", num(percentile(path_benchmark_cagr, 0.05))},
            {"cagr_p50_pct", num(percentile(path_benchmark_cagr, 0.50))},
            {"cagr_p95_pct", num(percentile(path_benchmark_cagr, 0.95))}}},
          {"prob_beats_benchmark", prob_beats},
          {"share_paths_below_minus_90_pct", share_ruined}}},
        {"data", data_list},
        {"equity_curves", {{"uri", options.equity_uri}}}};

    json warnings = json::array();
    const auto warn = [&](const char* code, const char* message) {
        warnings.push_back(json{{"code", code}, {"message", message}});
    };
    if (2 * synthetic_decisions > asset.e - asset.s + 1) {
        warn("MOSTLY_SYNTHETIC",
             "Over half the range is synthetic history, simulated from the underlying by a "
             "recipe: the result is mostly about the simulation's years.");
    }
    if (!research_grade) {
        warn("EXERCISE_ONLY_DATA",
             "At least one series is exercise_only: this tests the pipeline and the data "
             "source, not a research-grade conclusion.");
    }
    if (!split) {
        warn("NO_OUT_OF_SAMPLE",
             "There was no out-of-sample split, so the benchmark gate was judged on the same "
             "range the parameters were chosen on.");
    }
    if (!swept) {
        warn("NO_SEARCH_CORRECTION",
             "Nothing was swept, so no multiple-testing correction was applied -- but this "
             "rule was still chosen by someone who had seen others.");
    } else {
        warn("UNCALIBRATED_CORRECTION",
             "The effective-N correction has not yet been calibrated on null data.");
    }
    if (any_excluded) {
        warn("FEW_TRADES_IN_SWEEP",
             "Some sweep points had too few entries to be ranked; they are shown, not chosen.");
    }
    if (open_at_end) {
        warn("OPEN_POSITION_AT_END",
             "The selected rule was still holding at the end; that trade is marked to market.");
    }
    if (at_edge) {
        warn("SWEEP_EDGE",
             "The selected value is at the edge of the swept range; the optimum may lie "
             "outside it.");
    }
    results["warnings"] = warnings;

    // --- The trace ------------------------------------------------------------------
    json trace;
    trace["trace_version"] = "0.1";
    trace["form"] = json::parse(canonical);
    trace["asset"] = series_trace(asset);
    trace["has_split"] = split;
    trace["metric"] = bound.metric;
    trace["min_trades"] = bound.min_trades;
    trace["null_draws"] = options.null_draws;
    json dims = json::array();
    for (const std::size_t index : bound.swept) {
        dims.push_back(
            json{{"path", bound.params[index].path}, {"values", bound.params[index].values}});
    }
    trace["dims"] = dims;
    json trace_points = json::array();
    for (const Measured& m : points) {
        trace_points.push_back(
            json{{"equity", m.analysis.equity}, {"fills", fills_json(m.analysis.fills)}});
    }
    trace["points"] = trace_points;
    json trace_benchmarks = json::array();
    for (std::size_t i = 0; i < benchmarks.size(); ++i) {
        json b = series_trace(bound.benchmarks[i]);
        b["equity"] = benchmarks[i].analysis.equity;
        b["fills"] = fills_json(benchmarks[i].analysis.fills);
        trace_benchmarks.push_back(b);
    }
    trace["benchmarks"] = trace_benchmarks;
    trace["bootstrap"] = {{"paths", trace_paths}, {"first_path_closes", first_path_closes}};
    trace["effective_n"] = num(n_eff);

    // --- The equity file ------------------------------------------------------------
    std::ostringstream csv;
    csv << "date,strategy";
    for (const Series& series : bound.benchmarks) {
        csv << "," << series.symbol;
    }
    csv << "\n";
    csv.precision(17);
    for (std::size_t t = asset.s; t <= asset.e; ++t) {
        csv << iso_date(asset.days[t]) << "," << best.analysis.equity[t];
        for (std::size_t i = 0; i < benchmarks.size(); ++i) {
            const Series& series = bound.benchmarks[i];
            const auto it = std::lower_bound(series.days.begin(), series.days.end(), asset.days[t]);
            csv << ",";
            if (it != series.days.end() && *it == asset.days[t]) {
                csv << benchmarks[i]
                           .analysis.equity[static_cast<std::size_t>(it - series.days.begin())];
            }
        }
        csv << "\n";
    }

    // --- The curves -----------------------------------------------------------------
    const auto curve_of = [](const Series& series,
                             const std::vector<double>& equity,
                             std::string id,
                             std::string label,
                             bool is_benchmark) {
        JobCurve curve;
        curve.id = std::move(id);
        curve.label = std::move(label);
        curve.is_benchmark = is_benchmark;
        double peak = equity[series.s];
        for (std::size_t t = series.s; t <= series.e; ++t) {
            curve.times.push_back(static_cast<double>(series.bars.bars[t].ts) / 1e9);
            curve.equity.push_back(equity[t]);
            curve.normalised.push_back(equity[t] / equity[series.s]);
            peak = std::max(peak, equity[t]);
            curve.drawdown.push_back(equity[t] / peak - 1.0);
        }
        return curve;
    };
    outcome.curves.push_back(
        curve_of(asset, best.analysis.equity, "strategy", "Drawdown entry", false));
    for (std::size_t i = 0; i < benchmarks.size(); ++i) {
        const Series& series = bound.benchmarks[i];
        outcome.curves.push_back(curve_of(series,
                                          benchmarks[i].analysis.equity,
                                          "benchmark:" + series.symbol,
                                          "Buy and hold " + series.symbol,
                                          true));
    }

    outcome.results = std::move(results);
    outcome.trace = std::move(trace);
    outcome.equity_csv = csv.str();
    return outcome;
}

}  // namespace marketscan::research
