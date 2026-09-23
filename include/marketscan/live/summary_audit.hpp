#ifndef MARKETSCAN_LIVE_SUMMARY_AUDIT_HPP
#define MARKETSCAN_LIVE_SUMMARY_AUDIT_HPP

/*
 * Summaries that assert their own arithmetic.
 *
 * WHY
 *
 * `ms_live` once printed "128 submitted, 124 accepted, 0 rejected" while four
 * orders had plainly been refused. Every refusal was in the log with its reason;
 * nothing was lost. The summary simply said something untrue, and the only thing
 * that caught it was a person reading the line and doing the subtraction.
 *
 * That is the vacuous-pass law's corollary from docs/AUDIT.md: attention is an
 * excellent detector and a worthless control. It does not run on every commit,
 * and it does not run at all once the reader trusts the line. So the arithmetic a
 * person would have to do is done by the program, and a summary that contradicts
 * itself fails the run instead of being printed.
 *
 * HARD FAILURE, NOT A WARNING
 *
 * A guard that logs and continues is a guard that will be scrolled past.
 * `exit_code()` is `kInconsistentSummary` when any identity fails.
 *
 * AND A PASS SAYS HOW MUCH IT CHECKED
 *
 * `report()` names the number of identities that held. An audit that checked
 * nothing would otherwise print exactly what an audit that checked everything
 * prints, which is the shape this file exists to close -- so an audit with no
 * identities in it is itself a failure.
 *
 * WHY IT LIVES IN THE LIBRARY RATHER THAN BESIDE THE SUBCOMMANDS
 *
 * Because it needs to be tested, and `apps/` is not in the test build. It
 * returns the text rather than printing it for the same reason: a guard whose
 * only output is stdout can only be tested by capturing stdout.
 */

#include <cstdint>
#include <string>
#include <vector>

#include "marketscan/live/order_tracker.hpp"

namespace marketscan::live {

/// The summary contradicted itself. Distinct from 1 (diverged), 2 (could not
/// run) and 3 (vacuous), because "the numbers do not add up" is a different
/// finding from all three and wants its own exit code.
inline constexpr int kInconsistentSummary = 4;

/// Accumulates the identities a printed summary must satisfy.
class SummaryAudit {
public:
    /// `left == right`, or a recorded failure.
    void check(const std::string& name, std::uint64_t left, std::uint64_t right) {
        record({name, left, right, false});
    }

    /// `left <= right`. Used where the relation genuinely is a bound rather than
    /// an equation, and said so rather than rounded up into one.
    void check_at_most(const std::string& name, std::uint64_t left, std::uint64_t right) {
        record({name, left, right, true});
    }

    /// Every identity an OrderTracker checked, prefixed with whose it was.
    void absorb(const std::string& whose, const StatsReconciliation& reconciliation) {
        for (const StatsIdentity& identity : reconciliation.identities) {
            record({whose + ": " + identity.name, identity.left, identity.right, identity.at_most});
        }
    }

    [[nodiscard]] bool ok() const noexcept { return failures_.empty() && checked_ > 0; }

    [[nodiscard]] std::size_t checked() const noexcept { return checked_; }

    [[nodiscard]] std::size_t failed() const noexcept { return failures_.size(); }

    /// 0 when every identity held and at least one was checked.
    [[nodiscard]] int exit_code() const noexcept { return ok() ? 0 : kInconsistentSummary; }

    /// What to print. One line on success, naming the count; the failing
    /// identities with both sides otherwise.
    [[nodiscard]] std::string report() const {
        if (checked_ == 0) {
            return "\nSUMMARY AUDIT: nothing was checked, so the numbers above are "
                   "unverified.\n";
        }
        if (failures_.empty()) {
            return "summary audit   " + std::to_string(checked_) + " identities hold\n";
        }

        std::string out = "\nINCONSISTENT SUMMARY: " + std::to_string(failures_.size()) + " of " +
                          std::to_string(checked_) + " identities fail.\n";
        for (const Failure& failure : failures_) {
            out += "  " + failure.name + "  ->  " + std::to_string(failure.left) +
                   (failure.at_most ? " <= " : " != ") + std::to_string(failure.right) + "\n";
        }
        out += "\nThe counts above describe the same run and disagree about it, so at least\n"
               "one of them is wrong. A summary that states something untrue is its own bug\n"
               "even when nothing else is lost -- see docs/AUDIT.md.\n";
        return out;
    }

private:
    struct Failure {
        std::string name;
        std::uint64_t left = 0;
        std::uint64_t right = 0;
        bool at_most = false;
    };

    void record(Failure candidate) {
        ++checked_;
        const bool holds = candidate.at_most ? candidate.left <= candidate.right
                                             : candidate.left == candidate.right;
        if (!holds) {
            failures_.push_back(std::move(candidate));
        }
    }

    std::vector<Failure> failures_;
    std::size_t checked_ = 0;
};

}  // namespace marketscan::live

#endif  // MARKETSCAN_LIVE_SUMMARY_AUDIT_HPP
