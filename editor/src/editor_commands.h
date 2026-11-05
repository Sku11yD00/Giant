// The editor's own commands: everything the editor can do so far, as
// commands people and the AI share (rule 7 in AGENTS.md).
#pragma once

#include "giant/ai/commands.h"
#include "giant/core/log_history.h"
#include "giant/platform/window.h"
#include "giant/render/renderer.h"
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

class MeshLibrary;

// What the commands read and change. Owned by main; outlives the registry.
struct EditorState {
    platform::Window* window = nullptr;
    rhi::Device* device = nullptr;
    const LogHistory* log = nullptr;
    // The scene being edited; its sky colour is the viewport background.
    world::Scene scene;
    // Changes when the whole scene is replaced (new_scene, load_scene). Undo
    // steps capture it and do nothing if it no longer matches, so an undo
    // never touches a different scene that reuses the same entity ids (a
    // script can replace the scene without recording history).
    u64 scene_generation = 1;
    u64 next_scene_generation = 2;
    EditorCamera camera;
    world::EntityId selected = 0; // in the outliner; view state
    // Imported meshes; set by main (and tests). Entities with shape mesh
    // name a file in it.
    MeshLibrary* meshes = nullptr;
    // LOD error and debug view: view state, not undoable.
    render::RenderSettings render_settings;
    // Last frame's renderer counts, updated by the main loop.
    render::RenderStats scene_stats;
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

// A change to the current scene: its undo and redo only run while the scene
// is still the one they were recorded against (see scene_generation).
ai::Change scene_change(EditorState& state, std::function<void()> undo,
                        std::function<void()> redo);

// Registers every editor command. Asserts on a bad registration (a bug).
void register_editor_commands(ai::CommandRegistry& registry, EditorState& state);
// The scene, lighting and camera commands (scene_commands.cpp).
void register_scene_commands(ai::CommandRegistry& registry, EditorState& state);

// What the AI is told when it connects over MCP.
const char* mcp_instructions();

} // namespace giant::editor
