#include "game_package.h"

#include "editor_commands.h"
#include "mesh_library.h"
#include "physics_state.h"
#include "play_state.h"
#include "world_streaming.h"

#include "giant/ai/commands.h"
#include "giant/ai/json.h"
#include "giant/core/assert.h"
#include "giant/core/text.h"
#include "giant/core/text_files.h"
#include "giant/world/material_maps.h"

#include <algorithm>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace giant::editor {

namespace {

using ai::CommandContext;
using ai::CommandResult;
using ai::Json;
namespace fs = std::filesystem;

constexpr usize k_most_text_bytes = usize{512} * 1024 * 1024; // a JSON file read whole to look for names
constexpr usize k_most_files = 200000;                          // a package's (a world of a great many cells)

std::string name_of(const fs::path& path) { return path_to_utf8(path.filename()); }
std::string extension_of(const fs::path& path) { return ascii_lower(path_to_utf8(path.extension())); }

// A path's text with '/' between its parts.
std::string slashed(const fs::path& path) {
    std::string text = path_to_utf8(path);
    std::replace(text.begin(), text.end(), '\\', '/');
    return text;
}

// The chat's key (a person's own, never a game's) and the chat's library and what it brings.
bool never_packaged(const fs::path& file) {
    const std::string name = ascii_lower(name_of(file));
    return name == "claude_key.txt" || name.starts_with("giant.chat.") || name.starts_with("anthropic.") ||
           name.starts_with("microsoft.extensions.ai.");
}

// A name Windows may read as another file's (a dot or space at its end, a short name's ~): what
// it names cannot be told from how it is spelled, so no path given by name may have one.
bool other_spelling(std::string_view part) {
    return !part.empty() && (part.back() == '.' || part.back() == ' ' || part.find('~') != std::string_view::npos);
}

// Each string literal of JSON `text`, as written between its quotes (escapes not undone), to `take`.
template <typename Take> void each_string(std::string_view text, Take&& take) {
    usize i = 0;
    while (i < text.size()) {
        if (text[i] != '"') {
            ++i;
            continue;
        }
        const usize begin = ++i;
        bool escaped = false;
        while (i < text.size() && text[i] != '"') {
            if (text[i] == '\\') {
                escaped = true;
                ++i;
            }
            ++i;
        }
        if (i >= text.size()) return;
        take(text.substr(begin, i - begin), escaped);
        ++i;
    }
}

// The string as it reads (escapes undone); nothing if it is none JSON has.
std::optional<std::string> unescaped(std::string_view literal) {
    const Json json = Json::parse("\"" + std::string(literal) + "\"", nullptr, false);
    if (!json.is_string()) return std::nullopt;
    return json.get<std::string>();
}

// The files of `folder` and all under it, sorted; not what is under its own folder named
// `skip_top` (whatever its case; empty: none). Links are not followed (a link out of the folder
// is not the folder's): `links` counts them. False when the folder cannot be read through or
// holds too many.
bool files_under(const fs::path& folder, std::string_view skip_top, std::vector<fs::path>& out, usize* links = nullptr) {
    std::error_code ec;
    fs::recursive_directory_iterator it(folder, fs::directory_options::none, ec);
    if (ec) return false;
    for (const fs::recursive_directory_iterator end; it != end; it.increment(ec)) {
        const fs::path& path = it->path();
        std::error_code kind;
        if (it->is_symlink(kind)) {
            if (links != nullptr) *links += 1;
            if (it->is_directory(kind)) it.disable_recursion_pending();
            continue;
        }
        if (it->is_directory(kind)) {
            if (!skip_top.empty() && it.depth() == 0 && ascii_lower(name_of(path)) == skip_top) it.disable_recursion_pending();
            continue;
        }
        if (!it->is_regular_file(kind)) continue;
        if (out.size() >= k_most_files) return false;
        out.push_back(path);
    }
    if (ec) return false; // (an error ends the walk as its end does: told apart here)
    std::sort(out.begin(), out.end());
    return true;
}

// What `text` names of the files there are: each string that is a path to a file, from the
// working folder, or from `prefix` under it (a .gltf's own folder; empty: the working folder).
void names_in(std::string_view text, const fs::path& working, const std::string& prefix, std::set<std::string>& seen,
              std::set<std::string>& files, std::set<std::string>& outside) {
    each_string(text, [&](std::string_view literal, bool escaped) {
        if (literal.size() < 3 || literal.size() > world::k_max_mesh_path_bytes + 32) return;
        std::string name;
        if (escaped) {
            const std::optional<std::string> plain = unescaped(literal);
            if (!plain) return;
            name = *plain;
        } else {
            name = std::string(literal);
        }
        // ("<mesh>#image<N>": an image inside the mesh's file.)
        if (const usize hash = name.rfind("#image"); hash != std::string::npos) name.resize(hash);
        if (name.empty() || name.find('\0') != std::string::npos || name.find('\n') != std::string::npos) return;
        // (A name on another machine, "//server/share/x": never looked for. Text from a world or a
        // mod must not make the editor call out, and wait for the answer.)
        if (name.starts_with("//") || name.starts_with("\\\\")) return;
        std::string key;
        fs::path keyed;
        try {
            const fs::path as_named = path_from_utf8(name);
            const bool rooted = as_named.is_absolute() || as_named.has_root_name() || as_named.has_root_directory();
            // (As the program reads a name: MeshLibrary::normalize.)
            key = MeshLibrary::normalize(rooted ? name : slashed(path_from_utf8(prefix) / as_named));
            keyed = path_from_utf8(key);
        } catch (const std::exception&) {
            return; // (not text a path can be)
        }
        if (!seen.insert(key).second) return;
        std::error_code ec;
        // (One with ".." in it is from the working folder too; an absolute one is where it says.)
        if (!fs::is_regular_file(keyed.is_absolute() ? keyed : working / keyed, ec)) return;
        (path_inside(key) ? files : outside).insert(key);
    });
}

// A .gltf names its buffers and images, from its own folder: those of every .gltf among `files`
// are added. (Each looked into once; what one names that is a .gltf is looked into as one too.)
void follow_gltf(const fs::path& working, std::set<std::string>& files, std::set<std::string>& outside, std::vector<std::string>& unread) {
    std::set<std::string> looked;
    for (bool more = true; more;) {
        more = false;
        const std::set<std::string> now = files; // (a copy: names_in adds to `files`)
        for (const std::string& file : now) {
            if (extension_of(path_from_utf8(file)) != ".gltf" || !looked.insert(file).second) continue;
            more = true;
            const std::optional<std::string> text = read_text_file(working / path_from_utf8(file), k_most_text_bytes);
            if (!text) {
                unread.push_back(file);
                continue;
            }
            std::set<std::string> seen_here; // (a name is from this file's folder: not what the same name was from another's)
            names_in(*text, working, slashed(path_from_utf8(file).parent_path()), seen_here, files, outside);
        }
    }
}

// `inner` is `outer` or under it (both as they are on the disk, as far as they are there). True
// when it cannot be told: what asks is a guard.
bool is_under(const fs::path& inner, const fs::path& outer) {
    std::error_code ec;
    const fs::path a = fs::weakly_canonical(fs::absolute(inner, ec), ec);
    if (ec || a.empty()) return true;
    const fs::path b = fs::weakly_canonical(fs::absolute(outer, ec), ec);
    if (ec || b.empty()) return true;
    return std::mismatch(a.begin(), a.end(), b.begin(), b.end()).second == b.end(); // (part by part: "world2" is not under "world")
}

// What a package is made of, before any of it is written: each file and where it goes (a path
// from the package's folder, with '/').
struct Plan {
    struct Copy {
        fs::path from;
        std::string to;
    };
    std::vector<Copy> copies;
    std::map<std::string, usize, std::less<>> by_place; // `to` in lower case (two spellings of a place are one on Windows)
    // Empty, or why `from` cannot go to `to`. The same file to the same place twice is once.
    std::string add(const fs::path& from, const std::string& to) {
        if (never_packaged(from) || never_packaged(path_from_utf8(to)))
            return path_to_utf8(from) + " is never part of a game (the chat's own)";
        // (Windows reads such a name as the one without: what it is cannot be told from its spelling.)
        if (const std::string name = name_of(from); !name.empty() && (name.back() == '.' || name.back() == ' '))
            return path_to_utf8(from) + ": a name ending in a dot or a space is not packaged";
        const auto [found, added] = by_place.emplace(ascii_lower(to), copies.size());
        if (!added) {
            std::error_code ec;
            if (fs::equivalent(copies[found->second].from, from, ec) && !ec) return {};
            return path_to_utf8(from) + " and " + path_to_utf8(copies[found->second].from) + " would both be " + to + " in the game's folder";
        }
        copies.push_back({from, to});
        return {};
    }
    // A file's place may not be under another file (`a` and `a/b`): the first such, or empty.
    std::string file_as_folder() const {
        for (const Copy& copy : copies) {
            for (usize slash = copy.to.find('/'); slash != std::string::npos; slash = copy.to.find('/', slash + 1)) {
                if (const auto over = by_place.find(ascii_lower(std::string_view(copy.to).substr(0, slash))); over != by_place.end())
                    return copy.to + " would lie under the file " + copies[over->second].to + " in the game's folder";
            }
        }
        return {};
    }
};

} // namespace

