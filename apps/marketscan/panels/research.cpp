// Research -- a question about a trading rule, answered by a graded job.
//
// SPEC_RESEARCH_VIEW.md section 4. Three states in one window: a rail on the
// left for asking and for recent runs, and on the right either the form, the
// job's progress, or its Results.
//
// NOTHING HERE COMPUTES A NUMBER
//
// The Results are drawn from `ResearchView`, which is the Results document
// read field by field and nothing else, and the chart from the job's own
// curves through the one EquityChart. The order on screen is fixed -- verdict,
// then the gates behind it, then the stats, then analysis -- because analysis,
// however careful, can mislead, so it comes last and answers to everything
// above it.
//
// THE FORM IS DRAWN FROM THE SCHEMA
//
// One widget per kind of schema node, and none written for a particular field.
// What it decides about the schema is in schema_form.hpp, where a test can
// reach it.

#include <algorithm>
#include <array>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <imgui.h>
#include <implot.h>

#include "../chat.hpp"
#include "../markdown.hpp"
#include "../plain_words.hpp"
#include "../research_view.hpp"
#include "../schema_form.hpp"
#include "../theme.hpp"
#include "equity_chart.hpp"
#include "panels.hpp"

namespace marketscan::app::panels {
namespace {

namespace sf = schema_form;
using nlohmann::json;

constexpr float kLabelColumn = 210.0F;

[[nodiscard]] ImVec4 to_vec(unsigned int color) {
    return ImGui::ColorConvertU32ToFloat4(color);
}

void ink(unsigned int color, const std::string& text) {
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::TextUnformatted(text.c_str());
    ImGui::PopStyleColor();
}

void wrapped(unsigned int color, const std::string& text) {
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::TextWrapped("%s", text.c_str());
    ImGui::PopStyleColor();
}

void heading(const char* text) {
    ImGui::Spacing();
    ink(theme::colors().text_faint, text);
    ImGui::Separator();
}

[[nodiscard]] unsigned int grade_color(const std::string& grade) {
    const Palette& p = theme::colors();
    if (grade == "supported") {
        return p.status_good;
    }
    if (grade == "fragile") {
        return p.status_warning;
    }
    if (grade == "not_supported") {
        return p.status_serious;
    }
    // "Not enough data" is not bad news about the idea, so it wears ink, not a
    // status colour. SPEC_RESEARCH_VIEW.md 4.3.
    return p.text_dim;
}

[[nodiscard]] const char* status_mark(const std::string& status) {
    if (status == "pass") {
        return "PASS";
    }
    if (status == "fail") {
        return "FAIL";
    }
    return "--  ";
}

// --- The form -------------------------------------------------------------------

struct FormContext {
    const json& root;
    const json& provenance;
    const std::vector<research::SchemaError>& errors;
    bool changed = false;
};

bool input_text(const char* id, std::string& value, float width) {
    char buffer[512];
    std::strncpy(buffer, value.c_str(), sizeof(buffer) - 1);
    buffer[sizeof(buffer) - 1] = '\0';
    ImGui::SetNextItemWidth(width);
    if (ImGui::InputText(id, buffer, sizeof(buffer))) {
        value = buffer;
        return true;
    }
    return false;
}

/// The provenance word beside a field, never colour alone: stated, inferred,
/// default, MISSING. The quote or note is in the tooltip.
void provenance_badge(const FormContext& fc, const std::string& path) {
    if (!fc.provenance.is_object() || !fc.provenance.contains(path)) {
        return;
    }
    const json& entry = fc.provenance.at(path);
    const std::string source = entry.value("source", "");
    const Palette& p = theme::colors();
    const unsigned int color = source == "missing"    ? p.status_serious
                               : source == "inferred" ? p.accent
                                                      : p.text_faint;
    ImGui::SameLine();
    ink(color, source == "missing" ? "MISSING" : source);
    if (ImGui::IsItemHovered()) {
        std::string tip;
        if (entry.contains("quote")) {
            tip += "\"" + entry.at("quote").get<std::string>() + "\"";
        }
        if (entry.contains("note")) {
            tip += (tip.empty() ? "" : "\n") + entry.at("note").get<std::string>();
        }
        if (!tip.empty()) {
            ImGui::SetTooltip("%s", tip.c_str());
        }
    }
}

void field_errors(const FormContext& fc, const std::string& path) {
    for (const research::SchemaError& e : fc.errors) {
        if (e.instance_path == path) {
            ImGui::Indent(kLabelColumn);
            wrapped(theme::colors().status_serious, "! " + e.message);
            ImGui::Unindent(kLabelColumn);
        }
    }
}

void edit_object(FormContext& fc, const json& node, json& value, const std::string& path);

void edit_value(FormContext& fc,
                const json& raw,
                json& value,
                const std::string& path,
                const std::string& label) {
    const json& node = sf::resolve(fc.root, raw);
    ImGui::PushID(path.c_str());
    if (!label.empty()) {
        ImGui::AlignTextToFramePadding();
        ink(theme::colors().text_dim, label);
        ImGui::SameLine(kLabelColumn);
    }

    if (node.contains("oneOf")) {
        const json& branches = node.at("oneOf");
        const std::size_t current = sf::branch_of(fc.root, branches, value);
        const std::string preview = sf::branch_label(fc.root, branches[current]);
        ImGui::SetNextItemWidth(170.0F);
        if (ImGui::BeginCombo("##kind", preview.c_str())) {
            for (std::size_t i = 0; i < branches.size(); ++i) {
                const std::string name = sf::branch_label(fc.root, branches[i]);
                if (ImGui::Selectable(name.c_str(), i == current) && i != current) {
                    value = sf::default_for(fc.root, branches[i]);
                    fc.changed = true;
                }
            }
            ImGui::EndCombo();
        }
        const json& branch =
            sf::resolve(fc.root, branches[sf::branch_of(fc.root, branches, value)]);
        provenance_badge(fc, path);
        if (value.is_object()) {
            ImGui::Indent(24.0F);
            edit_object(fc, branch, value, path);
            ImGui::Unindent(24.0F);
        } else if (value.is_number()) {
            double number = value.get<double>();
            ImGui::SameLine();
            ImGui::SetNextItemWidth(120.0F);
            if (ImGui::InputDouble("##number", &number, 0.0, 0.0, "%.4g")) {
                value = number;
                fc.changed = true;
            }
        } else if (value.is_string()) {
            std::string text = value.get<std::string>();
            ImGui::SameLine();
            if (input_text("##text", text, 140.0F)) {
                value = text;
                fc.changed = true;
            }
        }
        field_errors(fc, path);
        ImGui::PopID();
        return;
    }

    const std::string type = node.contains("type") && node.at("type").is_string()
                                 ? node.at("type").get<std::string>()
                                 : std::string();
    if (node.contains("const")) {
        ink(theme::colors().text_faint, node.at("const").dump());
    } else if (node.contains("enum")) {
        const std::string preview = value.is_string() ? value.get<std::string>() : value.dump();
        ImGui::SetNextItemWidth(220.0F);
        if (ImGui::BeginCombo("##enum", preview.c_str())) {
            for (const json& option : node.at("enum")) {
                const std::string name =
                    option.is_string() ? option.get<std::string>() : option.dump();
                if (ImGui::Selectable(name.c_str(), option == value) && option != value) {
                    value = option;
                    fc.changed = true;
                }
            }
            ImGui::EndCombo();
        }
    } else if (type == "boolean") {
        bool flag = value.is_boolean() && value.get<bool>();
        if (ImGui::Checkbox("##flag", &flag)) {
            value = flag;
            fc.changed = true;
        }
    } else if (type == "integer") {
        int number = value.is_number() ? value.get<int>() : 0;
        ImGui::SetNextItemWidth(120.0F);
        if (ImGui::InputInt("##int", &number)) {
            value = number;
            fc.changed = true;
        }
    } else if (type == "number") {
        double number = value.is_number() ? value.get<double>() : 0.0;
        ImGui::SetNextItemWidth(120.0F);
        if (ImGui::InputDouble("##number", &number, 0.0, 0.0, "%.4g")) {
            value = number;
            fc.changed = true;
        }
    } else if (type == "string") {
        std::string text = value.is_string() ? value.get<std::string>() : "";
        if (input_text("##text", text, 220.0F)) {
            value = text;
            fc.changed = true;
        }
    } else if (type == "object") {
        provenance_badge(fc, path);
        ImGui::NewLine();
        ImGui::Indent(24.0F);
        edit_object(fc, node, value, path);
        ImGui::Unindent(24.0F);
        field_errors(fc, path);
        ImGui::PopID();
        return;
    } else if (type == "array") {
        provenance_badge(fc, path);
        ImGui::NewLine();
        ImGui::Indent(24.0F);
        const json& items = node.contains("items") ? node.at("items") : json::object();
        const std::size_t min_items = node.value("minItems", static_cast<std::size_t>(0));
        const std::size_t max_items = node.value("maxItems", static_cast<std::size_t>(64));
        std::size_t remove = static_cast<std::size_t>(-1);
        for (std::size_t i = 0; i < value.size(); ++i) {
            edit_value(
                fc, items, value[i], path + "/" + std::to_string(i), "#" + std::to_string(i + 1));
            if (value.size() > min_items) {
                ImGui::SameLine();
                ImGui::PushID(static_cast<int>(i));
                if (ImGui::SmallButton("remove")) {
                    remove = i;
                }
                ImGui::PopID();
            }
        }
        if (remove != static_cast<std::size_t>(-1)) {
            value.erase(remove);
            fc.changed = true;
        }
        if (value.size() < max_items && ImGui::SmallButton("+ add")) {
            value.push_back(sf::default_for(fc.root, items));
            fc.changed = true;
        }
        ImGui::Unindent(24.0F);
        field_errors(fc, path);
        ImGui::PopID();
        return;
    }
    provenance_badge(fc, path);
    field_errors(fc, path);
    ImGui::PopID();
}

void edit_object(FormContext& fc, const json& raw, json& value, const std::string& path) {
    const json& node = sf::resolve(fc.root, raw);
    if (!value.is_object() || !node.contains("properties")) {
        return;
    }
    std::vector<std::string> required;
    if (node.contains("required")) {
        required = node.at("required").get<std::vector<std::string>>();
    }
    for (const std::string& key : sf::property_order(node)) {
        const json& prop = node.at("properties").at(key);
        const json& p = sf::resolve(fc.root, prop);
        // A constant is shown by whatever chose it -- the branch combo -- and
        // is not editable on its own.
        if (p.is_object() && p.contains("const")) {
            continue;
        }
        const bool is_required = std::find(required.begin(), required.end(), key) != required.end();
        const std::string child = path + "/" + sf::pointer_token(key);
        if (value.contains(key)) {
            edit_value(fc, prop, value[key], child, sf::label_for(key));
            if (!is_required) {
                ImGui::SameLine();
                ImGui::PushID(child.c_str());
                if (ImGui::SmallButton("x")) {
                    value.erase(key);
                    fc.changed = true;
                }
                ImGui::PopID();
            }
        } else if (!is_required) {
            ImGui::PushID(child.c_str());
            ImGui::PushStyleColor(ImGuiCol_Text, theme::colors().text_faint);
            if (ImGui::SmallButton(("+ " + sf::label_for(key)).c_str())) {
                value[key] = sf::default_for(fc.root, prop);
                fc.changed = true;
            }
            ImGui::PopStyleColor();
            ImGui::PopID();
        }
    }
}

void draw_form(ResearchSession& session, const ResearchSnapshot& snap, UiState& ui) {
    if (ui.research_form_version != snap.form_version) {
        ui.research_form = snap.form;
        ui.research_form_version = snap.form_version;
    }
    static const json kNoProvenance = json::object();
    const json& root = session.form_schema_json();
    json& form = ui.research_form;
    FormContext fc{root,
                   form.contains("provenance") ? form.at("provenance") : kNoProvenance,
                   snap.form_errors};

    if (snap.state == ResearchState::Refused) {
        heading("THE JOB REFUSED THIS FORM");
        for (const research::JobError& e : snap.job_errors) {
            wrapped(theme::colors().status_serious,
                    "! " + (e.path.empty() ? std::string("data") : e.path) + ": " + e.message);
        }
    }

    const json& props = root.at("properties");

    struct Section {
        const char* title;
        std::vector<const char*> keys;
    };

    const Section main_sections[] = {
        {"QUESTION", {"prompt"}},
        {"ASSET", {"universe"}},
        {"DATES", {"date_range"}},
        {"STRATEGY", {"template", "strategy"}},
        {"JUDGED BY", {"objective", "benchmarks"}},
    };
    const auto draw_key = [&](const char* key) {
        if (!form.contains(key)) {
            return;
        }
        const json& node = sf::resolve(root, props.at(key));
        const std::string path = std::string("/") + key;
        if (node.value("type", "") == "object") {
            ImGui::PushID(path.c_str());
            edit_object(fc, node, form[key], path);
            ImGui::PopID();
            field_errors(fc, path);
        } else {
            edit_value(fc, props.at(key), form[key], path, sf::label_for(key));
        }
    };
    for (const Section& section : main_sections) {
        heading(section.title);
        for (const char* key : section.keys) {
            draw_key(key);
        }
    }
    ImGui::Spacing();
    if (ImGui::CollapsingHeader("Capital, costs, cash, execution and robustness")) {
        for (const char* key : {"capital", "costs", "cash", "execution", "robustness"}) {
            ink(theme::colors().text_faint, sf::label_for(key));
            draw_key(key);
        }
    }

    // Errors the form cannot show next to a field -- the document root, or a
    // path the editor does not draw.
    heading("RUN");
    if (snap.form_errors.empty()) {
        ink(theme::colors().text_dim, "The form is valid.");
    } else {
        ink(theme::colors().status_serious,
            std::to_string(snap.form_errors.size()) + " problem(s) with the form:");
        for (const research::SchemaError& e : snap.form_errors) {
            wrapped(theme::colors().status_serious,
                    "  " + (e.instance_path.empty() ? std::string("/") : e.instance_path) + ": " +
                        e.message);
        }
    }
    ImGui::BeginDisabled(!snap.form_errors.empty());
    if (ImGui::Button("  Run  ")) {
        session.run();
    }
    ImGui::EndDisabled();

    if (fc.changed) {
        session.set_form(form);
        // Take the next version as our own, so the working copy is not swapped
        // under the cursor for the edit it just made.
        ui.research_form_version = session.snapshot()->form_version;
    }
}

// --- Running --------------------------------------------------------------------

void draw_progress(ResearchSession& session, const ResearchSnapshot& snap) {
    heading("RUNNING");
    ink(theme::colors().text, snap.stage);
    const float fraction =
        snap.total > 0 ? static_cast<float>(snap.done) / static_cast<float>(snap.total) : 0.0F;
    const std::string overlay = std::to_string(snap.done) + " / " + std::to_string(snap.total);
    ImGui::ProgressBar(fraction, ImVec2(-1.0F, 0.0F), overlay.c_str());
    // No partial verdict, ever: a grade from half the gates is a different
    // claim from the one the screen makes.
    wrapped(theme::colors().text_faint, "No verdict is shown until every gate has been measured.");
    if (ImGui::Button("Cancel")) {
        session.cancel();
    }
}

// --- Results --------------------------------------------------------------------

void metric_row(const char* label, const std::vector<std::string>& cells) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ink(theme::colors().text_dim, label);
    for (const std::string& cell : cells) {
        ImGui::TableNextColumn();
        right_text(cell);
    }
}

void draw_windows(const ResearchView& v) {
    std::vector<const WindowView*> columns;
    std::vector<std::string> titles;
    for (const WindowView& w : v.windows) {
        columns.push_back(&w);
        titles.push_back("rule " + w.name);
    }
    if (!v.benchmarks.empty()) {
        for (const WindowView& w : v.benchmarks.front().second) {
            columns.push_back(&w);
            titles.push_back(v.benchmarks.front().first + " " + w.name);
        }
    }
    const int count = static_cast<int>(columns.size()) + 1;
    if (!ImGui::BeginTable("##windows", count, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders)) {
        return;
    }
    ImGui::TableSetupColumn("metric");
    for (const std::string& title : titles) {
        ImGui::TableSetupColumn(title.c_str());
    }
    ImGui::TableHeadersRow();
    const auto row = [&](const char* label, auto get) {
        std::vector<std::string> cells;
        for (const WindowView* w : columns) {
            cells.push_back(get(w->metrics));
        }
        metric_row(label, cells);
    };
    row("total return", [](const MetricsView& m) { return format_points(m.total_return_pct); });
    row("CAGR", [](const MetricsView& m) { return format_points(m.cagr_pct); });
    row("volatility", [](const MetricsView& m) { return format_metric(m.volatility_pct) + "%"; });
    row("Sharpe", [](const MetricsView& m) { return format_metric(m.sharpe); });
    row("Sortino", [](const MetricsView& m) { return format_metric(m.sortino); });
    row("max drawdown", [](const MetricsView& m) { return format_points(m.max_drawdown_pct); });
    row("longest drawdown, days",
        [](const MetricsView& m) { return format_metric(m.max_drawdown_days, 0); });
    row("Calmar", [](const MetricsView& m) { return format_metric(m.calmar); });
    row("ulcer index", [](const MetricsView& m) { return format_metric(m.ulcer_index); });
    row("time in market",
        [](const MetricsView& m) { return format_metric(m.time_in_market_pct, 1) + "%"; });
    row("entries", [](const MetricsView& m) { return std::to_string(m.n_entries); });
    row("win rate", [](const MetricsView& m) { return format_metric(m.win_rate_pct, 1) + "%"; });
    row("average trade",
        [](const MetricsView& m) { return format_points(m.avg_trade_return_pct); });
    row("worst trade", [](const MetricsView& m) { return format_points(m.worst_trade_pct); });
    ImGui::EndTable();

    // The dates the windows cover, under the table rather than as a row of
    // numbers pretending to be metrics.
    for (std::size_t i = 0; i < columns.size(); ++i) {
        ink(theme::colors().text_faint,
            titles[i] + ": " + columns[i]->from + " to " + columns[i]->to);
    }
}

void draw_sweep(const ResearchView& v, const PlainWords& words) {
    if (!v.has_surface) {
        wrapped(theme::colors().text_dim, "Nothing was swept: one parameter set was run.");
        return;
    }
    const SurfaceView& s = v.surface;
    const Palette& palette = theme::colors();
    wrapped(palette.text_dim,
            "Ranked by " + words.objective(s.metric) + ". Setting " +
                std::to_string(s.selected_index + 1) + " of " + std::to_string(s.points.size()) +
                " was chosen; " + format_metric(s.effective_n, 0) +
                " effectively different settings; neighbours keep " +
                format_metric(s.plateau_ratio) + " of its edge.");

    if (s.swept.size() == 1) {
        std::vector<double> xs;
        std::vector<double> in_sample;
        std::vector<double> out_of_sample;
        std::vector<double> excluded_x;
        std::vector<double> excluded_y;
        for (const SurfacePointView& p : s.points) {
            xs.push_back(p.params.front());
            in_sample.push_back(p.in_sample_metric);
            out_of_sample.push_back(p.out_of_sample_metric);
            if (p.excluded) {
                excluded_x.push_back(p.params.front());
                excluded_y.push_back(p.in_sample_metric);
            }
        }
        push_chart_style(ChartDensity::Operator);
        if (ImPlot::BeginPlot("##sweep", ImVec2(-1.0F, 320.0F), ImPlotFlags_NoTitle)) {
            const std::string& path = s.swept.front();
            ImPlot::SetupAxes(words.param_label(path).c_str(),
                              words.objective(s.metric).c_str(),
                              ImPlotAxisFlags_AutoFit,
                              ImPlotAxisFlags_AutoFit);
            ImPlotSpec in_spec;
            in_spec.LineColor = to_vec(theme::series_color(0));
            in_spec.LineWeight = 2.0F;
            in_spec.Marker = ImPlotMarker_Circle;
            ImPlot::PlotLine(
                "earlier years", xs.data(), in_sample.data(), static_cast<int>(xs.size()), in_spec);
            ImPlotSpec out_spec;
            out_spec.LineColor = to_vec(palette.benchmark);
            out_spec.LineWeight = 1.4F;
            out_spec.Marker = ImPlotMarker_Square;
            ImPlot::PlotLine("later years",
                             xs.data(),
                             out_of_sample.data(),
                             static_cast<int>(xs.size()),
                             out_spec);
            if (!excluded_x.empty()) {
                ImPlotSpec hollow;
                hollow.Marker = ImPlotMarker_Circle;
                hollow.MarkerSize = 7.0F;
                hollow.MarkerFillColor = ImVec4(0, 0, 0, 0);
                hollow.MarkerLineColor = to_vec(palette.text_faint);
                ImPlot::PlotScatter("too few trades",
                                    excluded_x.data(),
                                    excluded_y.data(),
                                    static_cast<int>(excluded_x.size()),
                                    hollow);
            }
            const double sx = xs[s.selected_index];
            const double sy = in_sample[s.selected_index];
            ImPlotSpec chosen;
            chosen.Marker = ImPlotMarker_Diamond;
            chosen.MarkerSize = 9.0F;
            chosen.MarkerFillColor = to_vec(palette.accent);
            chosen.MarkerLineColor = to_vec(palette.text);
            ImPlot::PlotScatter("chosen", &sx, &sy, 1, chosen);
            ImPlot::EndPlot();
        }
        pop_chart_style();
        return;
    }

    // Two or more swept dimensions: a table, every point, the selected one
    // marked. Excluded points are listed, not dropped.
    const int columns = static_cast<int>(s.swept.size()) + 4;
    if (!ImGui::BeginTable("##surface",
                           columns,
                           ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders |
                               ImGuiTableFlags_ScrollY,
                           ImVec2(0.0F, 340.0F))) {
        return;
    }
    for (const std::string& path : s.swept) {
        ImGui::TableSetupColumn(words.param_label(path).c_str());
    }
    ImGui::TableSetupColumn("earlier years");
    ImGui::TableSetupColumn("later years");
    ImGui::TableSetupColumn("entries");
    ImGui::TableSetupColumn("");
    ImGui::TableHeadersRow();
    for (std::size_t i = 0; i < s.points.size(); ++i) {
        const SurfacePointView& p = s.points[i];
        ImGui::TableNextRow();
        for (std::size_t d = 0; d < p.params.size(); ++d) {
            ImGui::TableNextColumn();
            right_text(words.param_value(s.swept[d], p.params[d]));
        }
        ImGui::TableNextColumn();
        right_text(format_metric(p.in_sample_metric));
        ImGui::TableNextColumn();
        right_text(format_metric(p.out_of_sample_metric));
        ImGui::TableNextColumn();
        right_text(std::to_string(p.n_entries));
        ImGui::TableNextColumn();
        if (i == s.selected_index) {
            ink(palette.accent, "chosen");
        } else if (p.excluded) {
            ink(palette.text_faint, "too few trades");
        }
    }
    ImGui::EndTable();
}

void draw_timing(const ResearchView& v) {
    if (!ImGui::BeginTable("##timing", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders)) {
        return;
    }
    metric_row("random alternatives", {std::to_string(v.timing_draws)});
    metric_row(("null " + v.timing_metric + ", 5th percentile").c_str(),
               {format_metric(v.timing_p5)});
    metric_row(("null " + v.timing_metric + ", median").c_str(), {format_metric(v.timing_p50)});
    metric_row(("null " + v.timing_metric + ", 95th percentile").c_str(),
               {format_metric(v.timing_p95)});
    metric_row("the rule's percentile", {format_metric(v.timing_percentile, 1)});
    metric_row("p-value", {format_metric(v.timing_p_value, 4)});
    metric_row("p-value, corrected for the search", {format_metric(v.timing_p_corrected, 4)});
    ImGui::EndTable();
    wrapped(theme::colors().text_faint,
            "Each alternative keeps the rule's holding periods and places them at random dates. "
            "Both sides are measured frictionless, so the comparison is about timing alone.");
}

void draw_range(const ResearchView& v) {
    if (!ImGui::BeginTable("##range", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders)) {
        return;
    }
    ImGui::TableSetupColumn("CAGR across resampled histories");
    ImGui::TableSetupColumn("5th");
    ImGui::TableSetupColumn("median");
    ImGui::TableSetupColumn("95th");
    ImGui::TableHeadersRow();
    metric_row("the rule",
               {format_points(v.bootstrap_strategy[0]),
                format_points(v.bootstrap_strategy[1]),
                format_points(v.bootstrap_strategy[2])});
    metric_row("holding it",
               {format_points(v.bootstrap_benchmark[0]),
                format_points(v.bootstrap_benchmark[1]),
                format_points(v.bootstrap_benchmark[2])});
    ImGui::EndTable();
    ink(theme::colors().text_dim,
        "the rule beat holding on " + format_share(v.bootstrap_prob_beats) + " of paths; " +
            format_share(v.bootstrap_share_ruined) + " of paths fell below -90%");
    wrapped(theme::colors().text_faint,
            std::to_string(v.bootstrap_paths) + " paths, blocks of about " +
                std::to_string(v.bootstrap_block_days) +
                " days. This is the spread of results across reshuffled histories -- how much "
                "the result depends on the order things happened in. It is not a forecast and "
                "not an expected return.");
}

void draw_trades(const ResearchView& v) {
    if (!ImGui::BeginTable("##trades",
                           8,
                           ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders |
                               ImGuiTableFlags_ScrollY,
                           ImVec2(0.0F, 340.0F))) {
        return;
    }
    for (const char* title :
         {"entry", "entry price", "exit", "exit price", "return", "days", "worst", "why"}) {
        ImGui::TableSetupColumn(title);
    }
    ImGui::TableHeadersRow();
    for (const TradeView& t : v.trades) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        mono_text(t.entry_date);
        ImGui::TableNextColumn();
        right_text(format_price(t.entry_price));
        ImGui::TableNextColumn();
        mono_text(t.exit_date.empty() ? "open" : t.exit_date);
        ImGui::TableNextColumn();
        right_text(format_price(t.exit_price));
        ImGui::TableNextColumn();
        right_text(format_points(t.return_pct), theme::pnl_color(t.return_pct));
        ImGui::TableNextColumn();
        right_text(std::to_string(t.holding_days));
        ImGui::TableNextColumn();
        right_text(format_points(t.max_adverse_pct));
        ImGui::TableNextColumn();
        ink(theme::colors().text_dim, t.exit_reason + (t.window == "out_of_sample" ? ", OOS" : ""));
    }
    ImGui::EndTable();
}

