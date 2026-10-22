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
#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include <imgui.h>
#include <implot.h>

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

void draw_sweep(const ResearchView& v) {
    if (!v.has_surface) {
        wrapped(theme::colors().text_dim, "Nothing was swept: one parameter set was run.");
        return;
    }
    const SurfaceView& s = v.surface;
    const Palette& palette = theme::colors();
    ink(palette.text_dim,
        "objective " + s.metric + " -- selected point " + std::to_string(s.selected_index + 1) +
            " of " + std::to_string(s.points.size()) + ", effective variants " +
            format_metric(s.effective_n, 0) + ", plateau ratio " + format_metric(s.plateau_ratio));

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
            ImPlot::SetupAxes(sf::label_for(path.substr(path.rfind('/') + 1)).c_str(),
                              s.metric.c_str(),
                              ImPlotAxisFlags_AutoFit,
                              ImPlotAxisFlags_AutoFit);
            ImPlotSpec in_spec;
            in_spec.LineColor = to_vec(theme::series_color(0));
            in_spec.LineWeight = 2.0F;
            in_spec.Marker = ImPlotMarker_Circle;
            ImPlot::PlotLine(
                "in sample", xs.data(), in_sample.data(), static_cast<int>(xs.size()), in_spec);
            ImPlotSpec out_spec;
            out_spec.LineColor = to_vec(palette.benchmark);
            out_spec.LineWeight = 1.4F;
            out_spec.Marker = ImPlotMarker_Square;
            ImPlot::PlotLine("out of sample",
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
                ImPlot::PlotScatter("too few entries",
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
            ImPlot::PlotScatter("selected", &sx, &sy, 1, chosen);
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
        ImGui::TableSetupColumn(sf::label_for(path.substr(path.rfind('/') + 1)).c_str());
    }
    ImGui::TableSetupColumn("in sample");
    ImGui::TableSetupColumn("out of sample");
    ImGui::TableSetupColumn("entries");
    ImGui::TableSetupColumn("");
    ImGui::TableHeadersRow();
    for (std::size_t i = 0; i < s.points.size(); ++i) {
        const SurfacePointView& p = s.points[i];
        ImGui::TableNextRow();
        for (const double value : p.params) {
            ImGui::TableNextColumn();
            right_text(format_metric(value, 4));
        }
        ImGui::TableNextColumn();
        right_text(format_metric(p.in_sample_metric));
        ImGui::TableNextColumn();
        right_text(format_metric(p.out_of_sample_metric));
        ImGui::TableNextColumn();
        right_text(std::to_string(p.n_entries));
        ImGui::TableNextColumn();
        if (i == s.selected_index) {
            ink(palette.accent, "selected");
        } else if (p.excluded) {
            ink(palette.text_faint, "too few entries");
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

void draw_results(ResearchSession& session, const ResearchRun& run) {
    // Read once, when the run was made or opened -- never here, mid-frame.
    const ResearchView& v = run.view;
    const Palette& palette = theme::colors();

    // 1. Verdict.
    heading("HISTORICAL EVIDENCE");
    ImGui::PushFont(theme::large_font());
    ink(grade_color(v.grade), grade_words(v.grade));
    ImGui::PopFont();
    if (v.data_grade != "research") {
        ImGui::SameLine();
        ink(palette.status_warning, "   ! unverified data");
    }
    ink(palette.text,
        "out of sample  " + format_points(v.strategy_oos_cagr_pct) + " CAGR, against " +
            format_points(v.benchmark_oos_cagr_pct) + " for the benchmark");
    ink(palette.text,
        "max drawdown   " + format_points(v.strategy_oos_max_drawdown_pct) + ", against " +
            format_points(v.benchmark_oos_max_drawdown_pct));
    ink(palette.text_dim,
        "resampled histories  5th " + format_points(v.range_p5) + "   median " +
            format_points(v.range_p50) + "   95th " + format_points(v.range_p95) +
            "   (a spread, not a forecast)");
    for (const auto& [code, message] : v.warnings) {
        wrapped(palette.status_warning, "! " + code + "  " + message);
    }

    // 2. Why.
    heading("WHY");
    if (ImGui::BeginTable("##gates", 3, ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("status", ImGuiTableColumnFlags_WidthFixed, 56.0F);
        ImGui::TableSetupColumn("gate", ImGuiTableColumnFlags_WidthFixed, 150.0F);
        ImGui::TableSetupColumn("measured");
        for (const GateView& g : v.gates) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            const unsigned int color = g.status == "pass"   ? palette.status_good
                                       : g.status == "fail" ? palette.status_serious
                                                            : palette.text_faint;
            ImGui::PushFont(theme::mono_font());
            ink(color, status_mark(g.status));
            ImGui::PopFont();
            ImGui::TableNextColumn();
            ink(g.id == v.deciding_gate ? palette.text : palette.text_dim, g.id);
            ImGui::TableNextColumn();
            ImGui::TextWrapped("%s", g.summary.c_str());
        }
        ImGui::EndTable();
    }

    // 3. Stats.
    heading("STATS");
    if (ImGui::BeginTabBar("##stats")) {
        if (ImGui::BeginTabItem("Equity")) {
            const SeriesColors colors = colors_for(run.curves);
            ChartRequest request;
            request.density = ChartDensity::Operator;
            request.normalised = true;
            const ChartModel model = build_chart_model(run.curves, colors, request);
            draw_equity_chart(model, "##research_equity", ImVec2(-1.0F, 300.0F));
            ink(palette.text_faint, "drawdown");
            draw_drawdown_strip(model, "##research_drawdown", ImVec2(-1.0F, 110.0F));
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Windows")) {
            draw_windows(v);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Sweep")) {
            draw_sweep(v);
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

    // 4. Analysis.
    heading("ANALYSIS  -  written by AI from the numbers above");
    wrapped(palette.text_faint,
            "No analysis: the assistant is not connected yet (part R2). Everything above is the "
            "engine's, and stands without it.");

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

// --- The rail -------------------------------------------------------------------

void draw_rail(ResearchSession& session, const ResearchSnapshot& snap) {
    const Palette& palette = theme::colors();
    heading("ASK");
    static char question[512] = "";
    ImGui::BeginDisabled(!snap.assistant_available);
    ImGui::InputTextMultiline("##question", question, sizeof(question), ImVec2(-1.0F, 70.0F));
    ImGui::EndDisabled();
    wrapped(palette.text_dim, snap.assistant_reason);
    if (ImGui::Button("Fill in a form by hand")) {
        session.set_form(session.starter_form());
        session.edit();
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
        if (ImGui::Selectable(recent.title.c_str(), open)) {
            session.open(recent.directory);
        }
        ImGui::Indent(12.0F);
        ink(grade_color(recent.grade), grade_words(recent.grade));
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
    draw_rail(session, *snap);
    ImGui::EndChild();

    ImGui::SameLine();
    ImGui::BeginChild("##research_main", ImVec2(0.0F, 0.0F));
    switch (snap->state) {
        case ResearchState::Running:
            draw_progress(session, *snap);
            break;
        case ResearchState::Done:
            if (snap->run) {
                draw_results(session, *snap->run);
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