bool game_name_ok(std::string_view name) {
    if (name.empty() || name.size() > 64 || name.front() == ' ' || name.back() == ' ') return false;
    if (!std::all_of(name.begin(), name.end(), [](char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == ' ' || c == '-' || c == '_';
        }))
        return false;
    // Not what the package's own folders are called, nor a name Windows keeps for a device.
    const std::string lower = ascii_lower(name);
    for (const std::string_view taken : {"world", "managed", "scripts", "d3d12", "giant_cache", "con", "prn", "aux", "nul"})
        if (lower == taken) return false;
    const bool port = lower.size() == 4 && (lower.starts_with("com") || lower.starts_with("lpt")) && lower[3] >= '0' && lower[3] <= '9';
    return !port;
}

bool game_version_ok(std::string_view version) {
    if (version.empty() || version.size() > 32) return false;
    return std::all_of(version.begin(), version.end(), [](char c) { return c > ' ' && c < 127 && c != '"' && c != '\\'; });
}

bool path_inside(std::string_view path) {
    if (path.empty() || path.size() > 4096 || path.find('\\') != std::string_view::npos || path.find(':') != std::string_view::npos ||
        path.front() == '/' || path.back() == '/')
        return false;
    usize begin = 0;
    while (begin < path.size()) {
        const usize end = std::min(path.find('/', begin), path.size());
        const std::string_view part = path.substr(begin, end - begin);
        if (part.empty() || part == "." || part == ".." || other_spelling(part)) return false;
        begin = end + 1;
    }
    return true;
}

