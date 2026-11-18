// The mods beside the open world (world/mods.h; living-world plan LW7):
// <world>/mods/<id>/mod.json for each, and <world>/mods/enabled.json for
// which are on. Read when asked (list_mods, enable_mod), never by a frame.
// A mod's world.json (places, person looks, routines, behaviours) is laid
// over the world when it is opened (lay_mods); assets and scripts are parts
// to come.
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
// world.json files, world/mods.h), and the world's things as they were just
// then: what save_world tells the world's own from (world::world_own).
struct ModsLaid {
    bool any = false; // a mod changed something: the four lists are what there was then
    std::vector<world::Place> places;
    std::vector<world::PersonLook> person_looks;
    std::vector<world::Routine> routines;
    std::vector<world::Behaviour> behaviours;
    std::vector<std::string> mods; // the ids of the mods loaded then, in order
    world::ModsApplied applied;    // what each did, and those left out (places in `mods`)
};
// Lays the world's loaded mods over the scene's places, person looks,
// routines and behaviours (the world's own, just read). A mod with no
// world.json changes nothing; one whose world.json cannot be used is left
// out with why (in `applied.problems`). Reads the disk.
ModsLaid lay_mods(const std::filesystem::path& world_folder, world::Scene& scene);

// Where a world's mods are: <world>/mods.
std::filesystem::path mods_folder(const std::filesystem::path& world_folder);

// Reads the world's mods (no mods folder: an empty list).
ModList read_mods(const std::filesystem::path& world_folder);

// Turns the mod on or off: writes <world>/mods/enabled.json (making mods/
// when it is not there). What went wrong, or nothing. Refused when the list
// there cannot be read (it is never overwritten unread), and when a mod to
// turn on is not there. One that is not there can be turned off.
// A folder that is a link is no mod (a mod's files are its own folder's).
std::optional<std::string> set_mod_on(const std::filesystem::path& world_folder, const std::string& id, bool on);

} // namespace giant::editor
