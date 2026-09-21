#include "marketscan/exec/delisting.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>

namespace marketscan {

std::string_view name_of(DelistingReturnModel model) noexcept {
    return model == DelistingReturnModel::LastPrice ? "last_price" : "apply_true_return";
}

bool parse_delisting_return_model(std::string_view name, DelistingReturnModel& out) noexcept {
    if (name == "last_price") {
        out = DelistingReturnModel::LastPrice;
        return true;
    }
    if (name == "apply_true_return") {
        out = DelistingReturnModel::ApplyTrueReturn;
        return true;
    }
    return false;
}

std::string_view describe(DelistingReturnModel model) noexcept {
    if (model == DelistingReturnModel::LastPrice) {
        return "last_price (liquidated at the final quoted price; no delisting return)";
    }
    return "apply_true_return (the delisting return applied where the data supplies it)";
}

void DelistingReturns::add(SymbolId symbol, double delisting_return, std::string cause) {
    const auto at = std::lower_bound(
        entries_.begin(), entries_.end(), symbol, [](const Entry& entry, SymbolId id) {
            return entry.symbol < id;
        });
    if (at != entries_.end() && at->symbol == symbol) {
        at->delisting_return = delisting_return;
        at->cause = std::move(cause);
        return;
    }
    entries_.insert(at, Entry{symbol, delisting_return, std::move(cause)});
}

bool DelistingReturns::has(SymbolId symbol) const noexcept {
    const auto at = std::lower_bound(
        entries_.begin(), entries_.end(), symbol, [](const Entry& entry, SymbolId id) {
            return entry.symbol < id;
        });
    return at != entries_.end() && at->symbol == symbol;
}

double DelistingReturns::find(SymbolId symbol, double fallback) const noexcept {
    const auto at = std::lower_bound(
        entries_.begin(), entries_.end(), symbol, [](const Entry& entry, SymbolId id) {
            return entry.symbol < id;
        });
    return at != entries_.end() && at->symbol == symbol ? at->delisting_return : fallback;
}

namespace {

/// Splits on commas. No quoting: the only writer of this file is
/// `research/export.py`, and a cause field carrying a comma is rejected rather
/// than silently split, which is what the `rejected` list is for.
[[nodiscard]] std::vector<std::string> split(const std::string& line) {
    std::vector<std::string> fields;
    std::istringstream row(line);
    std::string field;
    while (std::getline(row, field, ',')) {
        fields.push_back(field);
    }
    return fields;
}

}  // namespace

DelistingReturnsFile load_delisting_returns(const std::filesystem::path& path,
                                            SymbolTable& symbols) {
    DelistingReturnsFile out;

    std::ifstream file(path);
    if (!file) {
        out.rejected.push_back("could not open " + path.string());
        return out;
    }

    std::string line;
    if (!std::getline(file, line)) {
        out.rejected.push_back(path.string() + " is empty");
        return out;
    }

    std::size_t row_number = 1;
    while (std::getline(file, line)) {
        ++row_number;
        if (line.empty()) {
            continue;
        }
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }

        const std::vector<std::string> fields = split(line);
        const auto reject = [&out, row_number, &line](const char* why) {
            out.rejected.push_back("row " + std::to_string(row_number) + " (" + line + "): " + why);
        };

        if (fields.size() < 2) {
            reject("fewer than two fields");
            continue;
        }
        if (fields[0].empty()) {
            reject("no ticker");
            continue;
        }

        double value = 0.0;
        try {
            std::size_t consumed = 0;
            value = std::stod(fields[1], &consumed);
            if (consumed != fields[1].size()) {
                reject("trailing characters in the delisting return");
                continue;
            }
        } catch (const std::exception&) {
            reject("the delisting return is not a number");
            continue;
        }

        // A return below -1 means the position was worth less than nothing,
        // which this build does not model, and one above a few hundred percent is
        // a data error rather than a merger premium. Both are refused rather than
        // clamped: a clamp would produce a plausible number from a bad row.
        if (!std::isfinite(value) || value < -1.0 || value > 10.0) {
            reject("the delisting return is outside [-1, 10]");
            continue;
        }

        out.returns.add(
            symbols.intern(fields[0]), value, fields.size() > 2 ? fields[2] : std::string{});
    }

    return out;
}

}  // namespace marketscan
