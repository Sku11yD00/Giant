// Game saves: what a play has made of the world, kept apart from the
// world itself (save_world's files are never touched). A save is a slot,
// a folder: its manifest (save.json) names the world it is of and a chunk
// file for each section (the player, the weather) and for each cell that
// holds anything, so a big world's save holds only the cells play reached
// and a save after it writes only those that changed.
//
//   save.json                 the manifest: the commit point
//   save.prev.json            the one before it (opened if save.json is bad)
//   cells/<x>_<z>.<G>.json    a cell's chunk, as of generation G
//   sections/<name>.<G>.json  a section's chunk
//
// A commit writes its chunks under the next generation's names, then the
// manifest beside and renamed over the old one: a crash at any point
// leaves the last whole save as it was (only partial or unnamed files,
// which the next commit removes). Chunks are text the caller makes (the
// editor's JSON); this module only keeps them. Commits, opens and reads
// block on the disk: the caller runs them on a job (rule 9).
//
//   save::Changes c;
//   c.sections["player"] = player_json;
//   c.cells[{3, -2}] = damage_json;
//   const save::Committed done = save::commit(dir, base, c, meta); // on a job
#pragma once

#include "giant/core/types.h"

#include <compare>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace giant::save {

// The manifest format this engine writes (and the newest it reads).
constexpr u32 k_format = 1;
// Cells a save holds at most (a 64 km square of 64 m cells), sections, and
// a chunk's size (a cell's 65536 felled trees fit well within it).
constexpr usize k_max_cells = 1u << 20;
constexpr usize k_max_sections = 256;
constexpr usize k_max_chunk_bytes = usize{64} << 20;
// The longest play a save holds (about 31700 years).
constexpr double k_max_play_seconds = 1e12;

struct CellKey {
    i32 x = 0;
    i32 z = 0;
    auto operator<=>(const CellKey&) const = default;
};

// What a save is: which world, when, and its chunk files (relative to the slot).
struct Manifest {
    u32 format = k_format;
    std::string world_name;
    u64 world_hash = 0;      // hash_text of the world's index (world.json)
    i64 saved_unix = 0;      // seconds since 1970, when it was committed
    double play_seconds = 0; // the play's own time
    u64 generation = 0;      // 0: nothing committed yet
    std::map<std::string, std::string> sections;
    std::map<CellKey, std::string> cells;
    bool operator==(const Manifest&) const = default;
};

std::string save_manifest(const Manifest& manifest);
// Strict: unknown keys, a newer format, bad names or file names outside
// the slot's folders are errors. Replaces `out` only on success.
std::optional<std::string> load_manifest(std::string_view text, Manifest& out);

// A name for a slot or section: 1 to 64 of lowercase letters, digits, '-' and '_' (not a
// Windows device name: con, nul, com1 and the like).
bool valid_name(std::string_view name);

// What changed since the base: sections and cells (nullopt: it holds nothing now).
struct Changes {
    std::map<std::string, std::string> sections;
    std::map<CellKey, std::optional<std::string>> cells;
};

// What a commit says of itself (beside the chunks): the world's, and times.
struct Meta {
    std::string world_name;
    u64 world_hash = 0;
    i64 saved_unix = 0;
    double play_seconds = 0;
};

struct Committed {
    std::string error;  // empty: committed
    Manifest manifest;  // the slot's manifest now (on error: the base)
    u32 written = 0;    // chunk files written
    u32 removed = 0;    // files cleaned away (old generations, partial files)
};

// Commits `changes` over `base` into the slot folder (made if missing).
// `base` must be the manifest the slot holds now (from open_slot or the
// last commit), or an empty one: then the slot's old chunks are left out
// (the changes are the whole save). Blocking.
Committed commit(const std::filesystem::path& slot, const Manifest& base, const Changes& changes, const Meta& meta);

struct Opened {
    std::string error;          // empty: opened
    Manifest manifest;
    bool from_previous = false; // save.json was missing or bad: save.prev.json was opened
};
// The slot's manifest (its chunks are read when wanted). Blocking.
Opened open_slot(const std::filesystem::path& slot);

// A chunk the manifest names, read whole (nullopt: missing, unreadable or too big). Blocking.
std::optional<std::string> read_chunk(const std::filesystem::path& slot, std::string_view file);

// The slots in a folder of them, by name, each opened (a bad one with its error). Blocking.
struct Listed {
    std::string name;
    Opened opened;
};
std::vector<Listed> list_slots(const std::filesystem::path& root);

// Removes a slot folder (`name` must be valid). False: none there, or it could not.
bool delete_slot(const std::filesystem::path& root, std::string_view name);

// 64-bit FNV-1a: which world a save is of.
u64 hash_text(std::string_view text);

} // namespace giant::save