std::string game_info_text(const GameInfo& info) {
    const auto quoted = [](const std::string& text) { return Json(text).dump(-1, ' ', false, Json::error_handler_t::replace); };
    return "{\n  \"giant_game\": 1,\n  \"name\": " + quoted(info.name) + ",\n  \"version\": " + quoted(info.version) +
           ",\n  \"world\": " + quoted(info.world) + ",\n  \"scripts\": " + quoted(info.scripts) + "\n}\n";
}

bool is_game_info(std::string_view text) {
    const Json json = Json::parse(text, nullptr, false);
    if (!json.is_object()) return false;
    const auto mark = json.find("giant_game");
    return mark != json.end() && mark->is_number_integer() && *mark == 1;
}

std::optional<GameInfo> game_info_from_text(std::string_view text, std::string& error) {
    const Json json = Json::parse(text, nullptr, false);
    if (!json.is_object()) {
        error = "not a JSON object";
        return std::nullopt;
    }
    if (!is_game_info(text)) {
        error = "not a game's (no \"giant_game\": 1)";
        return std::nullopt;
    }
    GameInfo info;
    for (const auto& [key, out] : {std::pair<const char*, std::string*>{"name", &info.name},
                                   std::pair<const char*, std::string*>{"version", &info.version},
                                   std::pair<const char*, std::string*>{"world", &info.world},
                                   std::pair<const char*, std::string*>{"scripts", &info.scripts}}) {
        const auto found = json.find(key);
        if (found == json.end()) continue;
        if (!found->is_string()) {
            error = std::string(key) + " is not text";
            return std::nullopt;
        }
        *out = found->get<std::string>();
    }
    if (!game_name_ok(info.name)) {
        error = "name is not 1 to 64 letters, digits, spaces, - and _ (or is a name that is taken)";
    } else if (!game_version_ok(info.version)) {
        error = "version is not 1 to 32 printable characters";
    } else if (!path_inside(info.world)) {
        error = "world is not a folder beside the program";
    } else if (!info.scripts.empty() && !path_inside(info.scripts)) {
        error = "scripts is not a file beside the program";
    } else {
        return info;
    }
    return std::nullopt;
}

