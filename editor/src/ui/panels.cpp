#include "panels.h"

#include "giant/core/log.h"

#include <imgui.h>
#include <imgui_internal.h> // DockBuilder, for the first-run layout

#include <algorithm>
#include <cstring>

namespace giant::editor {

namespace {

constexpr const char* k_scene = "Scene";
constexpr const char* k_log = "Log";
constexpr const char* k_commands = "Commands";
constexpr const char* k_engine = "Engine";

const char* const k_levels[] = {"trace", "info", "warning", "error"};

ImVec4 level_color(LogLevel level) {
    switch (level) {
    case LogLevel::error: return {1.0f, 0.45f, 0.4f, 1.0f};
    case LogLevel::warning: return {1.0f, 0.8f, 0.35f, 1.0f};
    case LogLevel::trace: return {0.6f, 0.6f, 0.6f, 1.0f};
    default: return {0.9f, 0.9f, 0.9f, 1.0f};
    }
}

bool contains_ignoring_case(const std::string& text, const std::string& needle) {
    if (needle.empty()) {
        return true;
    }
    const auto it = std::search(text.begin(), text.end(), needle.begin(), needle.end(),
                                [](char a, char b) {
                                    const auto lower = [](char c) {
                                        return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32)
                                                                      : c;
                                    };
                                    return lower(a) == lower(b);
                                });
    return it != text.end();
}

} // namespace

EditorPanels::EditorPanels(ai::CommandRegistry& commands, EditorState& state, const LogHistory& log)
    : m_commands(commands), m_state(state), m_log(log) {}

void EditorPanels::run(const char* command, const ai::Json& args, u64 merge_group) {
    const ai::CommandResult result = m_commands.execute(command, args, {merge_group, true});
    if (!result.ok) {
        log_warning("editor", std::string(command) + ": " + result.error);
    }
}

u64 EditorPanels::edit_group() const {
    return (static_cast<u64>(ImGui::GetItemID()) << 32) | m_edit_epoch;
}

void EditorPanels::draw(const FrameInfo& frame) {
    const ImGuiID dockspace =
        ImGui::DockSpaceOverViewport(0, nullptr, ImGuiDockNodeFlags_PassthruCentralNode);
    if (m_reset_layout) {
        m_reset_layout = false;
        build_default_layout(dockspace);
    } else if (!m_layout_built) {
        const ImGuiDockNode* node = ImGui::DockBuilderGetNode(dockspace);
        if (node == nullptr || node->IsLeafNode()) {
            build_default_layout(dockspace); // first run, or no saved layout
        }
    }
    m_layout_built = true;

    // Global shortcuts, unless a text field has the keyboard.
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Z, ImGuiInputFlags_RouteGlobal)) {
        run("undo");
    }
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Y, ImGuiInputFlags_RouteGlobal) ||
        ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Z, ImGuiInputFlags_RouteGlobal)) {
        run("redo");
    }

    draw_menu_bar();
    if (m_show_scene) draw_scene_settings();
    if (m_show_log) draw_log();
    if (m_show_commands) draw_commands();
    if (m_show_engine) draw_engine(frame);
    if (m_show_demo) ImGui::ShowDemoWindow(&m_show_demo);

    if (!ImGui::IsAnyItemActive()) {
        ++m_edit_epoch; // the next edit starts a new undo step
    }
}

void EditorPanels::build_default_layout(unsigned int dockspace_id) {
    ImGui::DockBuilderRemoveNode(dockspace_id);
    ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockspace_id, ImGui::GetMainViewport()->WorkSize);
    ImGuiID center = dockspace_id;
    const ImGuiID left = ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, 0.22f, nullptr, &center);
    const ImGuiID right =
        ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.28f, nullptr, &center);
    const ImGuiID bottom =
        ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.3f, nullptr, &center);
    ImGui::DockBuilderDockWindow(k_scene, left);
    ImGui::DockBuilderDockWindow(k_engine, left);
    ImGui::DockBuilderDockWindow(k_commands, right);
    ImGui::DockBuilderDockWindow(k_log, bottom);
    ImGui::DockBuilderFinish(dockspace_id);
}

void EditorPanels::draw_menu_bar() {
    if (!ImGui::BeginMainMenuBar()) {
        return;
    }
    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("Quit")) {
            run("quit_editor");
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Edit")) {
        if (ImGui::MenuItem("Undo", "Ctrl+Z", false, m_commands.can_undo())) {
            run("undo");
        }
        if (ImGui::MenuItem("Redo", "Ctrl+Y", false, m_commands.can_redo())) {
            run("redo");
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("View")) {
        ImGui::MenuItem(k_scene, nullptr, &m_show_scene);
        ImGui::MenuItem(k_log, nullptr, &m_show_log);
        ImGui::MenuItem(k_commands, nullptr, &m_show_commands);
        ImGui::MenuItem(k_engine, nullptr, &m_show_engine);
        ImGui::Separator();
        if (ImGui::MenuItem("Reset layout")) {
            m_reset_layout = true; // applied at the start of the next frame
        }
        ImGui::MenuItem("UI demo (for developers)", nullptr, &m_show_demo);
        ImGui::EndMenu();
    }
    ImGui::EndMainMenuBar();
}

void EditorPanels::draw_scene_settings() {
    if (ImGui::Begin(k_scene, &m_show_scene)) {
        ImGui::SeparatorText("Viewport");
        float color[3] = {m_state.clear_color.r, m_state.clear_color.g, m_state.clear_color.b};
        // The command takes linear colour; the picker shows it as such.
        if (ImGui::ColorEdit3("Background", color, ImGuiColorEditFlags_Float)) {
            run("set_clear_color",
                {{"r", std::clamp(color[0], 0.0f, 1.0f)},
                 {"g", std::clamp(color[1], 0.0f, 1.0f)},
                 {"b", std::clamp(color[2], 0.0f, 1.0f)}},
                edit_group());
        }
        ImGui::TextDisabled("Linear RGB. Undo with Ctrl+Z.");
    }
    ImGui::End();
}