void draw_data(const ResearchView& v) {
    for (const DataView& d : v.data) {
        ink(theme::colors().text,
            d.series_id + "  " + d.used_from + " to " + d.used_to + "  " + d.quality_tier +
                (d.synthetic ? "  partly synthetic" : ""));
    }
    if (v.regimes == 0) {
        wrapped(theme::colors().text_faint,
                "No regime breakdown: named stress periods come from the data catalog, which "
                "is not built yet.");
    }
    ink(theme::colors().text_faint,
        "run " + v.run_id + ", " + v.started_at + ", " + std::to_string(v.duration_ms) + " ms");
}

// --- The scorecard and the plain verdict -----------------------------------------

/// `color` with its alpha replaced: a tint of a palette colour, not a new one.
[[nodiscard]] unsigned int tint(unsigned int color, unsigned int alpha) {
    return (color & ~IM_COL32_A_MASK) | (alpha << IM_COL32_A_SHIFT);
}

/// Letters lean on the status palette as reinforcement only.
[[nodiscard]] unsigned int letter_color(const std::string& letter) {
    const Palette& p = theme::colors();
    if (letter == "A" || letter == "B") {
        return p.status_good;
    }
    if (letter == "C") {
        return p.status_warning;
    }
    if (letter == "D" || letter == "F") {
        return p.status_serious;
    }
    return p.text_faint;
}

