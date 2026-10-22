#include "schema_form.hpp"

#include <algorithm>

namespace marketscan::app::schema_form {

using nlohmann::json;

namespace {

[[nodiscard]] bool type_matches(const json& node, const json& value) {
    if (!node.contains("type")) {
        return true;
    }
    const auto one = [&value](const std::string& type) {
        if (type == "number") {
            return value.is_number();
        }
        if (type == "integer") {
            return value.is_number_integer() ||
                   (value.is_number_float() &&
                    value.get<double>() ==
                        static_cast<double>(static_cast<long long>(value.get<double>())));
        }
        if (type == "string") {
            return value.is_string();
        }
        if (type == "boolean") {
            return value.is_boolean();
        }
        if (type == "object") {
            return value.is_object();
        }
        if (type == "array") {
            return value.is_array();
        }
        return value.is_null();
    };
    const json& type = node.at("type");
    if (type.is_string()) {
        return one(type.get<std::string>());
    }
    for (const json& t : type) {
        if (one(t.get<std::string>())) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool matches(const json& root, const json& raw, const json& value) {
    const json& node = resolve(root, raw);
    if (node.is_boolean()) {
        return node.get<bool>();
    }
    if (!type_matches(node, value)) {
        return false;
    }
    if (node.contains("const") && node.at("const") != value) {
        return false;
    }
    if (node.contains("oneOf")) {
        bool any = false;
        for (const json& branch : node.at("oneOf")) {
            any = any || matches(root, branch, value);
        }
        if (!any) {
            return false;
        }
    }
    if (value.is_object()) {
        if (node.contains("properties")) {
            for (const auto& [key, prop] : node.at("properties").items()) {
                const json& p = resolve(root, prop);
                if (p.is_object() && p.contains("const") &&
                    (!value.contains(key) || value.at(key) != p.at("const"))) {
                    return false;
                }
            }
        }
        if (node.contains("required")) {
            for (const json& key : node.at("required")) {
                if (!value.contains(key.get<std::string>())) {
                    return false;
                }
            }
        }
        if (node.contains("additionalProperties") && node.at("additionalProperties") == false) {
            const json empty = json::object();
            const json& props = node.contains("properties") ? node.at("properties") : empty;
            for (const auto& [key, unused] : value.items()) {
                (void)unused;
                if (!props.contains(key)) {
                    return false;
                }
            }
        }
    }
    return true;
}

}  // namespace

const json& resolve(const json& root, const json& node) {
    const json* current = &node;
    for (int depth = 0; depth < 32 && current->is_object() && current->contains("$ref"); ++depth) {
        const std::string ref = current->at("$ref").get<std::string>();
        current = &root.at(json::json_pointer(ref.substr(1)));
    }
    return *current;
}

json default_for(const json& root, const json& raw) {
    const json& node = resolve(root, raw);
    if (!node.is_object()) {
        return nullptr;
    }
    if (node.contains("const")) {
        return node.at("const");
    }
    if (node.contains("default")) {
        return node.at("default");
    }
    if (node.contains("x-default") && node.contains("type") &&
        type_matches(node, node.at("x-default")) &&
        (!node.contains("enum") ||
         std::find(node.at("enum").begin(), node.at("enum").end(), node.at("x-default")) !=
             node.at("enum").end())) {
        return node.at("x-default");
    }
    if (node.contains("enum")) {
        return node.at("enum").front();
    }
    if (node.contains("oneOf")) {
        return default_for(root, node.at("oneOf").front());
    }
    const std::string type = node.contains("type") && node.at("type").is_string()
                                 ? node.at("type").get<std::string>()
                                 : std::string();
    if (type == "object") {
        json out = json::object();
        if (node.contains("required") && node.contains("properties")) {
            for (const json& key : node.at("required")) {
                const std::string name = key.get<std::string>();
                out[name] = default_for(root, node.at("properties").at(name));
            }
        }
        return out;
    }
    if (type == "array") {
        json out = json::array();
        const std::size_t count =
            node.contains("minItems") ? node.at("minItems").get<std::size_t>() : 0;
        for (std::size_t i = 0; i < count && node.contains("items"); ++i) {
            out.push_back(default_for(root, node.at("items")));
        }
        return out;
    }
    if (type == "number" || type == "integer") {
        double value = 0.0;
        if (node.contains("minimum")) {
            value = node.at("minimum").get<double>();
        } else if (node.contains("exclusiveMinimum")) {
            value = node.at("exclusiveMinimum").get<double>() + 1.0;
        }
        if (type == "integer") {
            return static_cast<long long>(value);
        }
        return value;
    }
    if (type == "boolean") {
        return false;
    }
    if (type == "string") {
        const std::string format = node.value("format", "");
        if (format == "date") {
            return "2000-01-03";
        }
        return "";
    }
    return nullptr;
}

std::size_t branch_of(const json& root, const json& one_of, const json& value) {
    for (std::size_t i = 0; i < one_of.size(); ++i) {
        if (matches(root, one_of[i], value)) {
            return i;
        }
    }
    return 0;
}

std::string branch_label(const json& root, const json& raw) {
    const json& node = resolve(root, raw);
    if (node.contains("const")) {
        return node.at("const").is_string() ? node.at("const").get<std::string>()
                                            : node.at("const").dump();
    }
    if (node.contains("properties")) {
        for (const auto& [key, prop] : node.at("properties").items()) {
            const json& p = resolve(root, prop);
            if (p.is_object() && p.contains("const") && p.at("const").is_string()) {
                return p.at("const").get<std::string>();
            }
        }
    }
    if (node.contains("required") && !node.at("required").empty()) {
        return node.at("required").front().get<std::string>();
    }
    const std::string type = node.contains("type") && node.at("type").is_string()
                                 ? node.at("type").get<std::string>()
                                 : std::string();
    if (type == "null") {
        return "none";
    }
    if (type == "number" || type == "integer") {
        return "value";
    }
    if (type == "string") {
        return node.value("format", "text");
    }
    return type.empty() ? "option" : type;
}

std::vector<std::string> property_order(const json& node) {
    std::vector<std::string> out;
    if (!node.is_object() || !node.contains("properties")) {
        return out;
    }
    const json& props = node.at("properties");
    if (node.contains("required")) {
        for (const json& key : node.at("required")) {
            if (props.contains(key.get<std::string>())) {
                out.push_back(key.get<std::string>());
            }
        }
    }
    for (const auto& [key, unused] : props.items()) {
        (void)unused;
        if (std::find(out.begin(), out.end(), key) == out.end()) {
            out.push_back(key);
        }
    }
    return out;
}

std::string label_for(const std::string& key) {
    std::string out;
    for (const char c : key) {
        out += c == '_' ? ' ' : c;
    }
    const std::string suffix = " pct";
    if (out.size() > suffix.size() && out.ends_with(suffix)) {
        out.replace(out.size() - suffix.size(), suffix.size(), " %");
    }
    return out;
}

std::string pointer_token(const std::string& key) {
    std::string out;
    for (const char c : key) {
        if (c == '~') {
            out += "~0";
        } else if (c == '/') {
            out += "~1";
        } else {
            out += c;
        }
    }
    return out;
}

}  // namespace marketscan::app::schema_form
