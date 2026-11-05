#include "giant/ai/json.h"

#include "giant_test.h"

using namespace giant::ai;

namespace {
const Json k_schema = Json::parse(R"({
  "type": "object",
  "additionalProperties": false,
  "required": ["name", "size"],
  "properties": {
    "name": {"type": "string", "minLength": 1, "maxLength": 8},
    "size": {"type": "integer", "minimum": 1, "maximum": 10},
    "scale": {"type": "number"},
    "mode": {"enum": ["fast", "slow"]},
    "tags": {"type": "array", "maxItems": 2, "items": {"type": "string"}},
    "color": {"type": "object", "properties": {"r": {"type": "number", "maximum": 1}}}
  }
})");
} // namespace

GIANT_TEST("json: parse never throws on bad input") {
    GIANT_CHECK(parse_json("{\"a\": 1}").has_value());
    GIANT_CHECK(!parse_json("{\"a\": ").has_value());
    GIANT_CHECK(!parse_json("").has_value());
}

GIANT_TEST("json: text output survives invalid UTF-8") {
    Json value = std::string("bad \xff byte");
    GIANT_CHECK(!to_json_text(value).empty());
}

GIANT_TEST("json: valid arguments pass") {
    GIANT_CHECK(validate_args(k_schema, Json::parse(R"({"name": "a", "size": 3})")).empty());
    GIANT_CHECK(validate_args(k_schema, Json::parse(R"({"name": "a", "size": 3.0, "scale": 2.5,
        "mode": "fast", "tags": ["x"], "color": {"r": 0.5}})"))
                    .empty());
}

GIANT_TEST("json: each rule rejects bad arguments with a path") {
    const auto check = [](const char* args, const char* expected) {
        const std::string error = validate_args(k_schema, Json::parse(args));
        GIANT_CHECK(error.find(expected) != std::string::npos);
    };
    check(R"({"size": 3})", "name: is required");
    check(R"({"name": 5, "size": 3})", "name: must be a string");
    check(R"({"name": "", "size": 3})", "name: too short");
    check(R"({"name": "waytoolong", "size": 3})", "name: too long");
    check(R"({"name": "a", "size": 2.5})", "size: must be an integer");
    check(R"({"name": "a", "size": 0})", "size: below the minimum");
    check(R"({"name": "a", "size": 11})", "size: above the maximum");
    check(R"({"name": "a", "size": 3, "mode": "medium"})", "mode: must be one of");
    check(R"({"name": "a", "size": 3, "tags": ["a", "b", "c"]})", "tags: allows at most");
    check(R"({"name": "a", "size": 3, "tags": [1]})", "tags[0]: must be a string");
    check(R"({"name": "a", "size": 3, "color": {"r": 2}})", "color.r: above the maximum");
    check(R"({"name": "a", "size": 3, "extra": true})", "extra: is not a known argument");
    check(R"([1, 2])", "arguments: must be an object");
}

GIANT_TEST("json: odd schemas do not throw") {
    // additionalProperties as a schema object, a non-object schema, a huge
    // whole number given as a float.
    const Json schema = Json::parse(R"({"type": "object", "additionalProperties": {"type": "string"},
        "properties": {"n": {"type": "integer"}}})");
    GIANT_CHECK(validate_args(schema, Json::parse(R"({"x": 1, "n": 1e15})")).empty());
    GIANT_CHECK(validate_args(Json(42), Json::object()).empty());
}

GIANT_TEST("json: deeply nested input is refused instead of overflowing the stack") {
    const std::string deep = std::string(10000, '[') + std::string(10000, ']');
    GIANT_CHECK(!parse_json(deep).has_value());
    const std::string ok = std::string(60, '[') + std::string(60, ']');
    GIANT_CHECK(parse_json(ok).has_value());
    // Brackets inside strings do not count.
    GIANT_CHECK(parse_json("{\"a\": \"" + std::string(200, '[') + "\\\"\"}").has_value());
}

GIANT_TEST("json: limits written in C++ work like parsed ones") {
    const Json schema = {{"type", "string"}, {"minLength", 3}};
    GIANT_CHECK(!validate_args(schema, "a").empty());
    GIANT_CHECK(validate_args(schema, "abc").empty());
    const Json items = {{"type", "array"}, {"maxItems", 1}};
    GIANT_CHECK(!validate_args(items, Json::array({1, 2})).empty());
}

GIANT_TEST("json: integers beyond 2^53 written as floats are refused") {
    const Json schema = {{"type", "integer"}};
    GIANT_CHECK(validate_args(schema, 3.0).empty());
    GIANT_CHECK(!validate_args(schema, 1e300).empty());
}
