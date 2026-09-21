// The Universe Inspector -- Session C Part 4, Session D Part 5.
//
// The survivorship guard is structural: when a universe is installed, a strategy
// is never shown a bar for a security that was not a member that day, and the
// host context gives it no API by which to ask about one. That is enforced in
// dispatch and asserted in tests, which makes it correct and invisible.
//
// This panel is where it becomes visible. Someone looking at it should be able to
// watch a company leave the universe and confirm the strategy stopped seeing it
// -- the membership history, who is in it right now, every add and drop, the
// forced exits that actually happened, and for each identity decision the
// heuristic made, *which rule made it*.
//
// EVERY NUMBER HERE IS READ, NOT COMPUTED
//
// The same rule SPEC_VIEW_MODES.md sets for the dual view, and for the same
// reason. `Session::universe_snapshot_locked` assembles this once per publish;
// nothing below recomputes a membership, a count or a date. A panel that derived
// its own member list would eventually disagree with the guard it is supposed to
// be showing, and it would disagree in a screenshot before it disagreed in a bug
// report.

#include <algorithm>
#include <string>

#include <imgui.h>

#include "../theme.hpp"
#include "panels.hpp"

namespace marketscan::app::panels {
namespace {

/// A label and a value, on one line, in the house style.
void stat(const char* label, const std::string& value, unsigned int color = 0) {
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(theme::colors().text_dim), "%s", label);
    ImGui::SameLine();
    right_text(value, color);
}

/// A date, or a word for the two sentinels.
///
/// `forever` is a parameter because the same sentinel reads differently in
/// different columns: a membership period that never ends means the name is
/// "still a member", and a history whose last date is open means it runs
/// "onward". Sharing one spelling produced "covers 2000-01-03 to still a
/// member", which is not a sentence.
[[nodiscard]] std::string date_only(Timestamp ts, const char* forever = "still a member") {
    if (ts == kNever) {
        return "-";
    }
    if (ts == kForever) {
        return forever;
    }
    const std::string full = format_timestamp(ts);
    return full.size() >= 10 ? full.substr(0, 10) : full;
}

/// The provenance of one identity decision, as a phrase rather than a code.
///
/// "threshold" and "below threshold" are the two halves of the same rule and a
/// reader should not have to know that; what they need is which way it went and
/// what decided it.
[[nodiscard]] std::string provenance(const IdentityDecision& decision) {
    if (decision.rule == "override") {
        return decision.split ? "override: split" : "override: kept";
    }
    if (decision.rule == "near miss") {
        return decision.split ? "threshold (near miss): split" : "threshold (near miss): kept";
    }
    if (decision.rule == "threshold") {
        return "threshold: split";
    }
    if (decision.rule == "below threshold") {
        return "threshold: kept";
    }
    return decision.rule;
}

[[nodiscard]] std::string measurement(const IdentityMeasurement& measured) {
    if (!measured.resolved()) {
        return measured.reason.empty() ? "not measured" : measured.reason;
    }
    switch (measured.verdict) {
        case IdentityVerdict::Continuous:
            return "measured: continuous";
        case IdentityVerdict::Discontinuous:
            return "measured: discontinuous";
        case IdentityVerdict::Unresolved:
            break;
    }
    return "measured: unresolved";
}

void draw_members(const UniverseSnapshot& universe) {
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(theme::colors().text_dim),
                       "%zu members as of the session clock",
                       universe.members.size());
    ImGui::SameLine();
    help_marker("Membership as of the instant the engine has reached, not today. "
                "Scrub the transport and this list changes with it.");

    constexpr ImGuiTableFlags kFlags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                       ImGuiTableFlags_ScrollY | ImGuiTableFlags_Sortable |
                                       ImGuiTableFlags_SizingStretchProp;
    if (!ImGui::BeginTable("members", 4, kFlags, ImVec2(0.0F, 220.0F))) {
        return;
    }
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Ticker");
    ImGui::TableSetupColumn("Member since");
    ImGui::TableSetupColumn("Leaves");
    ImGui::TableSetupColumn("Held");
    ImGui::TableHeadersRow();

    for (const UniverseMember& member : universe.members) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        mono_text(member.ticker);
        ImGui::TableNextColumn();
        mono_text(date_only(member.since));
        ImGui::TableNextColumn();
        mono_text(date_only(member.until));
        ImGui::TableNextColumn();
        // A glyph and a word, not a colour alone. SPEC_VIEW_MODES section 4.
        if (member.held) {
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(theme::colors().accent), "* held");
        } else {
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(theme::colors().text_faint), "-");
        }
    }
    ImGui::EndTable();
}

