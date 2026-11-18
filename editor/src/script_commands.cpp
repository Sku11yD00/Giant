// load_scripts, unload_scripts, get_scripts: the C# scripts a command names (docs/scripting.md), and
// the person's say over them. The loading itself is the main loop's (EditorState::scripts_to_load).
#include "editor_commands.h"

#include "world_streaming.h"

#include "giant/core/assert.h"
#include "giant/core/text.h"

#include <algorithm>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace giant::editor {

namespace {

using ai::CommandContext;
using ai::CommandResult;
using ai::Json;

void add(ai::CommandRegistry& registry, ai::CommandDesc desc) {
    const bool added = registry.add(std::move(desc));
    GIANT_ASSERT(added, "script command could not be registered");
}

// Whether the file is in the open world's mods folder: a mod's scripts run by allowing the mod, never by naming the file.
bool in_open_worlds_mods(const EditorState& state, const std::filesystem::path& file) {
    return state.world != nullptr && state.world->is_open() && in_mods_folder(state.world->folder(), file);
}

bool allowed(const EditorState& state, const std::string& key) {
    return std::find(state.scripts_allowed.begin(), state.scripts_allowed.end(), key) != state.scripts_allowed.end();
}

// Whether load_scripts with these arguments (not yet checked) would only put the question to the
// person and load nothing (ChatGate::asks_itself): a file there is, outside the mods folder, not allowed yet.
bool load_only_asks(const EditorState& state, const Json& args) {
    if (state.scripts_consent != ScriptsConsent::ask || !state.dotnet_available || !args.is_object() || !args.contains("path") ||
        !args["path"].is_string()) {
        return false;
    }
    const std::filesystem::path file = path_from_utf8(args["path"].get<std::string>());
    std::error_code error;
    if (!std::filesystem::is_regular_file(file, error) || in_open_worlds_mods(state, file)) {
        return false;
    }
    const std::string key = script_file_key(file);
    return !key.empty() && !allowed(state, key);
}

} // namespace

std::string script_file_key(const std::filesystem::path& file) {
    std::error_code error;
    const std::filesystem::path real = std::filesystem::canonical(file, error);
    return error ? std::string() : path_to_utf8(real);
}

std::string answer_scripts_request(EditorState& state, bool allow) {
    if (!state.scripts_request) {
        return "nothing was asked";
    }
    const std::string file = *state.scripts_request;
    state.scripts_request.reset();
    if (!allow) {
        return "the scripts of " + file + " were not allowed";
    }
    // (As load_scripts would say now: the world may have been opened since the question was put.)
    if (in_open_worlds_mods(state, path_from_utf8(file))) {
        return "the scripts of " + file + " were not loaded: the file is in the open world's mods folder (enable_mod allows a mod's scripts)";
    }
    if (!allowed(state, file)) {
        if (state.scripts_allowed.size() >= k_most_scripts_allowed) {
            state.scripts_allowed.erase(state.scripts_allowed.begin()); // (the oldest is asked about again)
        }
        state.scripts_allowed.push_back(file);
    }
    state.scripts_to_load = file;
    state.scripts_unload_requested = false;
    return "the scripts of " + file + " are allowed until the editor is closed, and are loading";
}

