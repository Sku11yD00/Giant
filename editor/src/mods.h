// The mods beside the open world (world/mods.h; living-world plan LW7):
// <world>/mods/<id>/mod.json for each, and <world>/mods/enabled.json for
// which are on. Read when asked (list_mods, enable_mod), never by a frame.
// A mod's world.json (places, person looks, routines, behaviours, kits, people)
// is laid over the world when it is opened (lay_mods), its own files
// ("mod:...") found in its folder; scripts are the part to come.
#pragma once

#include "giant/core/types.h"
#include "giant/world/mods.h"
#include "giant/world/scene.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace giant::editor {

struct ModList {
    struct Found {
        world::Mod mod;
        bool on = false;
        usize place = 0;     // loaded: its place in the order, from 1; else 0
        std::optional<u32> slot; // its slot in the world's list (world::ModSlot), when it was ever given one
        std::string problem; // on and not loaded: why
    };
    struct Broken {
        std::string folder; // its name under mods/
        std::string error;  // why it is no mod
    };
    std::vector<Found> mods;          // by id
    std::vector<Broken> broken;       // folders under mods/ that are no mod, by name (none whose name begins with . or _)
    std::vector<std::string> missing; // on, and no such mod there (sorted)
    std::vector<std::string> order;   // the ids loaded, first to last
    std::string list_error;           // enabled.json is there and cannot be used: why (then none is on)
    std::string folder_error;         // mods/ is there and could not be read through: why (the lists may be short)
    bool too_many = false;            // more folders than world::k_max_mods: the rest were not looked at
};

// What the mods that load changed of a world as it was opened (their
// world.json files, world/mods.h), and the things they set as they left
// them: what save_world tells the world's own from (world::world_own).
// These stay as they are for as long as the world is open.
struct ModsLaid {
    bool any = false; // a mod changed something
    // The things mods set (added or replaced), each as the last mod to set it left it.
    std::vector<world::Place> places;
    std::vector<world::PersonLook> person_looks;
    std::vector<world::Routine> routines;
    std::vector<world::Behaviour> behaviours;
    std::vector<world::Kit> kits;
    std::vector<world::Person> people; // those mods added, as they left them (in the order of their ids)
    std::vector<std::string> mods; // the ids of the mods loaded then, in order
    std::vector<u32> slots;        // each one's slot then (as long as `mods`; world::k_no_mod_slot: it had none)
    world::ModsApplied applied;    // what each did, and those left out (places in `mods`)
    // Each loaded mod's world.json as it was then (as long as `mods`): to tell when one changed since.
    struct File {
        bool there = false;
        u64 bytes = 0;
        i64 written = 0; // its last write time, in the file clock's ticks
        bool operator==(const File&) const = default;
    };
    std::vector<File> files;
};
// A mod's world.json as it is now (ModsLaid::files).
ModsLaid::File mod_world_file(const std::filesystem::path& world_folder, const std::string& id);
// Lays the world's loaded mods over the scene's places, person looks,
// routines, behaviours, kits and people (the world's own, just read). A mod with no
// world.json changes nothing; one whose world.json cannot be used is left
// out with why (in `applied.problems`). The mod's own files' paths
// (world::resolve_mod_files) become paths in <world>/mods/<id>/. Reads the disk.
ModsLaid lay_mods(const std::filesystem::path& world_folder, world::Scene& scene);

// Where a world's mods are: <world>/mods.
std::filesystem::path mods_folder(const std::filesystem::path& world_folder);

// Reads the world's mods (no mods folder: an empty list).
ModList read_mods(const std::filesystem::path& world_folder);

// Turns the mod on or off: writes <world>/mods/enabled.json (making mods/
// when it is not there). One turned on is given its slot there when it has
// none (kept when it is turned off: its people's ids come from it). What
// went wrong, or nothing. Refused when the list
// there cannot be read (it is never overwritten unread), and when a mod to
// turn on is not there. One that is not there can be turned off.
// A folder that is a link is no mod (a mod's files are its own folder's).
std::optional<std::string> set_mod_on(const std::filesystem::path& world_folder, const std::string& id, bool on);

} // namespace giant::editor
