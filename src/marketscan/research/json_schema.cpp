#include "marketscan/research/json_schema.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <fstream>
#include <map>
#include <regex>
#include <set>
#include <string_view>
#include <utility>

namespace marketscan::research {

namespace {

using nlohmann::json;

// The closed vocabulary. See the header for why it is closed.
const std::set<std::string, std::less<>> kAnnotations = {
    "$schema", "$id", "$comment", "title", "description", "default", "examples"};

const std::set<std::string, std::less<>> kKeywords = {"$ref",
                                                      "$defs",
                                                      "properties",
                                                      "additionalProperties",
                                                      "items",
                                                      "allOf",
                                                      "anyOf",
                                                      "oneOf",
                                                      "if",
                                                      "then",
                                                      "else",
                                                      "type",
                                                      "enum",
                                                      "const",
                                                      "required",
                                                      "minimum",
                                                      "maximum",
                                                      "exclusiveMinimum",
                                                      "exclusiveMaximum",
                                                      "minItems",
                                                      "maxItems",
                                                      "pattern",
                                                      "format"};

const std::set<std::string, std::less<>> kTypes = {
    "null", "boolean", "object", "array", "number", "integer", "string"};

const std::set<std::string, std::less<>> kFormats = {"date", "date-time"};

/// Deep enough for any contract here; shallow enough that a $ref cycle which
/// consumes no input fails rather than overflowing the stack.
constexpr int kMaxDepth = 256;

std::string escape_token(std::string_view token) {
    std::string out;
    out.reserve(token.size());
    for (const char c : token) {
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

std::string child(const std::string& pointer, std::string_view token) {
    return pointer + "/" + escape_token(token);
}

std::string child(const std::string& pointer, std::size_t index) {
    return pointer + "/" + std::to_string(index);
}

bool is_non_negative_integer(const json& value) {
    return value.is_number_unsigned() || (value.is_number_integer() && value.get<long long>() >= 0);
}

bool all_digits(std::string_view text) {
    return !text.empty() &&
           std::all_of(text.begin(), text.end(), [](char c) { return c >= '0' && c <= '9'; });
}

int to_int(std::string_view text) {
    int value = 0;
    for (const char c : text) {
        value = value * 10 + (c - '0');
    }
    return value;
}

int days_in_month(int year, int month) {
    static constexpr int kDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month == 2) {
        const bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
        return leap ? 29 : 28;
    }
    return kDays[month - 1];
}

/// hh:mm, both in range. Used for the time and for a numeric offset.
bool is_hours_minutes(std::string_view text) {
    if (text.size() != 5 || text[2] != ':' || !all_digits(text.substr(0, 2)) ||
        !all_digits(text.substr(3, 2))) {
        return false;
    }
    return to_int(text.substr(0, 2)) <= 23 && to_int(text.substr(3, 2)) <= 59;
}

bool matches_type(const json& value, std::string_view type) {
    if (type == "null") {
        return value.is_null();
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
    if (type == "string") {
        return value.is_string();
    }
    if (type == "number") {
        return value.is_number();
    }
    // "integer": 2020-12 counts 1.0 as an integer. A number with no fractional
    // part is the definition, not its spelling in the source text.
    if (value.is_number_integer()) {
        return true;
    }
    if (value.is_number_float()) {
        const double d = value.get<double>();
        return std::isfinite(d) && std::trunc(d) == d;
    }
    return false;
}

}  // namespace

bool is_valid_date(const std::string& text) {
    if (text.size() != 10 || text[4] != '-' || text[7] != '-') {
        return false;
    }
    const std::string_view view(text);
    if (!all_digits(view.substr(0, 4)) || !all_digits(view.substr(5, 2)) ||
        !all_digits(view.substr(8, 2))) {
        return false;
    }
    const int year = to_int(view.substr(0, 4));
    const int month = to_int(view.substr(5, 2));
    const int day = to_int(view.substr(8, 2));
    if (year < 1 || month < 1 || month > 12) {
        return false;
    }
    return day >= 1 && day <= days_in_month(year, month);
}

bool is_valid_date_time(const std::string& text) {
    // Shortest form: YYYY-MM-DDThh:mm:ssZ, twenty characters.
    if (text.size() < 20 || text[10] != 'T' || !is_valid_date(text.substr(0, 10))) {
        return false;
    }
    const std::string_view view(text);
    const std::string_view clock = view.substr(11, 8);
    if (!is_hours_minutes(clock.substr(0, 5)) || clock[5] != ':' ||
        !all_digits(clock.substr(6, 2)) || to_int(clock.substr(6, 2)) > 59) {
        return false;
    }
    std::string_view rest = view.substr(19);
    if (!rest.empty() && rest.front() == '.') {
        std::size_t digits = 1;
        while (digits < rest.size() && rest[digits] >= '0' && rest[digits] <= '9') {
            ++digits;
        }
        if (digits == 1) {
            return false;
        }
        rest.remove_prefix(digits);
    }
    if (rest == "Z") {
        return true;
    }
    return rest.size() == 6 && (rest.front() == '+' || rest.front() == '-') &&
           is_hours_minutes(rest.substr(1));
}

struct JsonSchema::Impl {
    json root;

    /// Compiled patterns, keyed by the schema object that holds them.
    std::map<const json*, std::regex> patterns;

    /// Every "$ref" string, resolved once at compile time.
    std::map<std::string, const json*, std::less<>> refs;

    // --- Compilation -----------------------------------------------------

    std::set<const json*> visited;
    std::vector<std::pair<std::string, std::string>> pending_refs;  // ref, where

    void compile_node(const json& node, const std::string& path) {
        if (node.is_boolean()) {
            visited.insert(&node);
            return;
        }
        if (!node.is_object()) {
            throw SchemaCompileError("schema at '" + path + "' is not an object or a boolean");
        }
        visited.insert(&node);

        for (const auto& [key, value] : node.items()) {
            if (key.starts_with("x-") || kAnnotations.contains(key)) {
                continue;
            }
            if (!kKeywords.contains(key)) {
                throw SchemaCompileError("unsupported keyword '" + key + "' at '" + path +
                                         "'. The validator implements a closed subset; "
                                         "see json_schema.hpp.");
            }
        }

        const auto subschema_map = [&](const char* key) {
            if (!node.contains(key)) {
                return;
            }
            const json& map = node.at(key);
            if (!map.is_object()) {
                throw SchemaCompileError("'" + std::string(key) + "' at '" + path +
                                         "' must be an object");
            }
            for (const auto& [name, sub] : map.items()) {
                compile_node(sub, child(child(path, key), name));
            }
        };
        const auto subschema = [&](const char* key) {
            if (node.contains(key)) {
                compile_node(node.at(key), child(path, key));
            }
        };
        const auto subschema_list = [&](const char* key) {
            if (!node.contains(key)) {
                return;
            }
            const json& list = node.at(key);
            if (!list.is_array() || list.empty()) {
                throw SchemaCompileError("'" + std::string(key) + "' at '" + path +
                                         "' must be a non-empty array");
            }
            for (std::size_t i = 0; i < list.size(); ++i) {
                compile_node(list[i], child(child(path, key), i));
            }
        };
        const auto number = [&](const char* key) {
            if (node.contains(key) && !node.at(key).is_number()) {
                throw SchemaCompileError("'" + std::string(key) + "' at '" + path +
                                         "' must be a number");
            }
        };
        const auto count = [&](const char* key) {
            if (node.contains(key) && !is_non_negative_integer(node.at(key))) {
                throw SchemaCompileError("'" + std::string(key) + "' at '" + path +
                                         "' must be a non-negative integer");
            }
        };

        subschema_map("$defs");
        subschema_map("properties");
        subschema("additionalProperties");
        subschema("items");
        subschema_list("allOf");
        subschema_list("anyOf");
        subschema_list("oneOf");
        subschema("if");
        subschema("then");
        subschema("else");
        number("minimum");
        number("maximum");
        number("exclusiveMinimum");
        number("exclusiveMaximum");
        count("minItems");
        count("maxItems");

        if (node.contains("type")) {
            const json& type = node.at("type");
            const auto known = [](const json& t) {
                return t.is_string() && kTypes.contains(t.get<std::string>());
            };
            const bool ok = known(type) || (type.is_array() && !type.empty() &&
                                            std::all_of(type.begin(), type.end(), known));
            if (!ok) {
                throw SchemaCompileError("'type' at '" + path + "' names an unknown type");
            }
        }
        if (node.contains("enum") && (!node.at("enum").is_array() || node.at("enum").empty())) {
            throw SchemaCompileError("'enum' at '" + path + "' must be a non-empty array");
        }
        if (node.contains("required")) {
            const json& required = node.at("required");
            if (!required.is_array() || !std::all_of(required.begin(),
                                                     required.end(),
                                                     [](const json& r) { return r.is_string(); })) {
                throw SchemaCompileError("'required' at '" + path +
                                         "' must be an array of strings");
            }
        }
        if (node.contains("format")) {
            const json& format = node.at("format");
            if (!format.is_string() || !kFormats.contains(format.get<std::string>())) {
                throw SchemaCompileError("'format' at '" + path +
                                         "' is not one this validator asserts");
            }
        }
        if (node.contains("pattern")) {
            const json& pattern = node.at("pattern");
            if (!pattern.is_string()) {
                throw SchemaCompileError("'pattern' at '" + path + "' must be a string");
            }
            try {
                patterns.emplace(&node,
                                 std::regex(pattern.get<std::string>(), std::regex::ECMAScript));
            } catch (const std::regex_error& e) {
                throw SchemaCompileError("'pattern' at '" + path +
                                         "' is not a valid regular expression: " + e.what());
            }
        }
        if (node.contains("$ref")) {
            const json& ref = node.at("$ref");
            if (!ref.is_string() || !ref.get<std::string>().starts_with("#")) {
                throw SchemaCompileError("'$ref' at '" + path +
                                         "' must be a local reference beginning '#'");
            }
            pending_refs.emplace_back(ref.get<std::string>(), path);
        }
    }

    void resolve_refs() {
        for (const auto& [ref, where] : pending_refs) {
            if (refs.contains(ref)) {
                continue;
            }
            const json* target = nullptr;
            try {
                const json::json_pointer pointer(ref.substr(1));
                if (root.contains(pointer)) {
                    target = &root.at(pointer);
                }
            } catch (const json::exception&) {
                target = nullptr;
            }
            if (target == nullptr) {
                throw SchemaCompileError("'$ref' \"" + ref + "\" at '" + where +
                                         "' does not resolve");
            }
            // A reference into something that is not a schema position -- a
            // `required` list, say -- would be evaluated as a schema anyway.
            if (!visited.contains(target)) {
                throw SchemaCompileError("'$ref' \"" + ref + "\" at '" + where +
                                         "' points at something that is not a schema");
            }
            refs.emplace(ref, target);
        }
        visited.clear();
        pending_refs.clear();
    }

    // --- Validation ------------------------------------------------------

    void check(const json& schema,
               const std::string& spath,
               const json& doc,
               const std::string& dpath,
               std::vector<SchemaError>& out,
               int depth) const {
        const auto fail = [&](const std::string& keyword, std::string message) {
            out.push_back({dpath, child(spath, keyword), std::move(message)});
        };

        if (depth > kMaxDepth) {
            out.push_back({dpath, spath, "schema nesting is too deep; is there a $ref cycle?"});
            return;
        }
        if (schema.is_boolean()) {
            if (!schema.get<bool>()) {
                out.push_back({dpath, spath, "no value is allowed here"});
            }
            return;
        }

        if (schema.contains("$ref")) {
            const std::string& ref = schema.at("$ref").get_ref<const std::string&>();
            check(*refs.find(ref)->second, ref.substr(1), doc, dpath, out, depth + 1);
        }

        if (schema.contains("type")) {
            const json& type = schema.at("type");
            bool ok = false;
            if (type.is_string()) {
                ok = matches_type(doc, type.get_ref<const std::string&>());
            } else {
                for (const json& t : type) {
                    ok = ok || matches_type(doc, t.get_ref<const std::string&>());
                }
            }
            if (!ok) {
                fail("type", "expected " + type.dump() + ", got " + doc.type_name());
            }
        }
        if (schema.contains("enum")) {
            const json& options = schema.at("enum");
            if (std::find(options.begin(), options.end(), doc) == options.end()) {
                fail("enum", doc.dump() + " is not one of " + options.dump());
            }
        }
        if (schema.contains("const") && doc != schema.at("const")) {
            fail("const", "expected " + schema.at("const").dump() + ", got " + doc.dump());
        }

        if (doc.is_object()) {
            check_object(schema, spath, doc, dpath, out, depth);
        }
        if (doc.is_array()) {
            check_array(schema, spath, doc, dpath, out, depth);
        }
        if (doc.is_number()) {
            const double value = doc.get<double>();
            const auto bound = [&](const char* key, auto ok, const char* words) {
                if (schema.contains(key)) {
                    const double limit = schema.at(key).get<double>();
                    if (!ok(value, limit)) {
                        fail(key, doc.dump() + " is not " + words + " " + schema.at(key).dump());
                    }
                }
            };
            bound("minimum", [](double v, double l) { return v >= l; }, ">=");
            bound("maximum", [](double v, double l) { return v <= l; }, "<=");
            bound("exclusiveMinimum", [](double v, double l) { return v > l; }, ">");
            bound("exclusiveMaximum", [](double v, double l) { return v < l; }, "<");
        }
        if (doc.is_string()) {
            const std::string& text = doc.get_ref<const std::string&>();
            if (schema.contains("pattern") &&
                !std::regex_search(text, patterns.find(&schema)->second)) {
                fail("pattern", "\"" + text + "\" does not match " + schema.at("pattern").dump());
            }
            if (schema.contains("format")) {
                const std::string& format = schema.at("format").get_ref<const std::string&>();
                const bool ok = format == "date" ? is_valid_date(text) : is_valid_date_time(text);
                if (!ok) {
                    fail("format", "\"" + text + "\" is not a valid " + format);
                }
            }
        }

        check_combinators(schema, spath, doc, dpath, out, depth);
    }

    void check_object(const json& schema,
                      const std::string& spath,
                      const json& doc,
                      const std::string& dpath,
                      std::vector<SchemaError>& out,
                      int depth) const {
        if (schema.contains("required")) {
            for (const json& name : schema.at("required")) {
                const std::string& key = name.get_ref<const std::string&>();
                if (!doc.contains(key)) {
                    out.push_back({dpath,
                                   child(spath, "required"),
                                   "missing required property '" + key + "'"});
                }
            }
        }
        const json* properties = schema.contains("properties") ? &schema.at("properties") : nullptr;
        for (const auto& [key, value] : doc.items()) {
            if (properties != nullptr && properties->contains(key)) {
                check(properties->at(key),
                      child(child(spath, "properties"), key),
                      value,
                      child(dpath, key),
                      out,
                      depth + 1);
            } else if (schema.contains("additionalProperties")) {
                const json& extra = schema.at("additionalProperties");
                if (extra.is_boolean() && !extra.get<bool>()) {
                    out.push_back({child(dpath, key),
                                   child(spath, "additionalProperties"),
                                   "unexpected property '" + key + "'"});
                } else {
                    check(extra,
                          child(spath, "additionalProperties"),
                          value,
                          child(dpath, key),
                          out,
                          depth + 1);
                }
            }
        }
    }

    void check_array(const json& schema,
                     const std::string& spath,
                     const json& doc,
                     const std::string& dpath,
                     std::vector<SchemaError>& out,
                     int depth) const {
        const auto size = doc.size();
        if (schema.contains("minItems") && size < schema.at("minItems").get<std::size_t>()) {
            out.push_back({dpath,
                           child(spath, "minItems"),
                           "has " + std::to_string(size) + " items, needs at least " +
                               schema.at("minItems").dump()});
        }
        if (schema.contains("maxItems") && size > schema.at("maxItems").get<std::size_t>()) {
            out.push_back({dpath,
                           child(spath, "maxItems"),
                           "has " + std::to_string(size) + " items, allows at most " +
                               schema.at("maxItems").dump()});
        }
        if (schema.contains("items")) {
            for (std::size_t i = 0; i < size; ++i) {
                check(schema.at("items"),
                      child(spath, "items"),
                      doc[i],
                      child(dpath, i),
                      out,
                      depth + 1);
            }
        }
    }

    void check_combinators(const json& schema,
                           const std::string& spath,
                           const json& doc,
                           const std::string& dpath,
                           std::vector<SchemaError>& out,
                           int depth) const {
        const auto branch_errors = [&](const char* key) {
            std::vector<std::vector<SchemaError>> all;
            const json& list = schema.at(key);
            for (std::size_t i = 0; i < list.size(); ++i) {
                std::vector<SchemaError> errors;
                check(list[i], child(child(spath, key), i), doc, dpath, errors, depth + 1);
                all.push_back(std::move(errors));
            }
            return all;
        };

        if (schema.contains("allOf")) {
            for (auto& errors : branch_errors("allOf")) {
                out.insert(out.end(), errors.begin(), errors.end());
            }
        }
        if (schema.contains("anyOf")) {
            const auto all = branch_errors("anyOf");
            if (std::none_of(all.begin(), all.end(), [](const auto& e) { return e.empty(); })) {
                out.push_back(
                    {dpath,
                     child(spath, "anyOf"),
                     "matches none of the " + std::to_string(all.size()) + " alternatives"});
            }
        }
        if (schema.contains("oneOf")) {
            const auto all = branch_errors("oneOf");
            const auto matched =
                std::count_if(all.begin(), all.end(), [](const auto& e) { return e.empty(); });
            if (matched == 0) {
                // Closest: a branch whose discriminator matched -- no failed
                // `const` -- before one that is a different kind of thing
                // altogether, then the fewest errors, then the first. Without
                // the first key, {"type": "rolling_high"} is one error away
                // from being an all_time_high and gets reported as one.
                const auto rank = [](const std::vector<SchemaError>& errors) {
                    const bool wrong_kind =
                        std::any_of(errors.begin(), errors.end(), [](const SchemaError& e) {
                            return e.schema_path.ends_with("/const");
                        });
                    return std::pair{wrong_kind, errors.size()};
                };
                const auto closest =
                    std::min_element(all.begin(), all.end(), [&](const auto& a, const auto& b) {
                        return rank(a) < rank(b);
                    });
                out.push_back({dpath,
                               child(spath, "oneOf"),
                               "matches none of the " + std::to_string(all.size()) +
                                   " alternatives; the closest fails because:"});
                out.insert(out.end(), closest->begin(), closest->end());
            } else if (matched > 1) {
                out.push_back({dpath,
                               child(spath, "oneOf"),
                               "matches " + std::to_string(matched) +
                                   " alternatives, must match exactly one"});
            }
        }
        if (schema.contains("if")) {
            std::vector<SchemaError> condition;
            check(schema.at("if"), child(spath, "if"), doc, dpath, condition, depth + 1);
            const char* branch = condition.empty() ? "then" : "else";
            if (schema.contains(branch)) {
                check(schema.at(branch), child(spath, branch), doc, dpath, out, depth + 1);
            }
        }
    }
};

JsonSchema JsonSchema::compile(nlohmann::json schema) {
    auto impl = std::make_shared<Impl>();
    // Moved in before anything takes an address: every pointer in the maps is
    // into impl->root, which is never modified afterwards.
    impl->root = std::move(schema);
    impl->compile_node(impl->root, "");
    impl->resolve_refs();
    return JsonSchema(std::move(impl));
}

JsonSchema JsonSchema::load(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw SchemaCompileError("cannot open schema '" + path.string() + "'");
    }
    nlohmann::json schema;
    try {
        schema = nlohmann::json::parse(in);
    } catch (const nlohmann::json::parse_error& e) {
        throw SchemaCompileError("schema '" + path.string() + "' is not JSON: " + e.what());
    }
    return compile(std::move(schema));
}

std::vector<SchemaError> JsonSchema::validate(const nlohmann::json& document) const {
    std::vector<SchemaError> errors;
    impl_->check(impl_->root, "", document, "", errors, 0);
    return errors;
}

bool JsonSchema::is_valid(const nlohmann::json& document) const {
    return validate(document).empty();
}

}  // namespace marketscan::research
