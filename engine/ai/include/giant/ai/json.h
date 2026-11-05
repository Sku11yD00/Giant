// The JSON type used by commands and the MCP server.
//
// Rules: parse with `parse_json` (never throws), write with `to_json_text`
// (never throws on bad UTF-8), and read fields with the checked helpers below
// or after `validate_args`; no exceptions may leave the ai module.
#pragma once

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <string_view>

namespace giant::ai {

using Json = nlohmann::json;

// Null if `text` is not valid JSON.
std::optional<Json> parse_json(std::string_view text);

// Compact (indent < 0) or indented text; invalid UTF-8 is replaced.
std::string to_json_text(const Json& value, int indent = -1);

// Checks `args` against the subset of JSON Schema that commands use:
// type (object, string, number, integer, boolean, array), properties,
// required, additionalProperties: false, enum, minimum, maximum, items,
// minItems, maxItems, minLength, maxLength. Returns an empty string when the
// value matches, otherwise what is wrong and where ("color.r: above 1").
std::string validate_args(const Json& schema, const Json& args);

} // namespace giant::ai