void draw_events(const UniverseSnapshot& universe) {
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(theme::colors().text_dim),
                       "%zu membership changes over the whole history",
                       universe.events.size());

    constexpr ImGuiTableFlags kFlags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                       ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp;
    if (!ImGui::BeginTable("membership_events", 3, kFlags, ImVec2(0.0F, 200.0F))) {
        return;
    }
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Date");
    ImGui::TableSetupColumn("Ticker");
    ImGui::TableSetupColumn("Change");
    ImGui::TableHeadersRow();

    // Clipped rather than truncated: a real history is tens of thousands of
    // rows and scrolling back to the start of it is the point of the panel.
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(universe.events.size()));
    while (clipper.Step()) {
        for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
            const MembershipEvent& event = universe.events[static_cast<std::size_t>(row)];
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            mono_text(date_only(event.ts));
            ImGui::TableNextColumn();
            mono_text(event.ticker);
            ImGui::TableNextColumn();
            // Sign and word, colour as reinforcement only.
            const unsigned int color = event.added ? theme::colors().profit : theme::colors().loss;
            ImGui::TextColored(
                ImGui::ColorConvertU32ToFloat4(color), "%s", event.added ? "+ added" : "- dropped");
        }
    }
    ImGui::EndTable();
}

void draw_departures(const UniverseSnapshot& universe) {
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(theme::colors().text_dim),
                       "%zu forced exits in this run",
                       universe.departures.size());
    ImGui::SameLine();
    help_marker("A position held in a name that left the index, liquidated by the engine "
                "rather than by the strategy. This is the guard doing something a "
                "strategy never asked for -- which is why it is stamped with its own "
                "client order id and never mistaken for a decision.\n\n"
                "The price is the last one the book had. See 'the assumed exit' in "
                "docs/EXECUTION.md for what that assumption is worth.");

    if (universe.departures.empty()) {
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(theme::colors().text_faint),
                           "Nothing has been held into a departure yet.");
        return;
    }

    constexpr ImGuiTableFlags kFlags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                       ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp;
    if (!ImGui::BeginTable("departures", 5, kFlags, ImVec2(0.0F, 160.0F))) {
        return;
    }
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Date");
    ImGui::TableSetupColumn("Ticker");
    ImGui::TableSetupColumn("Theory");
    ImGui::TableSetupColumn("Position");
    ImGui::TableSetupColumn("Proceeds");
    ImGui::TableHeadersRow();

    for (const DepartureRecord& record : universe.departures) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        mono_text(date_only(record.ts));
        ImGui::TableNextColumn();
        mono_text(record.ticker);
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(record.strategy.c_str());
        ImGui::TableNextColumn();
        right_text(format_quantity(record.quantity) +
                   (record.quantity < 0.0 ? " (short)" : " (long)"));
        ImGui::TableNextColumn();
        money_cell(record.cash_delta);
    }
    ImGui::EndTable();
}

void draw_identity(const UniverseSnapshot& universe) {
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(theme::colors().text_dim),
                       "%zu identity decisions: %zu split, %zu kept",
                       universe.decisions.size(),
                       universe.split_count,
                       universe.kept_count);
    ImGui::SameLine();
    help_marker("A ticker-keyed membership history cannot express identity: a gap in a "
                "ticker's membership is either the same company leaving and returning, "
                "or a second company that was given the ticker after the first one died. "
                "Getting it wrong in one direction costs history; getting it wrong in "
                "the other fabricates a price series.\n\n"
                "Every decision the heuristic made is here with the rule that made it, "
                "so a call that nearly went the other way is visible rather than magic.");

    if (universe.decisions.empty()) {
        ImGui::TextColored(
            ImGui::ColorConvertU32ToFloat4(theme::colors().text_faint),
            "No ticker in this history has a gap in its membership, so no identity "
            "decision was needed. An empty table here is the heuristic having nothing "
            "to do, not the heuristic failing to run.");
        return;
    }

    if (universe.disagreement_count > 0) {
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(theme::colors().warning),
                           "! %zu measurement(s) contradict the threshold",
                           universe.disagreement_count);
        ImGui::SameLine();
        help_marker("The price series says one thing and the gap length says another. "
                    "Neither is automatically right; the row is flagged so a human "
                    "decides, and the decision goes in data/security_overrides.csv.");
    }

    constexpr ImGuiTableFlags kFlags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                       ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp;
    if (!ImGui::BeginTable("identity", 5, kFlags, ImVec2(0.0F, 220.0F))) {
        return;
    }
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Ticker");
    ImGui::TableSetupColumn("Gap");
    ImGui::TableSetupColumn("Resumed");
    ImGui::TableSetupColumn("Decided by");
    ImGui::TableSetupColumn("Price series said");
    ImGui::TableHeadersRow();

    for (const IdentityDecision& decision : universe.decisions) {
        ImGui::TableNextRow();
        if (decision.disagrees()) {
            ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0,
                                   (theme::colors().warning & 0x00FFFFFFU) | 0x22000000U);
        }
        ImGui::TableNextColumn();
        mono_text(decision.ticker);
        ImGui::TableNextColumn();
        right_text(format_duration(decision.gap));
        ImGui::TableNextColumn();
        mono_text(date_only(decision.gap_end));
        ImGui::TableNextColumn();
        // The glyph carries the outcome; the colour only reinforces it.
        const unsigned int color = decision.split ? theme::colors().warning : theme::colors().text;
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(color),
                           "%s%s",
                           decision.split ? "/ " : "= ",
                           provenance(decision).c_str());
        if (!decision.note.empty() && ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", decision.note.c_str());
        }
        ImGui::TableNextColumn();
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(decision.measured.resolved()
                                                              ? theme::colors().text_dim
                                                              : theme::colors().text_faint),
                           "%s",
                           measurement(decision.measured).c_str());
    }
    ImGui::EndTable();
}

}  // namespace

