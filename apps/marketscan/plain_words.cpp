#include "plain_words.hpp"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <stdexcept>

namespace marketscan::app {

namespace {

using nlohmann::json;

[[nodiscard]] std::string spaced(std::string id) {
    std::replace(id.begin(), id.end(), '_', ' ');
    return id;
}

[[nodiscard]] std::string last_part(const std::string& path) {
    const auto slash = path.rfind('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

[[nodiscard]] bool ends_with(const std::string& text, const std::string& tail) {
    return text.size() >= tail.size() &&
           text.compare(text.size() - tail.size(), tail.size(), tail) == 0;
}

}  // namespace

PlainWords::PlainWords(const json& words) {
    for (const auto& [id, entry] : words.at("gates").items()) {
        gates_[id] = {entry.at("name").get<std::string>(), entry.at("asks").get<std::string>()};
    }
    for (const auto& [code, text] : words.at("warnings").items()) {
        warnings_[code] = text.get<std::string>();
    }
    for (const auto& [key, entry] : words.at("params").items()) {
        params_[key] = {entry.at("label").get<std::string>(), entry.at("unit").get<std::string>()};
    }
    for (const auto& [metric, text] : words.at("objectives").items()) {
        objectives_[metric] = text.get<std::string>();
    }
    for (const auto& [id, entry] : words.at("subgrades").items()) {
        subgrades_[id] = {entry.at("name").get<std::string>(),
                          entry.at("reads").get<std::string>()};
    }
}

PlainWords PlainWords::load(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        throw std::runtime_error("cannot read " + file.string());
    }
    return PlainWords(json::parse(in));
}

std::string PlainWords::gate_name(const std::string& id) const {
    const auto it = gates_.find(id);
    return it == gates_.end() ? spaced(id) : it->second.first;
}

std::string PlainWords::gate_asks(const std::string& id) const {
    const auto it = gates_.find(id);
    return it == gates_.end() ? std::string() : it->second.second;
}

std::string PlainWords::warning(const std::string& code, const std::string& message) const {
    const auto it = warnings_.find(code);
    return it == warnings_.end() ? message : it->second;
}

std::string PlainWords::param_label(const std::string& path) const {
    const std::string key = last_part(path);
    const auto it = params_.find(key);
    return it == params_.end() ? spaced(key) : it->second.label;
}

std::string PlainWords::param_value(const std::string& path, double value) const {
    const std::string key = last_part(path);
    const auto it = params_.find(key);
    const std::string unit = it != params_.end()      ? it->second.unit
                             : ends_with(key, "_pct") ? "%"
                             : ends_with(key, "days") ? "days"
                                                      : "";
    const std::string shown = plain_number(value);
    if (unit == "%") {
        return shown + "%";
    }
    return unit.empty() ? shown : shown + " " + unit;
}

std::string PlainWords::objective(const std::string& metric) const {
    const auto it = objectives_.find(metric);
    return it == objectives_.end() ? spaced(metric) : it->second;
}

std::string PlainWords::subgrade_name(const std::string& id) const {
    const auto it = subgrades_.find(id);
    return it == subgrades_.end() ? spaced(id) : it->second.first;
}

std::string PlainWords::subgrade_reads(const std::string& id) const {
    const auto it = subgrades_.find(id);
    return it == subgrades_.end() ? std::string() : it->second.second;
}

std::string plain_number(double value) {
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.4f", value);
    std::string text = buffer;
    while (!text.empty() && text.back() == '0') {
        text.pop_back();
    }
    if (!text.empty() && text.back() == '.') {
        text.pop_back();
    }
    return text == "-0" ? "0" : text;
}

}  // namespace marketscan::app
