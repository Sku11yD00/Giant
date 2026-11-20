// A game as a folder that can be handed to someone (`package_game`): the
// program under the game's name, the files it runs with, the world as
// saved, the files the world names, the scripts' assemblies and game.json.
//
//   <folder>/<name>[.exe]     the program (this one, copied)
//   <folder>/game.json        {"giant_game": 1, "name", "version", "world", "scripts"}: text
//   <folder>/D3D12/           the DirectX 12 runtime beside the program, where there is one
//   <folder>/d3d10warp.dll    the software renderer beside the program (--software), where there is one
//   <folder>/managed/         Giant.Scripting (the C# API), not the chat
//   <folder>/world/           the world's folder (its mods too), without its saves
//   <folder>/scripts/         the scripts' assembly, its .deps.json and .runtimeconfig.json, the libraries beside it
//   <folder>/<path>           each file the world names, at the path it names it by
//
// The program, started with game.json beside it, is that game: its working
// folder is its own (the world's paths are from there), it opens the world
// and loads the scripts as `--game` and `--scripts` would: unasked, as a
// program someone chose to run does what it is made to. A game.json without
// "giant_game": 1 is some other program's file and is let be. The game
// writes into its own folder as it runs: world/saves/ and giant_cache/.
//
// Everything that goes in is found before anything is written (a plan of
// copies): what cannot go in is said with nothing made. Two files for one
// place (two spellings of a path, on Windows two cases) are refused unless
// they are one file.
//
// The files a world uses are found in its text: every string in its JSON
// files (world.json, the cells, the mods') that names a file under the
// working folder is one, and for a .gltf the files it names in turn. A
// string that only looks like a file's path costs a file copied for
// nothing; a file named by an absolute path, or one outside the working
// folder, cannot go into the folder at its path and is refused by name. A
// name on another machine ("//server/...") is never looked for. Not found:
// what a .glb names outside itself, a .gltf's names written with percent
// signs.
// What scripts load by name as they run (sounds, screens) is not in the
// world's text: `include` names those.
//
// Never in it: the chat's library and any file named as the chat's key is
// (refused by name, wherever they are found). Not taken along: the world's
// saves, the textures' cache, the editor's own files, symbols beside the
// program and the scripts. (What `include` names is the person's to name.)
#pragma once

#include "giant/core/types.h"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace giant::editor {

// game.json: what the program beside it is to run.
struct GameInfo {
    std::string name;            // the game's (and its program's): see game_name_ok
    std::string version = "1";   // 1 to 32 printable ASCII characters
    std::string world = "world"; // the world's folder, from the program's
    std::string scripts;         // the scripts' assembly, from the program's (empty: none)
    bool operator==(const GameInfo&) const = default;
};
inline constexpr std::string_view k_game_file = "game.json";
// 1 to 64 of letters, digits, spaces, - and _, not starting or ending with a space, and not a name the
// package's own folders have (world, managed, scripts, D3D12, giant_cache) or Windows keeps for a device.
bool game_name_ok(std::string_view name);
bool game_version_ok(std::string_view version);
std::string game_info_text(const GameInfo& info);
// Whether `text` is a game's game.json at all (a JSON object with "giant_game": 1): one that is
// not is another program's file.
bool is_game_info(std::string_view text);
// Nothing (and `error` says why) for text that is no game.json: a name or version not as above, a
// world or scripts path that is not a relative one inside the folder.
std::optional<GameInfo> game_info_from_text(std::string_view text, std::string& error);

// A path as the world's text names it, if it is one a package can hold: relative, with '/', no "."
// or ".." part, no part ending in a dot or a space or with ~ in it.
bool path_inside(std::string_view path);

// The files `world_folder`'s text names (see above), as it names them, each once, sorted; `outside`
// those that exist but cannot go into a package at their path.
struct WorldFiles {
    std::vector<std::string> files;
    std::vector<std::string> outside;
    std::vector<std::string> unread; // .gltf files that could not be read for what they name
    std::string error;               // not empty: the world's folder or a JSON file of it could not be read
};
WorldFiles world_files(const std::filesystem::path& world_folder, const std::filesystem::path& working_folder);

struct PackageRequest {
    std::filesystem::path folder;         // made; must not be there, or be empty
    GameInfo info;                        // name and version (world and scripts are the package's own)
    std::filesystem::path program;        // the program's file
    std::filesystem::path managed;        // where Giant.Scripting.dll is (empty or not there: left out, said)
    std::filesystem::path world;          // the world's folder
    std::filesystem::path working;        // the folder the world's paths are from
    std::filesystem::path scripts;        // the scripts' assembly, a .dll (empty: none)
    std::vector<std::string> include;     // more files and folders under `working` (a .gltf's files with it)
};
struct PackageResult {
    std::string error; // empty: made
    GameInfo info;     // as written
    std::string program;
    usize files = 0;
    u64 bytes = 0;
    std::vector<std::string> notes; // what was left out that one might miss
};
// Waits on the disk (a command the person asked for). Every refusal comes before anything is
// written; only a copy that fails (a full disk) leaves a folder half made, which stays (it was
// empty before: nothing of anyone's is lost, and nothing is deleted).
PackageResult package_game(const PackageRequest& request);
// (The command: register_package_commands, editor_commands.h.)

} // namespace giant::editor
