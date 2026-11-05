// The editor's own commands: everything the editor can do so far, as
// commands people and the AI share (rule 7 in AGENTS.md).
#pragma once

#include "giant/ai/commands.h"
#include "giant/core/log_history.h"
#include "giant/platform/window.h"
#include "giant/rhi/rhi.h"
#include "giant/world/scene.h"

#include <functional>
#include <vector>

namespace giant::editor {

// The viewport camera. View state: moving it is not undoable, like
// scrolling a document.
struct EditorCamera {
    DVec3 position{0.0, 2.0, 8.0};
    float yaw_degrees = 0.0f;     // 0 looks down -Z (north), 90 looks west (-X)
    float pitch_degrees = -10.0f; // up is positive, -89 to 89
    float vertical_fov_degrees = 60.0f;
    float speed = 8.0f; // metres per second when flying
};

// What the commands read and change. Owned by main; outlives the registry.
struct EditorState {
    platform::Window* window = nullptr;
    rhi::Device* device = nullptr;
    const LogHistory* log = nullptr;
    // The scene being edited; its sky colour is the viewport background.
    world::Scene scene;
    EditorCamera camera;
    world::EntityId selected = 0; // in the outliner; view state
    // Last frame's renderer counts, updated by the main loop.
    u32 scene_draws = 0;
    u32 scene_instances = 0;
    bool quit_requested = false;
    // Renders the editor as it looks now (viewport and panels) and returns
    // RGBA8 pixels at the window's pixel size; empty on failure. Set by main.
    std::function<std::vector<u8>()> render_screenshot;

    // C# scripts. Commands cannot run commands, so loading is requested here
    // and done by the main loop right after the command.
    std::string scripts_loaded;      // path of the loaded assembly, empty if none
    std::string scripts_to_load;     // set by load_scripts
    bool scripts_unload_requested = false;
    u32 scripts_running = 0;         // updated by the main loop
    bool dotnet_available = true;    // false once starting .NET failed
};

// Registers every editor command. Asserts on a bad registration (a bug).
void register_editor_commands(ai::CommandRegistry& registry, EditorState& state);
// The scene, lighting and camera commands (scene_commands.cpp).
void register_scene_commands(ai::CommandRegistry& registry, EditorState& state);

// What the AI is told when it connects over MCP.
const char* mcp_instructions();

} // namespace giant::editor
