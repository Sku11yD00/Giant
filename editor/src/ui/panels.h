// The editor's panels: menu bar, scene settings, log, commands and history,
// engine info. Every change goes through a command (AGENTS.md rule 7), so
// what a person does here is what the AI can do over MCP, and both share one
// undo history. Panel visibility and layout are view state, not commands.
#pragma once

#include "../chat.h"
#include "../editor_commands.h"
#include "../people_list.h"

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
    void draw_people();        // the People panel: list, filter, follow one, what it does and why
    void draw_chat();          // the Chat panel: the conversation with Claude, and a box to write in
    void draw_log();
    void draw_commands();
    void draw_engine(const FrameInfo& frame);
    void draw_mods_scripts_request(); // asks the person whether a mod's scripts may run (EditorState::mods_scripts_request)
    void build_default_layout(unsigned int dockspace_id);
    // Runs a command from the UI; failures are logged for the person to see.
    void run(const char* command, const ai::Json& args = ai::Json::object(), u64 merge_group = 0);
    // One merge group per continuous edit of the current widget.
    u64 edit_group() const;

    ai::CommandRegistry& m_commands;
    EditorState& m_state;
    const LogHistory& m_log;

    bool m_show_scene = true;
    bool m_show_people = true;
    bool m_show_chat = true;
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

    PeopleList m_people_list;                       // the People panel's (filter and those shown)
    std::array<char, 96> m_people_filter_buffer{};  // ImGui edits this in place
    world::PersonId m_person_shown = 0;             // the one whose state is shown (0: none)
    u64 m_person_scene = 0;                         // the scene that one is of
    std::string m_person_why;                       // person_why of that one, made anew a few times a second
    int m_person_why_frame = -1;                    // the frame it was made
    int m_people_frame = -1;                        // the frame the list was last updated

    ChatView m_chat;                          // the Chat panel's copy of the conversation
    std::array<char, 4096> m_chat_input{};    // ImGui edits this in place
    std::string m_chat_error;                 // why the last message could not be sent
    bool m_chat_details = false;              // show what each tool was given and gave back
    bool m_chat_to_end = true;                // scroll to the newest line at the next draw
    bool m_chat_focus = false;                // put the keyboard back in the box at the next draw

    u32 m_edit_epoch = 1; // bumped whenever no widget is being edited
};

} // namespace giant::editor
