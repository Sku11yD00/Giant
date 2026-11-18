#include "giant/world/mods.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <charconv>
#include <set>
#include <system_error>
#include <utility>

namespace giant::world {

namespace {

using Json = nlohmann::ordered_json;

constexpr const char* k_mod_format = "giant-mod";
constexpr const char* k_mods_format = "giant-mods";
constexpr int k_version = 1;
constexpr u32 k_max_version_part = 999999;

std::optional<LoadError> bad(std::string message) { return LoadError{std::move(message)}; }

// The text parsed as an object of `format`, with only the keys named.
std::optional<LoadError> parse_object(std::string_view text, const char* format, const char* what,
                                      std::initializer_list<std::string_view> keys, Json& out) {
    if (text.size() > k_max_mod_file_bytes) {
        return bad(std::string(what) + " is over " + std::to_string(k_max_mod_file_bytes) + " bytes");
    }
    out = Json::parse(text, nullptr, false);
    if (out.is_discarded()) {
        return bad(std::string(what) + " is not JSON");
    }
    if (!out.is_object()) {
        return bad(std::string(what) + " must be an object");
    }
    const auto format_at = out.find("format");
    if (format_at == out.end() || !format_at->is_string() || format_at->get_ref<const std::string&>() != format) {
        return bad(std::string("format: must be \"") + format + "\"");
    }
    const auto version_at = out.find("format_version");
    if (version_at == out.end() || !version_at->is_number_integer() || version_at->get<i64>() != k_version) {
        return bad("format_version: must be " + std::to_string(k_version) + " (a newer editor may have written this)");
    }
    for (const auto& [key, value] : out.items()) {
        if (key != "format" && key != "format_version" && std::find(keys.begin(), keys.end(), key) == keys.end()) {
            return bad(key + ": unknown key");
        }
    }
    return std::nullopt;
}

// The string at `key` ("" when it is not there and need not be).
std::optional<LoadError> read_string(const Json& json, const char* key, bool required, std::string& out) {
    const auto at = json.find(key);
    if (at == json.end()) {
        return required ? bad(std::string(key) + ": missing") : std::nullopt;
    }
    if (!at->is_string()) {
        return bad(std::string(key) + ": must be text");
    }
    out = at->get<std::string>();
    return std::nullopt;
}

std::optional<LoadError> read_version(const Json& json, const std::string& where, ModVersion& out) {
    if (!json.is_string()) {
        return bad(where + ": must be a version as text, such as \"1.0.0\"");
    }
    const std::optional<ModVersion> version = mod_version_from_text(json.get_ref<const std::string&>());
    if (!version) {
        return bad(where + ": must be a version such as \"1.0.0\" (three numbers, each 0 to " +
                   std::to_string(k_max_version_part) + ")");
    }
    out = *version;
    return std::nullopt;
}

} // namespace

std::optional<ModVersion> mod_version_from_text(std::string_view text) {
    ModVersion version;
    u32* const parts[3] = {&version.major, &version.minor, &version.patch};
    const char* at = text.data();
    const char* const end = text.data() + text.size();
    for (int i = 0; i < 3; ++i) {
        if (at == end || *at < '0' || *at > '9') {
            return std::nullopt; // (no sign, no space)
        }
        const auto [next, error] = std::from_chars(at, end, *parts[i]);
        if (error != std::errc() || *parts[i] > k_max_version_part) {
            return std::nullopt;
        }
        at = next;
        if (i < 2) {
            if (at == end || *at != '.') {
                return std::nullopt;
            }
            ++at;
        }
    }
    return at == end ? std::optional<ModVersion>(version) : std::nullopt;
}

std::string to_string(const ModVersion& version) {
    return std::to_string(version.major) + "." + std::to_string(version.minor) + "." + std::to_string(version.patch);
}

bool mod_id_ok(std::string_view id) {
    if (id.empty() || id.size() > k_max_mod_id_bytes || id[0] < 'a' || id[0] > 'z') {
        return false;
    }
    return std::all_of(id.begin(), id.end(), [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'; });
}

std::optional<std::string> check_mod(const Mod& mod) {
    const std::string id_rule = "must be 1 to " + std::to_string(k_max_mod_id_bytes) +
                                " of a-z, 0-9 and _, beginning with a letter";
    if (!mod_id_ok(mod.id)) {
        return "id: " + id_rule;
    }
    if (mod.name.empty() || mod.name.size() > k_max_name_bytes) {
        return "name: must be 1 to " + std::to_string(k_max_name_bytes) + " bytes";
    }
    if (mod.author.size() > k_max_name_bytes) {
        return "author: must be at most " + std::to_string(k_max_name_bytes) + " bytes";
    }
    if (mod.description.size() > k_max_mod_text_bytes) {
        return "description: must be at most " + std::to_string(k_max_mod_text_bytes) + " bytes";
    }
    for (const u32 part : {mod.version.major, mod.version.minor, mod.version.patch}) {
        if (part > k_max_version_part) {
            return "version: each number must be 0 to " + std::to_string(k_max_version_part);
        }
    }
    if (mod.needs.size() > k_max_mod_needs) {
        return "needs: at most " + std::to_string(k_max_mod_needs);
    }
    if (mod.after.size() > k_max_mod_needs) {
        return "after: at most " + std::to_string(k_max_mod_needs);
    }
    std::set<std::string_view> seen;
    for (usize i = 0; i < mod.needs.size(); ++i) {
        const ModNeed& need = mod.needs[i];
        const std::string where = "needs[" + std::to_string(i) + "]";
        if (!mod_id_ok(need.id)) {
            return where + ".id: " + id_rule;
        }
        if (need.id == mod.id) {
            return where + ".id: a mod cannot need itself";
        }
        if (!seen.insert(need.id).second) {
            return where + ".id: \"" + need.id + "\" is needed twice";
        }
        for (const u32 part : {need.at_least.major, need.at_least.minor, need.at_least.patch}) {
            if (part > k_max_version_part) {
                return where + ".at_least: each number must be 0 to " + std::to_string(k_max_version_part);
            }
        }
    }
    for (usize i = 0; i < mod.after.size(); ++i) {
        const std::string where = "after[" + std::to_string(i) + "]";
        if (!mod_id_ok(mod.after[i])) {
            return where + ": " + id_rule;
        }
        if (mod.after[i] == mod.id) {
            return where + ": a mod cannot come after itself";
        }
        if (!seen.insert(mod.after[i]).second) {
            return where + ": \"" + mod.after[i] + "\" is named twice (a mod that is needed is loaded before already)";
        }
    }
    return std::nullopt;
}

std::string save_mod(const Mod& mod) {
    Json json = {{"format", k_mod_format},
                 {"format_version", k_version},
                 {"id", mod.id},
                 {"name", mod.name},
                 {"version", to_string(mod.version)}};
    if (!mod.author.empty()) {
        json["author"] = mod.author;
    }
    if (!mod.description.empty()) {
        json["description"] = mod.description;
    }
    if (!mod.needs.empty()) {
        Json needs = Json::array();
        for (const ModNeed& need : mod.needs) {
            needs.push_back({{"id", need.id}, {"at_least", to_string(need.at_least)}});
        }
        json["needs"] = std::move(needs);
    }
    if (!mod.after.empty()) {
        json["after"] = mod.after;
    }
    return json.dump(2, ' ', false, Json::error_handler_t::replace) + "\n";
}

std::optional<LoadError> load_mod(std::string_view text, Mod& out) {
    Json json;
    if (auto error = parse_object(text, k_mod_format, "the mod's mod.json",
                                  {"id", "name", "version", "author", "description", "needs", "after"}, json)) {
        return error;
    }
    Mod mod;
    if (auto error = read_string(json, "id", true, mod.id)) {
        return error;
    }
    if (auto error = read_string(json, "name", true, mod.name)) {
        return error;
    }
    const auto version = json.find("version");
    if (version == json.end()) {
        return bad("version: missing");
    }
    if (auto error = read_version(*version, "version", mod.version)) {
        return error;
    }
    if (auto error = read_string(json, "author", false, mod.author)) {
        return error;
    }
    if (auto error = read_string(json, "description", false, mod.description)) {
        return error;
    }
    if (const auto needs = json.find("needs"); needs != json.end()) {
        if (!needs->is_array() || needs->size() > k_max_mod_needs) {
            return bad("needs: must be an array of at most " + std::to_string(k_max_mod_needs) + " {id, at_least}");
        }
        for (usize i = 0; i < needs->size(); ++i) {
            const Json& n = (*needs)[i];
            const std::string where = "needs[" + std::to_string(i) + "]";
            if (!n.is_object()) {
                return bad(where + ": must be an object {id, at_least}");
            }
            for (const auto& [key, value] : n.items()) {
                if (key != "id" && key != "at_least") {
                    return bad(where + "." + key + ": unknown key");
                }
            }
            ModNeed need;
            const auto id = n.find("id");
            if (id == n.end() || !id->is_string()) {
                return bad(where + ".id: must be a mod's id");
            }
            need.id = id->get<std::string>();
            if (const auto at_least = n.find("at_least"); at_least != n.end()) {
                if (auto error = read_version(*at_least, where + ".at_least", need.at_least)) {
                    return error;
                }
            }
            mod.needs.push_back(std::move(need));
        }
    }
    if (const auto after = json.find("after"); after != json.end()) {
        if (!after->is_array() || after->size() > k_max_mod_needs) {
            return bad("after: must be an array of at most " + std::to_string(k_max_mod_needs) + " mod ids");
        }
        for (usize i = 0; i < after->size(); ++i) {
            if (!(*after)[i].is_string()) {
                return bad("after[" + std::to_string(i) + "]: must be a mod's id");
            }
            mod.after.push_back((*after)[i].get<std::string>());
        }
    }
    if (const auto wrong = check_mod(mod)) {
        return bad(*wrong);
    }
    out = std::move(mod);
    return std::nullopt;
}

std::string save_enabled_mods(std::span<const std::string> ids) {
    std::vector<std::string> sorted(ids.begin(), ids.end());
    std::sort(sorted.begin(), sorted.end());
    sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());
    const Json json = {{"format", k_mods_format}, {"format_version", k_version}, {"enabled", sorted}};
    return json.dump(2, ' ', false, Json::error_handler_t::replace) + "\n";
}

std::optional<LoadError> load_enabled_mods(std::string_view text, std::vector<std::string>& out) {
    Json json;
    if (auto error = parse_object(text, k_mods_format, "the list of mods that are on", {"enabled"}, json)) {
        return error;
    }
    const auto enabled = json.find("enabled");
    if (enabled == json.end() || !enabled->is_array() || enabled->size() > k_max_mods) {
        return bad("enabled: must be an array of at most " + std::to_string(k_max_mods) + " mod ids");
    }
    std::vector<std::string> ids;
    for (usize i = 0; i < enabled->size(); ++i) {
        const Json& id = (*enabled)[i];
        if (!id.is_string() || !mod_id_ok(id.get_ref<const std::string&>())) {
            return bad("enabled[" + std::to_string(i) + "]: must be a mod's id");
        }
        ids.push_back(id.get<std::string>());
    }
    std::sort(ids.begin(), ids.end());
    if (std::adjacent_find(ids.begin(), ids.end()) != ids.end()) {
        return bad("enabled: \"" + *std::adjacent_find(ids.begin(), ids.end()) + "\" is there twice");
    }
    out = std::move(ids);
    return std::nullopt;
}

ModOrder order_mods(std::span<const Mod> mods, std::span<const u8> on) {
    const usize count = mods.size();
    ModOrder result;
    // in: on and not left out. Each mod left out is given its one problem.
    std::vector<u8> in(count, 0);
    std::vector<std::string> why(count);
    const auto find = [&](std::string_view id) -> usize { // the first of the id
        for (usize i = 0; i < count; ++i) {
            if (mods[i].id == id) {
                return i;
            }
        }
        return count;
    };
    for (usize i = 0; i < count; ++i) {
        if (i >= on.size() || on[i] == 0) {
            continue;
        }
        if (find(mods[i].id) != i) {
            why[i] = "another mod with the id \"" + mods[i].id + "\" comes before it";
        } else {
            in[i] = 1;
        }
    }
    // Where each one's needs and afters are in the list (count: not there), found once.
    std::vector<std::vector<usize>> need_at(count), after_at(count);
    for (usize i = 0; i < count; ++i) {
        if (in[i] == 0) {
            continue;
        }
        for (const ModNeed& need : mods[i].needs) {
            need_at[i].push_back(find(need.id));
        }
        for (const std::string& id : mods[i].after) {
            after_at[i].push_back(find(id));
        }
    }
    // Those whose needs are not met go, and then those that needed them.
    for (bool changed = true; changed;) {
        changed = false;
        for (usize i = 0; i < count; ++i) {
            if (in[i] == 0) {
                continue;
            }
            for (usize n = 0; n < mods[i].needs.size(); ++n) {
                const ModNeed& need = mods[i].needs[n];
                const usize other = need_at[i][n];
                std::string problem;
                if (other == count) {
                    problem = "it needs the mod \"" + need.id + "\", which is not there";
                } else if (other >= on.size() || on[other] == 0) {
                    problem = "it needs the mod \"" + need.id + "\", which is not on";
                } else if (mods[other].version < need.at_least) {
                    problem = "it needs the mod \"" + need.id + "\" at " + to_string(need.at_least) + " or newer, and " +
                              to_string(mods[other].version) + " is there";
                } else if (in[other] == 0) {
                    problem = "it needs the mod \"" + need.id + "\", which is not loaded";
                }
                if (!problem.empty()) {
                    in[i] = 0;
                    why[i] = std::move(problem);
                    changed = true;
                    break;
                }
            }
        }
    }
    // Each in turn once all it waits for are loaded; the least id first among those ready.
    std::vector<u8> loaded(count, 0);
    const auto waits = [&](usize i) {
        for (const usize other : need_at[i]) {
            if (loaded[other] == 0) { // (there and in: checked above)
                return true;
            }
        }
        for (const usize other : after_at[i]) {
            if (other != count && in[other] != 0 && loaded[other] == 0) {
                return true;
            }
        }
        return false;
    };
    for (;;) {
        usize next = count;
        for (usize i = 0; i < count; ++i) {
            if (in[i] != 0 && loaded[i] == 0 && !waits(i) && (next == count || mods[i].id < mods[next].id)) {
                next = i;
            }
        }
        if (next == count) {
            break;
        }
        loaded[next] = 1;
        result.order.push_back(next);
    }
    // What is left waits on a circle.
    std::string circle;
    for (usize i = 0; i < count; ++i) {
        if (in[i] != 0 && loaded[i] == 0) {
            circle += (circle.empty() ? "" : ", ") + mods[i].id;
        }
    }
    for (usize i = 0; i < count; ++i) {
        if (in[i] != 0 && loaded[i] == 0) {
            why[i] = "these mods each wait for another of them to load first (needs or after): " + circle;
        }
        if (!why[i].empty()) {
            result.problems.push_back({i, std::move(why[i])});
        }
    }
    return result;
}

} // namespace giant::world
