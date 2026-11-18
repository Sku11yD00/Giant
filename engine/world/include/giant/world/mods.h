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
constexpr usize k_max_mod_scripts_bytes = 256; // the path of a mod's scripts

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
    // Its scripts (part 3d): a C# assembly in its own folder, by its path there ("scripts/MyMod.dll": a .dll,
    // under the rules of a mod's own files, check_mod_file_path), or none. Scripts are programs: they run
    // only where the person at the machine allowed them (ModScriptsAllowed).
    std::string scripts;
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

// A mod's number among a world's mods, its own for good (kept when it is
// turned off): the ids of the people it brings come from it
// (mod_person_id), so they stay the same whatever else is on.
struct ModSlot {
    std::string id;
    u32 slot = 0; // under k_max_mod_slots
    bool operator==(const ModSlot&) const = default;
};
// The world's list: the mods that are on, and every slot given.
struct ModsOn {
    std::vector<std::string> ids; // those on: each once, sorted
    std::vector<ModSlot> slots;   // by id; each id once and each slot once
    bool operator==(const ModsOn&) const = default;
};
// (Sorts both, and leaves out an id there twice and a slot given twice or past the last: what is written loads.)
std::string save_enabled_mods(const ModsOn& on);
std::optional<LoadError> load_enabled_mods(std::string_view text, ModsOn& out);
// The mod's slot, or nothing when it has none.
std::optional<u32> find_mod_slot(const ModsOn& on, std::string_view id);
// The mod's slot, given now when it had none (the lowest free); nothing when none is free.
std::optional<u32> give_mod_slot(ModsOn& on, std::string_view id);

// A mod whose scripts may run on this machine. A script is a program with
// the person's own rights, so that is theirs to say, and it is kept with
// the machine (the editor's settings folder), never with the world: a
// world got from someone else runs nothing until it is allowed here.
struct ModScriptsAllowed {
    std::string world; // the world's folder, as the editor names it (whole, from the root): mod_scripts_world_ok
    std::string mod;   // the mod's id
    auto operator<=>(const ModScriptsAllowed&) const = default;
};
constexpr usize k_max_mods_scripts_allowed = 1024;
constexpr usize k_max_allowed_world_bytes = 1024;
constexpr usize k_max_mods_scripts_allowed_bytes = usize{4} << 20; // the file
// Whether a world's folder can be kept in the list: 1 to k_max_allowed_world_bytes of UTF-8 on one line, no
// control characters (so that what is written is read back the same).
bool mod_scripts_world_ok(std::string_view world);
// JSON text (format "giant-mods-scripts"), sorted, each once, with a trailing newline. (Leaves out what
// would not load: one past the most, a world's folder that is empty or too long, an id that is none.)
std::string save_mods_scripts_allowed(std::span<const ModScriptsAllowed> allowed);
// Replaces `out` only on success; strict. Sorted.
std::optional<LoadError> load_mods_scripts_allowed(std::string_view text, std::vector<ModScriptsAllowed>& out);

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
// And people (part 3): always added, never in the place of another. They
// are written as a world's are but for their ids, which are numbers in the
// mod (1 to k_max_mod_people, in order; no next_id): in the world, number n
// of the mod with slot s is person mod_person_id(s, n). They may use the
// world's looks, places, routines and behaviours, an earlier mod's, or the
// mod's own.
struct ModWorld {
    std::vector<Place> places;
    std::vector<PersonLook> person_looks;
    std::vector<Routine> routines;
    std::vector<Behaviour> behaviours;
    std::vector<Person> people;
    // Kits (part 3c), by name as the rest: one the world has is replaced, whole (its placements then build
    // the mod's); any other is added, for the mod's scripts to place.
    std::vector<Kit> kits;
    bool empty() const {
        return places.empty() && person_looks.empty() && routines.empty() && behaviours.empty() && people.empty() && kits.empty();
    }
    bool operator==(const ModWorld&) const = default;
};
// JSON text, stable key order, with a trailing newline.
std::string save_mod_world(const ModWorld& world);
// Replaces `out` only on success; strict, like load_scene. Each thing is
// checked alone and no name may be there twice. (What it names of the world,
// a routine's place say, is not looked at: as in a world's own file, a
// place that is not there is one nobody can be sent to. What its people
// use is looked at when it is laid over the world.)
std::optional<LoadError> load_mod_world(std::string_view text, ModWorld& out);

