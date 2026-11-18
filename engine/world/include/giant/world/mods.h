// Mods (living-world plan LW7): what a mod says of itself, and the order
// mods load in. A mod is a folder beside a world (<world>/mods/<id>/) with a
// manifest, mod.json (format "giant-mod"): its id (the folder's name), a
// name and version for people, the mods it needs (each at a version or
// newer) and the mods it loads after when they are there. Which mods are
// on is the world's own list (<world>/mods/enabled.json, format
// "giant-mods"). Here: the manifest, the list, and the order; reading the
// folders is the editor's (editor/src/mods.h).
#pragma once

#include "giant/core/types.h"
#include "giant/world/scene.h"

#include <compare>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace giant::world {

constexpr usize k_max_mods = 256;             // beside one world
constexpr usize k_max_mod_id_bytes = 64;
constexpr usize k_max_mod_needs = 32;         // and as many `after`
constexpr usize k_max_mod_text_bytes = 2048;  // a description
constexpr usize k_max_mod_file_bytes = 65536; // a manifest, or the list of those on

// A version as people write it: "1.4.2" (each part 0 to 999999, written without a 0 before it).
struct ModVersion {
    u32 major = 0, minor = 0, patch = 0;
    auto operator<=>(const ModVersion&) const = default;
};
std::optional<ModVersion> mod_version_from_text(std::string_view text);
std::string to_string(const ModVersion& version);

// Another mod this one cannot do without, at `at_least` or newer.
struct ModNeed {
    std::string id;
    ModVersion at_least;
    bool operator==(const ModNeed&) const = default;
};
struct Mod {
    std::string id;          // its own among mods: 1 to k_max_mod_id_bytes of a-z, 0-9 and _, a letter first (mod_id_ok)
    std::string name;        // for people: 1 to k_max_name_bytes, one line, no control characters (author too)
    ModVersion version;
    std::string author;      // at most k_max_name_bytes
    std::string description; // at most k_max_mod_text_bytes; no control characters but new lines and tabs
    std::vector<ModNeed> needs;     // each id once, never its own; loaded before it
    std::vector<std::string> after; // mods it loads after when they are on (need not be there); each once, none it needs
    bool operator==(const Mod&) const = default;
};

// (Not a name Windows keeps for a device either: con, prn, aux, nul, com0 to com9, lpt0 to lpt9.)
bool mod_id_ok(std::string_view id);
// What is wrong with it, or nothing.
std::optional<std::string> check_mod(const Mod& mod);

// JSON text, stable key order, with a trailing newline.
std::string save_mod(const Mod& mod);
// Replaces `out` only on success; strict, like load_scene.
std::optional<LoadError> load_mod(std::string_view text, Mod& out);

// The mods that are on, by id (each once, sorted).
std::string save_enabled_mods(std::span<const std::string> ids);
std::optional<LoadError> load_enabled_mods(std::string_view text, std::vector<std::string>& out);

// Why a mod that is on is not loaded.
struct ModProblem {
    usize mod = 0; // its place in the list given
    std::string message;
    bool operator==(const ModProblem&) const = default;
};
struct ModOrder {
    std::vector<usize> order;         // the mods loaded, first to last (places in the list given)
    std::vector<ModProblem> problems; // every mod that is on and not loaded, by place
    bool operator==(const ModOrder&) const = default;
};
// The order the mods that are on load in: each after all it needs and all
// it asks to come after, and otherwise by id, so the same mods always load
// the same way. A mod is left out, with the reason, when another of its id
// came before it in the list, when one it needs is not there, not on, too
// old or itself left out, or when it is in a circle of mods each waiting
// for another (by needs or after; each circle is named by its own mods).
// One that only asked to come after a mod left out loads without it.
// Nothing is left out without a problem.
// `on` says which are on (as long as `mods`).
ModOrder order_mods(std::span<const Mod> mods, std::span<const u8> on);