/// A letter in a tinted rounded square, `size` pixels a side.
void letter_box(const std::string& letter, float size) {
    const unsigned int color = letter_color(letter);
    const char* text = letter.empty() ? "-" : letter.c_str();
    const ImVec2 at = ImGui::GetCursorScreenPos();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(at, ImVec2(at.x + size, at.y + size), tint(color, 46), size * 0.24F);
    const ImVec2 extent = ImGui::CalcTextSize(text);
    draw->AddText(
        ImVec2(at.x + (size - extent.x) * 0.5F, at.y + (size - extent.y) * 0.5F), color, text);
    ImGui::Dummy(ImVec2(size, size));
}

[[nodiscard]] double measured(const SubgradeView& sub, const char* key) {
    for (const auto& [name, value] : sub.numbers) {
        if (name == key) {
            return value;
        }
    }
    return std::nan("");
}

/// What a sub-grade read, in words. Formatting only.
[[nodiscard]] std::string measured_text(const SubgradeView& sub) {
    if (sub.status == "not_applicable") {
        return "doesn't apply";
    }
    if (sub.id == "return") {
        return format_points(measured(sub, "cagr_pct")) + " a year";
    }
    if (sub.id == "risk") {
        return format_points(measured(sub, "max_drawdown_pct")) + " worst drop";
    }
    if (sub.id == "volatility") {
        return format_metric(measured(sub, "volatility_pct"), 1) + "% a year";
    }
    if (sub.id == "risk_adjusted") {
        return "Sharpe " + format_metric(measured(sub, "sharpe"));
    }
    if (sub.id == "versus_holding") {
        return format_points(measured(sub, "cagr_difference_pct")) + " a year, drop " +
               format_points(measured(sub, "max_drawdown_difference_pct"));
    }
    for (const auto& [name, words] : sub.words) {
        if (name == "grade") {
            std::string lower = grade_words(words);
            std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });
            return lower;
        }
    }
    return {};
}

