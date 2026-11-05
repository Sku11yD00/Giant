#include "panels.h"

#include "giant/core/log.h"

#include <imgui.h>
#include <imgui_internal.h> // DockBuilder, for the first-run layout

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <utility>

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

namespace {
ai::Json triple_json(const float v[3]) { return ai::Json::array({v[0], v[1], v[2]}); }
ai::Json triple_json(const Vec3& v) { return ai::Json::array({v.x, v.y, v.z}); }
} // namespace

void EditorPanels::draw_scene_settings() {
    if (ImGui::Begin(k_scene, &m_show_scene)) {
        draw_outliner();
        draw_properties();
        draw_lighting();
    }
    ImGui::End();
}

void EditorPanels::draw_outliner() {
    ImGui::SeparatorText("Entities");
    for (const auto& [label, shape] : {std::pair{"+ Cube", "cube"}, std::pair{"+ Sphere", "sphere"},
                                       std::pair{"+ Plane", "plane"}}) {
        if (ImGui::SmallButton(label)) {
            // In front of the camera, so it shows up where the person looks.
            const EditorCamera& c = m_state.camera;
            const Quat q = quat_from_euler({c.pitch_degrees, c.yaw_degrees, 0.0f});
            const Vec3 ahead = rotate(q, {0.0f, 0.0f, -5.0f});
            const DVec3 at = c.position + DVec3{ahead.x, ahead.y, ahead.z};
            const ai::CommandResult result = m_commands.execute(
                "create_entity", {{"shape", shape}, {"position", {at.x, at.y, at.z}}}, {});
            if (result.ok) {
                m_state.selected = result.value["id"].get<world::EntityId>();
            } else {
                log_warning("editor", "create_entity: " + result.error);
            }
        }
        ImGui::SameLine();
    }
    ImGui::BeginDisabled(m_state.selected == 0);
    if (ImGui::SmallButton("Delete")) {
        run("delete_entities", {{"ids", {m_state.selected}}});
    }
    ImGui::EndDisabled();

    const auto& entities = m_state.scene.entities();
    if (ImGui::BeginChild("outliner", ImVec2(0, 160), ImGuiChildFlags_Borders)) {
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(entities.size()));
        while (clipper.Step()) {
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                const world::Entity& e = entities[static_cast<usize>(i)];
                ImGui::PushID(static_cast<int>(e.id));
                const std::string label = e.name.empty() ? "(unnamed)" : e.name;
                if (ImGui::Selectable(label.c_str(), e.id == m_state.selected)) {
                    m_state.selected = e.id; // selection is view state
                }
                if (!e.visible) {
                    ImGui::SameLine();
                    ImGui::TextDisabled("(hidden)");
                }
                ImGui::PopID();
            }
        }
        if (entities.empty()) {
            ImGui::TextDisabled("Empty scene. Add a shape above, or ask the AI to build one.");
        }
    }
    ImGui::EndChild();
}

void EditorPanels::draw_properties() {
    const world::Entity* e = m_state.scene.find(m_state.selected);
    if (e == nullptr) {
        return;
    }
    ImGui::SeparatorText("Selected");
    const ai::Json id = e->id;
    const auto set = [this, &id](const char* field, ai::Json value) {
        run("set_entity", {{"id", id}, {field, std::move(value)}}, edit_group());
    };

    std::array<char, 129> name{};
    std::snprintf(name.data(), name.size(), "%s", e->name.c_str());
    if (ImGui::InputText("Name", name.data(), name.size(), ImGuiInputTextFlags_EnterReturnsTrue)) {
        set("name", std::string(name.data()));
    }
    int shape = static_cast<int>(e->shape);
    const char* const shapes[] = {"cube", "sphere", "plane"};
    if (ImGui::Combo("Shape", &shape, shapes, IM_ARRAYSIZE(shapes))) {
        set("shape", shapes[shape]);
    }
    double position[3] = {e->transform.position.x, e->transform.position.y,
                          e->transform.position.z};
    if (ImGui::DragScalarN("Position", ImGuiDataType_Double, position, 3, 0.05f, nullptr, nullptr,
                           "%.3f")) {
        set("position", {position[0], position[1], position[2]});
    }
    const EulerDegrees r = euler_from_quat(e->transform.rotation);
    float rotation[3] = {r.pitch, r.yaw, r.roll};
    if (ImGui::DragFloat3("Rotation", rotation, 0.5f, -360.0f, 360.0f, "%.1f")) {
        set("rotation", {{"pitch", rotation[0]}, {"yaw", rotation[1]}, {"roll", rotation[2]}});
    }
    float scale[3] = {e->transform.scale.x, e->transform.scale.y, e->transform.scale.z};
    if (ImGui::DragFloat3("Scale", scale, 0.02f, 0.0001f, 1e6f, "%.3f")) {
        for (float& v : scale) {
            v = std::max(v, 0.0001f);
        }
        set("scale", triple_json(scale));
    }
    float color[3] = {e->material.base_color.x, e->material.base_color.y, e->material.base_color.z};
    if (ImGui::ColorEdit3("Base colour", color, ImGuiColorEditFlags_Float)) {
        for (float& v : color) {
            v = std::clamp(v, 0.0f, 1.0f);
        }
        set("base_color", triple_json(color));
    }
    float roughness = e->material.roughness;
    if (ImGui::SliderFloat("Roughness", &roughness, 0.0f, 1.0f)) {
        set("roughness", std::clamp(roughness, 0.0f, 1.0f));
    }
    float metallic = e->material.metallic;
    if (ImGui::SliderFloat("Metallic", &metallic, 0.0f, 1.0f)) {
        set("metallic", std::clamp(metallic, 0.0f, 1.0f));
    }
    float emissive[3] = {e->material.emissive.x, e->material.emissive.y, e->material.emissive.z};
    if (ImGui::ColorEdit3("Emissive", emissive, ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR)) {
        for (float& v : emissive) {
            v = std::clamp(v, 0.0f, 1000.0f);
        }
        set("emissive", triple_json(emissive));
    }
    bool visible = e->visible;
    if (ImGui::Checkbox("Visible", &visible)) {
        set("visible", visible);
    }
}

