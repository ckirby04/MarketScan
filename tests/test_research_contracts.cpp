// The research contracts, from the engine's side.
//
// The research layer's half is research/tests/test_contracts.py. Both reproduce
// the verdicts in data/samples/research/schema_cases.json, which is how two
// validators that share no code are shown to agree -- see contracts/README.md.
//
// The second half of this file is the validator's own mechanics, and in
// particular its refusals. A validator whose only tests are documents it accepts
// is a validator that may accept everything.

#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include "marketscan/research/json_schema.hpp"

using marketscan::research::is_valid_date;
using marketscan::research::is_valid_date_time;
using marketscan::research::JsonSchema;
using marketscan::research::SchemaCompileError;
using nlohmann::json;

namespace {

const std::filesystem::path kRepo = MARKETSCAN_SOURCE_DIR;

json read_json(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    REQUIRE(in.good());
    return json::parse(in);
}

json corpus() {
    return read_json(kRepo / "data" / "samples" / "research" / "schema_cases.json");
}

bool has_error_containing(const std::vector<marketscan::research::SchemaError>& errors,
                          const std::string& text) {
    for (const auto& e : errors) {
        if (e.message.find(text) != std::string::npos) {
            return true;
        }
    }
    return false;
}

}  // namespace

// --- The contracts ----------------------------------------------------------

TEST_CASE("every research contract schema compiles inside the supported subset") {
    const json cases = corpus();
    for (const auto& [name, path] : cases.at("schemas").items()) {
        INFO(name);
        CHECK_NOTHROW(JsonSchema::load(kRepo / path.get<std::string>()));
    }
}

TEST_CASE("the assistant's tool schemas compile inside the supported subset") {
    const json tools = read_json(kRepo / "contracts" / "research" / "assistant" / "tools.json");
    REQUIRE(tools.size() >= 1);
    for (const json& tool : tools) {
        INFO(tool.at("name").get<std::string>());
        CHECK_NOTHROW(JsonSchema::compile(tool.at("input_schema")));
    }
}

TEST_CASE("the engine's validator reproduces every verdict in the schema corpus") {
    const json cases = corpus();

    std::map<std::string, JsonSchema> schemas;
    for (const auto& [name, path] : cases.at("schemas").items()) {
        schemas.emplace(name, JsonSchema::load(kRepo / path.get<std::string>()));
    }
    std::map<std::string, json> bases;
    for (const auto& [name, path] : cases.at("bases").items()) {
        bases.emplace(name, read_json(kRepo / path.get<std::string>()));
    }

    // Not vacuous: every schema is exercised by documents it must accept and by
    // documents it must refuse.
    std::map<std::string, std::set<bool>> seen;

    for (const json& c : cases.at("cases")) {
        const std::string name = c.at("name").get<std::string>();
        const std::string schema = c.at("schema").get<std::string>();
        const bool expected = c.at("valid").get<bool>();
        INFO(name);

        // nlohmann's patch throws on a replace or remove of a missing path,
        // which is what keeps a case from silently testing a different document.
        const json document = c.contains("document")
                                  ? c.at("document")
                                  : bases.at(c.at("base").get<std::string>()).patch(c.at("patch"));

        const auto errors = schemas.at(schema).validate(document);
        if (errors.empty() != expected) {
            for (const auto& e : errors) {
                UNSCOPED_INFO(e.instance_path << " [" << e.schema_path << "] " << e.message);
            }
        }
        CHECK(errors.empty() == expected);
        seen[schema].insert(expected);
    }

    REQUIRE(seen.size() == cases.at("schemas").size());
    for (const auto& [schema, verdicts] : seen) {
        INFO(schema);
        CHECK(verdicts == std::set<bool>{true, false});
    }
}

// --- Refusals at compile time -------------------------------------------------

TEST_CASE("a schema keyword outside the subset is refused, not ignored") {
    const json schema = {{"type", "string"}, {"minLength", 1}};
    CHECK_THROWS_AS(JsonSchema::compile(schema), SchemaCompileError);

    // Nested, where an ignored keyword would be hardest to notice.
    const json nested = {{"properties", {{"a", {{"patternProperties", json::object()}}}}}};
    CHECK_THROWS_AS(JsonSchema::compile(nested), SchemaCompileError);
}

