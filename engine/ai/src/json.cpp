#include "giant/ai/json.h"

#include <cmath>
#include <cstdint>
#include <string>

namespace giant::ai {

namespace {

// Deeper documents are refused: copying or printing JSON recurses once per
// level, so a hostile message could otherwise overflow the stack.
constexpr int k_max_depth = 64;

bool within_depth(std::string_view text) {
    int depth = 0;
    bool in_string = false;
    bool escaped = false;
    for (const char c : text) {
        if (in_string) {
            if (escaped) {
                escaped = false;
            } else if (c == '\\') {
                escaped = true;
            } else if (c == '"') {
                in_string = false;
            }
        } else if (c == '"') {
            in_string = true;
        } else if (c == '[' || c == '{') {
            if (++depth > k_max_depth) {
                return false;
            }
        } else if (c == ']' || c == '}') {
            --depth;
        }
    }
    return true;
}

} // namespace

std::optional<Json> parse_json(std::string_view text) {
    if (!within_depth(text)) {
        return std::nullopt;
    }
    Json value = Json::parse(text.begin(), text.end(), nullptr, /*allow_exceptions=*/false);
    if (value.is_discarded()) {
        return std::nullopt;
    }
    return value;
}

std::string to_json_text(const Json& value, int indent) {
    return value.dump(indent, ' ', /*ensure_ascii=*/false, Json::error_handler_t::replace);
}

namespace {

// A non-negative whole-number keyword (minLength etc.), however it was
// written: parsed text gives unsigned numbers, C++ initialisers signed ones.
std::optional<std::size_t> count_keyword(const Json& schema, const char* key) {
    const auto it = schema.find(key);
    if (it == schema.end()) return std::nullopt;
    if (it->is_number_unsigned()) return it->get<std::size_t>();
    if (it->is_number_integer() && it->get<std::int64_t>() >= 0) {
        return static_cast<std::size_t>(it->get<std::int64_t>());
    }
    return std::nullopt;
}

std::string at(const std::string& path) { return path.empty() ? "arguments" : path; }

bool has_type(const Json& value, const std::string& type) {
    if (type == "object") return value.is_object();
    if (type == "array") return value.is_array();
    if (type == "string") return value.is_string();
    if (type == "boolean") return value.is_boolean();
    if (type == "number") return value.is_number();
    if (type == "integer") {
        if (value.is_number_integer()) return true;
        // 3.0 counts as an integer, as JSON Schema says. Floats beyond 2^53
        // are refused: they cannot be converted to an integer type safely.
        if (!value.is_number_float()) return false;
        const double d = value.get<double>();
        return std::isfinite(d) && std::trunc(d) == d && std::fabs(d) <= 9007199254740992.0;
    }
    if (type == "null") return value.is_null();
    return false;
}

std::string validate(const Json& schema, const Json& value, const std::string& path) {
    if (!schema.is_object()) {
        return {};
    }
    if (auto it = schema.find("type"); it != schema.end() && it->is_string()) {
        const std::string type = it->get<std::string>();
        if (!has_type(value, type)) {
            return at(path) + ": must be " + (type == "integer" || type == "array" || type == "object"
                                                  ? "an " + type
                                                  : "a " + type);
        }
    } else if (it != schema.end() && it->is_array()) { // one of several types
        bool any = false;
        for (const Json& type : *it) any = any || (type.is_string() && has_type(value, type.get<std::string>()));
        if (!any) return at(path) + ": must be one of the types " + to_json_text(*it);
    }
    if (auto it = schema.find("enum"); it != schema.end() && it->is_array()) {
        bool found = false;
        for (const Json& option : *it) {
            found = found || option == value;
        }
        if (!found) {
            return at(path) + ": must be one of " + to_json_text(*it);
        }
    }
    if (value.is_number()) {
        const double number = value.get<double>();
        if (auto it = schema.find("minimum"); it != schema.end() && it->is_number() &&
                                              number < it->get<double>()) {
            return at(path) + ": below the minimum " + to_json_text(*it);
        }
        if (auto it = schema.find("maximum"); it != schema.end() && it->is_number() &&
                                              number > it->get<double>()) {
            return at(path) + ": above the maximum " + to_json_text(*it);
        }
    }
    if (value.is_string()) {
        // Length in bytes; good enough for the limits commands set.
        const auto length = value.get_ref<const std::string&>().size();
        if (const auto min = count_keyword(schema, "minLength"); min && length < *min) {
            return at(path) + ": too short";
        }
        if (const auto max = count_keyword(schema, "maxLength"); max && length > *max) {
            return at(path) + ": too long";
        }
    }
    if (value.is_array()) {
        if (const auto min = count_keyword(schema, "minItems"); min && value.size() < *min) {
            return at(path) + ": needs at least " + std::to_string(*min) + " items";
        }
        if (const auto max = count_keyword(schema, "maxItems"); max && value.size() > *max) {
            return at(path) + ": allows at most " + std::to_string(*max) + " items";
        }
        if (auto it = schema.find("items"); it != schema.end()) {
            for (std::size_t i = 0; i < value.size(); ++i) {
                std::string error = validate(*it, value[i], path + "[" + std::to_string(i) + "]");
                if (!error.empty()) {
                    return error;
                }
            }
        }
    }
    if (value.is_object()) {
        const std::string prefix = path.empty() ? "" : path + ".";
        if (auto it = schema.find("required"); it != schema.end() && it->is_array()) {
            for (const Json& name : *it) {
                if (name.is_string() && !value.contains(name.get<std::string>())) {
                    return prefix + name.get<std::string>() + ": is required";
                }
            }
        }
        const auto properties_it = schema.find("properties");
        const Json* properties =
            properties_it != schema.end() && properties_it->is_object() ? &*properties_it : nullptr;
        const auto extra = schema.find("additionalProperties");
        const bool closed = extra != schema.end() && extra->is_boolean() && !extra->get<bool>();
        for (const auto& [key, item] : value.items()) {
            const auto property = properties != nullptr ? properties->find(key) : schema.end();
            if (properties != nullptr && property != properties->end()) {
                std::string error = validate(*property, item, prefix + key);
                if (!error.empty()) {
                    return error;
                }
            } else if (closed) {
                return prefix + key + ": is not a known argument";
            }
        }
    }
    return {};
}

} // namespace

std::string validate_args(const Json& schema, const Json& args) { return validate(schema, args, ""); }

} // namespace giant::ai
