#ifndef MARKETSCAN_RESEARCH_JSON_SCHEMA_HPP
#define MARKETSCAN_RESEARCH_JSON_SCHEMA_HPP

/*
 * A JSON Schema validator for the research contracts in contracts/research/.
 *
 * A SUBSET, AND IT SAYS SO
 *
 * This implements the part of JSON Schema 2020-12 the contracts use, and
 * **refuses to compile a schema that uses anything else.** The alternative --
 * ignoring an unknown keyword, which is what the standard permits -- means a
 * `minLength` or `patternProperties` added to a contract next month would be
 * silently skipped, and every document would pass a rule nobody was checking.
 * The output of a validator that skipped a rule is identical to the output of
 * one that enforced it, which is the vacuous-pass shape docs/AUDIT.md is about.
 * So the supported set is closed, and growing it is an edit here with a test.
 *
 * Supported:
 *   applicators  $ref (local "#/..." only), $defs, properties,
 *                additionalProperties, items, allOf, anyOf, oneOf,
 *                if / then / else
 *   assertions   type, enum, const, required, minimum, maximum,
 *                exclusiveMinimum, exclusiveMaximum, minItems, maxItems,
 *                pattern, format ("date", "date-time")
 *   annotations  $schema, $id, $comment, title, description, default,
 *                examples, and any key beginning "x-"
 *
 * `format` is an assertion here, not an annotation. The engine parses these
 * dates, so a form that carries "2026-02-30" is invalid rather than merely
 * unusual. contracts/README.md records the same choice for the Python side.
 *
 * ONE OF TWO IMPLEMENTATIONS
 *
 * The other is research/contracts.py, on the `jsonschema` package. Both are run
 * over data/samples/research/schema_cases.json and must reproduce every
 * expected verdict. Neither is derived from the other.
 */

#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace marketscan::research {

/// A schema that cannot be compiled: malformed, an unsupported keyword, an
/// unresolvable $ref, or a pattern that is not a valid regular expression.
class SchemaCompileError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// One reason a document failed.
struct SchemaError {
    /// JSON Pointer into the document, "" for the root.
    std::string instance_path;

    /// JSON Pointer into the schema, to the keyword that failed.
    std::string schema_path;

    std::string message;
};

class JsonSchema {
public:
    /// Throws SchemaCompileError.
    [[nodiscard]] static JsonSchema compile(nlohmann::json schema);

    /// Reads and compiles a schema file. Throws SchemaCompileError, including
    /// when the file is missing or is not JSON.
    [[nodiscard]] static JsonSchema load(const std::filesystem::path& path);

    /// Every failure, in document order as far as the schema's structure
    /// allows. Empty means valid.
    ///
    /// Where a oneOf matches no branch, the errors reported are those of the
    /// branch that came closest (one whose `const` discriminator matched, then
    /// fewest errors, then first), because "no
    /// branch matched" alone tells a reader -- or the assistant fixing its own
    /// form -- nothing about what to change.
    [[nodiscard]] std::vector<SchemaError> validate(const nlohmann::json& document) const;

    [[nodiscard]] bool is_valid(const nlohmann::json& document) const;

private:
    struct Impl;

    explicit JsonSchema(std::shared_ptr<const Impl> impl) : impl_(std::move(impl)) {}

    std::shared_ptr<const Impl> impl_;
};

/// "YYYY-MM-DD", a real calendar date. Exposed for the tests.
[[nodiscard]] bool is_valid_date(const std::string& text);

/// RFC 3339 date-time: a valid date, "T", hh:mm:ss with optional fraction, and
/// "Z" or a numeric offset. Seconds run 00-59; a leap second is refused.
[[nodiscard]] bool is_valid_date_time(const std::string& text);

}  // namespace marketscan::research

#endif  // MARKETSCAN_RESEARCH_JSON_SCHEMA_HPP
