#ifndef MARKETSCAN_APP_SCHEMA_FORM_HPP
#define MARKETSCAN_APP_SCHEMA_FORM_HPP

/*
 * What the Research view's form editor needs to know about a JSON Schema.
 *
 * SPEC_RESEARCH_VIEW.md section 4.2: the form is drawn from the schema, one
 * widget per type, and no widget is written for a particular field -- adding
 * a template must not need GUI code. The drawing is in panels/research.cpp;
 * the decisions it makes about the schema are here, where a test can reach
 * them without a window.
 */

#include <cstddef>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace marketscan::app::schema_form {

/// `node` with its `$ref` chain followed, against the schema's root.
[[nodiscard]] const nlohmann::json& resolve(const nlohmann::json& root, const nlohmann::json& node);

/// A value that satisfies `node` as far as a default can: `const`, then
/// `default`, then a type-compatible `x-default`, then the first enum value,
/// the first oneOf branch, the required properties of an object, and the
/// smallest permitted number.
[[nodiscard]] nlohmann::json default_for(const nlohmann::json& root, const nlohmann::json& node);

/// Which branch of a `oneOf` the value belongs to: the first whose type,
/// constants and required properties it matches, or 0 if none does.
[[nodiscard]] std::size_t
branch_of(const nlohmann::json& root, const nlohmann::json& one_of, const nlohmann::json& value);

/// A short name for a branch: its discriminating constant if it has one
/// ("rolling_high"), its first required key ("sweep"), or its type ("value",
/// "none").
[[nodiscard]] std::string branch_label(const nlohmann::json& root, const nlohmann::json& branch);

/// The order to show an object's properties in: its `required` list as written,
/// then the rest by name. A parsed document has lost the schema's own key
/// order, and `required` is the one place the author's order survives --
/// "start" before "end", "min" before "max".
[[nodiscard]] std::vector<std::string> property_order(const nlohmann::json& node);

/// "threshold_pct" -> "threshold %". For a label, not an identifier.
[[nodiscard]] std::string label_for(const std::string& key);

/// A JSON Pointer token, escaped.
[[nodiscard]] std::string pointer_token(const std::string& key);

}  // namespace marketscan::app::schema_form

#endif  // MARKETSCAN_APP_SCHEMA_FORM_HPP