void register_script_commands(ai::CommandRegistry& registry, EditorState& state) {
    // (Claude's load_scripts is not asked about twice: where the command will only ask, the chat's gate lets it. Where
    // it would load at once, a file allowed before or an editor that loads by command, the gate asks as for any change.)
    state.chat_gate.asks_itself = [&state](std::string_view command, const Json& args) {
        return command == "load_scripts" && load_only_asks(state, args);
    };
    add(registry,
        {"load_scripts",
         "Loads a compiled C# script assembly (.dll built against Giant.Scripting) and starts its "
         "scripts; replaces the assembly loaded before (its scripts get OnStop first; the open world's mods' scripts run on). Rebuild the .dll and call "
         "this again to reload. Scripts change the scene through commands, but as game logic: "
         "their changes are not added to the undo history. Check get_log afterwards for script "
         "errors. A script is a program that runs with the person's rights on their machine, so a file is "
         "loaded only once they allowed it: the first time a file is named they are asked in the editor's window "
         "(the reply says `asked`: true, and nothing is loaded until they allow it; get_scripts tells `asked` "
         "while they have not answered, and `loaded` once it runs), and a file they allowed loads again without "
         "asking until the editor is closed (64 files at most: then the one allowed longest ago is asked about again). One question at a time; "
         "a load that needs no question withdraws one still open. (An editor started with "
         "--allow-scripts-by-command, for automated runs, loads at once; one with no window refuses.)",
         Json::parse(R"({"type": "object", "additionalProperties": false, "required": ["path"],
             "properties": {"path": {"type": "string", "minLength": 1, "maxLength": 1024,
                                     "description": "Path to the script .dll."}}})"),
         false,
         [&state](const Json& args, CommandContext&) {
             if (!state.dotnet_available) {
                 return CommandResult::failure(".NET could not be started; see get_log");
             }
             const std::string path = args["path"].get<std::string>();
             std::error_code error;
             const std::filesystem::path file = path_from_utf8(path);
             if (!std::filesystem::is_regular_file(file, error)) {
                 return CommandResult::failure("no such file: " + path);
             }
             if (in_open_worlds_mods(state, file)) {
                 return CommandResult::failure("that file is in the open world's mods folder: a mod's scripts run once the person has allowed "
                                               "the mod's scripts (enable_mod with scripts: true asks them), not by loading the file");
             }
             std::string load = path;
             if (state.scripts_consent != ScriptsConsent::by_command) {
                 // (The person's to say, in the editor's window: not this command's. What they allowed
                 // is the file where it really is, so a link pointed elsewhere since is asked about anew.)
                 const std::string key = script_file_key(file);
                 if (key.empty()) {
                     return CommandResult::failure("where that file really is could not be told: " + path);
                 }
                 if (!allowed(state, key)) {
                     if (state.scripts_consent == ScriptsConsent::none) {
                         return CommandResult::failure(k_no_window_to_ask);
                     }
                     // (One question at a time: what they are reading is never changed under them.)
                     if (state.scripts_request && *state.scripts_request != key) {
                         return CommandResult::failure("the person is still being asked about another script file (" + *state.scripts_request +
                                                       "): ask again once they have answered");
                     }
                     state.scripts_request = key;
                     return CommandResult::success(
                         {{"asked", true},
                          {"file", key},
                          {"note", "the person is being asked in the editor's window whether this file's scripts may run on their machine; "
                                   "nothing is loaded until they allow it (get_scripts tells)"}});
                 }
                 load = key;
             }
             state.scripts_request.reset(); // (a question about another file: this request is the latest)
             state.scripts_to_load = load;
             state.scripts_unload_requested = false; // the latest request wins
             return CommandResult::success({{"loading", load}, {"note", "starts after this frame"}});
         }});

    add(registry,
        {"unload_scripts", "Stops (OnStop) and unloads the loaded C# scripts (not those of the open world's mods: they go when the world is closed, or the mod's scripts are no longer allowed and the world is opened again). A script file the person is still being asked about (load_scripts) is no longer wanted: the question is withdrawn.",
         Json{{"type", "object"}, {"additionalProperties", false}}, false,
         [&state](const Json&, CommandContext&) {
             state.scripts_unload_requested = true;
             state.scripts_to_load.clear(); // the latest request wins
             state.scripts_request.reset(); // (and a file the person is still asked about is no longer wanted)
             return CommandResult::success({{"unloading", !state.scripts_loaded.empty()}});
         }});

    add(registry,
        {"get_scripts",
         "Reports the loaded C# script assembly, the file the person is being asked about in the editor's window after a load_scripts "
         "(`asked`; null when none), the open world's mods whose scripts are loaded beside it (`mods`: "
         "ids, in the order loaded; list_mods `world.scripts` tells which were to run and which not, with why) and how many "
         "scripts are running in all (a script that throws is stopped and logged).",
         Json{{"type", "object"}, {"additionalProperties", false}}, true,
         [&state](const Json&, CommandContext&) {
             return CommandResult::success({{"loaded", state.scripts_loaded},
                                            {"asked", state.scripts_request ? Json(*state.scripts_request) : Json(nullptr)},
                                            {"mods", state.mods_scripts_loaded},
                                            {"running", state.scripts_running},
                                            {"dotnet_available", state.dotnet_available}});
         }});
}

} // namespace giant::editor