WorldFiles world_files(const fs::path& world_folder, const fs::path& working_folder) {
    WorldFiles out;
    std::set<std::string> seen, files, outside;
    std::vector<fs::path> all;
    if (!files_under(world_folder, "saves", all)) {
        out.error = "the world's folder could not be read through (or holds too many files)";
        return out;
    }
    for (const fs::path& file : all) {
        if (extension_of(file) != ".json") continue;
        const std::optional<std::string> text = read_text_file(file, k_most_text_bytes);
        if (!text) {
            out.error = path_to_utf8(file) + " could not be read: the files it names would be left out";
            return out;
        }
        names_in(*text, working_folder, std::string(), seen, files, outside);
    }
    follow_gltf(working_folder, files, outside, out.unread);
    out.files.assign(files.begin(), files.end());
    out.outside.assign(outside.begin(), outside.end());
    return out;
}

PackageResult package_game(const PackageRequest& request) {
    PackageResult result;
    result.info = request.info;
    result.info.world = "world";
    result.info.scripts.clear();
    std::error_code ec;
    const auto fail = [&](std::string why) {
        result.error = std::move(why);
        return result;
    };
    if (!game_name_ok(request.info.name))
        return fail("the name is 1 to 64 letters, digits, spaces, - and _, and not world, managed, scripts, D3D12, giant_cache or a "
                    "device's (it names the program)");
    if (!game_version_ok(request.info.version)) return fail("the version is 1 to 32 printable characters");
    if (!fs::is_regular_file(request.world / "world.json", ec)) return fail("no world is saved in " + path_to_utf8(request.world));
    if (!fs::is_regular_file(request.program, ec)) return fail("the program " + path_to_utf8(request.program) + " is not there");
    if (fs::exists(request.folder, ec)) {
        if (!fs::is_directory(request.folder, ec)) return fail(path_to_utf8(request.folder) + " is a file");
        if (!fs::is_empty(request.folder, ec) || ec)
            return fail(path_to_utf8(request.folder) + " is not empty: a game is packaged into a new folder (nothing is written over)");
    }
    // (Not into what is copied: it would be copied into itself.)
    if (is_under(request.folder, request.world)) return fail("the folder is inside the world's own");

    // Everything that goes in is found, and everything that cannot is said, before anything is written.
    Plan plan;
    std::string why;
    const auto add = [&](const fs::path& from, const std::string& to) {
        why = plan.add(from, to);
        return why.empty();
    };
    // The program, under the game's name, and what lies beside it that it runs with: the DirectX 12
    // runtime and the software renderer (--software).
    result.program = request.info.name + path_to_utf8(request.program.extension());
    if (!add(request.program, result.program)) return fail(why);
    const fs::path beside = request.program.parent_path();
    if (const fs::path runtime = beside / "D3D12"; fs::is_directory(runtime, ec)) {
        std::vector<fs::path> files;
        if (!files_under(runtime, {}, files)) return fail("the D3D12 folder beside the program could not be read");
        for (const fs::path& file : files) {
            const fs::path within = fs::relative(file, runtime, ec);
            if (ec || !add(file, "D3D12/" + slashed(within))) return fail(ec ? "the D3D12 folder beside the program could not be read" : why);
        }
    }
    if (const fs::path warp = beside / "d3d10warp.dll"; fs::is_regular_file(warp, ec) && !add(warp, "d3d10warp.dll")) return fail(why);
    // The C# API: the library and what .NET reads to load it. (Not its symbols or docs, not the chat, the samples or the tests.)
    const bool managed = !request.managed.empty() && fs::is_regular_file(request.managed / "Giant.Scripting.dll", ec);
    if (managed) {
        for (const char* name : {"Giant.Scripting.dll", "Giant.Scripting.deps.json", "Giant.Scripting.runtimeconfig.json"}) {
            if (fs::is_regular_file(request.managed / name, ec) && !add(request.managed / name, std::string("managed/") + name)) return fail(why);
        }
    } else {
        result.notes.emplace_back("the C# API (managed/Giant.Scripting.dll) was not found: the game runs no scripts");
    }
    // The world as saved, without its saves (a person's own games, not the game's).
    std::vector<fs::path> of_world;
    usize links = 0;
    if (!files_under(request.world, "saves", of_world, &links))
        return fail("the world's folder could not be read through (or holds too many files)");
    bool mods_scripts = false;
    for (const fs::path& file : of_world) {
        const fs::path within = fs::relative(file, request.world, ec);
        if (ec) return fail("the world's folder could not be read through");
        const std::string place = slashed(within);
        if (!add(file, "world/" + place)) return fail(why);
        mods_scripts = mods_scripts || (ascii_lower(place).starts_with("mods/") && extension_of(file) == ".dll");
    }
    if (links > 0) result.notes.push_back(std::to_string(links) + " links in the world's folder were not followed: what they lead to is not in the game");
    if (mods_scripts) result.notes.emplace_back("a mod's scripts do not run in a game yet (no one is there to allow them)");
    // The files the world names, and those asked for, at the paths they are named by.
    WorldFiles used = world_files(request.world, request.working);
    if (!used.error.empty()) return fail(used.error);
    std::set<std::string> named(used.files.begin(), used.files.end());
    std::set<std::string> outside(used.outside.begin(), used.outside.end());
    for (const std::string& asked : request.include) {
        std::string more;
        try {
            more = MeshLibrary::normalize(asked); // ("./a" is "a": one file, one place)
            while (more.size() > 1 && more.back() == '/') more.pop_back();
        } catch (const std::exception&) {
            return fail("include: " + asked + " is not a path");
        }
        if (!path_inside(more)) return fail("include: " + asked + " is not a path under the working folder");
        const fs::path path = request.working / path_from_utf8(more);
        if (fs::is_regular_file(path, ec)) {
            named.insert(more);
        } else if (fs::is_directory(path, ec)) {
            std::vector<fs::path> under;
            if (!files_under(path, {}, under)) return fail("include: " + asked + " could not be read through (or holds too many files)");
            for (const fs::path& file : under) {
                const fs::path within = fs::relative(file, path, ec);
                if (ec) return fail("include: " + asked + " could not be read through");
                std::string name = more + "/" + slashed(within);
                if (!path_inside(name)) return fail("include: " + path_to_utf8(file) + " has a name that is not packaged");
                named.insert(std::move(name));
            }
        } else {
            return fail("include: " + asked + " is not there");
        }
    }
    std::vector<std::string> unread;
    follow_gltf(request.working, named, outside, unread); // (a .gltf asked for names its files too)
    unread.insert(unread.end(), used.unread.begin(), used.unread.end());
    if (!outside.empty()) {
        std::string list;
        usize listed = 0;
        for (const std::string& one : outside) {
            if (listed++ == 8) break;
            list += (list.empty() ? "" : ", ") + one;
        }
        return fail("the world names " + std::to_string(outside.size()) +
                    " files that are not under the working folder, or whose names are not packaged, which a game's folder cannot hold at "
                    "their paths (" + list + (outside.size() > 8 ? ", ..." : "") + "): bring them under it and name them from there");
    }
    for (const std::string& name : named)
        if (!add(request.working / path_from_utf8(name), name)) return fail(why);
    for (const std::string& file : unread) result.notes.push_back(file + " could not be read: the files it names may be missing");
    // The scripts: the assembly, what .NET reads to load it, and the libraries beside it. (Not
    // every file there: a folder shared with other things is not the game's.)
    if (!request.scripts.empty()) {
        if (!fs::is_regular_file(request.scripts, ec)) return fail("the scripts " + path_to_utf8(request.scripts) + " are not there");
        if (extension_of(request.scripts) != ".dll") return fail("the scripts are an assembly (.dll)");
        if (!managed) return fail("the scripts cannot run without the C# API, which was not found");
        const fs::path folder = request.scripts.parent_path().empty() ? fs::path(".") : request.scripts.parent_path();
        const std::string stem = ascii_lower(path_to_utf8(request.scripts.stem()));
        std::vector<fs::path> files;
        fs::directory_iterator it(folder, ec);
        if (ec) return fail("the scripts' folder could not be read");
        for (const fs::directory_iterator end; it != end; it.increment(ec)) {
            std::error_code kind;
            if (!it->is_regular_file(kind)) continue;
            const std::string name = ascii_lower(name_of(it->path()));
            const bool library = extension_of(it->path()) == ".dll" && !name.starts_with("giant.scripting.") && !never_packaged(it->path());
            if (library || name == stem + ".deps.json" || name == stem + ".runtimeconfig.json") files.push_back(it->path());
        }
        if (ec) return fail("the scripts' folder could not be read");
        std::sort(files.begin(), files.end());
        for (const fs::path& file : files)
            if (!add(file, "scripts/" + name_of(file))) return fail(why);
        result.info.scripts = "scripts/" + name_of(request.scripts);
        if (!plan.by_place.contains(ascii_lower(result.info.scripts))) return fail("the scripts are never part of a game (the chat's own)");
        result.notes.push_back(std::to_string(files.size()) + " files of the scripts' folder are in scripts/ (the assembly and the libraries beside it)");
        result.notes.emplace_back("scripts need the .NET runtime on the machine the game runs on, and a game runs its scripts unasked");
    }
    if (plan.by_place.contains(ascii_lower(std::string(k_game_file)))) return fail("a file would be the game's own " + std::string(k_game_file));
    if (std::string over = plan.file_as_folder(); !over.empty()) return fail(std::move(over));
    // Written.
    fs::create_directories(request.folder, ec);
    if (ec) return fail("could not make " + path_to_utf8(request.folder) + ": " + ec.message());
    for (const Plan::Copy& copy : plan.copies) {
        const fs::path to = request.folder / path_from_utf8(copy.to);
        fs::create_directories(to.parent_path(), ec);
        if (ec || !fs::copy_file(copy.from, to, fs::copy_options::none, ec) || ec)
            return fail("could not copy " + path_to_utf8(copy.from) + " to " + path_to_utf8(to) + (ec ? ": " + ec.message() : std::string()));
        const std::uintmax_t size = fs::file_size(to, ec);
        result.files += 1;
        result.bytes += ec ? 0 : static_cast<u64>(size);
    }
    result.notes.emplace_back("a game cooks its textures the first time it runs, and keeps them and its saves in its own folder "
                              "(giant_cache/, world/saves/): it must be able to write there");
    if (!write_text_file(request.folder / path_from_utf8(std::string(k_game_file)), game_info_text(result.info)))
        return fail("could not write " + std::string(k_game_file));
    result.files += 1;
    return result;
}