// --- Chips, cards and section titles ------------------------------------------------
//
// The web app's look, as far as ImGui allows: tinted rounded chips for grades
// and statuses, cards for groups of figures, and large numbers. Every colour is
// a palette colour or a tint of one, as in the web app.

/// The right edge of the chat's reading column, in screen space, while the
/// transcript is drawn; unbounded otherwise. ImGui's own width (the content
/// region) is the window's, not the column's.
float g_column_right = FLT_MAX;

/// The width available from the cursor to the column's edge.
[[nodiscard]] float column_avail() {
    return std::min(ImGui::GetContentRegionAvail().x,
                    g_column_right - ImGui::GetCursorScreenPos().x);
}

/// A label in `color` on a rounded tint of it, flowing like any other item.
void chip(const std::string& text,
          unsigned int color,
          ImFont* font = nullptr,
          ImVec2 pad = ImVec2(10.0F, 3.0F)) {
    if (font != nullptr) {
        ImGui::PushFont(font);
    }
    const ImVec2 extent = ImGui::CalcTextSize(text.c_str());
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const ImVec2 box(extent.x + pad.x * 2.0F, extent.y + pad.y * 2.0F);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(at, ImVec2(at.x + box.x, at.y + box.y), tint(color, 40), box.y * 0.5F);
    draw->AddText(ImVec2(at.x + pad.x, at.y + pad.y), color, text.c_str());
    ImGui::Dummy(box);
    if (font != nullptr) {
        ImGui::PopFont();
    }
}

/// A rounded panel that grows to its contents. Pair with end_card().
void begin_card(const char* id, unsigned int background, unsigned int border, float width = 0.0F) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, background);
    ImGui::PushStyleColor(ImGuiCol_Border, border);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 12.0F);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16.0F, 12.0F));
    ImGui::BeginChild(id,
                      ImVec2(width > 0.0F ? width : column_avail(), 0.0F),
                      ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding |
                          ImGuiChildFlags_Borders);
}

void end_card() {
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);
}

/// A results section's title: small, faint, upper case, with room above.
void section(const char* text) {
    ImGui::Dummy(ImVec2(0.0F, 10.0F));
    std::string upper = text;
    std::transform(upper.begin(), upper.end(), upper.begin(), [](unsigned char c) {
        return static_cast<char>(std::toupper(c));
    });
    ink(theme::colors().text_faint, upper);
    ImGui::Dummy(ImVec2(0.0F, 2.0F));
}

/// A horizontal bar `fraction` full, drawn rather than a ProgressBar: a framed
/// widget would set a table row's text baseline and push later columns down.
void meter(float width, float height, double fraction, unsigned int color) {
    const ImVec2 at = ImGui::GetCursorScreenPos();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const unsigned int track = tint(theme::colors().accent, 30);
    draw->AddRectFilled(at, ImVec2(at.x + width, at.y + height), track, height * 0.5F);
    const float filled = width * static_cast<float>(std::clamp(fraction, 0.0, 1.0));
    draw->AddRectFilled(at, ImVec2(at.x + filled, at.y + height), color, height * 0.5F);
    ImGui::Dummy(ImVec2(width, height));
}

/// Puts the cursor where an item `height` tall sits in the middle of a row
/// `row` tall starting at `top`: the letter boxes set the row, and every other
/// column lines up with them from the same top.
void middle_of_row(float top, float row, float height) {
    ImGui::SetCursorPosY(top + (row - height) * 0.5F);
}

void draw_scorecard(const ScorecardView& card, const PlainWords& words) {
    constexpr float kRow = 30.0F;
    constexpr float kBar = 6.0F;
    const Palette& palette = theme::colors();

    begin_card("##score_hero", palette.surface_raised, tint(palette.border, 140));
    ImGui::PushFont(theme::hero_font());
    letter_box(card.overall_letter, 84.0F);
    ImGui::PopFont();
    ImGui::SameLine(0.0F, 20.0F);
    ImGui::BeginGroup();
    ink(palette.text_faint, "OVERALL SCORE");
    ImGui::PushFont(theme::large_font());
    ink(palette.text, format_metric(card.overall_score, 0));
    ImGui::PopFont();
    ImGui::SameLine(0.0F, 4.0F);
    ImGui::AlignTextToFramePadding();
    ink(palette.text_faint, "/ 100");
    meter(300.0F, 7.0F, card.overall_score / 100.0, palette.accent);
    ImGui::Dummy(ImVec2(0.0F, 2.0F));
    ink(palette.text_faint,
        "A separate reading from the verdict: a rule can score well and still be fragile.");
    ImGui::EndGroup();

    ImGui::Dummy(ImVec2(0.0F, 6.0F));
    if (ImGui::BeginTable("##subgrades", 5, ImGuiTableFlags_BordersInnerH)) {
        ImGui::TableSetupColumn("letter", ImGuiTableColumnFlags_WidthFixed, 40.0F);
        ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthFixed, 130.0F);
        ImGui::TableSetupColumn("bar", ImGuiTableColumnFlags_WidthFixed, 136.0F);
        ImGui::TableSetupColumn("score", ImGuiTableColumnFlags_WidthFixed, 36.0F);
        ImGui::TableSetupColumn("read", ImGuiTableColumnFlags_WidthStretch);
        for (const SubgradeView& sub : card.subgrades) {
            ImGui::TableNextRow(0, kRow + 6.0F);
            ImGui::TableNextColumn();
            const float top = ImGui::GetCursorPosY() + 3.0F;
            ImGui::SetCursorPosY(top);
            letter_box(sub.letter, kRow);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", words.subgrade_reads(sub.id).c_str());
            }
            const float line = ImGui::GetTextLineHeight();
            ImGui::TableNextColumn();
            middle_of_row(top, kRow, line);
            ink(palette.text, words.subgrade_name(sub.id));
            ImGui::TableNextColumn();
            if (!std::isnan(sub.score)) {
                middle_of_row(top, kRow, kBar);
                meter(128.0F, kBar, sub.score / 100.0, letter_color(sub.letter));
            }
            ImGui::TableNextColumn();
            middle_of_row(top, kRow, line);
            if (!std::isnan(sub.score)) {
                ink(palette.text_faint, format_metric(sub.score, 0));
            }
            ImGui::TableNextColumn();
            middle_of_row(top, kRow, line);
            ink(palette.text_dim, measured_text(sub));
        }
        ImGui::EndTable();
    }
    end_card();
}

