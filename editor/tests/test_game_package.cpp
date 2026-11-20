// Packaging a game (game_package.h): game.json as text, the files a world's text names, and the
// folder as written. The command and the packaged program run end to end in tools/game_smoke.py.
#include "../src/game_package.h"

#include "giant/core/text.h"
#include "giant/core/text_files.h"

#include "giant_test.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <optional>
#include <set>
#include <string>
#include <system_error>
#include <vector>

using namespace giant;
using namespace giant::editor;
namespace fs = std::filesystem;

namespace {

fs::path fresh_folder() {
    static std::atomic<int> n{0};
    const fs::path p = fs::temp_directory_path() /
                       ("giant_package_test_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "_" +
                        std::to_string(n++));
    fs::remove_all(p);
    fs::create_directories(p);
    return p;
}

void put(const fs::path& file, const std::string& text) {
    fs::create_directories(file.parent_path());
    GIANT_CHECK(write_text_file(file, text));
}

// Every file under `folder`, as paths from it with '/', sorted.
std::set<std::string> tree(const fs::path& folder) {
    std::set<std::string> out;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file()) continue;
        std::string name = path_to_utf8(fs::relative(it->path(), folder));
        std::replace(name.begin(), name.end(), '\\', '/');
        out.insert(name);
    }
    return out;
}

std::string as_json_text(const fs::path& path) {
    std::string text = path_to_utf8(path);
    std::string out;
    for (const char c : text) out += c == '\\' ? std::string("\\\\") : std::string(1, c);
    return out;
}

// A working folder with a world in it that names files, some it cannot take along.
struct Work {
    fs::path root = fresh_folder();
    fs::path working = root / "work";
    fs::path world = root / "work" / "My World";
    fs::path elsewhere = root / "elsewhere.png";
    Work() {
        put(elsewhere, "png");
        put(working / "assets" / "hut.gltf", R"({"buffers": [{"uri": "hut.bin"}], "images": [{"uri": "maps/wood.png"}, {"uri": "gone.png"}]})");
        put(working / "assets" / "hut.bin", "bin");
        put(working / "assets" / "maps" / "wood.png", "png");
        put(working / "assets" / "man.glb", "glb");
        put(working / "assets" / "hair.abc", "abc");
        put(working / "assets" / "unused.png", "png");
        put(working / "top.png", "png");
        put(working / "sounds" / "shot.wav", "wav");
        put(working / "sounds" / "deep" / "echo.wav", "wav");
        put(world / "world.json", R"({"meshes": ["assets/hut.gltf", "assets/man.glb"], "name": "a name, not a file",
            "looks": [{"detail": "assets/man.glb#image2", "groom": "assets\/hair.abc"}], "missing": "assets/never.png"})");
        put(world / "cells" / "0_0.cell.json", R"({"entities": [{"mesh": "assets/hut.gltf", "map": "top.png"}]})");
        put(world / "mods" / "more" / "mod.json", R"({"id": "more"})");
        put(world / "saves" / "save-1" / "save.json", R"({"mesh": "assets/unused.png"})");
    }
    ~Work() {
        std::error_code ec;
        fs::remove_all(root, ec);
    }
    PackageRequest request(const char* folder = "out") const {
        PackageRequest r;
        r.folder = root / folder;
        r.info.name = "Long Road";
        r.info.version = "0.3";
        r.program = root / "bin" / "giant_editor.exe";
        r.managed = root / "bin" / "managed";
        r.world = world;
        r.working = working;
        put(r.program, "program");
        put(root / "bin" / "D3D12" / "D3D12Core.dll", "core");
        put(root / "bin" / "editor_only.txt", "no");
        for (const char* name : {"Giant.Scripting.dll", "Giant.Scripting.deps.json", "Giant.Scripting.runtimeconfig.json",
                                 "Giant.Scripting.pdb", "Giant.Scripting.xml"})
            put(r.managed / name, "api");
        put(r.managed / "chat" / "Giant.Chat.dll", "chat");
        put(r.managed / "samples" / "HelloGiant" / "HelloGiant.dll", "sample");
        return r;
    }
};

} // namespace

