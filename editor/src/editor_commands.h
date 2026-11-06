// The editor's own commands: everything the editor can do so far, as
// commands people and the AI share (rule 7 in AGENTS.md).
#pragma once

#include "giant/ai/commands.h"
#include "giant/core/log_history.h"
#include "giant/platform/window.h"
#include "giant/render/renderer.h"
#include "giant/rhi/rhi.h"
#include "giant/world/scene.h"

#include <algorithm>
#include <array>
#include <functional>
#include <string>
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

// The viewport's terrain brush (editor/src/terrain_brush_tool.h): view
// state, set in the Scene panel's Terrain section.
struct BrushSettings {
    bool enabled = false;
    std::string op = "raise"; // sculpt_terrain's op
    double radius = 8.0;
    float strength = 0.5f;
    float falloff = 0.5f;
    float height = 0.0f; // flatten
    // The largest radius the panel offers, in metres for a sample spacing:
    // 128 samples, so a frame's dab is a small part of it.
    static double max_radius(double spacing) { return 128.0 * spacing; }
    bool flatten_to_start = true; // flatten to the ground where the stroke began, not `height`
};

class MeshLibrary;
class TerrainMeshes;
class FarGround;
class ScatterPlacer;
class WeatherState;
class WorldStreaming;

// The last few seconds of frame times, for get_engine_info (performance
// checks such as the N1 gate run over MCP).
struct FrameTimes {
    static constexpr usize k_frames = 240;
    std::array<float, k_frames> ms{};
    usize count = 0; // frames recorded, ever
    void add(float frame_ms) { ms[count++ % k_frames] = frame_ms; }
    usize size() const { return std::min(count, k_frames); }
    // The recorded times, oldest first.
    std::vector<float> in_order() const {
        std::vector<float> out;
        out.reserve(size());
        const usize start = count > k_frames ? count % k_frames : 0;
        for (usize i = 0; i < size(); ++i) out.push_back(ms[(start + i) % k_frames]);
        return out;
    }
    float average() const {
        float sum = 0.0f;
        for (usize i = 0; i < size(); ++i) sum += ms[i];
        return size() > 0 ? sum / static_cast<float>(size()) : 0.0f;
    }
    float worst() const {
        float w = 0.0f;
        for (usize i = 0; i < size(); ++i) w = std::max(w, ms[i]);
        return w;
    }
    // The frame time `fraction` of frames are at or under (0.5 median,
    // 0.99 the 99th percentile). Consistency matters as much as the average.
    float percentile(float fraction) const {
        if (size() == 0) return 0.0f;
        std::array<float, k_frames> sorted = ms;
        const auto end = sorted.begin() + static_cast<std::ptrdiff_t>(size());
        std::sort(sorted.begin(), end);
        const auto rank = static_cast<usize>(fraction * static_cast<float>(size() - 1) + 0.5f);
        return sorted[std::min(rank, size() - 1)];
    }
    // Stutters: frames half again over the median (at least one missed
    // display refresh) and over it by more than 2 ms (not timer noise).
    usize hitches() const {
        const float median = percentile(0.5f);
        const float limit = std::max(1.5f * median, median + 2.0f);
        usize n = 0;
        for (usize i = 0; i < size(); ++i) n += ms[i] > limit ? usize{1} : usize{0};
        return n;
    }
};

// The newest finished frame's GPU time per pass (Device::gpu_timings), with
// repeats of a name (the two culling passes, say) added up, in the order
// first seen.
std::vector<rhi::GpuTiming> gpu_passes(const rhi::Device& device);

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
    BrushSettings brush;
    world::EntityId selected = 0; // in the outliner; view state
    // Imported meshes; set by main (and tests). Entities with shape mesh
    // name a file in it.
    MeshLibrary* meshes = nullptr;
    // The world streamed by cell (open_world, save_world); set by main (and
    // tests). Null: worlds are not available.
    WorldStreaming* world = nullptr;
    // The terrain's tile meshes (get_terrain reports them); set by main.
    TerrainMeshes* terrain_meshes = nullptr;
    // The streamed world's far ground (get_terrain reports it; save_world
    // hands it the chunks written); set by main.
    FarGround* far_ground = nullptr;
    // The scatter layers' placements (get_scatter reports them); set by main.
    ScatterPlacer* scatter = nullptr;
    // The weather the view shows (blends, wind-carried clouds); set by main.
    WeatherState* weather = nullptr;
    // LOD error and debug view: view state, not undoable.
    render::RenderSettings render_settings;
    // Last frame's renderer counts, updated by the main loop.
    render::RenderStats scene_stats;
    FrameTimes frame_times; // updated by the main loop
    bool quit_requested = false;
    // Renders the editor as it looks now (viewport and panels) and returns
    // RGBA8 pixels at the window's pixel size; empty on failure. Set by main.
    std::function<std::vector<u8>()> render_screenshot;
    // What the voxel world holds at a point, per level (finest first). Set by main.
    std::function<std::vector<render::VoxelProbeLevel>(const DVec3&)> probe_voxels;

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
// The terrain commands (terrain_commands.cpp).
void register_terrain_commands(ai::CommandRegistry& registry, EditorState& state);
// The scatter commands (scatter_commands.cpp).
void register_scatter_commands(ai::CommandRegistry& registry, EditorState& state);
// The weather commands (weather_commands.cpp).
void register_weather_commands(ai::CommandRegistry& registry, EditorState& state);

// What the AI is told when it connects over MCP.
const char* mcp_instructions();

} // namespace giant::editor
