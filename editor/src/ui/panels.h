// The editor's panels: menu bar, scene settings, log, commands and history,
// engine info. Every change goes through a command (AGENTS.md rule 7), so
// what a person does here is what the AI can do over MCP, and both share one
// undo history. Panel visibility and layout are view state, not commands.
#pragma once

#include "../editor_commands.h"

#include "giant/ai/commands.h"
#include "giant/core/log_history.h"

#include <array>
#include <string>

namespace giant::editor {

struct FrameInfo {
    float frame_ms = 0.0f;
    u32 ui_draw_calls = 0;
};

class EditorPanels {
public:
    EditorPanels(ai::CommandRegistry& commands, EditorState& state, const LogHistory& log);

    // Builds this frame's UI. Call between ImGuiLayer::begin_frame and
    // end_frame.
    void draw(const FrameInfo& frame);

private:
    void draw_menu_bar();
    void draw_scene_settings(); // the Scene panel: outliner, properties, lighting
    void draw_outliner();
    void create_ahead(ai::Json args); // create_entity 5 m in front of the camera, selected
    void draw_properties();
    void draw_lighting();
    void draw_weather();
    void draw_clock();
    void draw_terrain_brush(); // the viewport brush's settings (EditorState::brush)
    void draw_terrain_layers(); // the ground's layers (set_terrain_layers)
    void draw_scatter();       // the scatter layers' main settings
    void draw_water();         // the lakes, rivers and sea: level, waves, shown
    void draw_effects();       // effects running, and buttons playing each once
    void draw_log();
    void draw_commands();
    void draw_engine(const FrameInfo& frame);
    void build_default_layout(unsigned int dockspace_id);
    // Runs a command from the UI; failures are logged for the person to see.
    void run(const char* command, const ai::Json& args = ai::Json::object(), u64 merge_group = 0);
    // One merge group per continuous edit of the current widget.
    u64 edit_group() const;

    ai::CommandRegistry& m_commands;
    EditorState& m_state;
    const LogHistory& m_log;

    bool m_show_scene = true;
    bool m_show_log = true;
    bool m_show_commands = true;
    bool m_show_engine = true;
    bool m_show_demo = false;
    bool m_layout_built = false;
    bool m_reset_layout = false;

    int m_log_level = 1; // index into trace, info, warning, error
    bool m_log_follow = true;
    u64 m_log_hidden_before = 0; // "Clear" hides lines up to this sequence
    std::array<char, 128> m_log_filter_buffer{}; // ImGui edits this in place
    std::string m_log_filter;
    std::array<char, 1024> m_import_path{}; // the "Add mesh" field

    u32 m_edit_epoch = 1; // bumped whenever no widget is being edited
};

} // namespace giant::editor
