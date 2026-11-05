#include "giant/ai/json.h"

#include <cmath>
#include <string>

namespace giant::ai {

std::optional<Json> parse_json(std::string_view text) {
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

std::string at(const std::string& path) { return path.empty() ? "arguments" : path; }

bool has_type(const Json& value, const std::string& type) {
    if (type == "object") return value.is_object();
    if (type == "array") return value.is_array();
    if (type == "string") return value.is_string();
    if (type == "boolean") return value.is_boolean();
    if (type == "number") return value.is_number();
    if (type == "integer") {
        if (value.is_number_integer()) return true;
        // 3.0 counts as an integer, as JSON Schema says.
        if (!value.is_number_float()) return false;
        const double d = value.get<double>();
        return std::isfinite(d) && std::trunc(d) == d;
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
        if (auto it = schema.find("minLength");
            it != schema.end() && it->is_number_unsigned() && length < it->get<std::size_t>()) {
            return at(path) + ": too short";
        }
        if (auto it = schema.find("maxLength");
            it != schema.end() && it->is_number_unsigned() && length > it->get<std::size_t>()) {
            return at(path) + ": too long";
        }
    }
    if (value.is_array()) {
        if (auto it = schema.find("minItems");
            it != schema.end() && it->is_number_unsigned() && value.size() < it->get<std::size_t>()) {
            return at(path) + ": needs at least " + to_json_text(*it) + " items";
        }
        if (auto it = schema.find("maxItems");
            it != schema.end() && it->is_number_unsigned() && value.size() > it->get<std::size_t>()) {
            return at(path) + ": allows at most " + to_json_text(*it) + " items";
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