[[nodiscard]] const char* status_word(const std::string& status) {
    if (status == "pass") {
        return "Pass";
    }
    if (status == "fail") {
        return "Fail";
    }
    return "Not checked";
}

/// One headline figure as a card: the rule's number large, holding's beside it.
void figure_card(const char* id,
                 const char* label,
                 double rule,
                 double other,
                 const std::string& holding,
                 float width) {
    const Palette& palette = theme::colors();
    begin_card(id, palette.surface, tint(palette.border, 140), width);
    ink(palette.text_faint, label);
    ImGui::BeginGroup();
    ImGui::PushFont(theme::large_font());
    ink(palette.text, format_points(rule));
    ImGui::PopFont();
    ink(palette.text_faint, "the rule");
    ImGui::EndGroup();
    ImGui::SameLine(0.0F, 30.0F);
    ImGui::BeginGroup();
    ImGui::PushFont(theme::large_font());
    ink(palette.text_dim, format_points(other));
    ImGui::PopFont();
    ink(palette.text_faint, holding);
    ImGui::EndGroup();
    end_card();
}

/// `in_conversation`: drawn inside a chat, where the analysis is the
/// assistant's next message and the run lives on the service, not on disk.
void draw_results(ResearchSession& session,
                  const ResearchRun& run,
                  const PlainWords& words,
                  UiState& ui,
                  bool in_conversation = false) {
    // Read once, when the run was made or opened -- never here, mid-frame.
    const ResearchView& v = run.view;
    const Palette& palette = theme::colors();
    const bool unverified = v.data_grade != "research";

    // 1. Verdict. Insufficient evidence keeps its muted ink, tint and all:
    // "not enough data" is not bad news about the idea (SPEC_RESEARCH_VIEW 4.3).
    section("Historical evidence");
    chip(std::string("\xE2\x97\x86  ") + grade_words(v.grade),
         grade_color(v.grade),
         theme::large_font(),
         ImVec2(16.0F, 6.0F));
    if (unverified) {
        ImGui::SameLine(0.0F, 10.0F);
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 8.0F);
        chip("Unverified data", palette.status_warning);
        wrapped(palette.text_faint,
                "The price data is test-grade: a trial run of the method, not a finding.");
    }

    if (v.has_surface && !v.selected_params.empty()) {
        ImGui::Dummy(ImVec2(0.0F, 6.0F));
        begin_card("##chosen", tint(palette.accent, 22), tint(palette.accent, 70));
        ink(palette.text_faint, "Setting chosen");
        ImGui::PushFont(theme::large_font());
        bool first = true;
        for (const auto& [path, value] : v.selected_params) {
            if (!first) {
                ImGui::SameLine(0.0F, 24.0F);
            }
            first = false;
            ink(palette.text, words.param_label(path) + ":");
            ImGui::SameLine();
            ink(palette.accent, words.param_value(path, value));
        }
        ImGui::PopFont();
        ink(palette.text_faint,
            "Picked from " + std::to_string(v.surface.points.size()) + " settings tried, for the " +
                words.objective(v.surface.metric) + ".");
        end_card();
    }

    // The headline figures are out of sample when there is a later period, and
    // over the whole run when there is not (NO_OUT_OF_SAMPLE).
    std::string period = "the whole period";
    for (const WindowView& w : v.windows) {
        if (w.name == "out_of_sample") {
            period = w.from + " to " + w.to + ", the years not used to choose";
        }
    }
    const std::string holding =
        v.benchmarks.empty() ? "the benchmark" : "holding " + v.benchmarks.front().first;
    ImGui::Dummy(ImVec2(0.0F, 6.0F));
    ink(palette.text_faint, "Measured over " + period);
    const float half = std::max(240.0F, (column_avail() - ImGui::GetStyle().ItemSpacing.x) * 0.5F);
    figure_card("##return",
                "Annual return",
                v.strategy_oos_cagr_pct,
                v.benchmark_oos_cagr_pct,
                holding,
                half);
    ImGui::SameLine();
    figure_card("##drop",
                "Worst drop",
                v.strategy_oos_max_drawdown_pct,
                v.benchmark_oos_max_drawdown_pct,
                holding,
                half);
    wrapped(palette.text_dim,
            "Across reshuffled versions of history, the rule's annual return ran from " +
                format_points(v.range_p5) + " to " + format_points(v.range_p95) + " (middle " +
                format_points(v.range_p50) + "). A spread, not a forecast.");
    int note = 0;
    for (const auto& [code, message] : v.warnings) {
        // Said once already, beside the grade.
        if (unverified && code == "EXERCISE_ONLY_DATA") {
            continue;
        }
        ImGui::PushID(note++);
        begin_card("##note", tint(palette.status_warning, 30), tint(palette.status_warning, 0));
        ink(palette.status_warning, "!");
        ImGui::SameLine(0.0F, 10.0F);
        ImGui::PushTextWrapPos(0.0F);
        ink(palette.text_dim, words.warning(code, message));
        ImGui::PopTextWrapPos();
        end_card();
        ImGui::PopID();
    }

    // The scorecard: a separate reading of the same numbers.
    if (v.has_scorecard) {
        section("Scorecard");
        draw_scorecard(v.scorecard, words);
    }

    // 2. Why.
    section("Why");
    begin_card("##why", palette.surface, tint(palette.border, 140));
    if (ImGui::BeginTable("##gates", 2, ImGuiTableFlags_BordersInnerH)) {
        ImGui::TableSetupColumn("status", ImGuiTableColumnFlags_WidthFixed, 110.0F);
        // Stretch: inside an auto-sizing card a table otherwise fits its columns
        // to their contents, and the descriptions wrap at a third of the width.
        ImGui::TableSetupColumn("check", ImGuiTableColumnFlags_WidthStretch);
        for (const GateView& g : v.gates) {
            ImGui::TableNextRow(0, 0.0F);
            if (g.id == v.deciding_gate) {
                ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, tint(palette.accent, 20));
            }
            ImGui::TableNextColumn();
            ImGui::Dummy(ImVec2(0.0F, 4.0F));
            const unsigned int color = g.status == "pass"   ? palette.status_good
                                       : g.status == "fail" ? palette.status_serious
                                                            : palette.text_faint;
            chip(status_word(g.status), color);
            ImGui::TableNextColumn();
            ImGui::Dummy(ImVec2(0.0F, 4.0F));
            ink(palette.text, words.gate_name(g.id));
            if (g.id == v.deciding_gate) {
                ImGui::SameLine();
                ink(palette.text_faint, "- decided the grade");
            }
            if (const std::string asks = words.gate_asks(g.id); !asks.empty()) {
                wrapped(palette.text_dim, asks);
            }
            wrapped(palette.text_faint, g.summary);
            ImGui::Dummy(ImVec2(0.0F, 4.0F));
        }
        ImGui::EndTable();
    }
    end_card();

    // 3. Stats.
    section("Details");
    if (ImGui::BeginTabBar("##stats")) {
        if (ImGui::BeginTabItem("Equity")) {
            ImGui::Checkbox("Log scale", &ui.research_log_scale);
            const SeriesColors colors = colors_for(run.curves);
            ChartRequest request;
            request.density = ChartDensity::Operator;
            request.normalised = true;
            request.log_scale = ui.research_log_scale;
            const ChartModel model = build_chart_model(run.curves, colors, request);
            draw_equity_chart(model, "##research_equity", ImVec2(-1.0F, 300.0F));
            ink(palette.text_faint, "Fall from the previous high");
            draw_drawdown_strip(model, "##research_drawdown", ImVec2(-1.0F, 110.0F));
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Windows")) {
            draw_windows(v);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Sweep")) {
            draw_sweep(v, words);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Timing")) {
            draw_timing(v);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Range")) {
            draw_range(v);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Trades")) {
            draw_trades(v);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Data")) {
            draw_data(v);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    if (in_conversation) {
        ImGui::Spacing();
        // The run's form, to change by hand and run here.
        if (ImGui::Button("Edit this form by hand")) {
            session.set_form(run.form);
            session.edit();
            ui.research_chat = false;
        }
        return;
    }

    // 4. Analysis.
    section("Analysis - written by AI from the numbers above");
    wrapped(palette.text_faint,
            "No analysis for a run made here: the assistant analyses runs started from a "
            "conversation. Everything above is the engine's, and stands without it.");

    ImGui::Spacing();
    if (ImGui::Button("Edit form")) {
        session.edit();
    }
    ImGui::SameLine();
    ImGui::BeginDisabled();
    ImGui::Button("Open in Operator");
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("Replaying a research run in Operator is not built yet.");
    }
    if (!run.directory.empty()) {
        ImGui::SameLine();
        ink(palette.text_faint, "saved to " + run.directory.string());
    }
}

