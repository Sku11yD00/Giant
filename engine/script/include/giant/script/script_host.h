// Runs C# game scripts on .NET. The host starts the .NET runtime inside the
// engine process, loads the engine's C# API (Giant.Scripting.dll) and then
// script assemblies, and calls their OnStart/OnUpdate/OnStop.
//
// Scripts change the engine only through commands, by calling the command
// handler given here; the editor connects it to its command registry. So
// a C# script can do exactly what a person or the AI can, and nothing that
// skips the rules.
//
// Everything runs on the thread that calls the host (the main thread).
#pragma once

#include "giant/core/types.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace giant::script {

struct CommandReply {
    bool ok = false;
    std::string text; // result JSON on success, the error on failure
};

using CommandHandler = std::function<CommandReply(std::string_view name, std::string_view args_json)>;

struct ScriptHostDesc {
    // Folder holding Giant.Scripting.dll and Giant.Scripting.runtimeconfig.json.
    std::filesystem::path managed_dir;
    // Where to find .NET. Empty: DOTNET_ROOT, then the usual install folders,
    // then the folder of `dotnet` on the PATH.
    std::filesystem::path dotnet_root;
    CommandHandler commands;
};

class ScriptHost {
public:
    // Starts .NET. Returns null (and logs why, e.g. .NET 10 is not
    // installed) on failure. .NET runs once per process: only one host may
    // ever be created.
    static std::unique_ptr<ScriptHost> create(const ScriptHostDesc& desc);
    ~ScriptHost();

    ScriptHost(const ScriptHost&) = delete;
    ScriptHost& operator=(const ScriptHost&) = delete;

    // Loads a script assembly (replacing any loaded one, which is stopped
    // and unloaded first) and starts its scripts. False on failure (logged).
    [[nodiscard]] bool load_scripts(const std::filesystem::path& assembly);
    // Calls OnUpdate on every running script.
    void update(float delta_seconds);
    // Stops and unloads the loaded scripts, if any.
    void unload_scripts();
    // Scripts running now (a script that threw is stopped and not counted).
    u32 running_scripts() const;

    struct Api;
    struct ConstructKey {
    private:
        friend class ScriptHost;
        ConstructKey() = default;
    };
    ScriptHost(ConstructKey, std::unique_ptr<Api> api);

private:
    std::unique_ptr<Api> m_api;
};

// Where .NET would be looked for, for messages and tests.
std::filesystem::path find_dotnet_root();

} // namespace giant::script