TEST_CASE("annotations and x- keys are accepted and change nothing") {
    const json schema = {{"type", "integer"},
                         {"description", "d"},
                         {"title", "t"},
                         {"default", 3},
                         {"x-ask", "never"},
                         {"x-default", {1, 2}}};
    const auto s = JsonSchema::compile(schema);
    CHECK(s.is_valid(3));
    CHECK_FALSE(s.is_valid("3"));
}

TEST_CASE("a format the validator does not assert is refused") {
    CHECK_THROWS_AS(JsonSchema::compile({{"type", "string"}, {"format", "email"}}),
                    SchemaCompileError);
}

TEST_CASE("a $ref must be local and must resolve to a schema") {
    CHECK_THROWS_AS(JsonSchema::compile({{"$ref", "other.json#/x"}}), SchemaCompileError);
    CHECK_THROWS_AS(JsonSchema::compile({{"$ref", "#/$defs/missing"}}), SchemaCompileError);
    // Resolves, but to a `required` list rather than a schema.
    CHECK_THROWS_AS(JsonSchema::compile({{"required", json::array({"a"})}, {"$ref", "#/required"}}),
                    SchemaCompileError);
}

TEST_CASE("an invalid pattern is refused at compile time") {
    CHECK_THROWS_AS(JsonSchema::compile({{"pattern", "(["}}), SchemaCompileError);
}

TEST_CASE("loading a missing or malformed schema file is a compile error") {
    CHECK_THROWS_AS(JsonSchema::load(kRepo / "contracts" / "research" / "no_such.schema.json"),
                    SchemaCompileError);
    CHECK_THROWS_AS(JsonSchema::load(kRepo / "contracts" / "research" / "FORM.md"),
                    SchemaCompileError);
}

// --- Mechanics ----------------------------------------------------------------

TEST_CASE("errors name the document path that failed") {
    const auto s = JsonSchema::compile({{"type", "object"},
                                        {"required", json::array({"a"})},
                                        {"properties", {{"a", {{"type", "integer"}}}}},
                                        {"additionalProperties", false}});
    const auto errors = s.validate({{"a", "x"}, {"b/c", 1}});
    REQUIRE(errors.size() == 2);
    CHECK(errors[0].instance_path == "/a");
    CHECK(errors[0].schema_path == "/properties/a/type");
    // JSON Pointer escaping: "/" in a key is "~1".
    CHECK(errors[1].instance_path == "/b~1c");

    const auto missing = s.validate(json::object());
    REQUIRE(missing.size() == 1);
    CHECK(missing[0].instance_path.empty());
    CHECK(has_error_containing(missing, "'a'"));
}

TEST_CASE("a oneOf with no match reports the closest branch's reasons") {
    const auto form =
        JsonSchema::load(kRepo / "contracts" / "research" / "strategy_form.schema.json");
    json document = read_json(kRepo / "contracts" / "research" / "examples" / "tqqq_drawdown.json");
    document["strategy"]["reference"] = {{"type", "rolling_high"}};
    const auto errors = form.validate(document);
    REQUIRE_FALSE(errors.empty());
    // What the assistant needs in order to fix its own form: the missing field,
    // not only "matched none of the alternatives".
    CHECK(has_error_containing(errors, "window_days"));
}

TEST_CASE("a oneOf matching more than one branch fails") {
    const auto s = JsonSchema::compile({{"oneOf", {{{"type", "number"}}, {{"minimum", 0}}}}});
    CHECK_FALSE(s.is_valid(5));
    CHECK(s.is_valid(-5));
    CHECK(s.is_valid("not a number, so minimum does not apply"));
}

TEST_CASE("anyOf, allOf and if/then/else") {
    const auto any = JsonSchema::compile({{"anyOf", {{{"type", "string"}}, {{"type", "null"}}}}});
    CHECK(any.is_valid(nullptr));
    CHECK_FALSE(any.is_valid(1));

    const auto all = JsonSchema::compile({{"allOf", {{{"minimum", 1}}, {{"maximum", 2}}}}});
    CHECK(all.is_valid(1.5));
    CHECK_FALSE(all.is_valid(3));

    const auto cond = JsonSchema::compile({{"if", {{"type", "string"}}},
                                           {"then", {{"pattern", "^a"}}},
                                           {"else", {{"type", "integer"}}}});
    CHECK(cond.is_valid("abc"));
    CHECK_FALSE(cond.is_valid("bcd"));
    CHECK(cond.is_valid(4));
    CHECK_FALSE(cond.is_valid(4.5));
}

