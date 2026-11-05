#include "editor_commands.h"

#include "giant/core/assert.h"

#include <string>

namespace giant::editor {

namespace {

using ai::CommandContext;
using ai::CommandResult;
using ai::Json;

constexpr const char* k_version = "0.1.0";

Json color_to_json(const rhi::Color& c) { return {{"r", c.r}, {"g", c.g}, {"b", c.b}}; }

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

const char* mcp_instructions() {
    return "This is the Giant Engine editor. Every tool is an editor command; changes made "
           "through tools can be reverted with `undo` and repeated with `redo`, and "
           "`get_history` lists them. Read state with the get_* tools before changing it. "
           "Colours are linear (not sRGB), each channel 0 to 1. Check `get_log` after a change "
           "for warnings and errors.";
}

void register_editor_commands(ai::CommandRegistry& registry, EditorState& state) {
    add(registry,
        {"get_engine_info",
         "Describes the running editor: version, graphics backend and adapter, whether the GPU "
         "validation layer runs, frames presented and the window size in pixels.",
         Json{{"type", "object"}, {"additionalProperties", false}}, true,
         [&state](const Json&, CommandContext&) {
             const auto& adapter = state.device->adapter();
             return CommandResult::success(
                 {{"editor_version", k_version},
                  {"backend", rhi::to_string(state.device->backend())},
                  {"adapter", adapter.name},
                  {"software_adapter", adapter.software},
                  {"meets_minimum_spec", adapter.meets_minimum_spec()},
                  {"validation_enabled", state.device->validation_enabled()},
                  {"frames_presented", state.device->frames_presented()},
                  {"window", {{"pixel_width", state.window->pixel_width()},
                              {"pixel_height", state.window->pixel_height()},
                              {"point_width", state.window->point_width()},
                              {"point_height", state.window->point_height()},
                              {"minimized", state.window->minimized()}}}});
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
        {"get_clear_color", "Returns the viewport background colour (linear RGB, 0 to 1).",
         Json{{"type", "object"}, {"additionalProperties", false}}, true,
         [&state](const Json&, CommandContext&) {
             return CommandResult::success(color_to_json(state.clear_color));
         }});

    add(registry,
        {"set_clear_color",
         "Sets the viewport background colour, in linear RGB with each channel from 0 to 1 "
         "(the screen shows it sRGB-encoded). Undoable.",
         Json::parse(R"({"type": "object", "additionalProperties": false,
             "required": ["r", "g", "b"], "properties": {
             "r": {"type": "number", "minimum": 0, "maximum": 1},
             "g": {"type": "number", "minimum": 0, "maximum": 1},
             "b": {"type": "number", "minimum": 0, "maximum": 1}}})"),
         false,
         [&state](const Json& args, CommandContext& context) {
             const rhi::Color before = state.clear_color;
             const rhi::Color after{args["r"].get<float>(), args["g"].get<float>(),
                                    args["b"].get<float>(), 1.0f};
             state.clear_color = after;
             context.record({[&state, before] { state.clear_color = before; },
                             [&state, after] { state.clear_color = after; }});
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
        {"quit_editor",
         "Closes the editor after this frame. Not undoable; the MCP connection ends with it.",
         Json{{"type", "object"}, {"additionalProperties", false}}, false,
         [&state](const Json&, CommandContext&) {
             state.quit_requested = true;
             return CommandResult::success({{"quitting", true}});
         }});
}

} // namespace giant::editor