// --- The chat ---------------------------------------------------------------------
//
// docs/specs/SPEC_DESKTOP_CHAT.md section 2: the web app's conversation, drawn
// from the session's ChatSnapshot. The service does the thinking and the
// session the talking; this only draws, and posts what the reader does.

/// One word of a Markdown line, ready to place.
struct Word {
    std::string text;
    bool bold = false;
    bool code = false;
    bool space_before = false;
};

[[nodiscard]] std::vector<Word> words_of(const MdLine& line) {
    std::vector<Word> out;
    bool pending_space = false;
    for (const MdSpan& span : line) {
        std::size_t start = 0;
        for (;;) {
            const auto space = span.text.find(' ', start);
            const std::string piece = span.text.substr(start, space - start);
            if (piece.empty()) {
                pending_space = true;
            } else {
                out.push_back(Word{piece, span.bold, span.code, pending_space});
                pending_space = false;
            }
            if (space == std::string::npos) {
                break;
            }
            pending_space = true;
            start = space + 1;
        }
    }
    return out;
}

/// A line of spans, wrapped to the width available, word by word. Bold is drawn
/// twice a pixel apart: the theme has only Inter Regular (assets/fonts).
void rich_line(const MdLine& line, unsigned int color) {
    const Palette& palette = theme::colors();
    const float wrap = std::max(80.0F, column_avail());
    const float height = ImGui::GetTextLineHeightWithSpacing();
    const float space = ImGui::CalcTextSize(" ").x;
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    float x = 0.0F;
    int row = 0;
    for (const Word& word : words_of(line)) {
        ImFont* font = word.code ? theme::mono_font() : ImGui::GetFont();
        const float size = ImGui::GetFontSize();
        const float width = font->CalcTextSizeA(size, FLT_MAX, 0.0F, word.text.c_str()).x +
                            (word.bold ? 1.0F : 0.0F);
        float gap = x > 0.0F && word.space_before ? space : 0.0F;
        if (x > 0.0F && x + gap + width > wrap) {
            ++row;
            x = 0.0F;
            gap = 0.0F;
        }
        const ImVec2 at(origin.x + x + gap, origin.y + static_cast<float>(row) * height);
        const unsigned int ink_color = word.code ? palette.text_dim : color;
        draw->AddText(font, size, at, ink_color, word.text.c_str());
        if (word.bold) {
            draw->AddText(font, size, ImVec2(at.x + 1.0F, at.y), ink_color, word.text.c_str());
        }
        x += gap + width;
    }
    ImGui::Dummy(ImVec2(wrap, static_cast<float>(row + 1) * height));
}

void draw_markdown(const std::string& text, unsigned int color) {
    const std::vector<MdBlock> blocks = parse_markdown(text);
    for (std::size_t b = 0; b < blocks.size(); ++b) {
        const MdBlock& block = blocks[b];
        if (b > 0) {
            ImGui::Dummy(ImVec2(0.0F, 4.0F));
        }
        switch (block.kind) {
            case MdBlock::Kind::Paragraph:
                rich_line(block.lines.front(), color);
                break;
            case MdBlock::Kind::Bullets:
            case MdBlock::Kind::Numbers:
                for (std::size_t n = 0; n < block.lines.size(); ++n) {
                    const std::string marker = block.kind == MdBlock::Kind::Bullets
                                                   ? std::string("\xE2\x80\xA2")
                                                   : std::to_string(n + 1) + ".";
                    const float x = ImGui::GetCursorPosX();
                    ink(theme::colors().text_faint, marker);
                    ImGui::SameLine(0.0F, 0.0F);
                    ImGui::SetCursorPosX(x + 22.0F);
                    ImGui::BeginGroup();
                    rich_line(block.lines[n], color);
                    ImGui::EndGroup();
                }
                break;
            case MdBlock::Kind::Table: {
                std::size_t columns = 0;
                for (const auto& row : block.rows) {
                    columns = std::max(columns, row.size());
                }
                ImGui::PushID(static_cast<int>(b));
                if (columns > 0 && ImGui::BeginTable("##md_table",
                                                     static_cast<int>(columns),
                                                     ImGuiTableFlags_BordersInnerH |
                                                         ImGuiTableFlags_SizingStretchProp)) {
                    for (std::size_t r = 0; r < block.rows.size(); ++r) {
                        ImGui::TableNextRow();
                        for (std::size_t c = 0; c < columns; ++c) {
                            ImGui::TableNextColumn();
                            if (c < block.rows[r].size()) {
                                rich_line(block.rows[r][c],
                                          r == 0 ? theme::colors().text_faint : color);
                            }
                        }
                    }
                    ImGui::EndTable();
                }
                ImGui::PopID();
                break;
            }
        }
    }
}

/// The reader's message: a tinted bubble on the right, at most three quarters wide.
void user_bubble(const std::string& text) {
    const Palette& palette = theme::colors();
    constexpr float kPad = 12.0F;
    const float avail = column_avail();
    const float wrap = avail * 0.75F - kPad * 2.0F;
    const ImVec2 size = ImGui::CalcTextSize(text.c_str(), nullptr, false, wrap);
    const ImVec2 box(size.x + kPad * 2.0F, size.y + kPad * 1.4F);
    const ImVec2 start = ImGui::GetCursorScreenPos();
    const ImVec2 at(start.x + avail - box.x, start.y);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(at, ImVec2(at.x + box.x, at.y + box.y), tint(palette.accent, 40), 14.0F);
    draw->AddText(ImGui::GetFont(),
                  ImGui::GetFontSize(),
                  ImVec2(at.x + kPad, at.y + kPad * 0.7F),
                  palette.text,
                  text.c_str(),
                  nullptr,
                  wrap);
    ImGui::Dummy(ImVec2(avail, box.y));
}

/// The assistant at work: three pulsing dots and what it is doing.
void thinking(const std::string& activity) {
    const Palette& palette = theme::colors();
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const float y = at.y + ImGui::GetTextLineHeight() * 0.5F;
    const double t = ImGui::GetTime();
    for (int i = 0; i < 3; ++i) {
        const double phase = std::fmod(t * 1.6 - i * 0.18, 1.0);
        const auto alpha =
            static_cast<unsigned int>(70.0 + 185.0 * std::max(0.0, std::sin(phase * 3.14159)));
        ImGui::GetWindowDrawList()->AddCircleFilled(
            ImVec2(at.x + 4.0F + static_cast<float>(i) * 11.0F, y),
            3.5F,
            tint(palette.accent, alpha));
    }
    ImGui::Dummy(ImVec2(40.0F, ImGui::GetTextLineHeight()));
    ImGui::SameLine();
    ink(palette.text_faint, activity.empty() ? std::string("Thinking") : activity);
}

/// A tinted, rounded button: the chips' look, for the chat's actions.
[[nodiscard]] bool soft_button(const char* label, unsigned int color, bool large = false) {
    ImGui::PushStyleColor(ImGuiCol_Button, tint(color, 48));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, tint(color, 80));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, tint(color, 110));
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 16.0F);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                        large ? ImVec2(18.0F, 9.0F) : ImVec2(14.0F, 6.0F));
    const bool clicked = ImGui::Button(label);
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(4);
    return clicked;
}

/// One answer to a question: its label, and its hint under it in faint ink.
[[nodiscard]] bool option_button(
    const char* id, const ChatOption& option, bool chosen, bool enabled, float max_width) {
    const Palette& palette = theme::colors();
    constexpr float kPadX = 12.0F;
    constexpr float kPadY = 7.0F;
    const float wrap = max_width - kPadX * 2.0F;
    const ImVec2 label = ImGui::CalcTextSize(option.label.c_str(), nullptr, false, wrap);
    const ImVec2 hint = option.hint.empty()
                            ? ImVec2(0.0F, 0.0F)
                            : ImGui::CalcTextSize(option.hint.c_str(), nullptr, false, wrap);
    const ImVec2 box(std::max(label.x, hint.x) + kPadX * 2.0F,
                     label.y + (option.hint.empty() ? 0.0F : hint.y + 2.0F) + kPadY * 2.0F);
    const ImVec2 at = ImGui::GetCursorScreenPos();
    ImGui::BeginDisabled(!enabled);
    const bool clicked = ImGui::InvisibleButton(id, box);
    const bool hovered = ImGui::IsItemHovered();
    ImGui::EndDisabled();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const unsigned int fill =
        chosen ? tint(palette.accent, 60) : tint(palette.accent, hovered && enabled ? 34 : 16);
    draw->AddRectFilled(at, ImVec2(at.x + box.x, at.y + box.y), fill, 12.0F);
    if (chosen) {
        draw->AddRect(at, ImVec2(at.x + box.x, at.y + box.y), palette.accent, 12.0F, 0, 1.5F);
    }
    const unsigned int label_ink =
        enabled ? (chosen ? palette.accent : palette.text) : palette.text_faint;
    draw->AddText(ImGui::GetFont(),
                  ImGui::GetFontSize(),
                  ImVec2(at.x + kPadX, at.y + kPadY),
                  label_ink,
                  option.label.c_str(),
                  nullptr,
                  wrap);
    if (!option.hint.empty()) {
        draw->AddText(ImGui::GetFont(),
                      ImGui::GetFontSize(),
                      ImVec2(at.x + kPadX, at.y + kPadY + label.y + 2.0F),
                      palette.text_faint,
                      option.hint.c_str(),
                      nullptr,
                      wrap);
    }
    return clicked && enabled;
}