// --- The files a mod names (part 3b) ------------------------------------------
//
// A file a mod's world.json names (a look's mesh, its hair's file, a kit
// piece's mesh, their materials' image maps and skin detail) is the mod's own when it is
// written "mod:<path>": a path in the mod's folder. So a mod is one folder
// that can be copied whole. Any other is a path from the folder the game
// runs in, as a world's own are. Either way a mod is not trusted with the
// rest of the machine: the path has forward slashes and never leaves its
// folder (no "..", no ".", no empty part, so no root and no server; no
// colon, so no drive), and has nothing Windows reads as something else (a
// backslash, a part ending in a dot or a space or beginning with a space,
// a device's name, any of < > " | ? *) and no control character.
constexpr std::string_view k_mod_file_prefix = "mod:";
// What is wrong with a path a mod's world names, or nothing ("": none, nothing wrong).
std::optional<std::string> check_mod_file_path(std::string_view path);
// What is wrong among all the files its world names, with which (as "person_looks[2].mesh: ..."), or nothing.
std::optional<std::string> check_mod_files(const ModWorld& world);
// Whether its world names any file of the mod's own ("mod:...").
bool has_mod_own_files(const ModWorld& world);
// Makes each of the mod's own files' paths one a file is opened by:
// `mod_folder` (UTF-8, forward slashes, as the editor would open it), "/",
// and the path in it. What is wrong, or nothing; `world` is changed only
// when nothing is (a path longer than a scene keeps, k_max_mesh_path_bytes, is).
std::optional<std::string> resolve_mod_files(ModWorld& world, std::string_view mod_folder);

// The kinds of thing a mod changes, as changes name them.
enum class ModThing : u8 { place, person_look, routine, behaviour, kit };
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
// The people a mod added to the world.
struct ModPeople {
    usize mod = 0;   // its place in the list applied
    usize count = 0; // how many (their ids: mod_person_id of its slot and each one's number)
    bool operator==(const ModPeople&) const = default;
};
constexpr u32 k_no_mod_slot = 0xffffffffu;
struct ModsApplied {
    std::vector<ModChange> changes;   // in the order done
    std::vector<ModPeople> people;    // by mod: those that added any
    std::vector<ModProblem> problems; // mods whose world changes were left out, whole, with why (by place)
    bool operator==(const ModsApplied&) const = default;
};
// Lays the mods' changes over the world's lists, in the order given (the
// load order: a later mod's wins, and every such clash is in `changes`).
// A mod's changes go in whole or not at all: when the world would not be
// one that loads with them (more of a kind than a world keeps), or it sets
// a thing twice, they are left out and the reason given. So too when its
// people use what is not there, when it brings people and has no slot
// (`slots`, as long as `mods`: each one's, or k_no_mod_slot), or when the
// world already has a person of an id in its slot's range. The people there
// are never changed: a mod's are put among them, in the order of their ids.
ModsApplied apply_mod_worlds(std::span<const ModWorld* const> mods, std::span<const u32> slots, std::vector<Place>& places,
                             std::vector<PersonLook>& person_looks, std::vector<Routine>& routines, std::vector<Behaviour>& behaviours,
                             std::vector<Kit>& kits, std::vector<Person>& people, PersonId next_person_id);

// The world's own people, told from the mods' for saving the world while
// mods are on: those there `now` but the ones mods brought (`brought`, as
// the mods left them; both in the order of their ids). A person a mod
// brought is the mod's for as long as the world is open: never written,
// whatever was done to it since; those changed or removed are named in
// `not_kept` ("person <id>").
std::vector<Person> world_own_people(std::span<const Person> brought, std::span<const Person> now, std::vector<std::string>* not_kept = nullptr);
// Whether world_own_people(brought, now) would be `own`, without making it.
bool world_own_people_are(std::span<const Person> own, std::span<const Person> brought, std::span<const Person> now);

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
