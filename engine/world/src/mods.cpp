#include "giant/world/mods.h"
#include "giant/world/people.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <charconv>
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
    if (const auto twice = std::adjacent_find(ids.begin(), ids.end()); twice != ids.end()) {
        return bad("enabled: \"" + *twice + "\" is there twice");
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

// Who set each thing of a kind last, by name: a mod's place in the list (the world's own are not in it).
using SetBy = std::map<std::string, usize, std::less<>>;

// Lays `from` over `into` by name, noting each change (and who set what, in `set_by`).
template <typename T>
void lay_over(const std::vector<T>& from, std::vector<T>& into, SetBy& set_by, usize mod, ModThing thing,
              std::vector<ModChange>& changes) {
    if (from.empty()) {
        return;
    }
    // (Found by a map made once: a mod may bring as many as a world keeps.)
    // (Its own copies of the names: adding to `into` moves the things it holds.)
    std::map<std::string, usize, std::less<>> at;
    for (usize i = 0; i < into.size(); ++i) {
        at.emplace(into[i].name, i);
    }
    for (const T& one : from) {
        ModChange change{mod, thing, one.name, ModChange::Did::added, 0};
        if (const auto found = at.find(one.name); found != at.end()) {
            if (const auto by = set_by.find(one.name); by != set_by.end()) {
                change.did = ModChange::Did::replaced_mod;
                change.over = by->second;
            } else {
                change.did = ModChange::Did::replaced_world;
            }
            into[found->second] = one;
        } else {
            // (A name twice within a mod is refused when its file is loaded; given here all the same,
            // the second is added too, and the check of the whole then refuses the mod.)
            into.push_back(one);
        }
        set_by.insert_or_assign(one.name, mod);
        changes.push_back(std::move(change));
    }
}

} // namespace

ModsApplied apply_mod_worlds(std::span<const ModWorld* const> mods, std::vector<Place>& places, std::vector<PersonLook>& person_looks,
                             std::vector<Routine>& routines, std::vector<Behaviour>& behaviours, std::span<const Person> people,
                             PersonId next_person_id) {
    ModsApplied applied;
    SetBy set_places, set_looks, set_routines, set_behaviours;
    for (usize m = 0; m < mods.size(); ++m) {
        if (mods[m] == nullptr || mods[m]->empty()) {
            continue;
        }
        const ModWorld& mod = *mods[m];
        // As they were, for the kinds it touches: back to this when it cannot go in.
        const std::vector<Place> places_before = mod.places.empty() ? std::vector<Place>() : places;
        const std::vector<PersonLook> looks_before = mod.person_looks.empty() ? std::vector<PersonLook>() : person_looks;
        const std::vector<Routine> routines_before = mod.routines.empty() ? std::vector<Routine>() : routines;
        const std::vector<Behaviour> behaviours_before = mod.behaviours.empty() ? std::vector<Behaviour>() : behaviours;
        const SetBy set_before[4] = {set_places, set_looks, set_routines, set_behaviours};
        const usize changes_before = applied.changes.size();
        lay_over(mod.places, places, set_places, m, ModThing::place, applied.changes);
        lay_over(mod.person_looks, person_looks, set_looks, m, ModThing::person_look, applied.changes);
        lay_over(mod.routines, routines, set_routines, m, ModThing::routine, applied.changes);
        lay_over(mod.behaviours, behaviours, set_behaviours, m, ModThing::behaviour, applied.changes);
        std::optional<std::string> wrong = check_people(places, person_looks, routines, people, next_person_id);
        if (!wrong) {
            wrong = check_behaviours(behaviours, people);
        }
        if (wrong) {
            if (!mod.places.empty()) places = places_before;
            if (!mod.person_looks.empty()) person_looks = looks_before;
            if (!mod.routines.empty()) routines = routines_before;
            if (!mod.behaviours.empty()) behaviours = behaviours_before;
            set_places = set_before[0];
            set_looks = set_before[1];
            set_routines = set_before[2];
            set_behaviours = set_before[3];
            applied.changes.resize(changes_before);
            applied.problems.push_back({m, "its world changes were left out: with them, " + *wrong});
        }
    }
    return applied;
}

} // namespace giant::world