/// What the reader has picked for a set of questions, until it is sent.
struct Picks {
    std::vector<int> chosen;
    std::vector<std::array<char, 256>> other;
};

void draw_questions(ResearchSession& session,
                    const std::string& key,
                    const ChatItem& item,
                    bool answerable) {
    const Palette& palette = theme::colors();
    static std::map<std::string, Picks> picks_by_key;
    if (!item.message.empty()) {
        draw_markdown(item.message, palette.text);
    }
    if (!answerable) {
        return;  // answered: the reply below says what was chosen
    }
    Picks& picks = picks_by_key[key];
    picks.chosen.resize(item.questions.size(), -1);
    picks.other.resize(item.questions.size(), std::array<char, 256>{});
    const bool single = item.questions.size() == 1;
    const float avail = column_avail();
    for (std::size_t q = 0; q < item.questions.size(); ++q) {
        const ChatQuestion& question = item.questions[q];
        ImGui::PushID(static_cast<int>(q));
        ImGui::Dummy(ImVec2(0.0F, 6.0F));
        ink(palette.text, question.question);
        // Options flow along a row and wrap, as the web's do.
        float x = 0.0F;
        for (std::size_t o = 0; o < question.options.size(); ++o) {
            const ChatOption& option = question.options[o];
            const float width = std::min(avail,
                                         std::max(ImGui::CalcTextSize(option.label.c_str()).x,
                                                  ImGui::CalcTextSize(option.hint.c_str()).x) +
                                             24.0F);
            if (o > 0 && x + 8.0F + width <= avail) {
                ImGui::SameLine(0.0F, 8.0F);
                x += 8.0F;
            } else {
                x = 0.0F;
            }
            ImGui::PushID(static_cast<int>(o));
            if (option_button("##option",
                              option,
                              picks.chosen[q] == static_cast<int>(o),
                              true,
                              std::min(avail, 420.0F))) {
                picks.chosen[q] = static_cast<int>(o);
                picks.other[q][0] = '\0';
                if (single) {
                    session.chat_send(option.label);
                }
            }
            x += ImGui::GetItemRectSize().x;
            ImGui::PopID();
        }
        if (!single && question.allow_free_text) {
            ImGui::SetNextItemWidth(std::min(avail, 520.0F));
            if (ImGui::InputTextWithHint(
                    "##other", "Something else...", picks.other[q].data(), picks.other[q].size())) {
                picks.chosen[q] = -1;
            }
        }
        ImGui::PopID();
    }
    if (single) {
        ink(palette.text_faint, "Or reply below in your own words.");
        return;
    }
    bool complete = true;
    for (std::size_t q = 0; q < item.questions.size(); ++q) {
        complete = complete && (picks.chosen[q] >= 0 || picks.other[q][0] != '\0');
    }
    ImGui::Dummy(ImVec2(0.0F, 4.0F));
    ImGui::BeginDisabled(!complete);
    if (soft_button("Continue", palette.accent)) {
        std::string answer;
        for (std::size_t q = 0; q < item.questions.size(); ++q) {
            const std::string pick =
                picks.chosen[q] >= 0
                    ? item.questions[q].options[static_cast<std::size_t>(picks.chosen[q])].label
                    : std::string(picks.other[q].data());
            answer += (q > 0 ? "\n" : "") + item.questions[q].question + " " + pick;
        }
        session.chat_send(answer);
    }
    ImGui::EndDisabled();
}

/// The order things are shown in: a ready item goes after the assistant text
/// that follows it in the same turn, so Run comes after the explanation (as on
/// the web, Chat.tsx displayOrder).
[[nodiscard]] std::vector<std::size_t> display_order(const std::vector<ChatItem>& items) {
    std::vector<std::size_t> order;
    for (std::size_t n = 0; n < items.size(); ++n) {
        if (items[n].kind != "ready") {
            order.push_back(n);
            continue;
        }
        std::size_t end = n + 1;
        while (end < items.size() && items[end].kind == "assistant") {
            order.push_back(end++);
        }
        order.push_back(n);
        n = end - 1;
    }
    return order;
}

/// Scrolling state for the transcript, kept across frames.
struct Follow {
    std::string conversation;
    int to_bottom = 0;  // frames left in which to land at the bottom
    std::set<std::string> seen_running;
    std::set<std::string> revealed;
    bool jumped = false;  // this frame went to a run's verdict
};

[[nodiscard]] Follow& follow() {
    static Follow state;
    return state;
}

void draw_chat_run(ResearchSession& session,
                   const ChatSnapshot& chat,
                   const ChatItem& item,
                   const PlainWords& words,
                   UiState& ui) {
    const Palette& palette = theme::colors();
    const ChatRun* run = chat.run(item.job_id);
    ImGui::PushID(item.job_id.c_str());
    // Results that arrived while the reader watched: show them from the verdict
    // down, once, as the web does. Here, before the card opens, so it is the
    // transcript that scrolls and not the card.
    if (run != nullptr && run->state == "done" && run->run &&
        follow().seen_running.count(item.job_id) != 0 &&
        follow().revealed.insert(item.job_id).second) {
        ImGui::SetScrollHereY(0.0F);
        follow().jumped = true;
    }
    begin_card("##run", palette.surface, tint(palette.border, 140));
    if (run == nullptr || run->state.empty()) {
        thinking("Starting the backtest");
    } else if (!run->ended()) {
        follow().seen_running.insert(item.job_id);
        ink(palette.text, "Running the backtest");
        ImGui::SameLine();
        ink(palette.text_faint, run->state == "queued" ? "waiting to start" : run->stage);
        meter(std::min(420.0F, ImGui::GetContentRegionAvail().x),
              7.0F,
              run->total > 0 ? static_cast<double>(run->done) / run->total : 0.0,
              palette.accent);
        ink(palette.text_faint, "The verdict appears once every check has finished.");
    } else if (run->state != "done") {
        wrapped(palette.status_serious,
                "The backtest couldn't run. The assistant will explain what to change.");
        if (!run->errors.empty() && ImGui::TreeNode("Technical details")) {
            for (const research::JobError& e : run->errors) {
                wrapped(palette.text_faint, e.message);
            }
            ImGui::TreePop();
        }
    } else if (run->run) {
        draw_results(session, *run->run, words, ui, true);
    } else {
        thinking("Fetching the Results");
    }
    if (run != nullptr && !run->problem.empty()) {
        wrapped(palette.status_serious, run->problem);
    }
    end_card();
    ImGui::PopID();
}

void draw_composer(ResearchSession& session, const ChatView& open) {
    const Palette& palette = theme::colors();
    static std::array<char, 4096> draft{};
    const float send_width = 72.0F;
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 14.0F);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(12.0F, 10.0F));
    ImGui::PushStyleColor(ImGuiCol_FrameBg, palette.surface_raised);
    const bool entered = ImGui::InputTextMultiline(
        "##composer",
        draft.data(),
        draft.size(),
        ImVec2(ImGui::GetContentRegionAvail().x - send_width - 8.0F, 58.0F),
        ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CtrlEnterForNewLine);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
    if (ImGui::IsItemHovered() && draft[0] == '\0') {
        ImGui::SetTooltip("Enter sends; Ctrl+Enter starts a new line.");
    }
    ImGui::SameLine(0.0F, 8.0F);
    ImGui::BeginDisabled(open.busy || draft[0] == '\0');
    const bool clicked = soft_button("Send", palette.accent, true);
    ImGui::EndDisabled();
    if ((entered || clicked) && !open.busy && draft[0] != '\0') {
        session.chat_send(draft.data());
        draft[0] = '\0';
        follow().to_bottom = 3;
    }
}

const char* const kExamples[] = {
    "What's the best drawdown to buy TQQQ at, selling when it gets back to its old high?",
    "Does buying SPY after a 20% drop beat holding it?",
    "If I bought QQQ every time it fell 30% from its high and held, how would that have done?",
};

