#include "giant/world/mods.h"
#include "giant/world/people.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <functional>
#include <initializer_list>
#include <map>
#include <string_view>
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

// A key as an error shows it: its start, when it is long (it may be most of the file).
std::string shown_key(const std::string& key) { return key.size() <= 64 ? key : key.substr(0, 64) + "..."; }

// Text for people, shown in lists and to the AI: no control characters (a
// description may have new lines and tabs).
bool plain_text(std::string_view text, bool lines) {
    return std::all_of(text.begin(), text.end(), [lines](char c) {
        const unsigned char u = static_cast<unsigned char>(c);
        return (u >= 0x20 && u != 0x7f) || (lines && (c == '\n' || c == '\t'));
    });
}

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
        const bool newer = version_at != out.end() && version_at->is_number_integer() && version_at->get<i64>() > k_version;
        return bad("format_version: must be " + std::to_string(k_version) + (newer ? " (a newer editor wrote this)" : ""));
    }
    for (const auto& [key, value] : out.items()) {
        if (key != "format" && key != "format_version" && std::find(keys.begin(), keys.end(), key) == keys.end()) {
            return bad(shown_key(key) + ": unknown key");
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
        if (at == end || *at < '0' || *at > '9' || (*at == '0' && at + 1 != end && at[1] >= '0' && at[1] <= '9')) {
            return std::nullopt; // (no sign, no space, no 0 before a number: one way to write each)
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
    // (Names Windows keeps for devices: a folder cannot have them there.)
    for (const std::string_view device : {"con", "prn", "aux", "nul"}) {
        if (id == device) {
            return false;
        }
    }
    if (id.size() == 4 && (id.starts_with("com") || id.starts_with("lpt")) && id[3] >= '0' && id[3] <= '9') {
        return false;
    }
    return std::all_of(id.begin(), id.end(), [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'; });
}

std::optional<std::string> check_mod(const Mod& mod) {
    const std::string id_rule = "must be 1 to " + std::to_string(k_max_mod_id_bytes) +
                                " of a-z, 0-9 and _, beginning with a letter (and not a device's name on Windows: con, "
                                "prn, aux, nul, com0 to com9, lpt0 to lpt9)";
    if (!mod_id_ok(mod.id)) {
        return "id: " + id_rule;
    }
    if (mod.name.empty() || mod.name.size() > k_max_name_bytes) {
        return "name: must be 1 to " + std::to_string(k_max_name_bytes) + " bytes";
    }
    if (!plain_text(mod.name, false) || !plain_text(mod.author, false)) {
        return std::string(!plain_text(mod.name, false) ? "name" : "author") + ": must be one line of plain text (no control characters)";
    }
    if (!plain_text(mod.description, true)) {
        return "description: must be plain text (no control characters but new lines and tabs)";
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
                    return bad(where + "." + shown_key(key) + ": unknown key");
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

std::string save_enabled_mods(const ModsOn& on) {
    std::vector<std::string> sorted(on.ids.begin(), on.ids.end());
    std::sort(sorted.begin(), sorted.end());
    sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());
    Json json = {{"format", k_mods_format}, {"format_version", k_version}, {"enabled", sorted}};
    if (!on.slots.empty()) {
        // By id; an id or a slot there twice keeps its first (so that what is written always loads).
        std::vector<ModSlot> given(on.slots.begin(), on.slots.end());
        std::stable_sort(given.begin(), given.end(), [](const ModSlot& l, const ModSlot& r) { return l.id < r.id; });
        std::set<u32> taken;
        Json slots = Json::object();
        for (const ModSlot& slot : given) {
            if (!slots.contains(slot.id) && slot.slot < k_max_mod_slots && taken.insert(slot.slot).second) {
                slots.emplace(slot.id, slot.slot);
            }
        }
        json["slots"] = std::move(slots);
    }
    return json.dump(2, ' ', false, Json::error_handler_t::replace) + "\n";
}

std::optional<u32> find_mod_slot(const ModsOn& on, std::string_view id) {
    for (const ModSlot& slot : on.slots) {
        if (slot.id == id) {
            return slot.slot;
        }
    }
    return std::nullopt;
}

std::optional<u32> give_mod_slot(ModsOn& on, std::string_view id) {
    if (const auto has = find_mod_slot(on, id)) {
        return has;
    }
    std::vector<u8> taken(k_max_mod_slots, 0);
    for (const ModSlot& slot : on.slots) {
        if (slot.slot < k_max_mod_slots) {
            taken[slot.slot] = 1;
        }
    }
    const auto free_one = std::find(taken.begin(), taken.end(), u8{0});
    if (free_one == taken.end()) {
        return std::nullopt;
    }
    const u32 slot = static_cast<u32>(free_one - taken.begin());
    const auto at = std::lower_bound(on.slots.begin(), on.slots.end(), id, [](const ModSlot& l, std::string_view r) { return l.id < r; });
    on.slots.insert(at, ModSlot{std::string(id), slot});
    return slot;
}

std::optional<LoadError> load_enabled_mods(std::string_view text, ModsOn& out) {
    Json json;
    if (auto error = parse_object(text, k_mods_format, "the list of mods that are on", {"enabled", "slots"}, json)) {
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
    if (const auto twice = std::adjacent_find(ids.begin(), ids.end()); twice != ids.end()) {
        return bad("enabled: \"" + *twice + "\" is there twice");
    }
    // The slots given (need not be there: none given yet). A key written twice is read as its last.
    std::vector<ModSlot> slots;
    if (const auto given = json.find("slots"); given != json.end()) {
        if (!given->is_object() || given->size() > k_max_mod_slots) {
            return bad("slots: must be an object of at most " + std::to_string(k_max_mod_slots) + " mod ids, each with its slot");
        }
        std::vector<u8> taken(k_max_mod_slots, 0);
        for (const auto& [id, slot] : given->items()) {
            if (!mod_id_ok(id) || !slot.is_number_unsigned() || slot.get<u64>() >= k_max_mod_slots) {
                return bad("slots: \"" + id + "\" must be a mod's id with a slot from 0 to " + std::to_string(k_max_mod_slots - 1));
            }
            const u32 number = slot.get<u32>();
            if (taken[number] != 0) {
                return bad("slots: slot " + std::to_string(number) + " is given twice");
            }
            taken[number] = 1;
            slots.push_back({id, number});
        }
        std::sort(slots.begin(), slots.end(), [](const ModSlot& l, const ModSlot& r) { return l.id < r.id; });
    }
    out.ids = std::move(ids);
    out.slots = std::move(slots);
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
    std::vector<u8> loaded(count, 0);
    // Whether `other` is one to wait for: in, and not loaded yet.
    const auto waits_for = [&](usize other) { return other != count && in[other] != 0 && loaded[other] == 0; };
    const auto waits = [&](usize i) {
        return std::any_of(need_at[i].begin(), need_at[i].end(), [&](usize other) { return waits_for(other); }) ||
               std::any_of(after_at[i].begin(), after_at[i].end(), [&](usize other) { return waits_for(other); });
    };
    std::vector<u8> reached(count * count, 0); // (i, j): j is waited for by i, directly or through others
    std::vector<usize> stack;
    std::vector<u8> was_in;
    for (;;) {
        // Those whose needs are not met go, and then those that needed them.
        // (Each pass judges by who was in before it: what a mod is told does not depend on the list's order.)
        for (bool changed = true; changed;) {
            changed = false;
            was_in = in;
            for (usize i = 0; i < count; ++i) {
                if (in[i] == 0) {
                    continue;
                }
                for (usize n = 0; n < need_at[i].size(); ++n) {
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
                    } else if (was_in[other] == 0) {
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
        // What is left waits on a circle, or is in one. Those in one go, each
        // circle named by its own; the rest are looked at again (one that
        // needed them goes for that, one that only came after them loads).
        std::fill(reached.begin(), reached.end(), u8{0});
        bool stalled = false;
        for (usize i = 0; i < count; ++i) {
            if (in[i] == 0 || loaded[i] != 0) {
                continue;
            }
            stalled = true;
            stack.assign(1, i);
            while (!stack.empty()) {
                const usize at = stack.back();
                stack.pop_back();
                for (const std::vector<usize>* list : {&need_at[at], &after_at[at]}) {
                    for (const usize other : *list) {
                        if (waits_for(other) && reached[i * count + other] == 0) {
                            reached[i * count + other] = 1;
                            stack.push_back(other);
                        }
                    }
                }
            }
        }
        if (!stalled) {
            break;
        }
        for (usize i = 0; i < count; ++i) {
            if (in[i] == 0 || loaded[i] != 0 || reached[i * count + i] == 0) {
                continue; // (not in a circle itself)
            }
            std::vector<std::string_view> circle; // those it waits for that wait for it, and itself
            for (usize j = 0; j < count; ++j) {
                if (j == i || (reached[i * count + j] != 0 && reached[j * count + i] != 0)) {
                    circle.push_back(mods[j].id);
                }
            }
            std::sort(circle.begin(), circle.end());
            why[i] = "these mods each wait for another of them to load first (needs or after): ";
            constexpr usize k_most_named = 8; // (a circle may be every mod there is)
            for (usize c = 0; c < circle.size() && c < k_most_named; ++c) {
                why[i] += (c == 0 ? "" : ", ") + std::string(circle[c]);
            }
            if (circle.size() > k_most_named) {
                why[i] += " and " + std::to_string(circle.size() - k_most_named) + " more";
            }
        }
        // (After all are named: taking one out as it is found would hide its circle from the next.)
        for (usize i = 0; i < count; ++i) {
            if (in[i] != 0 && loaded[i] == 0 && reached[i * count + i] != 0) {
                in[i] = 0;
            }
        }
    }
    for (usize i = 0; i < count; ++i) {
        if (!why[i].empty()) {
            result.problems.push_back({i, std::move(why[i])});
        }
    }
    return result;
}

std::string_view mod_thing_name(ModThing thing) {
    switch (thing) {
    case ModThing::place: return "place";
    case ModThing::person_look: return "person_look";
    case ModThing::routine: return "routine";
    case ModThing::behaviour: return "behaviour";
    }
    return "place";
}

namespace {

// One kind of thing while mods are laid over it: where each name is (found
// once, when a mod first touches the kind), who set each last, and what the
// mod being laid did, so that it can be undone.
template <typename T>
struct Laying {
    std::vector<T>& into;
    ModThing thing;
    std::map<std::string, usize, std::less<>> at;     // every name's place in `into` (its own copies: `into` grows)
    std::map<std::string, usize, std::less<>> set_by; // the mod (its place in the list) that set a name last
    bool found = false;
    struct Done {
        usize index = 0;
        std::optional<T> was;         // replaced: what was there; added: nothing
        std::optional<usize> set_was; // who had set the name before, if a mod had
    };
    std::vector<Done> done; // what the mod being laid did, in order

    // Lays the mod's things over the kind; what is wrong with them, or nothing.
    std::optional<std::string> lay(const std::vector<T>& from, usize mod, std::vector<ModChange>& changes) {
        if (from.empty()) {
            return std::nullopt;
        }
        if (!found) {
            found = true;
            for (usize i = 0; i < into.size(); ++i) {
                at.emplace(into[i].name, i);
            }
        }
        for (const T& one : from) {
            ModChange change{mod, thing, one.name, ModChange::Did::added, 0};
            Done did;
            if (const auto by = set_by.find(one.name); by != set_by.end()) {
                if (by->second == mod) { // (refused when a mod's file is loaded; given here all the same)
                    return "it sets the " + std::string(mod_thing_name(thing)) + " \"" + one.name + "\" twice";
                }
                did.set_was = by->second;
            }
            if (const auto place = at.find(one.name); place != at.end()) {
                if (did.set_was) {
                    change.did = ModChange::Did::replaced_mod;
                    change.over = *did.set_was;
                } else {
                    change.did = ModChange::Did::replaced_world;
                }
                did.index = place->second;
                did.was = std::move(into[place->second]);
                into[place->second] = one;
            } else {
                did.index = into.size();
                at.emplace(one.name, into.size());
                into.push_back(one);
            }
            set_by.insert_or_assign(one.name, mod);
            done.push_back(std::move(did));
            changes.push_back(std::move(change));
        }
        return std::nullopt;
    }

    // Undoes what the mod being laid did (the last first).
    void undo() {
        for (usize i = done.size(); i-- > 0;) {
            Done& did = done[i];
            const std::string name = into[did.index].name;
            if (did.was) {
                into[did.index] = std::move(*did.was);
            } else {
                into.pop_back(); // (added ones are the last, undone last first)
                at.erase(name);
            }
            if (did.set_was) {
                set_by.insert_or_assign(name, *did.set_was);
            } else {
                set_by.erase(name);
            }
        }
        done.clear();
    }
};

} // namespace

namespace {

// Puts the mod's people among the world's, under the ids of its slot; what is wrong, or nothing.
// `at`: where they were put (their count is the mod's).
std::optional<std::string> lay_people(const std::vector<Person>& from, u32 slot, std::vector<Person>& into, usize& at) {
    if (slot >= k_max_mod_slots) {
        return "it brings people and has no slot in the world's list of mods (enable_mod it again: that gives it one)";
    }
    if (from.size() > k_max_mod_people) {
        return "a mod brings at most " + std::to_string(k_max_mod_people) + " people";
    }
    u32 last = 0;
    for (const Person& one : from) { // (refused when a mod's file is loaded; looked at here all the same)
        if (one.id <= last || one.id > k_max_mod_people) {
            return "its people must be numbered from 1 to " + std::to_string(k_max_mod_people) + ", in order, each once";
        }
        last = one.id;
    }
    const PersonId first = mod_person_id(slot, 0), past = mod_person_id(slot, k_max_mod_people) + 1;
    const auto where = std::lower_bound(into.begin(), into.end(), first, [](const Person& l, PersonId r) { return l.id < r; });
    if (where != into.end() && where->id < past) {
        return "there is already a person of an id that is this mod's to give (person " + std::to_string(where->id) +
               "): the world's own, or a mod's of the same slot";
    }
    at = static_cast<usize>(where - into.begin());
    into.insert(where, from.begin(), from.end());
    for (usize i = 0; i < from.size(); ++i) {
        into[at + i].id = mod_person_id(slot, from[i].id);
    }
    return std::nullopt;
}

template <typename Same>
void own_people(std::span<const Person> brought, std::span<const Person> now, std::vector<std::string>* not_kept, const Same& own) {
    usize b = 0;
    for (const Person& one : now) {
        while (b < brought.size() && brought[b].id < one.id) {
            if (not_kept != nullptr) not_kept->push_back("person " + std::to_string(brought[b].id)); // (gone)
            ++b;
        }
        if (b < brought.size() && brought[b].id == one.id) {
            if (not_kept != nullptr && !(brought[b] == one)) not_kept->push_back("person " + std::to_string(one.id));
            ++b;
            continue;
        }
        if (!own(one)) {
            return;
        }
    }
    for (; not_kept != nullptr && b < brought.size(); ++b) {
        not_kept->push_back("person " + std::to_string(brought[b].id));
    }
}

} // namespace

std::vector<Person> world_own_people(std::span<const Person> brought, std::span<const Person> now, std::vector<std::string>* not_kept) {
    std::vector<Person> out;
    own_people(brought, now, not_kept, [&out](const Person& one) {
        out.push_back(one);
        return true;
    });
    return out;
}

bool world_own_people_are(std::span<const Person> own, std::span<const Person> brought, std::span<const Person> now) {
    usize count = 0;
    bool same = true;
    own_people(brought, now, nullptr, [&](const Person& one) {
        same = count < own.size() && own[count] == one;
        ++count;
        return same;
    });
    return same && count == own.size();
}

ModsApplied apply_mod_worlds(std::span<const ModWorld* const> mods, std::span<const u32> slots, std::vector<Place>& places,
                             std::vector<PersonLook>& person_looks, std::vector<Routine>& routines, std::vector<Behaviour>& behaviours,
                             std::vector<Person>& people, PersonId next_person_id) {
    ModsApplied applied;
    Laying<Place> lay_places{places, ModThing::place, {}, {}, false, {}};
    Laying<PersonLook> lay_looks{person_looks, ModThing::person_look, {}, {}, false, {}};
    Laying<Routine> lay_routines{routines, ModThing::routine, {}, {}, false, {}};
    Laying<Behaviour> lay_behaviours{behaviours, ModThing::behaviour, {}, {}, false, {}};
    for (usize m = 0; m < mods.size(); ++m) {
        if (mods[m] == nullptr || mods[m]->empty()) {
            continue;
        }
        const ModWorld& mod = *mods[m];
        const usize changes_before = applied.changes.size();
        std::optional<std::string> wrong = lay_places.lay(mod.places, m, applied.changes);
        if (!wrong) wrong = lay_looks.lay(mod.person_looks, m, applied.changes);
        if (!wrong) wrong = lay_routines.lay(mod.routines, m, applied.changes);
        if (!wrong) wrong = lay_behaviours.lay(mod.behaviours, m, applied.changes);
        usize people_at = 0;
        bool people_laid = false;
        if (!wrong && !mod.people.empty()) {
            wrong = lay_people(mod.people, m < slots.size() ? slots[m] : k_no_mod_slot, people, people_at);
            people_laid = !wrong;
        }
        // The world with it must be one that loads.
        if (!wrong) {
            if (const auto people_wrong = check_people(places, person_looks, routines, people, next_person_id)) {
                wrong = "the world would not load with them (" + *people_wrong + ")";
            } else if (const auto behaviours_wrong = check_behaviours(behaviours, people)) {
                wrong = "the world would not load with them (" + *behaviours_wrong + ")";
            }
        }
        if (wrong) {
            if (people_laid) {
                people.erase(people.begin() + static_cast<std::ptrdiff_t>(people_at),
                             people.begin() + static_cast<std::ptrdiff_t>(people_at + mod.people.size()));
            }
            lay_behaviours.undo();
            lay_routines.undo();
            lay_looks.undo();
            lay_places.undo();
            applied.changes.resize(changes_before);
            applied.problems.push_back({m, "its world changes were left out: " + *wrong});
        } else {
            if (people_laid) {
                applied.people.push_back({m, mod.people.size()});
            }
            lay_places.done.clear();
            lay_looks.done.clear();
            lay_routines.done.clear();
            lay_behaviours.done.clear();
        }
    }
    return applied;
}

} // namespace giant::world
