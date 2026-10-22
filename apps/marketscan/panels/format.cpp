// Shared formatting for every panel.
//
// Centralised so that a P&L is coloured and signed the same way everywhere. A
// dashboard that shows the same number two different ways in two places is
// misleading without ever being wrong.

#include <cmath>
#include <cstdio>

#include <imgui.h>

#include "../theme.hpp"
#include "marketscan/metrics/performance.hpp"
#include "panels.hpp"

namespace marketscan::app::panels {
namespace {

/// Thousands separators, because a trading account is a seven-digit number and
/// nobody can read 1284930 at a glance.
[[nodiscard]] std::string group_digits(const std::string& digits) {
    std::string out;
    out.reserve(digits.size() + digits.size() / 3);
    const std::size_t lead = digits.size() % 3 == 0 ? 3 : digits.size() % 3;
    for (std::size_t i = 0; i < digits.size(); ++i) {
        if (i == lead || (i > lead && (i - lead) % 3 == 0)) {
            out.push_back(',');
        }
        out.push_back(digits[i]);
    }
    return out;
}

}  // namespace

std::string format_money(Money value, bool with_sign) {
    const bool negative = value.micros() < 0;
    const Money magnitude = negative ? Money::from_micros(-value.micros()) : value;

    char buffer[48];
    std::snprintf(buffer, sizeof buffer, "%.2f", magnitude.to_double());

    std::string text(buffer);
    const std::size_t dot = text.find('.');
    const std::string whole = text.substr(0, dot);
    const std::string fraction = text.substr(dot);

    std::string sign;
    if (negative) {
        sign = "-";
    } else if (with_sign && !value.is_zero()) {
        sign = "+";
    }
    return sign + group_digits(whole) + fraction;
}

std::string format_percent(double fraction, int precision) {
    if (!metrics::is_defined(fraction)) {
        return "--";
    }
    char buffer[32];
    std::snprintf(buffer, sizeof buffer, "%+.*f%%", precision, fraction * 100.0);
    return buffer;
}

std::string format_points(double percent, int precision) {
    if (!metrics::is_defined(percent)) {
        return "--";
    }
    char buffer[32];
    std::snprintf(buffer, sizeof buffer, "%+.*f%%", precision, percent);
    return buffer;
}

std::string format_share(double share, int precision) {
    if (!metrics::is_defined(share)) {
        return "--";
    }
    char buffer[32];
    std::snprintf(buffer, sizeof buffer, "%.*f%%", precision, share * 100.0);
    return buffer;
}

std::string format_metric(double value, int precision) {
    // A dash, never a zero. Zero is a result; "undefined" is not.
    if (!metrics::is_defined(value)) {
        return "--";
    }
    char buffer[32];
    std::snprintf(buffer, sizeof buffer, "%.*f", precision, value);
    return buffer;
}

std::string format_quantity(double value) {
    if (!std::isfinite(value)) {
        return "--";
    }
    char buffer[32];
    // Whole units print without a pointless ".00"; fractional ones keep four
    // places, because a crypto or FX size can be genuinely small.
    if (std::fabs(value - std::round(value)) < 1e-9) {
        std::snprintf(buffer, sizeof buffer, "%.0f", value);
    } else {
        std::snprintf(buffer, sizeof buffer, "%.4f", value);
    }
    return buffer;
}

std::string format_price(double value) {
    if (!std::isfinite(value) || value == 0.0) {
        return "--";
    }
    char buffer[32];
    std::snprintf(buffer, sizeof buffer, "%.4f", value);
    return buffer;
}

std::string format_timestamp(Timestamp ts) {
    if (ts == 0) {
        return "--";
    }
    // Trim the nanoseconds and the Z: a blotter column does not have room, and
    // nothing in this application trades below the millisecond.
    std::string iso = time::to_iso8601(ts);
    if (iso.size() >= 24) {
        iso = iso.substr(0, 10) + " " + iso.substr(11, 12);
    }
    return iso;
}

std::string format_time_of_day(Timestamp ts) {
    if (ts == 0) {
        return "--";
    }
    const std::string iso = time::to_iso8601(ts);
    return iso.size() >= 23 ? iso.substr(11, 12) : iso;
}

std::string format_duration(Duration nanos) {
    char buffer[32];
    const double value = static_cast<double>(nanos);
    if (nanos < 1'000) {
        std::snprintf(buffer, sizeof buffer, "%lldns", static_cast<long long>(nanos));
    } else if (nanos < 1'000'000) {
        std::snprintf(buffer, sizeof buffer, "%.2fus", value / 1e3);
    } else if (nanos < 1'000'000'000) {
        std::snprintf(buffer, sizeof buffer, "%.2fms", value / 1e6);
    } else {
        std::snprintf(buffer, sizeof buffer, "%.2fs", value / 1e9);
    }
    return buffer;
}

std::string format_count(std::uint64_t value) {
    return group_digits(std::to_string(value));
}

void mono_text(const std::string& text) {
    ImGui::PushFont(theme::mono_font(), 0.0F);
    ImGui::TextUnformatted(text.c_str());
    ImGui::PopFont();
}

void right_text(const std::string& text, unsigned int color) {
    ImGui::PushFont(theme::mono_font(), 0.0F);
    const float width = ImGui::CalcTextSize(text.c_str()).x;
    const float available = ImGui::GetContentRegionAvail().x;
    if (available > width) {
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + available - width);
    }
    if (color != 0) {
        ImGui::PushStyleColor(ImGuiCol_Text, color);
        ImGui::TextUnformatted(text.c_str());
        ImGui::PopStyleColor();
    } else {
        ImGui::TextUnformatted(text.c_str());
    }
    ImGui::PopFont();
}

void money_cell(Money value, bool with_sign) {
    right_text(format_money(value, with_sign), theme::pnl_color(value.to_double()));
}

void metric_cell(double value, int precision) {
    if (!metrics::is_defined(value)) {
        right_text("--", theme::colors().text_faint);
        return;
    }
    right_text(format_metric(value, precision));
}

void help_marker(const char* description) {
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::BeginItemTooltip()) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 28.0F);
        ImGui::TextUnformatted(description);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

}  // namespace marketscan::app::panels
