#include "editor_commands.h"

#include "giant/core/assert.h"
#include "giant/core/image.h"
#include "giant/core/log.h"
#include "giant/core/png.h"
#include "giant/core/text.h"

#include <algorithm>
#include <filesystem>
#include <fstream>

#include <string>

namespace giant::editor {

namespace {

using ai::CommandContext;
using ai::CommandResult;
using ai::Json;

constexpr const char* k_version = "0.1.0";

Json color_to_json(const Vec3& c) { return {{"r", c.x}, {"g", c.y}, {"b", c.z}}; }

void add(ai::CommandRegistry& registry, ai::CommandDesc desc) {
    const bool added = registry.add(std::move(desc));
    GIANT_ASSERT(added, "editor command could not be registered");
}

LogLevel parse_level(const std::string& name) {
    if (name == "error") return LogLevel::error;
    if (name == "warning") return LogLevel::warning;
    if (name == "info") return LogLevel::info;
    return LogLevel::trace;
}

} // namespace

std::vector<rhi::GpuTiming> gpu_passes(const rhi::Device& device) {
    std::vector<rhi::GpuTiming> passes;
    for (const rhi::GpuTiming& t : device.gpu_timings()) {
        const auto same = std::find_if(passes.begin(), passes.end(),
                                       [&t](const rhi::GpuTiming& p) { return p.name == t.name; });
        if (same != passes.end()) {
            same->ms += t.ms;
        } else {
            passes.push_back(t);
        }
    }
    return passes;
}

const char* mcp_instructions() {
    return "This is the Giant Engine editor. Every tool is an editor command; changes made "
           "through tools can be reverted with `undo` and repeated with `redo`, and "
           "`get_history` lists them. Read state with the get_* tools before changing it. "
           "Build scenes with create_entities (many at once) or create_entity, change them with "
           "set_entity and set_lighting, frame them with set_camera (look_at is easiest) and "
           "check the result with capture_screenshot. Imported models: create_entity with mesh "
           "(a .glb or .gltf path; imported on first use, or import_mesh first to see its "
           "size); set_render_settings shows how LOD works (debug_view lod_levels). Ground: "
           "create_terrain (flat or hills), then get_terrain with points gives heights to stand "
           "things on. Units are "
           "metres and degrees; +Y is up, "
           "-Z is north. Colours are linear (not sRGB), each channel 0 to 1. Check `get_log` "
           "after a change for warnings and errors.";
}

void register_editor_commands(ai::CommandRegistry& registry, EditorState& state) {
    register_scene_commands(registry, state);
    register_terrain_commands(registry, state);
    register_scatter_commands(registry, state);
    register_weather_commands(registry, state);
    register_clock_commands(registry, state);
    register_water_commands(registry, state);
    register_people_commands(registry, state);
    register_route_commands(registry, state);
    register_effects_commands(registry, state);
    register_physics_commands(registry, state);
    register_animation_commands(registry, state);
    register_cloth_commands(registry, state);
    register_hair_commands(registry, state);
    register_pose_commands(registry, state);
    register_swing_commands(registry, state);
    register_nav_commands(registry, state);
    register_walker_commands(registry, state);
    register_ragdoll_commands(registry, state);
    register_audio_commands(registry, state);
    register_save_commands(registry, state);
    register_game_ui_commands(registry, state);
    register_chat_commands(registry, state);
    register_mod_commands(registry, state);
    register_script_commands(registry, state);
    add(registry,
        {"get_engine_info",
         "Describes the running editor: version, graphics backend and adapter, whether the GPU "
         "validation layer runs, frames completed, the last frame's draw counts, frame times over the last 240 frames (average, median, 99th percentile, worst, hitches: frames 1.5x the median and 2 ms over it, and the times themselves, oldest first; wall clock, so vsync and "
         "time spent running commands count), the GPU's own time for the newest finished frame "
         "and each of its passes (gpu: frame_ms and passes, from GPU timestamps; 0 ms on the "
         "null backend) and the window size in pixels. With phases_since, frame_times.phases: where the main "
         "thread's time went in the frames since then (each phase's median, and the phases of those over "
         "phases_over_ms), for finding what makes a frame slow; with each slow frame, the GPU device's counts "
         "(device: resources created, new memory allocations, resources released, begin_frame's wait for the GPU "
         "in ms and its freeing after (release_ms); device_before: the two frames before, counts summed), and typical_device the median frame's; and `window`: the window itself was handled in the frame "
         "(moved, sized, minimised or restored, its focus come or gone, or drawn again several times at once as "
         "Windows has it while its title bar is held): a long `events` phase is then the desktop holding the "
         "window, not the program.",
         Json::parse(R"({"type": "object", "additionalProperties": false, "properties": {
             "phases_since": {"type": "integer", "minimum": 0,
                              "description": "The frames from this one on (frame_times.recorded counts them; those still kept, the last 240)."},
             "phases_over_ms": {"type": "number", "minimum": 0,
                                "description": "Frames longer than this are given in full (default 25: 1.5x a 60 fps frame)."}}})"),
         true,
         [&state](const Json& args, CommandContext&) {
             const auto& adapter = state.device->adapter();
             Json passes = Json::array();
             for (const rhi::GpuTiming& pass : gpu_passes(*state.device)) {
                 passes.push_back({{"name", pass.name}, {"ms", pass.ms}});
             }
             Json info = Json::object();
             if (args.contains("phases_since")) {
                 info = frame_phases(state.frame_times, args["phases_since"].get<usize>(),
                                     args.value("phases_over_ms", 25.0f));
             }
             Json result(
                 {{"editor_version", k_version},
                  {"backend", rhi::to_string(state.device->backend())},
                  {"adapter", adapter.name},
                  {"software_adapter", adapter.software},
                  {"meets_minimum_spec", adapter.meets_minimum_spec()},
                  {"validation_enabled", state.device->validation_enabled()},
                  {"frames_completed", state.device->frames_completed()},
                  {"last_frame", {{"scene_draws", state.scene_stats.draws},
                                  {"scene_instances", state.scene_stats.instances},
                                  {"scene_clusters", state.scene_stats.clusters},
                                  {"scene_triangles", state.scene_stats.triangles},
                                  {"gpu_draws", state.device->last_frame_stats().draws}}},
                  {"frame_times", {{"frames", state.frame_times.size()},
                                   {"average_ms", state.frame_times.average()},
                                   {"worst_ms", state.frame_times.worst()},
                                   {"median_ms", state.frame_times.percentile(0.5f)},
                                   {"p99_ms", state.frame_times.percentile(0.99f)},
                                   {"hitches", state.frame_times.hitches()},
                                   {"recorded", state.frame_times.count},
                                   {"recent_ms", state.frame_times.in_order()}}},
                  {"gpu", {{"frame_ms", state.device->gpu_frame_ms()}, {"passes", std::move(passes)}}},
                  {"window", {{"pixel_width", state.window->pixel_width()},
                              {"pixel_height", state.window->pixel_height()},
                              {"point_width", state.window->point_width()},
                              {"point_height", state.window->point_height()},
                              {"minimized", state.window->minimized()}}}});
             if (!info.empty()) {
                 result["frame_times"]["phases"] = std::move(info);
             }
             return CommandResult::success(std::move(result));
         }});

    add(registry,
        {"get_log",
         "Returns the newest editor log lines, oldest first. Use after a change to see warnings "
         "and errors.",
         Json::parse(R"({"type": "object", "additionalProperties": false, "properties": {
             "count": {"type": "integer", "minimum": 1, "maximum": 500,
                       "description": "How many lines (default 50)."},
             "min_level": {"enum": ["trace", "info", "warning", "error"],
                           "description": "Lowest level to include (default info)."}}})"),
         true,
         [&state](const Json& args, CommandContext&) {
             const auto count = args.value("count", 50);
             const LogLevel level = parse_level(args.value("min_level", std::string("info")));
             Json lines = Json::array();
             for (const auto& record : state.log->recent(static_cast<usize>(count), level)) {
                 lines.push_back({{"sequence", record.sequence},
                                  {"level", to_string(record.level)},
                                  {"channel", record.channel},
                                  {"text", record.text}});
             }
             return CommandResult::success({{"lines", std::move(lines)}});
         }});

    add(registry,
        {"get_clear_color",
         "Returns the viewport background colour: the scene's sky colour (linear RGB, 0 to 1). "
         "get_lighting has the rest of the lighting.",
         Json{{"type", "object"}, {"additionalProperties", false}}, true,
         [&state](const Json&, CommandContext&) {
             return CommandResult::success(color_to_json(state.scene.lighting.sky_color));
         }});

    add(registry,
        {"set_clear_color",
         "Sets the viewport background colour, which is the scene's sky colour (it also lights "
         "surfaces from above), in linear RGB with each channel from 0 to 1. Same as "
         "set_lighting with sky_color. Undoable.",
         Json::parse(R"({"type": "object", "additionalProperties": false,
             "required": ["r", "g", "b"], "properties": {
             "r": {"type": "number", "minimum": 0, "maximum": 1},
             "g": {"type": "number", "minimum": 0, "maximum": 1},
             "b": {"type": "number", "minimum": 0, "maximum": 1}}})"),
         false,
         [&state](const Json& args, CommandContext& context) {
             const Vec3 before = state.scene.lighting.sky_color;
             const Vec3 after{args["r"].get<float>(), args["g"].get<float>(),
                              args["b"].get<float>()};
             state.scene.lighting.sky_color = after;
             context.record(
                 scene_change(state, [&state, before] { state.scene.lighting.sky_color = before; },
                              [&state, after] { state.scene.lighting.sky_color = after; }));
             return CommandResult::success(color_to_json(after));
         }});

    add(registry,
        {"set_window_size",
         "Resizes the editor window, in screen points (on a high-DPI screen the pixel size is "
         "larger; get_engine_info reports both, the pixel size from the next frame on). Window "
         "size is view state, like a drag with the mouse, so it is not undoable.",
         Json::parse(R"({"type": "object", "additionalProperties": false,
             "required": ["width", "height"], "properties": {
             "width": {"type": "integer", "minimum": 320, "maximum": 7680},
             "height": {"type": "integer", "minimum": 240, "maximum": 4320}}})"),
         false,
         [&state](const Json& args, CommandContext&) {
             const u32 w = args["width"].get<u32>();
             const u32 h = args["height"].get<u32>();
             state.window->request_size(w, h);
             return CommandResult::success({{"requested", {{"width", w}, {"height", h}}}});
         }});

    add(registry,
        {"capture_screenshot",
         "Takes a screenshot of the editor as it looks now (viewport and panels) and returns it "
         "as a PNG image you can look at. Use it to check what a change looks like. Optionally "
         "also saves it as screenshots/<file_name>.png next to the editor.",
         Json::parse(R"({"type": "object", "additionalProperties": false, "properties": {
             "max_width": {"type": "integer", "minimum": 64, "maximum": 3840,
                           "description": "Scale down to at most this many pixels wide (default 1024)."},
             "file_name": {"type": "string", "minLength": 1, "maxLength": 64,
                           "description": "Letters, digits, - and _ only."}}})"),
         true,
         [&state](const Json& args, CommandContext&) {
             if (!state.render_screenshot) {
                 return CommandResult::failure("screenshots are not available");
             }
             const u32 width = state.window->pixel_width();
             const u32 height = state.window->pixel_height();
             if (width == 0 || height == 0) {
                 return CommandResult::failure("the window is minimised; restore it first");
             }
             const std::vector<u8> pixels = state.render_screenshot();
             if (pixels.size() != static_cast<usize>(width) * height * 4) {
                 return CommandResult::failure("the screenshot could not be rendered");
             }
             const ImageRgba8 small =
                 downscale_rgba(pixels, width, height, args.value("max_width", 1024u));
             const u32 w = small.width;
             const u32 h = small.height;
             std::string png = encode_png_rgba(w, h, small.pixels);
             if (png.empty()) {
                 return CommandResult::failure("the screenshot could not be encoded");
             }
             Json value = {{"width", w}, {"height", h}};
             if (args.contains("file_name")) {
                 const std::string name = args["file_name"].get<std::string>();
                 const bool safe = std::all_of(name.begin(), name.end(), [](char c) {
                     return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                            (c >= '0' && c <= '9') || c == '-' || c == '_';
                 });
                 if (!safe) {
                     return CommandResult::failure("file_name may only use letters, digits, - and _");
                 }
                 std::error_code error;
                 std::filesystem::create_directories("screenshots", error);
                 const std::filesystem::path path =
                     std::filesystem::path("screenshots") / (name + ".png");
                 std::ofstream file(path, std::ios::binary);
                 file.write(png.data(), static_cast<std::streamsize>(png.size()));
                 if (!file) {
                     return CommandResult::failure("could not write " + path_to_utf8(path));
                 }
                 value["saved_to"] = path_to_utf8(std::filesystem::absolute(path, error));
             }
             CommandResult result = CommandResult::success(std::move(value));
             result.images.push_back({"image/png", std::move(png)});
             return result;
         }});

    add(registry,
        {"quit_editor",
         "Closes the editor after this frame. Not undoable; the MCP connection ends with it.",
         Json{{"type", "object"}, {"additionalProperties", false}}, false,
         [&state](const Json&, CommandContext&) {
             state.quit_requested = true;
             return CommandResult::success({{"quitting", true}});
         }});
}

} // namespace giant::editor