void EditorPanels::draw_log() {
    if (ImGui::Begin(k_log, &m_show_log)) {
        ImGui::SetNextItemWidth(110.0f);
        ImGui::Combo("##level", &m_log_level, k_levels, IM_ARRAYSIZE(k_levels));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(220.0f);
        if (ImGui::InputTextWithHint("##filter", "filter", m_log_filter_buffer.data(),
                                     m_log_filter_buffer.size())) {
            m_log_filter = m_log_filter_buffer.data();
        }
        ImGui::SameLine();
        ImGui::Checkbox("Follow", &m_log_follow);
        ImGui::SameLine();
        const auto all = m_log.recent(2000, static_cast<LogLevel>(m_log_level));
        if (ImGui::Button("Clear") && !all.empty()) {
            m_log_hidden_before = all.back().sequence; // view only: the AI still sees them
        }
        ImGui::Separator();

        if (ImGui::BeginChild("lines", ImVec2(0, 0), ImGuiChildFlags_None,
                              ImGuiWindowFlags_HorizontalScrollbar)) {
            std::vector<const StoredLogRecord*> shown;
            for (const auto& record : all) {
                if (record.sequence > m_log_hidden_before &&
                    (contains_ignoring_case(record.text, m_log_filter) ||
                     contains_ignoring_case(record.channel, m_log_filter))) {
                    shown.push_back(&record);
                }
            }
            ImGuiListClipper clipper;
            clipper.Begin(static_cast<int>(shown.size()));
            while (clipper.Step()) {
                for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                    const StoredLogRecord& r = *shown[static_cast<usize>(i)];
                    ImGui::TextColored(level_color(r.level), "[%s] %s: %s", to_string(r.level),
                                       r.channel.c_str(), r.text.c_str());
                }
            }
            if (m_log_follow && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f) {
                ImGui::SetScrollHereY(1.0f);
            }
        }
        ImGui::EndChild();
    }
    ImGui::End();
}

void EditorPanels::draw_commands() {
    if (ImGui::Begin(k_commands, &m_show_commands)) {
        ImGui::SeparatorText("History");
        if (ImGui::Button("Undo")) {
            run("undo");
        }
        ImGui::SameLine();
        if (ImGui::Button("Redo")) {
            run("redo");
        }
        const auto undo = m_commands.undo_history();
        const auto redo = m_commands.redo_history();
        if (undo.empty() && redo.empty()) {
            ImGui::TextDisabled("Nothing to undo yet.");
        }
        for (const auto& entry : undo) {
            ImGui::BulletText("%s", entry.command.c_str());
        }
        for (auto it = redo.rbegin(); it != redo.rend(); ++it) {
            ImGui::TextDisabled("  (undone) %s", it->command.c_str());
        }

        ImGui::SeparatorText("All commands");
        ImGui::TextWrapped("People, the built-in assistant and Claude Code (over MCP) all use "
                           "these.");
        if (ImGui::BeginTable("commands", 2,
                              ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                                  ImGuiTableFlags_Resizable)) {
            ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthFixed, 140.0f);
            ImGui::TableSetupColumn("What it does");
            ImGui::TableHeadersRow();
            for (const auto& command : m_commands.commands()) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(command.name.c_str());
                if (command.read_only) {
                    ImGui::SameLine();
                    ImGui::TextDisabled("(read)");
                }
                ImGui::TableNextColumn();
                ImGui::TextWrapped("%s", command.description.c_str());
            }
            ImGui::EndTable();
        }
    }
    ImGui::End();
}

void EditorPanels::draw_engine(const FrameInfo& frame) {
    if (ImGui::Begin(k_engine, &m_show_engine)) {
        const rhi::AdapterInfo& adapter = m_state.device->adapter();
        ImGui::SeparatorText("Graphics");
        ImGui::Text("Backend: %s", rhi::to_string(m_state.device->backend()));
        ImGui::TextWrapped("GPU: %s%s", adapter.name.c_str(),
                           adapter.software ? " (software)" : "");
        ImGui::Text("Minimum spec: %s", adapter.meets_minimum_spec() ? "yes" : "no");
        ImGui::Text("Validation layer: %s", m_state.device->validation_enabled() ? "on" : "off");
        ImGui::SeparatorText("Frame");
        ImGui::Text("Frame time: %.2f ms", static_cast<double>(frame.frame_ms));
        ImGui::Text("UI draw calls: %u", frame.ui_draw_calls);
        ImGui::Text("Window: %u x %u pixels", m_state.window->pixel_width(),
                    m_state.window->pixel_height());
        ImGui::SeparatorText("C# scripts");
        if (m_state.scripts_loaded.empty()) {
            ImGui::TextDisabled(m_state.dotnet_available ? "None loaded (load_scripts command)."
                                                         : ".NET 10 not found: see the log.");
        } else {
            ImGui::TextWrapped("%s", m_state.scripts_loaded.c_str());
            ImGui::Text("Running: %u", m_state.scripts_running);
            if (ImGui::Button("Reload")) {
                run("load_scripts", {{"path", m_state.scripts_loaded}});
            }
            ImGui::SameLine();
            if (ImGui::Button("Unload")) {
                run("unload_scripts");
            }
        }
    }
    ImGui::End();
}

} // namespace giant::editor