void draw_chat(ResearchSession& session, const ResearchSnapshot& snap, UiState& ui) {
    const Palette& palette = theme::colors();
    const ChatView& open = snap.chat.open;
    Follow& f = follow();
    if (f.conversation != open.id) {
        f.conversation = open.id;
        f.to_bottom = 3;  // a conversation opens at its end
    }

    const float composer = 74.0F;
    ImGui::BeginChild("##transcript", ImVec2(0.0F, -composer));
    // From last frame's layout: was the reader at the bottom? Only then does new
    // content bring the view down with it.
    const bool at_bottom = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 60.0F;
    const float column = std::min(ImGui::GetContentRegionAvail().x, 860.0F);
    const float indent = std::max(0.0F, (ImGui::GetContentRegionAvail().x - column) * 0.5F);
    ImGui::Indent(indent);
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + column);
    g_column_right = ImGui::GetCursorScreenPos().x + column;
    ImGui::BeginGroup();
    ImGui::PushItemWidth(column);

    if (open.id.empty() && open.items.empty() && !open.busy) {
        // A new conversation: the question, and some to start from.
        ImGui::Dummy(ImVec2(0.0F, 60.0F));
        ImGui::PushFont(theme::hero_font());
        ink(palette.accent, "What do you want to test?");
        ImGui::PopFont();
        wrapped(palette.text_dim,
                "Describe a trading rule. It's tested on historical prices and graded on return, "
                "risk and how well it holds up.");
        ImGui::Dummy(ImVec2(0.0F, 10.0F));
        for (const char* example : kExamples) {
            if (soft_button(example, palette.accent)) {
                session.chat_send(example);
                f.to_bottom = 3;
            }
        }
    } else if (!open.id.empty() && open.items.empty() && open.problem.empty() && !open.busy) {
        ink(palette.text_faint, "Loading the conversation...");
    }

    std::size_t last_ready = SIZE_MAX;
    std::size_t last_run = SIZE_MAX;
    for (std::size_t n = 0; n < open.items.size(); ++n) {
        if (open.items[n].kind == "ready") {
            last_ready = n;
        }
        if (open.items[n].kind == "run") {
            last_run = n;
        }
    }
    for (const std::size_t n : display_order(open.items)) {
        const ChatItem& item = open.items[n];
        ImGui::PushID(static_cast<int>(n));
        ImGui::Dummy(ImVec2(0.0F, 10.0F));
        if (item.kind == "user") {
            user_bubble(item.text);
        } else if (item.kind == "assistant") {
            draw_markdown(item.text, palette.text);
        } else if (item.kind == "analysis") {
            ink(palette.text_faint, "Analysis - written by AI from the results above");
            draw_markdown(item.text, palette.text);
        } else if (item.kind == "questions") {
            const bool answerable = !item.answered && !open.busy && n + 1 == open.items.size();
            draw_questions(session, open.id + "#" + std::to_string(n), item, answerable);
        } else if (item.kind == "ready") {
            // Only the one that can still be run.
            const bool runnable = n == last_ready && (last_run == SIZE_MAX || last_run < n) &&
                                  open.ready && !open.busy;
            if (runnable) {
                if (soft_button("Run the backtest", palette.accent, true)) {
                    session.chat_run();
                    f.to_bottom = 3;
                }
                ImGui::SameLine(0.0F, 12.0F);
                ImGui::AlignTextToFramePadding();
                ink(palette.text_faint, "or reply to change something first");
            }
        } else if (item.kind == "run") {
            draw_chat_run(session, snap.chat, item, *snap.words, ui);
        } else if (item.kind == "error") {
            begin_card(
                "##error", tint(palette.status_serious, 30), tint(palette.status_serious, 0));
            wrapped(palette.status_serious, item.text);
            end_card();
        }
        ImGui::PopID();
    }
    if (open.busy) {
        ImGui::Dummy(ImVec2(0.0F, 10.0F));
        thinking(open.activity);
    }
    if (!open.problem.empty()) {
        ImGui::Dummy(ImVec2(0.0F, 8.0F));
        begin_card("##problem", tint(palette.status_serious, 30), tint(palette.status_serious, 0));
        wrapped(palette.status_serious, open.problem);
        end_card();
    }
    ImGui::Dummy(ImVec2(0.0F, 16.0F));

    ImGui::PopItemWidth();
    ImGui::EndGroup();
    g_column_right = FLT_MAX;
    ImGui::PopTextWrapPos();
    ImGui::Unindent(indent);
    if (f.to_bottom > 0) {
        --f.to_bottom;
        ImGui::SetScrollHereY(1.0F);
    } else if (at_bottom && !f.jumped) {
        ImGui::SetScrollHereY(1.0F);
    }
    f.jumped = false;
    ImGui::EndChild();

    // The message box, under the transcript, in the same column.
    const float avail = ImGui::GetContentRegionAvail().x;
    const float column_out = std::min(avail, 860.0F);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0F, (avail - column_out) * 0.5F));
    ImGui::BeginGroup();
    ImGui::PushItemWidth(column_out);
    ImGui::BeginChild("##composer_area", ImVec2(column_out, 0.0F));
    draw_composer(session, open);
    ImGui::EndChild();
    ImGui::PopItemWidth();
    ImGui::EndGroup();
}

// --- The rail -------------------------------------------------------------------

void draw_rail(ResearchSession& session, const ResearchSnapshot& snap, UiState& ui) {
    const Palette& palette = theme::colors();
    if (snap.chat.available) {
        ImGui::BeginDisabled(snap.chat.open.busy);
        if (soft_button("+  New chat", palette.accent)) {
            session.chat_new();
            ui.research_chat = true;
        }
        ImGui::EndDisabled();
        heading("CONVERSATIONS");
        if (snap.chat.conversations.empty()) {
            ink(palette.text_faint, "Your conversations will show here.");
        }
        // Not while a turn streams: it would land in the conversation left.
        ImGui::BeginDisabled(snap.chat.open.busy);
        for (const ConversationSummary& c : snap.chat.conversations) {
            ImGui::PushID(c.id.c_str());
            const bool current = ui.research_chat && c.id == snap.chat.open.id;
            if (ImGui::Selectable(c.title.empty() ? "New conversation" : c.title.c_str(),
                                  current)) {
                session.chat_open(c.id);
                ui.research_chat = true;
            }
            ImGui::PopID();
        }
        ImGui::EndDisabled();
        ImGui::Spacing();
    } else {
        heading("ASK");
        wrapped(palette.text_dim, snap.chat.reason);
    }
    if (soft_button("Fill in a form by hand", palette.accent)) {
        session.set_form(session.starter_form());
        session.edit();
        ui.research_chat = false;
    }

    heading("RECENT RUNS");
    if (snap.recent.empty()) {
        ink(palette.text_faint, "None yet.");
    }
    if (!snap.open_error.empty()) {
        wrapped(palette.status_serious, "! " + snap.open_error);
    }
    // Not while a job runs: it would finish and replace the run opened, unseen.
    ImGui::BeginDisabled(snap.state == ResearchState::Running);
    for (const RecentRun& recent : snap.recent) {
        ImGui::PushID(recent.directory.string().c_str());
        const bool open = snap.run && snap.run->directory == recent.directory;
        if (ImGui::Selectable(recent.title.c_str(), open && !ui.research_chat)) {
            session.open(recent.directory);
            ui.research_chat = false;
        }
        ImGui::Indent(12.0F);
        chip(grade_words(recent.grade), grade_color(recent.grade), nullptr, ImVec2(7.0F, 1.0F));
        ImGui::SameLine();
        ink(palette.text_faint,
            (recent.data_grade == "research" ? "" : "unverified  ") + recent.started_at);
        ImGui::Unindent(12.0F);
        ImGui::PopID();
    }
    ImGui::EndDisabled();
}

}  // namespace

void research(Context& context) {
    if (context.research == nullptr) {
        ink(theme::colors().text_dim, "The Research view is not available in this build.");
        return;
    }
    ResearchSession& session = *context.research;
    const std::shared_ptr<const ResearchSnapshot> snap = session.snapshot();
    if (!snap->setup_error.empty()) {
        heading("RESEARCH");
        wrapped(theme::colors().status_serious, snap->setup_error);
        return;
    }

    const float rail = std::max(260.0F, ImGui::GetContentRegionAvail().x * 0.26F);
    ImGui::BeginChild("##research_rail", ImVec2(rail, 0.0F), ImGuiChildFlags_Borders);
    draw_rail(session, *snap, context.ui);
    ImGui::EndChild();

    ImGui::SameLine();
    ImGui::BeginChild("##research_main", ImVec2(0.0F, 0.0F));
    if (snap->chat.available && context.ui.research_chat) {
        // The list once, when the chat is first shown.
        static bool listed = false;
        if (!listed) {
            listed = true;
            session.chat_refresh();
        }
        draw_chat(session, *snap, context.ui);
        ImGui::EndChild();
        return;
    }
    switch (snap->state) {
        case ResearchState::Running:
            draw_progress(session, *snap);
            break;
        case ResearchState::Done:
            if (snap->run) {
                draw_results(session, *snap->run, *snap->words, context.ui);
                break;
            }
            [[fallthrough]];
        case ResearchState::Editing:
        case ResearchState::Refused:
            draw_form(session, *snap, context.ui);
            break;
    }
    ImGui::EndChild();
}

}  // namespace marketscan::app::panels