TEST_CASE("a false schema accepts nothing and a true schema everything") {
    CHECK_FALSE(JsonSchema::compile(false).is_valid(json::object()));
    CHECK(JsonSchema::compile(true).is_valid(json::array()));
}

TEST_CASE("integer means a whole number however it is written, and never a boolean") {
    const auto s = JsonSchema::compile({{"type", "integer"}});
    CHECK(s.is_valid(json::parse("2")));
    CHECK(s.is_valid(json::parse("2.0")));
    CHECK_FALSE(s.is_valid(json::parse("2.5")));
    CHECK_FALSE(s.is_valid(true));
}

TEST_CASE("enum and const compare numbers by value and never equate a boolean with 1") {
    const auto e = JsonSchema::compile({{"enum", {1, "a"}}});
    CHECK(e.is_valid(json::parse("1.0")));
    CHECK_FALSE(e.is_valid(true));
    const auto c = JsonSchema::compile({{"const", false}});
    CHECK_FALSE(c.is_valid(0));
}

TEST_CASE("a $ref cycle that consumes no input fails rather than recursing forever") {
    const auto s =
        JsonSchema::compile({{"$defs", {{"a", {{"$ref", "#/$defs/a"}}}}}, {"$ref", "#/$defs/a"}});
    CHECK_FALSE(s.is_valid(1));
}

TEST_CASE("a compiled schema survives being copied and the original going away") {
    std::optional<JsonSchema> copy;
    {
        const auto original = JsonSchema::compile(
            {{"pattern", "^x"}, {"$ref", "#/$defs/n"}, {"$defs", {{"n", {{"type", "string"}}}}}});
        copy = original;
    }
    CHECK(copy->is_valid("xyz"));
    CHECK_FALSE(copy->is_valid("abc"));
    CHECK_FALSE(copy->is_valid(1));
}

// --- Formats ------------------------------------------------------------------

TEST_CASE("date format: real calendar dates only") {
    CHECK(is_valid_date("2024-02-29"));
    CHECK(is_valid_date("2000-02-29"));
    CHECK(is_valid_date("0001-01-01"));
    CHECK_FALSE(is_valid_date("1900-02-29"));
    CHECK_FALSE(is_valid_date("2023-02-29"));
    CHECK_FALSE(is_valid_date("0000-01-01"));
    CHECK_FALSE(is_valid_date("2026-13-01"));
    CHECK_FALSE(is_valid_date("2026-04-31"));
    CHECK_FALSE(is_valid_date("2026-1-01"));
    CHECK_FALSE(is_valid_date(" 2026-01-01"));
    CHECK_FALSE(is_valid_date("2026-01-01\n"));
    CHECK_FALSE(is_valid_date("20260101"));
}

TEST_CASE("date-time format: RFC 3339, upper-case T and Z, no leap second") {
    CHECK(is_valid_date_time("2026-10-21T00:00:00Z"));
    CHECK(is_valid_date_time("2026-10-21T23:59:59.999999-04:00"));
    CHECK_FALSE(is_valid_date_time("2026-10-21t00:00:00Z"));
    CHECK_FALSE(is_valid_date_time("2026-10-21T00:00:00z"));
    CHECK_FALSE(is_valid_date_time("2026-10-21T00:00:60Z"));
    CHECK_FALSE(is_valid_date_time("2026-10-21T24:00:00Z"));
    CHECK_FALSE(is_valid_date_time("2026-10-21T00:00:00+24:00"));
    CHECK_FALSE(is_valid_date_time("2026-10-21T00:00:00+00:60"));
    CHECK_FALSE(is_valid_date_time("2026-10-21T00:00Z"));
    CHECK_FALSE(is_valid_date_time("2026-10-21T00:00:00"));
    CHECK_FALSE(is_valid_date_time("2026-10-21T00:00:00.Z"));
}

TEST_CASE("format is an assertion here, not an annotation") {
    // The vacuous-pass guard: were format only an annotation, both would pass.
    const auto s = JsonSchema::compile({{"type", "string"}, {"format", "date"}});
    CHECK(s.is_valid("2024-02-29"));
    CHECK_FALSE(s.is_valid("2023-02-29"));
}