// --- What a mod changes of the world (LW7 part 2) ----------------------------
//
// A mod's world.json (format "giant-mod-world", beside its mod.json; need
// not be there): places, person looks, routines and behaviours, written as
// a world's own are. One whose name the world (or an earlier mod) has
// already takes its place, whole; any other is added. The world's files
// are not touched: what mods change is laid over the world as it is opened.
struct ModWorld {
    std::vector<Place> places;
    std::vector<PersonLook> person_looks;
    std::vector<Routine> routines;
    std::vector<Behaviour> behaviours;
    bool empty() const { return places.empty() && person_looks.empty() && routines.empty() && behaviours.empty(); }
    bool operator==(const ModWorld&) const = default;
};
// JSON text, stable key order, with a trailing newline.
std::string save_mod_world(const ModWorld& world);
// Replaces `out` only on success; strict, like load_scene. Each thing is
// checked alone and no name may be there twice. (What it names of the world,
// a routine's place say, is not looked at: as in a world's own file, a
// place that is not there is one nobody can be sent to.)
std::optional<LoadError> load_mod_world(std::string_view text, ModWorld& out);

// The kinds of thing a mod changes, as changes name them.
enum class ModThing : u8 { place, person_look, routine, behaviour };
std::string_view mod_thing_name(ModThing thing);

// One thing a mod did to the world.
struct ModChange {
    usize mod = 0; // its place in the list applied
    ModThing thing = ModThing::place;
    std::string name;
    enum class Did : u8 {
        added,          // new to the world
        replaced_world, // the world had one of the name: the mod's is used
        replaced_mod,   // an earlier mod had set it (`over`): this one's is used. A clash between mods.
    } did = Did::added;
    usize over = 0; // replaced_mod: the earlier mod's place in the list applied
    bool operator==(const ModChange&) const = default;
};
struct ModsApplied {
    std::vector<ModChange> changes;   // in the order done
    std::vector<ModProblem> problems; // mods whose world changes were left out, whole, with why (by place)
    bool operator==(const ModsApplied&) const = default;
};
// Lays the mods' changes over the world's lists, in the order given (the
// load order: a later mod's wins, and every such clash is in `changes`).
// A mod's changes go in whole or not at all: when the world would not be
// one that loads with them (more of a kind than a world keeps), or it sets
// a thing twice, they are left out and the reason given. `people` are the world's (they are not changed).
ModsApplied apply_mod_worlds(std::span<const ModWorld* const> mods, std::vector<Place>& places, std::vector<PersonLook>& person_looks,
                             std::vector<Routine>& routines, std::vector<Behaviour>& behaviours, std::span<const Person> people,
                             PersonId next_person_id);

// The world's own things of a kind, told apart from the mods', for saving
// the world while mods are on (its files stay its own, and it must load
// without them): given what the world's file holds (`own`), the things mods
// set as they left them (`brought`: added or replaced) and what there is
// `now`. A name mods set is the mods' for as long as the world is open: the
// world's own of that name is kept as the file has it (none, if it had
// none), whatever was done to the mods' thing since; those so changed or
// removed are named in `not_kept`, for telling the person. Every other name
// is the world's: as it is now, or gone if it is gone. In the order of
// `own`, then the new ones in the order of `now`.
template <typename T>
std::vector<T> world_own(std::span<const T> own, std::span<const T> brought, std::span<const T> now,
                         std::vector<std::string>* not_kept = nullptr) {
    const auto by_name = [](std::span<const T> list) {
        std::map<std::string_view, const T*> map;
        for (const T& one : list) {
            map.emplace(one.name, &one);
        }
        return map;
    };
    const std::map<std::string_view, const T*> owned = by_name(own), mods = by_name(brought), there = by_name(now);
    std::vector<T> out;
    for (const T& one : own) {
        if (mods.contains(one.name)) {
            out.push_back(one);
        } else if (const auto at = there.find(one.name); at != there.end()) {
            out.push_back(*at->second);
        }
    }
    for (const T& one : now) {
        if (!owned.contains(one.name) && !mods.contains(one.name)) {
            out.push_back(one);
        }
    }
    if (not_kept != nullptr) {
        for (const T& one : brought) {
            const auto at = there.find(one.name);
            if (at == there.end() || !(*at->second == one)) {
                not_kept->push_back(one.name);
            }
        }
    }
    return out;
}

} // namespace giant::world
