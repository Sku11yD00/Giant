// Mods (living-world plan LW7): what a mod says of itself, and the order
// mods load in. A mod is a folder beside a world (<world>/mods/<id>/) with a
// manifest, mod.json (format "giant-mod"): its id (the folder's name), a
// name and version for people, the mods it needs (each at a version or
// newer) and the mods it loads after when they are there. Which mods are
// on is the world's own list (<world>/mods/enabled.json, format
// "giant-mods"). Here: the manifest, the list, and the order; reading the
// folders is the editor's (mods_state.h).
#pragma once

#include "giant/core/types.h"
#include "giant/world/scene.h"

#include <compare>
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

// A version as people write it: "1.4.2" (each part 0 to 999999).
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
    std::string id;          // its own among mods: 1 to k_max_mod_id_bytes of a-z, 0-9 and _, a letter first
    std::string name;        // for people: 1 to k_max_name_bytes
    ModVersion version;
    std::string author;      // at most k_max_name_bytes
    std::string description; // at most k_max_mod_text_bytes
    std::vector<ModNeed> needs;     // each id once, never its own; loaded before it
    std::vector<std::string> after; // mods it loads after when they are on (need not be there); each once, none it needs
    bool operator==(const Mod&) const = default;
};

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
// old or itself left out, or when it is in (or needs one in) a circle of
// mods each waiting for another. Nothing is left out without a problem.
// `on` says which are on (as long as `mods`).
ModOrder order_mods(std::span<const Mod> mods, std::span<const u8> on);

} // namespace giant::world