void register_package_commands(ai::CommandRegistry& registry, EditorState& state) {
    const bool added = registry.add(
        {"package_game",
         "Writes the open world as a game into a new `folder` (it must not be there, or be empty; nothing is written "
         "over): the program under the game's `name` (1 to 64 letters, digits, spaces, - and _; not world, managed, "
         "scripts, D3D12, giant_cache or a device's name), the files it runs "
         "with, the C# API, the world as saved (its mods too, not its saves) under world/, every file the world's "
         "text names (meshes, the files a .gltf names, images, grooms) at the path it names it by, the `scripts` "
         "assembly (a .dll; default: the one loaded) with its .deps.json, its .runtimeconfig.json and the libraries "
         "beside it, under scripts/, and game.json (`name`, "
         "`version`, default \"1\", `world`, `scripts`). `include` adds files and folders under the working folder "
         "that scripts load by name as they run (sounds, screens). That program, started, is the game: it opens "
         "the world as --game does, with the folder as its working folder, runs its scripts unasked, and keeps its "
         "saves and its textures' cache in the folder. Never packaged: the chat and its key (refused by name). Not "
         "taken along: the saves, the textures' cache, the editor's files. Fails, writing nothing, when two files "
         "would have one place in the folder, or the world has unsaved changes "
         "(save_world first; scripts at work in the editor may keep changing it: unload them and name them with `scripts`) or names a file by an absolute path or one outside the working folder (`error` names "
         "them). Waits on the disk; not while playing. Returns `folder`, `program`, `files`, `bytes`, `game` and `notes`.",
         Json::parse(R"({"type": "object", "additionalProperties": false, "required": ["folder", "name"],
             "properties": {"folder": {"type": "string", "minLength": 1, "maxLength": 1024},
                            "name": {"type": "string", "minLength": 1, "maxLength": 64},
                            "version": {"type": "string", "minLength": 1, "maxLength": 32},
                            "scripts": {"type": "string", "maxLength": 1024},
                            "include": {"type": "array", "maxItems": 256,
                                        "items": {"type": "string", "minLength": 1, "maxLength": 1024}}}})"),
         false,
         [&state](const Json& args, CommandContext&) {
             if (state.world == nullptr || !state.world->is_open() || !state.world->active(state.scene_generation))
                 return CommandResult::failure("a game is packaged from an open world: open_world, or save_world with a folder, first");
             if (state.play != nullptr && state.play->playing()) return CommandResult::failure("not while playing: stop play first");
             WorldStreaming& w = *state.world;
             {
                 const PhysicsState::GroundAsSaved as_saved(state.physics, state.scene); // not the dents of simulating
                 if (w.unsaved_cells(state.scene) > 0 || w.unsaved_settings(state.scene))
                     return CommandResult::failure("the open world has unsaved changes: save_world first (a game is packaged as saved)");
             }
             PackageRequest request;
             std::error_code ec;
             try {
                 request.folder = path_from_utf8(args["folder"].get<std::string>());
                 request.scripts = path_from_utf8(args.value("scripts", state.scripts_loaded));
                 request.program = path_from_utf8(state.program);
                 request.managed = path_from_utf8(state.managed_folder);
             } catch (const std::exception&) {
                 return CommandResult::failure("a path is not text");
             }
             request.info.name = args["name"].get<std::string>();
             request.info.version = args.value("version", std::string("1"));
             request.world = w.folder();
             request.working = fs::current_path(ec);
             if (ec) return CommandResult::failure("the working folder cannot be told");
             if (const auto include = args.find("include"); include != args.end())
                 for (const Json& one : *include) request.include.push_back(one.get<std::string>());
             PackageResult made;
             try {
                 made = package_game(request);
             } catch (const std::exception& error) { // (a file's name that is not text, on its way to one)
                 return CommandResult::failure(std::string("the game could not be packaged: ") + error.what());
             }
             if (!made.error.empty()) return CommandResult::failure(made.error);
             return CommandResult::success({{"folder", path_to_utf8(fs::absolute(request.folder, ec))},
                                            {"program", made.program},
                                            {"files", made.files},
                                            {"bytes", made.bytes},
                                            {"game",
                                             {{"name", made.info.name},
                                              {"version", made.info.version},
                                              {"world", made.info.world},
                                              {"scripts", made.info.scripts}}},
                                            {"notes", made.notes}});
         }});
    GIANT_ASSERT(added, "package_game could not be registered");
}

} // namespace giant::editor