GIANT_TEST("game package: game.json as text and back; what is no game is refused with why") {
    GameInfo info;
    info.name = "Long Road";
    info.version = "0.3-beta";
    info.scripts = "scripts/Road.dll";
    const std::string text = game_info_text(info);
    GIANT_CHECK(text == "{\n  \"giant_game\": 1,\n  \"name\": \"Long Road\",\n  \"version\": \"0.3-beta\",\n  \"world\": \"world\",\n"
                        "  \"scripts\": \"scripts/Road.dll\"\n}\n");
    std::string error;
    GIANT_CHECK(game_info_from_text(text, error) == std::optional<GameInfo>(info) && error.empty());
    // Keys left out keep their defaults, but the name; others are not minded.
    const std::optional<GameInfo> least = game_info_from_text(R"({"giant_game": 1, "name": "A", "later": 1})", error);
    GIANT_CHECK(least && least->version == "1" && least->world == "world" && least->scripts.empty());
    // Some other program's game.json is not a game's at all; a game's with something wrong is, and is refused.
    for (const char* other : {"", "[]", "{}", R"({"name": "A"})", R"({"giant_game": 2, "name": "A"})", R"({"giant_game": "1"})"}) {
        error.clear();
        GIANT_CHECK(!is_game_info(other) && !game_info_from_text(other, error) && !error.empty());
    }
    for (const char* rest : {R"("name": 3)", R"("name": "")", R"("name": "a/b")", R"("name": " a")", R"("name": "World")",
                             R"("name": "nul")", R"("name": "a", "version": "")", R"("name": "a", "version": "1 2")",
                             R"("name": "a", "world": "../other")", R"("name": "a", "world": "/abs")",
                             R"("name": "a", "world": "C:/abs")", R"("name": "a", "scripts": "..\\x.dll")",
                             R"("name": "a", "world": "")", R"("version": "2")"}) {
        const std::string bad = std::string(R"({"giant_game": 1, )") + rest + "}";
        error.clear();
        GIANT_CHECK(is_game_info(bad) && !game_info_from_text(bad, error) && !error.empty());
    }
    GIANT_CHECK(game_name_ok("Long Road-2_b") && !game_name_ok("") && !game_name_ok("a.exe") && !game_name_ok("a ") &&
                !game_name_ok(std::string(65, 'a')) && game_name_ok(std::string(64, 'a')));
    // Not a name the package's own folders have, nor a device's on Windows, whatever the case.
    for (const char* taken : {"world", "Managed", "SCRIPTS", "d3d12", "giant_cache", "CON", "prn", "Aux", "nul", "com1", "LPT9"})
        GIANT_CHECK(!game_name_ok(taken));
    GIANT_CHECK(game_name_ok("worlds") && game_name_ok("com") && game_name_ok("com10") && game_name_ok("game"));
    GIANT_CHECK(path_inside("a/b.c") && path_inside("a") && !path_inside("") && !path_inside("../a") && !path_inside("a/../b") &&
                !path_inside("/a") && !path_inside("a\\b") && !path_inside("c:a") && !path_inside("a//b") && !path_inside("."));
    // Nor a spelling Windows reads as another file's.
    GIANT_CHECK(!path_inside("./a") && !path_inside("a/./b") && !path_inside("a/") && !path_inside("keys/claude_key.txt.") &&
                !path_inside("keys /a") && !path_inside("CLAUDE~1.TXT"));
}