void universe_inspector(Context& context) {
    if (!context.ui.show_universe) {
        return;
    }
    if (!ImGui::Begin("Universe", &context.ui.show_universe)) {
        ImGui::End();
        return;
    }

    const UniverseSnapshot& universe = context.snapshot.universe;

    if (!universe.active) {
        // Not an error, and the panel says which of the two it is rather than
        // going blank. A run without a universe is the only sensible thing for
        // one instrument; it is survivorship-unsafe for a cross-section, and the
        // difference has to be visible rather than remembered.
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(theme::colors().warning),
                           "No universe installed.");
        ImGui::TextWrapped(
            "Every symbol in the data is in scope, so there is no survivorship guard on "
            "this run. That is correct for a single instrument and for intraday work. For "
            "a cross-sectional study it means the result is survivorship-inflated by an "
            "unknown amount: nobody knew in 1999 which names would still be listed today.");
        ImGui::Spacing();
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(theme::colors().text_dim),
                           "Pass a membership history to the session to install one.");
        ImGui::End();
        return;
    }

    // --- The header: what is loaded, and what the guard did --------------
    stat("Source", universe.source);
    stat("Securities", format_count(static_cast<std::uint64_t>(universe.securities)));
    stat("Membership periods", format_count(static_cast<std::uint64_t>(universe.periods)));
    if (universe.snapshots > 0) {
        stat("Snapshot dates", format_count(static_cast<std::uint64_t>(universe.snapshots)));
    }
    const std::string span = universe.last == kForever
                                 ? date_only(universe.first) + " onward"
                                 : date_only(universe.first) + " to " + date_only(universe.last);
    stat("Covers", span);

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // The guard's effect as a number rather than a claim. Zero withheld events
    // on a universe with churn in it means the filter is not running.
    stat("Events withheld",
         format_count(universe.events_withheld),
         universe.events_withheld == 0 ? theme::colors().warning : theme::colors().text);
    ImGui::SameLine();
    help_marker("Bars a strategy was not shown because the security was not a member "
                "that day. This is the survivorship guard working, counted so it is a "
                "number and not a claim: a run where this stays at zero, on a universe "
                "that churns, is a run that was never filtered.");
    stat("Forced exits", format_count(universe.forced_exits));

    if (!universe.rejected.empty()) {
        ImGui::Spacing();
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(theme::colors().warning),
                           "! %zu row(s) of the source could not be used",
                           universe.rejected.size());
        if (ImGui::TreeNode("rejected_rows", "Show them")) {
            for (const std::string& reason : universe.rejected) {
                ImGui::BulletText("%s", reason.c_str());
            }
            ImGui::TreePop();
        }
    }

    ImGui::Spacing();

    if (ImGui::BeginTabBar("universe_tabs")) {
        if (ImGui::BeginTabItem("Members")) {
            draw_members(universe);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Adds and drops")) {
            draw_events(universe);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Forced exits")) {
            draw_departures(universe);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Identity")) {
            draw_identity(universe);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    ImGui::End();
}

}  // namespace marketscan::app::panels
