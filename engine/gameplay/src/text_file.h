// Reading and writing gameplay text files: {"format": ..., "version": 1,
// ...}, strict (an unknown field is an error, so a typo never passes
// silently as a default).
#pragma once

#include "giant/core/types.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <initializer_list>
#include <system_error>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace giant::gameplay {

using Json = nlohmann::ordered_json;

inline constexpr i64 k_text_version = 1;

// The file's JSON in `root`, or why it is not a `format` file of version 1
// with only `fields` (and "format" and "version") at the top. A key given
// twice in one object is an error too (JSON would keep the last silently).
inline std::optional<std::string> parse_text_file(std::string_view text, std::string_view format,
                                                  std::initializer_list<std::string_view> fields,
                                                  Json& root) {
    // One set of keys per open object.
    std::vector<std::vector<std::string>> keys;
    std::optional<std::string> repeated;
    const Json::parser_callback_t check_keys = [&](int, Json::parse_event_t event, Json& parsed) {
        if (event == Json::parse_event_t::object_start) {
            keys.emplace_back();
        } else if (event == Json::parse_event_t::object_end) {
            keys.pop_back();
        } else if (event == Json::parse_event_t::key && !keys.empty()) {
            const std::string key = parsed.get<std::string>();
            if (std::find(keys.back().begin(), keys.back().end(), key) != keys.back().end()) {
                if (!repeated) {
                    repeated = "\"" + key + "\" is given twice in one object";
                }
            } else {
                keys.back().push_back(key);
            }
        }
        return true;
    };
    try {
        root = Json::parse(text, check_keys);
    } catch (const Json::exception& e) {
        // parse_error, and out_of_range for a number too large (1e999).
        return std::string("not valid JSON: ") + e.what();
    }
    if (repeated) {
        return repeated;
    }
    if (!root.is_object()) {
        return std::string("must be a JSON object");
    }
    const auto f = root.find("format");
    if (f == root.end() || !f->is_string() || f->get<std::string>() != format) {
        return "format must be \"" + std::string(format) + "\"";
    }
    const auto v = root.find("version");
    if (v == root.end() || !v->is_number_integer() || v->get<i64>() != k_text_version) {
        return std::string("version must be 1");
    }
    for (const auto& [key, value] : root.items()) {
        if (key != "format" && key != "version" &&
            std::find(fields.begin(), fields.end(), key) == fields.end()) {
            return key + " is not a known field";
        }
    }
    return std::nullopt;
}

// A float as the shortest decimal that reads back as the same float, so
// files say 0.73, not 0.7300000190734863 (data is text people read and diff).
inline Json short_float(float value) {
    char buffer[32];
    const auto [end, error] = std::to_chars(buffer, buffer + sizeof(buffer), value);
    if (error != std::errc{}) {
        return value;
    }
    // The double nearest that decimal prints as the same digits.
    return std::strtod(std::string(buffer, end).c_str(), nullptr);
}

// {"format": format, "version": 1} followed by `fields`.
inline Json text_file_header(std::string_view format, std::vector<std::pair<std::string, Json>> fields) {
    Json root = {{"format", std::string(format)}, {"version", k_text_version}};
    for (auto& [name, value] : fields) {
        root[name] = std::move(value);
    }
    return root;
}

// Reads the fields of one JSON object; the first problem wins and names
// where it is ("items[3].weight_kg must be a number").
class FieldReader {
public:
    FieldReader(const Json& object, std::string where) : object_(object), where_(std::move(where)) {
        if (!object_.is_object()) {
            error_ = where_ + " must be an object";
        }
    }

    void string(const char* name, std::string& out, bool required) {
        if (const Json* j = get(name, required)) {
            if (!j->is_string()) {
                fail(name, "must be text");
            } else {
                out = j->get<std::string>();
            }
        }
    }
    void number(const char* name, float& out, bool required) {
        if (const Json* j = get(name, required)) {
            const double d = j->is_number() ? j->get<double>() : std::nan("");
            if (!std::isfinite(d) || std::abs(d) > static_cast<double>(std::numeric_limits<float>::max())) {
                fail(name, "must be a number");
            } else {
                out = static_cast<float>(d);
            }
        }
    }
    void whole(const char* name, u32& out, bool required) {
        if (const Json* j = get(name, required)) {
            if (!j->is_number_integer() || j->get<i64>() < 0 ||
                j->get<i64>() > static_cast<i64>(std::numeric_limits<u32>::max())) {
                fail(name, "must be a whole number, 0 or more");
            } else {
                out = static_cast<u32>(j->get<i64>());
            }
        }
    }
    void boolean(const char* name, bool& out, bool required) {
        if (const Json* j = get(name, required)) {
            if (!j->is_boolean()) {
                fail(name, "must be true or false");
            } else {
                out = j->get<bool>();
            }
        }
    }

    // The array under `name` (none when it is missing and not required, or
    // not an array, which fails).
    const Json* array(const char* name, bool required) {
        const Json* j = get(name, required);
        if (j != nullptr && !j->is_array()) {
            fail(name, "must be an array");
            return nullptr;
        }
        return j;
    }

    // Fails on a field nothing read.
    void finish() {
        if (error_) {
            return;
        }
        for (const auto& [key, value] : object_.items()) {
            bool known = false;
            for (const std::string& r : read_) {
                known = known || r == key;
            }
            if (!known) {
                fail(key.c_str(), "is not a known field");
                return;
            }
        }
    }

    const std::optional<std::string>& error() const { return error_; }

private:
    const Json* get(const char* name, bool required) {
        if (error_) {
            return nullptr;
        }
        read_.emplace_back(name);
        const auto it = object_.find(name);
        if (it == object_.end()) {
            if (required) {
                fail(name, "is missing");
            }
            return nullptr;
        }
        return &*it;
    }
    void fail(const char* name, const char* why) {
        if (!error_) {
            error_ = where_ + "." + name + " " + why;
        }
    }

    const Json& object_;
    std::string where_;
    std::vector<std::string> read_;
    std::optional<std::string> error_;
};

} // namespace giant::gameplay