void EditorPanels::draw_lighting() {
    ImGui::SeparatorText("Lighting");
    const world::Lighting& l = m_state.scene.lighting;
    const auto set = [this](const char* field, ai::Json value) {
        run("set_lighting", {{field, std::move(value)}}, edit_group());
    };
    float elevation = l.sun_elevation_degrees;
    if (ImGui::SliderFloat("Sun height", &elevation, -90.0f, 90.0f, "%.1f deg")) {
        set("sun_elevation_degrees", std::clamp(elevation, -90.0f, 90.0f));
    }
    float azimuth = l.sun_azimuth_degrees;
    if (ImGui::SliderFloat("Sun direction", &azimuth, 0.0f, 360.0f, "%.1f deg")) {
        set("sun_azimuth_degrees", std::clamp(azimuth, 0.0f, 360.0f));
    }
    float sun_color[3] = {l.sun_color.x, l.sun_color.y, l.sun_color.z};
    if (ImGui::ColorEdit3("Sun colour", sun_color, ImGuiColorEditFlags_Float)) {
        set("sun_color", triple_json(Vec3{std::clamp(sun_color[0], 0.0f, 1.0f),
                                          std::clamp(sun_color[1], 0.0f, 1.0f),
                                          std::clamp(sun_color[2], 0.0f, 1.0f)}));
    }
    float sun = l.sun_intensity;
    if (ImGui::SliderFloat("Sun strength", &sun, 0.0f, 20.0f)) {
        set("sun_intensity", std::clamp(sun, 0.0f, 100.0f));
    }
    float sky[3] = {l.sky_color.x, l.sky_color.y, l.sky_color.z};
    if (ImGui::ColorEdit3("Sky colour", sky, ImGuiColorEditFlags_Float)) {
        set("sky_color", triple_json(Vec3{std::clamp(sky[0], 0.0f, 1.0f),
                                          std::clamp(sky[1], 0.0f, 1.0f),
                                          std::clamp(sky[2], 0.0f, 1.0f)}));
    }
    float ground[3] = {l.ground_color.x, l.ground_color.y, l.ground_color.z};
    if (ImGui::ColorEdit3("Ground bounce", ground, ImGuiColorEditFlags_Float)) {
        set("ground_color", triple_json(Vec3{std::clamp(ground[0], 0.0f, 1.0f),
                                             std::clamp(ground[1], 0.0f, 1.0f),
                                             std::clamp(ground[2], 0.0f, 1.0f)}));
    }
    float ambient = l.ambient_intensity;
    if (ImGui::SliderFloat("Ambient", &ambient, 0.0f, 5.0f)) {
        set("ambient_intensity", std::clamp(ambient, 0.0f, 100.0f));
    }
    float exposure = l.exposure;
    if (ImGui::SliderFloat("Exposure", &exposure, 0.05f, 8.0f, "%.2f",
                           ImGuiSliderFlags_Logarithmic)) {
        set("exposure", std::clamp(exposure, 0.01f, 100.0f));
    }
    ImGui::TextDisabled("Colours are linear. Undo with Ctrl+Z.");
    ImGui::TextDisabled("Fly: hold right mouse, W A S D, Q / E, Shift.");
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
        ImGui::Text("Scene: %u instances in %u draws", frame.scene_instances, frame.scene_draws);
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
