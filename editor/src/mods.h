// The mods beside the open world (world/mods.h; living-world plan LW7):
// <world>/mods/<id>/mod.json for each, and <world>/mods/enabled.json for
// which are on. Read when asked (list_mods, enable_mod), never by a frame.
// So far a mod is its manifest: what mods hold is loaded by later parts.
#pragma once

#include "giant/core/types.h"
#include "giant/world/mods.h"

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
    std::vector<Broken> broken;       // folders under mods/ that are no mod, by name
    std::vector<std::string> missing; // on, and no such mod there (sorted)
    std::vector<std::string> order;   // the ids loaded, first to last
    std::string list_error;           // enabled.json is there and cannot be used: why (then none is on)
    bool too_many = false;            // more folders than world::k_max_mods: the rest were not looked at
};

// Where a world's mods are: <world>/mods.
std::filesystem::path mods_folder(const std::filesystem::path& world_folder);

// Reads the world's mods (no mods folder: an empty list).
ModList read_mods(const std::filesystem::path& world_folder);

// Turns the mod on or off: writes <world>/mods/enabled.json (making mods/
// when it is not there). What went wrong, or nothing. Refused when the list
// there cannot be read (it is never overwritten unread), and when a mod to
// turn on is not there. One that is not there can be turned off.
std::optional<std::string> set_mod_on(const std::filesystem::path& world_folder, const std::string& id, bool on);

} // namespace giant::editor
