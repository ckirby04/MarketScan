#include "marketscan/data/universe.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>

#include "marketscan/data/bar_store.hpp"

namespace marketscan {
namespace {

[[nodiscard]] std::string date_of(Timestamp ts) {
    if (ts == kNever) {
        return "--";
    }
    if (ts == kForever) {
        return "current";
    }
    return time::to_iso8601(ts).substr(0, 10);
}

[[nodiscard]] std::string days_of(Duration d) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(0)
        << static_cast<double>(d) / static_cast<double>(24LL * 3600 * 1'000'000'000LL);
    return out.str();
}

[[nodiscard]] std::string trim(std::string_view text) {
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end && (text[begin] == ' ' || text[begin] == '\t' || text[begin] == '\r' ||
                           text[begin] == '"')) {
        ++begin;
    }
    while (end > begin && (text[end - 1] == ' ' || text[end - 1] == '\t' || text[end - 1] == '\r' ||
                           text[end - 1] == '"')) {
        --end;
    }
    return std::string(text.substr(begin, end - begin));
}

/// The stamp a membership date maps to: the close of that session.
///
/// The same convention daily bars use. A membership interval measured against
/// midnight and a bar stamped at the close would put a name's first day either
/// inside or outside its own membership depending on which side of the
/// comparison it landed -- an off-by-one-session error that shows up as a
/// strategy trading a name the day before it joined.
[[nodiscard]] bool parse_date(std::string_view text, Timestamp& out) {
    const std::string value = trim(text);
    if (value.size() < 10) {
        return false;
    }
    int year = 0;
    int month = 0;
    int day = 0;
    char dash1 = 0;
    char dash2 = 0;
    std::istringstream in(value.substr(0, 10));
    in >> year >> dash1 >> month >> dash2 >> day;
    if (in.fail() || dash1 != '-' || dash2 != '-' || month < 1 || month > 12 || day < 1 ||
        day > 31) {
        return false;
    }
    out = bar_store::daily_close_stamp(
        calendar::days_from_civil(year, static_cast<unsigned>(month), static_cast<unsigned>(day)));
    return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// Universe
// ---------------------------------------------------------------------------

void Universe::add(SecurityId security, Timestamp from, Timestamp until) {
    if (built_) {
        throw std::logic_error("Universe::add after build(); a universe is read-only once built");
    }
    if (security == kInvalidSecurity || until <= from) {
        return;
    }
    periods_.push_back(MembershipPeriod{security, from, until});
}

void Universe::build() {
    std::sort(
        periods_.begin(), periods_.end(), [](const MembershipPeriod& a, const MembershipPeriod& b) {
            return a.security != b.security ? a.security < b.security : a.from < b.from;
        });

    // Merge touching or overlapping periods for one security, so that a name
    // present every day for a decade is one period rather than 2,520.
    std::vector<MembershipPeriod> merged;
    for (const MembershipPeriod& period : periods_) {
        if (!merged.empty() && merged.back().security == period.security &&
            period.from <= merged.back().until) {
            merged.back().until = std::max(merged.back().until, period.until);
        } else {
            merged.push_back(period);
        }
    }
    periods_ = std::move(merged);
    built_ = true;
    cache_valid_ = false;
}

const std::vector<SecurityId>& Universe::members_on(Timestamp ts) const {
    if (cache_valid_ && cached_ts_ == ts) {
        return cached_members_;
    }
    cached_members_.clear();
    for (const MembershipPeriod& period : periods_) {
        if (period.covers(ts)) {
            cached_members_.push_back(period.security);
        }
    }
    // Ascending by id. periods_ is sorted by security, so this is already true
    // unless a security has several periods, and sorting is cheap next to the
    // scan that produced it.
    std::sort(cached_members_.begin(), cached_members_.end());
    cached_members_.erase(std::unique(cached_members_.begin(), cached_members_.end()),
                          cached_members_.end());
    cached_ts_ = ts;
    cache_valid_ = true;
    return cached_members_;
}

bool Universe::is_member(SecurityId security, Timestamp ts) const noexcept {
    for (const MembershipPeriod& period : periods_) {
        if (period.security == security && period.covers(ts)) {
            return true;
        }
    }
    return false;
}

Timestamp Universe::membership_ends(SecurityId security, Timestamp ts) const noexcept {
    for (const MembershipPeriod& period : periods_) {
        if (period.security == security && period.covers(ts)) {
            return period.until;
        }
    }
    return kForever;
}

std::size_t Universe::security_count() const noexcept {
    std::size_t count = 0;
    SecurityId previous = kInvalidSecurity;
    for (const MembershipPeriod& period : periods_) {
        if (period.security != previous) {
            ++count;
            previous = period.security;
        }
    }
    return count;
}

Timestamp Universe::first() const noexcept {
    Timestamp earliest = kForever;
    for (const MembershipPeriod& period : periods_) {
        earliest = std::min(earliest, period.from);
    }
    return periods_.empty() ? kNever : earliest;
}

Timestamp Universe::last() const noexcept {
    Timestamp latest = kNever;
    for (const MembershipPeriod& period : periods_) {
        latest = std::max(latest, period.until);
    }
    return latest;
}

// ---------------------------------------------------------------------------
// Rules and overrides
// ---------------------------------------------------------------------------

const IdentityRules::Override* IdentityRules::find_override(std::string_view ticker,
                                                            Timestamp resumed_on) const noexcept {
    for (const Override& entry : overrides) {
        if (entry.ticker != ticker) {
            continue;
        }
        // An override with no date applies to every gap for that ticker; one
        // with a date applies to the gap that ended on it.
        if (entry.resumed_on == kNever || entry.resumed_on == resumed_on) {
            return &entry;
        }
    }
    return nullptr;
}

IdentityRules load_overrides(const std::filesystem::path& path, IdentityRules rules) {
    std::ifstream file(path);
    if (!file) {
        // Not an error. Overrides are corrections, and having none is normal.
        return rules;
    }

    std::string line;
    bool first = true;
    while (std::getline(file, line)) {
        const std::string trimmed = trim(line);
        if (trimmed.empty() || trimmed.front() == '#') {
            continue;
        }
        if (first) {
            first = false;
            if (trimmed.rfind("ticker", 0) == 0) {
                continue;  // header
            }
        }

        std::vector<std::string> fields;
        std::string field;
        std::istringstream row(trimmed);
        while (std::getline(row, field, ',')) {
            fields.push_back(trim(field));
        }
        if (fields.size() < 2) {
            continue;
        }

        IdentityRules::Override entry;
        entry.ticker = fields[0];
        entry.action = fields[1];
        if (fields.size() > 2 && !fields[2].empty()) {
            (void)parse_date(fields[2], entry.resumed_on);
        }
        if (fields.size() > 3) {
            entry.note = fields[3];
        }
        if (entry.action == "split" || entry.action == "keep") {
            rules.overrides.push_back(std::move(entry));
        }
    }
    return rules;
}

// ---------------------------------------------------------------------------
// Snapshot source
// ---------------------------------------------------------------------------

SnapshotMembershipSource::SnapshotMembershipSource(const std::filesystem::path& path)
    : name_("snapshots:" + path.filename().string()) {
    std::ifstream file(path);
    if (!file) {
        throw std::runtime_error("SnapshotMembershipSource: cannot open '" + path.string() + "'");
    }

    // date -> members on that date. Ordered, because the intervals are derived
    // by walking consecutive snapshots and the walk has to be chronological.
    std::map<Timestamp, std::set<std::string>> snapshots;

    std::string line;
    std::size_t line_number = 0;
    while (std::getline(file, line)) {
        ++line_number;
        const std::string trimmed = trim(line);
        if (trimmed.empty() || trimmed.front() == '#') {
            continue;
        }

        const std::size_t comma = trimmed.find(',');
        if (comma == std::string::npos) {
            continue;
        }

        Timestamp date = kNever;
        if (!parse_date(trimmed.substr(0, comma), date)) {
            continue;  // header row, or a line that is not a snapshot
        }

        std::set<std::string> members;
        std::string rest = trimmed.substr(comma + 1);
        std::string ticker;
        std::istringstream tickers(rest);
        while (std::getline(tickers, ticker, ',')) {
            const std::string cleaned = trim(ticker);
            if (!cleaned.empty()) {
                members.insert(cleaned);
            }
        }
        if (!members.empty()) {
            snapshots[date] = std::move(members);
        }
    }

    if (snapshots.empty()) {
        throw std::runtime_error("SnapshotMembershipSource: '" + path.string() +
                                 "' held no usable snapshots. Expected rows of "
                                 "date,TICKER,TICKER,...");
    }
    snapshots_ = snapshots.size();

    // --- Snapshots to intervals ----------------------------------------
    //
    // A name present in consecutive snapshots is one interval. A name that
    // disappears ends its interval at the snapshot it was last seen in, and one
    // that reappears starts a new one -- which is precisely the gap the
    // identity heuristic then has to rule on.
    std::map<std::string, Row> open;
    Timestamp previous = kNever;

    for (const auto& [date, members] : snapshots) {
        for (auto it = open.begin(); it != open.end();) {
            if (members.count(it->first) == 0) {
                // Membership ends at the first snapshot that does not list it,
                // exclusive -- so the last day it was a member is the previous
                // snapshot, which is what it was.
                it->second.until = date;
                rows_.push_back(it->second);
                it = open.erase(it);
            } else {
                ++it;
            }
        }
        for (const std::string& ticker : members) {
            if (open.find(ticker) == open.end()) {
                open[ticker] = Row{ticker, date, kForever};
            }
        }
        previous = date;
    }
    (void)previous;

    // Still open at the last snapshot: current members.
    for (auto& [ticker, row] : open) {
        rows_.push_back(row);
    }

    std::sort(rows_.begin(), rows_.end(), [](const Row& a, const Row& b) {
        return a.from != b.from ? a.from < b.from : a.ticker < b.ticker;
    });
}

// ---------------------------------------------------------------------------
// The split heuristic
// ---------------------------------------------------------------------------

MembershipHistory build_membership(const IMembershipSource& source,
                                   SecurityMaster& master,
                                   const IdentityRules& rules) {
    MembershipHistory history;

    std::vector<IMembershipSource::Row> rows = source.rows();

    // Chronological within a ticker, and tickers in a stable order between
    // themselves, so the ids assigned do not depend on how the source happened
    // to order its output.
    std::sort(rows.begin(),
              rows.end(),
              [](const IMembershipSource::Row& a, const IMembershipSource::Row& b) {
                  return a.ticker != b.ticker ? a.ticker < b.ticker : a.from < b.from;
              });

    std::string current_ticker;
    Timestamp previous_until = kNever;
    Timestamp flank_start = kNever;
    SecurityId current_id = kInvalidSecurity;

    for (const IMembershipSource::Row& row : rows) {
        if (row.ticker.empty() || row.until <= row.from) {
            history.rejected.push_back(row.ticker + ": period ends before it begins");
            continue;
        }

        if (row.ticker != current_ticker) {
            current_ticker = row.ticker;
            previous_until = kNever;
            current_id = kInvalidSecurity;
            flank_start = kNever;
        }

        if (current_id == kInvalidSecurity) {
            current_id = master.declare(row.ticker, row.from, row.until);
            history.universe.add(current_id, row.from, row.until);
            previous_until = row.until;
            flank_start = row.from;
            continue;
        }

        // --- A gap. The decision this whole module exists to make. ------
        const Duration gap = row.from - previous_until;
        IdentityDecision decision;
        decision.ticker = row.ticker;
        decision.flank_start = flank_start;
        decision.gap_start = previous_until;
        decision.gap_end = row.from;
        decision.gap = gap;

        const IdentityRules::Override* override_entry = rules.find_override(row.ticker, row.from);

        bool split = false;
        if (override_entry != nullptr) {
            split = override_entry->action == "split";
            decision.rule = "override";
            decision.note = override_entry->note;
        } else if (gap >= rules.split_gap) {
            split = true;
            decision.rule = "threshold";
            decision.note = "gap of " + days_of(gap) + " days is at or past the " +
                            days_of(rules.split_gap) + "-day threshold";
        } else if (gap + rules.near_miss_window >= rules.split_gap) {
            split = false;
            decision.rule = "near miss";
            decision.note = "gap of " + days_of(gap) + " days is within " +
                            days_of(rules.near_miss_window) +
                            " days of the threshold; kept as one "
                            "security, but this is the kind of call worth reviewing";
        } else {
            split = false;
            decision.rule = "below threshold";
            decision.note = "gap of " + days_of(gap) + " days";
        }

        if (split) {
            current_id =
                master.declare(row.ticker,
                               row.from,
                               row.until,
                               "gap of " + days_of(gap) + " days after " + date_of(previous_until));
        } else {
            current_id = master.declare(row.ticker, row.from, row.until);
        }
        decision.split = split;
        decision.result = current_id;

        // Every gap is recorded, split or not. A decision that was nearly made
        // the other way is exactly the one worth being able to look up.
        history.decisions.push_back(std::move(decision));

        history.universe.add(current_id, row.from, row.until);
        previous_until = row.until;

        // The flank for the *next* gap is this period, unless this one started
        // a new security -- in which case the earlier eras belong to somebody
        // else and would make the next flank check meaningless.
        flank_start = split ? row.from : flank_start;
    }

    history.universe.build();
    return history;
}

// ---------------------------------------------------------------------------
// Measurement
// ---------------------------------------------------------------------------

void measure_identities(MembershipHistory& history,
                        const std::filesystem::path& price_cache,
                        const IdentityProbeConfig& config) {
    // One series per ticker, loaded once even when a ticker has several gaps.
    std::map<std::string, std::vector<StoredBar>> series;

    for (IdentityDecision& decision : history.decisions) {
        auto cached = series.find(decision.ticker);
        if (cached == series.end()) {
            std::vector<StoredBar> bars;
            const std::filesystem::path file =
                price_cache / (decision.ticker + std::string(bar_store::kExtension));
            std::error_code ec;
            if (std::filesystem::exists(file, ec)) {
                try {
                    bars = bar_store::read(file).bars;
                } catch (const std::exception&) {
                    // A corrupt cache entry is missing data, not evidence.
                    bars.clear();
                }
            }
            cached = series.emplace(decision.ticker, std::move(bars)).first;
        }

        decision.measured = probe_identity(
            cached->second, decision.flank_start, decision.gap_start, decision.gap_end, config);
    }
}

std::size_t write_measured_overrides(const std::filesystem::path& path,
                                     const MembershipHistory& history,
                                     const IdentityProbeConfig& config,
                                     std::string_view source_description) {
    std::ofstream out(path, std::ios::trunc);
    if (!out) {
        throw std::runtime_error("cannot write '" + path.string() + "'");
    }

    out << "# Corrections to the automatic security-identity inference.\n";
    out << "#\n";
    out << "# GENERATED from measurement. Regenerate with:\n";
    out << "#     marketscan --headless --measure-identity <history> --prices <cache>\n";
    out << "#\n";
    out << "# A row exists here only where a price series settled the question. Every note\n";
    out << "# states what was observed rather than what is believed: \"coverage 97.4%, no hole\n";
    out << "# > 3d\" is a measurement, and \"AutoNation traded continuously\" is a claim about\n";
    out << "# the world that this file has no business making.\n";
    out << "#\n";
    out << "# A gap the data could not settle gets no row. It stays governed by the\n";
    out << "# " << days_of(IdentityRules{}.split_gap) << "-day threshold and stays open. That is "
        << "the honest outcome far more\n";
    out << "# often than either verdict, and it is not a failure.\n";
    out << "#\n";
    out << "# Source: " << source_description << "\n";
    out << "# Probe: continuous at coverage >= " << config.continuous_coverage << " with no hole > "
        << config.max_hole_days
        << " weekdays; discontinuous at coverage <= " << config.discontinuous_coverage
        << " with one hole covering >= " << config.discontinuous_hole
        << " of the gap; a continuous verdict also needs >= " << config.flank_coverage
        << " coverage of the membership period before the gap.\n";
    out << "#\n";
    out << "# WHICH WAY TO ERR. Splitting when you should not costs history: a strategy cannot\n";
    out << "# compute a twelve-month signal across the break, and it can see that it cannot.\n";
    out << "# Keeping when you should have split welds one company's price series to another's\n";
    out << "# and manufactures a return that never existed, invisibly. So when it is genuinely\n";
    out << "# ambiguous, split.\n";
    out << "#\n";
    out << "# HAND-WRITTEN ROWS. Anything added here by a person should say so in its note and\n";
    out << "# will be overwritten the next time this is regenerated. Put durable corrections in\n";
    out << "# a separate file and merge, rather than editing a generated one.\n";
    out << "# -------------------------------------------------------------------------------\n";
    out << "ticker,action,resumed_on,note\n";

    std::size_t written = 0;
    for (const IdentityDecision& decision : history.decisions) {
        if (!decision.measured.resolved()) {
            continue;
        }
        const bool keep = decision.measured.verdict == IdentityVerdict::Continuous;

        std::ostringstream note;
        note << std::fixed << std::setprecision(1) << "coverage "
             << decision.measured.coverage * 100.0 << "%";
        if (decision.measured.largest_hole_days > 0) {
            note << ", largest hole " << decision.measured.largest_hole_days << "d";
        } else {
            note << ", no hole";
        }
        if (!std::isnan(decision.measured.flank_coverage)) {
            note << ", flank " << decision.measured.flank_coverage * 100.0 << "%";
        }
        if (decision.measured.flagged) {
            note << " (seam " << std::setprecision(0) << decision.measured.seam_return * 100.0
                 << "% -- flagged)";
        }

        out << decision.ticker << "," << (keep ? "keep" : "split") << ","
            << date_of(decision.gap_end) << "," << note.str() << "\n";
        ++written;
    }

    if (written == 0) {
        out << "# No gap was settled by measurement. Every case is governed by the threshold.\n";
    }
    return written;
}

// ---------------------------------------------------------------------------
// The report
// ---------------------------------------------------------------------------

namespace {

[[nodiscard]] std::string percent(double fraction) {
    if (std::isnan(fraction)) {
        return "--";
    }
    std::ostringstream out;
    out << std::fixed << std::setprecision(1) << fraction * 100.0 << "%";
    return out.str();
}

[[nodiscard]] std::string signed_percent(double fraction) {
    if (std::isnan(fraction)) {
        return "--";
    }
    std::ostringstream out;
    out << std::showpos << std::fixed << std::setprecision(0) << fraction * 100.0 << "%";
    return out.str();
}

/// Disagreements, flags, and everything the data could not settle.
void write_measurement_sections(std::ofstream& out,
                                const MembershipHistory& history,
                                const IdentityProbeConfig& config) {
    std::size_t resolved = 0;
    std::size_t disagreements = 0;
    std::size_t flagged = 0;
    for (const IdentityDecision& decision : history.decisions) {
        resolved += decision.measured.resolved() ? 1 : 0;
        disagreements += decision.disagrees() ? 1 : 0;
        flagged += decision.measured.flagged ? 1 : 0;
    }

    out << "\n---\n\n";
    out << "# Measurement\n\n";
    out << "The threshold above is an inference. A company that merely left the index kept\n"
           "trading, so its price series spans the gap; a ticker that was surrendered and\n"
           "reassigned has a hole in it. That is measurable, and this section is the\n"
           "measurement.\n\n";

    out << "| | |\n|---|---|\n";
    out << "| Continuous at coverage | >= " << percent(config.continuous_coverage) << ", no hole > "
        << config.max_hole_days << " weekdays |\n";
    out << "| Discontinuous at coverage | <= " << percent(config.discontinuous_coverage)
        << ", with one hole >= " << percent(config.discontinuous_hole) << " of the gap |\n";
    out << "| Continuous also requires flank coverage | >= " << percent(config.flank_coverage)
        << " |\n";
    out << "| Seam flagged beyond | " << percent(config.seam_review) << " |\n";
    out << "| Gaps measured | " << history.decisions.size() << " |\n";
    out << "| Settled by data | " << resolved << " |\n";
    out << "| Disagreements with the threshold | " << disagreements << " |\n";
    out << "| Flagged for review | " << flagged << " |\n\n";

    out << "These thresholds are judgment calls in the same way the 730 days is, and they are\n"
           "stated here for the same reason: a number nobody can see is a number nobody can\n"
           "argue with.\n\n";

    out << "**The flank check is the one that matters.** A vendor serves one series per ticker,\n"
           "the current issuer's. Ask for `CCI` and you get Crown Castle from 1998, not\n"
           "Citicorp from before it -- so a naive coverage test reads a clean series straight\n"
           "through the gap and concludes \"continuous\", producing a confident and wrong `keep`\n"
           "on exactly the cases where a wrong keep welds two companies together. A continuous\n"
           "verdict therefore also requires the series to cover the membership period *before*\n"
           "the gap. Where it does not, the answer is unresolved.\n\n";

    // --- Disagreements -------------------------------------------------
    out << "## Disagreements\n\n";
    out << "Where the data and the threshold reached different answers. This is the headline of\n"
           "the document.\n\n";
    if (disagreements == 0) {
        out << "_None._ ";
        if (resolved == 0) {
            out << "Nothing was settled by data, so there was nothing to disagree with.\n\n";
        } else {
            out << "Every gap the data settled agreed with the threshold.\n\n";
        }
    } else {
        out << "| Ticker | Threshold | Measured | Coverage | Largest hole | Flank | Seam |\n";
        out << "|---|---|---|---|---|---|---|\n";
        for (const IdentityDecision& decision : history.decisions) {
            if (!decision.disagrees()) {
                continue;
            }
            out << "| `" << decision.ticker << "` | " << (decision.split ? "split" : "keep")
                << " | **" << to_string(decision.measured.verdict) << "** | "
                << percent(decision.measured.coverage) << " | "
                << decision.measured.largest_hole_days << "d | "
                << percent(decision.measured.flank_coverage) << " | "
                << signed_percent(decision.measured.seam_return) << " |\n";
        }
        out << "\n";
    }

    // --- Flagged --------------------------------------------------------
    out << "## Flagged for review\n\n";
    out << "A large return across the hole, whatever the verdict. A seam of this size is at\n"
           "least as likely to be a reverse split, a spin-off adjustment or unadjusted vendor\n"
           "data as a different company, so it flags and never decides.\n\n";
    if (flagged == 0) {
        out << "_None._\n\n";
    } else {
        out << "| Ticker | Verdict | Seam | Largest hole | Coverage |\n";
        out << "|---|---|---|---|---|\n";
        for (const IdentityDecision& decision : history.decisions) {
            if (!decision.measured.flagged) {
                continue;
            }
            out << "| `" << decision.ticker << "` | " << to_string(decision.measured.verdict)
                << " | " << signed_percent(decision.measured.seam_return) << " | "
                << decision.measured.largest_hole_days << "d | "
                << percent(decision.measured.coverage) << " |\n";
        }
        out << "\n";
    }

    // --- Unresolved -----------------------------------------------------
    out << "## Unresolved\n\n";
    out << "**The threshold governs these and they remain open.** Absence of data is never\n"
           "evidence of a split: a vendor that does not carry a long-dead ticker is a coverage\n"
           "limit, not a delisting.\n\n";
    out << "| Ticker | Threshold said | Why the data could not say |\n";
    out << "|---|---|---|\n";
    std::size_t unresolved = 0;
    for (const IdentityDecision& decision : history.decisions) {
        if (decision.measured.resolved()) {
            continue;
        }
        ++unresolved;
        out << "| `" << decision.ticker << "` | " << (decision.split ? "split" : "keep") << " | "
            << (decision.measured.reason.empty() ? "not measured" : decision.measured.reason)
            << " |\n";
    }
    if (unresolved == 0) {
        out << "| _none_ | | |\n";
    }
    out << "\n";

    // --- Policy ---------------------------------------------------------
    out << "## Decisions that are not measurements\n\n";
    out << "Two things here are policy rather than data, settled deliberately so they are not\n"
           "re-litigated in two years.\n\n";
    out << "**Companies that went private and re-listed are split.** `DELL`, `HCA`, `HLT`,\n"
           "`KMI` and `DG` each left the market entirely and came back. During the private\n"
           "interval there is no tradeable price at all, so any signal computed across the gap\n"
           "reads a discontinuity as a return; and each re-listed with a different capital\n"
           "structure and share count. Split is both correct and the safe direction, and the\n"
           "threshold already produces it -- no override is needed, only this note.\n\n";
    out << "**`TT` is split, and the continuity that matters was never in `TT`.** Trane Inc was\n"
           "acquired in 2008; the 2020 `TT` is Ingersoll-Rand renaming itself Trane\n"
           "Technologies. Splitting the ticker is right. The company's continuous history lives\n"
           "in `IR`, whose own 504-day gap is below the threshold and was correctly kept as one\n"
           "security -- so that continuity is preserved. What is *not* captured is the rename\n"
           "linking `IR` to the later `TT`: renames cannot be inferred from ticker-keyed\n"
           "membership data, and this one needs `SecurityMaster::add_ticker` and a source that\n"
           "knows about corporate actions.\n\n";
}

}  // namespace

void write_decision_report(const std::filesystem::path& path,
                           const MembershipHistory& history,
                           const SecurityMaster& master,
                           const IdentityRules& rules,
                           std::string_view source_description,
                           const IdentityProbeConfig& probe_config,
                           bool measured) {
    if (std::filesystem::path parent = path.parent_path(); !parent.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(parent, ec);
    }

    std::ofstream out(path, std::ios::trunc);
    if (!out) {
        throw std::runtime_error("cannot write '" + path.string() + "'");
    }

    std::size_t splits = 0;
    std::size_t near_misses = 0;
    for (const IdentityDecision& decision : history.decisions) {
        splits += decision.split ? 1 : 0;
        near_misses += decision.rule == "near miss" ? 1 : 0;
    }

    out << "# Identity decisions\n\n";
    out << "Generated by MarketScan from `" << source_description << "`. "
        << "Do not edit: regenerate with `marketscan --build-universe`.\n\n";

    out << "A ticker is not an identity. A ticker dropped from the index and "
           "reassigned years\nlater to an unrelated company is **two securities sharing one "
           "string**, and treating\nit as one welds the first company's price history to the "
           "second's -- manufacturing a\nreturn that never existed, usually an enormous one.\n\n";

    out << "The source here is ticker-keyed and cannot express identity, so it has to be "
           "inferred.\nThis file is every inference, including the ones that went the other "
           "way.\n\n";

    // The banner, stated before any table rather than implied by the word
    // "inference" somewhere below it.
    //
    // A reader who skims this file needs to leave knowing one thing: not a single
    // row here has been checked against a price series. `measured` counts the
    // decisions where the identity probe actually ran, and on every report this
    // project has produced it is zero -- so the sentence is generated from the
    // count rather than asserted, and it will start reading differently the day
    // the count changes.
    std::size_t measured_gaps = 0;
    for (const IdentityDecision& decision : history.decisions) {
        measured_gaps += decision.measured.resolved() ? std::size_t{1} : std::size_t{0};
    }

    out << "> ## Not one of these has been validated\n>\n";
    if (measured_gaps == 0) {
        out << "> **Every row below is inferred from the " << days_of(rules.split_gap)
            << "-day threshold, and none has\n> been checked against price data.** "
            << measured_gaps << " of " << history.decisions.size()
            << " gaps carry a measurement.\n>\n";
        out << "> That is not a gap in the build. The identity probe is written, unit-tested "
               "and\n> waiting; what it needs is the price history of securities that may have "
               "stopped\n> trading decades ago, and no source this project is permitted to use "
               "carries it.\n> See [`DATA_POLICY.md`](DATA_POLICY.md) for why, and "
               "[`STATUS.md`](STATUS.md) for the\n> reclassification of this work as "
               "CRSP-dependent.\n>\n";
        out << "> Over-splitting is the safe direction, so the failure mode here is lost "
               "history\n> rather than a fabricated price series. That is a real defence and "
               "it is not the\n> same thing as being right.\n\n";
    } else {
        out << "> " << measured_gaps << " of " << history.decisions.size()
            << " gaps carry a measurement against price data; the rest are\n> inferred from the "
            << days_of(rules.split_gap)
            << "-day threshold alone. A row's provenance is in its own\n> line below.\n\n";
    }

    out << "## The rule\n\n";
    out << "| | |\n|---|---|\n";
    out << "| Split threshold | " << days_of(rules.split_gap) << " days |\n";
    out << "| Near-miss window | " << days_of(rules.near_miss_window) << " days |\n";
    out << "| Overrides loaded | " << rules.overrides.size() << " |\n";
    out << "| Securities | " << master.size() << " |\n";
    out << "| Gaps considered | " << history.decisions.size() << " |\n";
    out << "| Splits made | " << splits << " |\n";
    out << "| Near misses | " << near_misses << " |\n\n";

    out << "The threshold is a judgment call and not a derived constant, which is why this "
           "report\nexists. **Over-splitting is the safe direction**: it costs history -- a "
           "strategy cannot\ncompute twelve-month momentum across a break -- and that is a "
           "visible, bounded loss.\nUnder-splitting fabricates a price series, which is invisible "
           "and poisons everything\ndownstream. When the rule is ambiguous, it splits.\n\n";

    out << "Corrections go in `data/security_overrides.csv`, which is consulted before the\n"
           "threshold.\n\n";

    if (history.decisions.empty()) {
        out << "## Decisions\n\nNo ticker in this source had a gap in its membership. "
               "Nothing to decide.\n";
    } else {
        out << "## Splits\n\n";
        out << "| Ticker | Gap from | Gap to | Days | Rule | Security |\n";
        out << "|---|---|---|---|---|---|\n";
        for (const IdentityDecision& decision : history.decisions) {
            if (!decision.split) {
                continue;
            }
            out << "| `" << decision.ticker << "` | " << date_of(decision.gap_start) << " | "
                << date_of(decision.gap_end) << " | " << days_of(decision.gap) << " | "
                << decision.rule << " | " << decision.result << " |\n";
        }
        if (splits == 0) {
            out << "| _none_ | | | | | |\n";
        }

        out << "\n## Gaps kept as one security\n\n";
        out << "Near misses first: these are the calls that nearly went the other way.\n\n";
        out << "| Ticker | Gap from | Gap to | Days | Rule |\n";
        out << "|---|---|---|---|---|\n";
        for (const bool want_near_miss : {true, false}) {
            for (const IdentityDecision& decision : history.decisions) {
                if (decision.split || (decision.rule == "near miss") != want_near_miss) {
                    continue;
                }
                out << "| `" << decision.ticker << "` | " << date_of(decision.gap_start) << " | "
                    << date_of(decision.gap_end) << " | " << days_of(decision.gap) << " | "
                    << decision.rule << " |\n";
            }
        }
    }

    if (!history.rejected.empty()) {
        out << "\n## Rows the source could not supply\n\n";
        for (const std::string& rejected : history.rejected) {
            out << "- " << rejected << "\n";
        }
    }

    if (measured) {
        write_measurement_sections(out, history, probe_config);
    }

    out << "\n---\n\n";
    out << "## When CRSP arrives\n\n";
    out << "PERMNO is a permanent security identifier that survives renames and is never "
           "reused --\nexactly what this module infers. At that point `SecurityId` becomes "
           "PERMNO directly,\nand this entire heuristic is **deleted rather than adapted**. "
           "The loader interface is\nshaped so that deletion is the whole of the change, and "
           "this report stops being\ngenerated because there is nothing left to decide.\n";
}

}  // namespace marketscan