GIANT_TEST("game package: the files a world's text names are found, those of a .gltf too, and those it cannot take are told") {
    const Work work;
    WorldFiles files = world_files(work.world, work.working);
    // The meshes, the image beside a mesh in its file, an escaped name, what the .gltf names from
    // its own folder; not what is missing, not what only a save names, not a name that is no file.
    GIANT_CHECK(files.files == std::vector<std::string>({"assets/hair.abc", "assets/hut.bin", "assets/hut.gltf", "assets/man.glb",
                                                         "assets/maps/wood.png", "top.png"}));
    GIANT_CHECK(files.outside.empty());
    // A file named by an absolute path, and one above the working folder: there, and not the package's to hold.
    put(work.world / "cells" / "1_0.cell.json",
        R"({"a": ")" + as_json_text(work.elsewhere) + R"(", "b": "../elsewhere.png", "c": "../nowhere.png"})");
    files = world_files(work.world, work.working);
    GIANT_CHECK(files.files.size() == 6 && files.outside.size() == 2);
    GIANT_CHECK(std::count(files.outside.begin(), files.outside.end(), "../elsewhere.png") == 1);
    fs::remove(work.world / "cells" / "1_0.cell.json");
    // Two .gltf files in two folders each name "skin.png": each its own. A name spelled the long way round is the same file.
    put(work.working / "a" / "one.gltf", R"({"images": [{"uri": "skin.png"}]})");
    put(work.working / "b" / "two.gltf", R"({"images": [{"uri": "skin.png"}, {"uri": "./skin.png"}]})");
    put(work.working / "a" / "skin.png", "png");
    put(work.working / "b" / "skin.png", "png");
    put(work.world / "cells" / "3_0.cell.json", R"({"m": ["a/one.gltf", "b\\two.gltf", "./a/../a/one.gltf"]})");
    files = world_files(work.world, work.working);
    GIANT_CHECK(files.error.empty() && files.files.size() == 10 && files.outside.empty());
    for (const char* found : {"a/one.gltf", "a/skin.png", "b/two.gltf", "b/skin.png"})
        GIANT_CHECK(std::count(files.files.begin(), files.files.end(), found) == 1);
    // A name on another machine is never looked for (and so never refused, nor waited for).
    put(work.world / "cells" / "4_0.cell.json", R"({"m": ["//nowhere.invalid/share/a.png", "\\\\nowhere.invalid\\share\\a.png"]})");
    files = world_files(work.world, work.working);
    GIANT_CHECK(files.files.size() == 10 && files.outside.empty());
    // A world's folder that is not there: said, not an empty list.
    GIANT_CHECK(!world_files(work.root / "no world", work.working).error.empty());
}

GIANT_TEST("game package: the folder holds the program by the game's name, the world, what it names, the scripts and game.json") {
    const Work work;
    PackageRequest request = work.request();
    request.scripts = work.root / "built" / "Road.dll";
    for (const char* name : {"Road.dll", "Road.deps.json", "Road.runtimeconfig.json", "Road.pdb", "Helper.dll", "Giant.Scripting.dll",
                             "Giant.Chat.dll", "Anthropic.dll", "Microsoft.Extensions.AI.Abstractions.dll", "notes.txt", "appsettings.json"})
        put(work.root / "built" / name, "x");
    put(work.root / "built" / "sub" / "Deeper.dll", "x");
    put(work.root / "bin" / "d3d10warp.dll", "warp");
    // (Asked for with a dot and a slash too many, and twice: one file, one place. A .gltf asked for brings its files.)
    put(work.working / "extra" / "prop.gltf", R"({"buffers": [{"uri": "prop.bin"}]})");
    put(work.working / "extra" / "prop.bin", "bin");
    request.include = {"./sounds/", "assets/unused.png", "sounds/shot.wav", "extra/prop.gltf", "top.png"};
    const PackageResult made = package_game(request);
    GIANT_CHECK(made.error.empty());
    const std::set<std::string> expected = {"Long Road.exe",
                                            "game.json",
                                            "D3D12/D3D12Core.dll",
                                            "d3d10warp.dll",
                                            "extra/prop.gltf",
                                            "extra/prop.bin",
                                            "managed/Giant.Scripting.dll",
                                            "managed/Giant.Scripting.deps.json",
                                            "managed/Giant.Scripting.runtimeconfig.json",
                                            "world/world.json",
                                            "world/cells/0_0.cell.json",
                                            "world/mods/more/mod.json",
                                            "assets/hair.abc",
                                            "assets/hut.bin",
                                            "assets/hut.gltf",
                                            "assets/man.glb",
                                            "assets/maps/wood.png",
                                            "assets/unused.png",
                                            "top.png",
                                            "sounds/shot.wav",
                                            "sounds/deep/echo.wav",
                                            "scripts/Road.dll",
                                            "scripts/Road.deps.json",
                                            "scripts/Road.runtimeconfig.json",
                                            "scripts/Helper.dll"};
    GIANT_CHECK(tree(request.folder) == expected);
    GIANT_CHECK(made.files == expected.size() && made.bytes > 0 && made.program == "Long Road.exe");
    std::string error;
    const std::optional<GameInfo> info = game_info_from_text(read_text_file(request.folder / "game.json").value_or(""), error);
    GIANT_CHECK(info && info->name == "Long Road" && info->version == "0.3" && info->world == "world" && info->scripts == "scripts/Road.dll");
    GIANT_CHECK(made.info == *info);
    GIANT_CHECK(read_text_file(request.folder / "Long Road.exe").value_or("") == "program");
    // Into the same folder again: it is not empty, and nothing is written over.
    GIANT_CHECK(!package_game(request).error.empty());

    // Without scripts, and the C# API not found: said, and the game is made without.
    PackageRequest plain = work.request("plain");
    plain.managed = work.root / "nowhere";
    const PackageResult bare = package_game(plain);
    GIANT_CHECK(bare.error.empty() && bare.info.scripts.empty() && !fs::exists(plain.folder / "managed") && !fs::exists(plain.folder / "scripts"));
    GIANT_CHECK(std::any_of(bare.notes.begin(), bare.notes.end(), [](const std::string& n) { return n.find("C# API") != std::string::npos; }));
    // Scripts without the API cannot run: refused, with nothing written.
    PackageRequest lost = work.request("lost");
    lost.managed = work.root / "nowhere";
    lost.scripts = request.scripts;
    GIANT_CHECK(!package_game(lost).error.empty() && !fs::exists(lost.folder));
    // A world kept in a folder called "world", naming a file of its own from the working folder: the file
    // is the world's copy and the named one at once (one place, one file), not a clash.
    const fs::path named_world = work.working / "world";
    put(named_world / "world.json", R"({"map": "world/own.png", "other": "top.png"})");
    put(named_world / "own.png", "png");
    put(named_world / "Saves" / "save-1" / "save.json", "{}"); // (a saves folder however its name is cased)
    PackageRequest own = work.request("own");
    own.world = named_world;
    const PackageResult owned = package_game(own);
    GIANT_CHECK(owned.error.empty());
    const std::set<std::string> in_it = tree(own.folder);
    GIANT_CHECK(in_it.contains("world/own.png") && in_it.contains("top.png") && in_it.contains("world/world.json") &&
                !in_it.contains("world/Saves/save-1/save.json"));
    // A mod with scripts of its own goes along, and it is said that they do not run.
    put(work.world / "mods" / "more" / "More.dll", "x");
    const PackageResult modded = package_game(work.request("modded"));
    GIANT_CHECK(modded.error.empty() && fs::exists(work.root / "modded" / "world" / "mods" / "more" / "More.dll"));
    GIANT_CHECK(std::any_of(modded.notes.begin(), modded.notes.end(), [](const std::string& n) { return n.find("mod's scripts") != std::string::npos; }));
}

GIANT_TEST("game package: what cannot be a game is refused before anything is written") {
    const Work work;
    const auto refused = [&](const PackageRequest& request, const char* saying) {
        const PackageResult made = package_game(request);
        const bool nothing = !fs::exists(request.folder) || fs::is_empty(request.folder);
        GIANT_CHECK(!made.error.empty() && made.error.find(saying) != std::string::npos && nothing);
        return !made.error.empty() && nothing;
    };
    PackageRequest r = work.request("a");
    r.info.name = "bad/name";
    GIANT_CHECK(refused(r, "name"));
    r = work.request("a");
    r.info.version = "";
    GIANT_CHECK(refused(r, "version"));
    r = work.request("a");
    r.world = work.root / "no world";
    GIANT_CHECK(refused(r, "no world is saved"));
    r = work.request("a");
    r.program = work.root / "no program";
    GIANT_CHECK(refused(r, "program"));
    r = work.request("a");
    r.scripts = work.root / "no scripts.dll";
    GIANT_CHECK(refused(r, "scripts"));
    // A folder that is a file, one with something in it, one inside the world.
    r = work.request("a");
    put(work.root / "a file", "x");
    r.folder = work.root / "a file";
    GIANT_CHECK(!package_game(r).error.empty());
    r.folder = work.working;
    GIANT_CHECK(!package_game(r).error.empty() && !fs::exists(work.working / "game.json"));
    r.folder = work.world / "package";
    GIANT_CHECK(refused(r, "inside the world"));
    // Includes that are not under the working folder, not there, or the chat's own.
    for (const char* bad : {"../elsewhere.png", "nothing.wav", "/etc/hosts"}) {
        r = work.request("a");
        r.include = {bad};
        GIANT_CHECK(refused(r, "include"));
    }
    for (const char* bad : {"keys/claude_key.txt.", "CLAUDE~1.TXT", "sounds/../../elsewhere.png"}) {
        r = work.request("a");
        r.include = {bad};
        GIANT_CHECK(refused(r, "include"));
    }
    // The chat's key and library, by whatever way they would come and however cased: never, and nothing is written.
    put(work.working / "keys" / "Claude_Key.txt", "not a key");
    r = work.request("a");
    r.include = {"keys"};
    GIANT_CHECK(refused(r, "never part of a game"));
    for (const char* chat : {"claude_key.txt", "Giant.Chat.dll", "ANTHROPIC.dll", "Microsoft.Extensions.AI.Abstractions.dll"}) {
        put(work.world / "mods" / "more" / chat, "x");
        r = work.request("a");
        GIANT_CHECK(refused(r, "never part of a game"));
        fs::remove(work.world / "mods" / "more" / chat);
    }
    put(work.working / "assets" / "Anthropic.Foo.dll", "x");
    put(work.world / "cells" / "2_0.cell.json", R"({"map": "assets/Anthropic.Foo.dll"})");
    r = work.request("a");
    GIANT_CHECK(refused(r, "never part of a game"));
    r = work.request("a");
    r.scripts = work.working / "assets" / "Anthropic.Foo.dll";
    fs::remove(work.world / "cells" / "2_0.cell.json");
    GIANT_CHECK(refused(r, "never part of a game"));
    r = work.request("a");
    r.scripts = work.working / "top.png";
    GIANT_CHECK(refused(r, ".dll"));
    // Two files for one place in the folder: a file asked for where the program, game.json or a
    // folder of the package's goes.
    put(work.working / "game.json", "{}");
    put(work.working / "world" / "world.json", "{}");
    put(work.working / "WORLD" / "cells" / "0_0.cell.json", "{}"); // (two cases of one place: one on Windows, so one here)
    put(work.working / "Long Road.exe" / "under.txt", "x");        // (a file that would lie under the program's file)
    for (const char* clash : {"game.json", "world/world.json", "WORLD/cells/0_0.cell.json", "Long Road.exe/under.txt"}) {
        r = work.request("a");
        r.include = {clash};
        GIANT_CHECK(refused(r, "game's"));
    }
    // A file the world names outside the working folder: named in the error.
    put(work.world / "cells" / "1_0.cell.json", R"({"b": "../elsewhere.png"})");
    r = work.request("a");
    GIANT_CHECK(refused(r, "../elsewhere.png"));
    fs::remove(work.world / "cells" / "1_0.cell.json");
    // A link in the world's folder is not followed, and it is said.
    std::error_code ec;
    fs::create_symlink(work.elsewhere, work.world / "cells" / "linked.cell.json", ec);
    if (!ec) { // (where links can be made: not as any person on Windows)
        const PackageResult made = package_game(work.request("linked"));
        GIANT_CHECK(made.error.empty() && !fs::exists(work.root / "linked" / "world" / "cells" / "linked.cell.json"));
        GIANT_CHECK(std::any_of(made.notes.begin(), made.notes.end(), [](const std::string& n) { return n.find("links") != std::string::npos; }));
    }
}
