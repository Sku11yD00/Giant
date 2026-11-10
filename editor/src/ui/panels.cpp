#include "panels.h"

#include "../mesh_library.h"
#include "../physics_state.h"
#include "../play_state.h"
#include "../weather_state.h"

#include "giant/core/log.h"
#include "giant/sim/swing.h"
#include "giant/world/weather.h"

#include <imgui.h>
#include <imgui_internal.h> // DockBuilder, for the first-run layout

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string_view>
#include <utility>
#include <vector>

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
    if (m_state.physics != nullptr) {
        // A play button: dynamic entities fall while on, and go back when off.
        bool on = m_state.physics->simulating();
        if (ImGui::MenuItem(on ? "Stop simulating" : "Simulate", nullptr, on)) {
            run("simulate", {{"on", !on}});
        }
    }
    if (m_state.play != nullptr) {
        // Walk the scene as the player (Esc stops).
        const bool playing = m_state.play->playing();
        if (ImGui::MenuItem(playing ? "Stop playing (Esc)" : "Play", nullptr, playing)) {
            run("play", {{"on", !playing}});
        }
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
        draw_weather();
        draw_terrain_brush();
        draw_scatter();
    }
    ImGui::End();
}

void EditorPanels::draw_scatter() {
    ImGui::SeparatorText("Scatter");
    if (m_state.scene.scatter.empty()) {
        ImGui::TextDisabled("No layers (add_scatter_layer adds one).");
        return;
    }
    for (const world::ScatterLayer& l : m_state.scene.scatter) {
        ImGui::PushID(l.name.c_str());
        const std::string name = l.name; // set_scatter_layer replaces the layer (the list keeps its size)
        const auto set = [&](const char* field, ai::Json value) {
            run("set_scatter_layer", {{"name", name}, {field, std::move(value)}}, edit_group());
        };
        bool visible = l.visible;
        if (ImGui::Checkbox(name.c_str(), &visible)) set("visible", visible);
        float spacing = l.spacing;
        if (ImGui::SliderFloat("Spacing (m)", &spacing, 1.0f, 64.0f, "%.2f",
                               ImGuiSliderFlags_Logarithmic)) {
            set("spacing", std::clamp(spacing, world::k_min_scatter_spacing, world::k_max_scatter_spacing));
        }
        float density = l.density;
        if (ImGui::SliderFloat("Density", &density, 0.0f, 1.0f, "%.2f")) set("density", std::clamp(density, 0.0f, 1.0f));
        float slope = l.slope_max;
        if (ImGui::SliderFloat("Steepest (deg)", &slope, l.slope_min, 90.0f, "%.0f")) {
            set("slope_max", std::clamp(slope, l.slope_min, 90.0f));
        }
        ImGui::PopID();
    }
}

void EditorPanels::draw_terrain_brush() {
    ImGui::SeparatorText("Terrain brush");
    BrushSettings& b = m_state.brush;
    ImGui::Checkbox("Sculpt (left mouse in the viewport)", &b.enabled);
    static constexpr const char* k_ops[] = {"raise", "lower", "smooth", "flatten"};
    int op = 0;
    for (int k = 0; k < 4; ++k) {
        if (b.op == k_ops[k]) op = k;
    }
    for (int k = 0; k < 4; ++k) {
        if (k > 0) ImGui::SameLine();
        if (ImGui::RadioButton(k_ops[k], op == k)) b.op = k_ops[k];
    }
    // Up to 128 samples (a dab stays a small part of a frame); larger
    // shapes come from import_height_map or sculpt_terrain over MCP.
    const double spacing = m_state.scene.terrain.empty() ? 1.0 : m_state.scene.terrain.spacing();
    const auto most = static_cast<float>(std::max(1.0, BrushSettings::max_radius(spacing)));
    b.radius = std::min(b.radius, static_cast<double>(most));
    float radius = static_cast<float>(b.radius);
    if (ImGui::SliderFloat("Radius (m)", &radius, 0.5f, most, "%.1f", ImGuiSliderFlags_Logarithmic)) {
        b.radius = std::clamp(static_cast<double>(radius), 0.5, static_cast<double>(most));
    }
    const bool metres = b.op == "raise" || b.op == "lower";
    ImGui::SliderFloat(metres ? "Strength (m a dab)" : "Strength", &b.strength, 0.0f, metres ? 10.0f : 1.0f, "%.2f");
    ImGui::SliderFloat("Falloff", &b.falloff, 0.0f, 1.0f, "%.2f");
    if (b.op == "flatten") {
        ImGui::Checkbox("To the ground where the stroke starts", &b.flatten_to_start);
        if (!b.flatten_to_start) ImGui::DragFloat("Height (m)", &b.height, 0.1f, -10000.0f, 10000.0f, "%.1f");
    }
}

void EditorPanels::create_ahead(ai::Json args) {
    // In front of the camera, so it shows up where the person looks.
    const EditorCamera& c = m_state.camera;
    const Quat q = quat_from_euler({c.pitch_degrees, c.yaw_degrees, 0.0f});
    const Vec3 ahead = rotate(q, {0.0f, 0.0f, -5.0f});
    const DVec3 at = c.position + DVec3{ahead.x, ahead.y, ahead.z};
    args["position"] = {at.x, at.y, at.z};
    const ai::CommandResult result = m_commands.execute("create_entity", args, {});
    if (result.ok) {
        m_state.selected = result.value["id"].get<world::EntityId>();
    } else {
        log_warning("editor", "create_entity: " + result.error);
    }
}

void EditorPanels::draw_outliner() {
    ImGui::SeparatorText("Entities");
    for (const auto& [label, shape] : {std::pair{"+ Cube", "cube"}, std::pair{"+ Sphere", "sphere"},
                                       std::pair{"+ Plane", "plane"}}) {
        if (ImGui::SmallButton(label)) {
            create_ahead({{"shape", shape}});
        }
        ImGui::SameLine();
    }
    if (ImGui::SmallButton("+ Decal")) {
        // A dark 2 m splat reaching half a metre up and down.
        create_ahead({{"shape", "decal"},
                      {"scale", {2.0, 1.0, 2.0}},
                      {"base_color", {0.05, 0.04, 0.03}},
                      {"roughness", 0.9},
                      {"decal", {{"shape", "splat"}}}});
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("+ Cloth")) {
        // A red banner hanging from its top edge.
        create_ahead({{"shape", "cloth"}, {"base_color", {0.6, 0.08, 0.06}}, {"roughness", 0.8}});
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("+ Lantern")) {
        // A lantern hanging half a metre below its hook, swinging in the wind.
        create_ahead({{"shape", "cube"},
                      {"scale", {0.25, 0.35, 0.25}},
                      {"base_color", {0.2, 0.15, 0.08}},
                      {"metallic", 1.0},
                      {"roughness", 0.5},
                      {"emissive", {2.0, 1.4, 0.6}},
                      {"swing", {{"on", true}, {"length", 0.5}, {"weight", 1.5}}}});
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(m_state.scene.find(m_state.selected) == nullptr);
    if (ImGui::SmallButton("Delete")) {
        run("delete_entities", {{"ids", {m_state.selected}}});
    }
    ImGui::EndDisabled();
    // An imported mesh: a glTF or GLB path from the working folder.
    ImGui::SetNextItemWidth(-ImGui::CalcTextSize("+ Mesh").x - ImGui::GetStyle().FramePadding.x * 4);
    const bool entered = ImGui::InputTextWithHint("##mesh_path", "assets/model.glb", m_import_path.data(),
                                                  m_import_path.size(), ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if ((ImGui::SmallButton("+ Mesh") || entered) && m_import_path[0] != '\0') {
        create_ahead({{"mesh", std::string(m_import_path.data())}});
    }

    // Kits: place one ahead of the camera, or turn the selection into one.
    if (!m_state.scene.kits.empty()) {
        ImGui::TextDisabled("Kits:");
        for (const world::Kit& kit : m_state.scene.kits) {
            ImGui::SameLine();
            ImGui::PushID(kit.name.c_str());
            if (ImGui::SmallButton(("+ " + kit.name).c_str())) {
                create_ahead({{"kit", kit.name}});
            }
            ImGui::PopID();
        }
    }
    {
        const world::Entity* selected = m_state.scene.find(m_state.selected);
        ImGui::BeginDisabled(selected == nullptr || selected->shape == world::Shape::kit);
        if (ImGui::SmallButton("Selected to kit")) {
            usize n = m_state.scene.kits.size() + 1;
            while (m_state.scene.find_kit("kit " + std::to_string(n)) != nullptr) ++n;
            const std::string name = "kit " + std::to_string(n);
            run("create_kit", {{"name", name}, {"from_entities", {m_state.selected}}, {"replace_entities", true}});
        }
        ImGui::EndDisabled();
    }

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
    // "kit" needs a kit to place: offered only when the scene has one.
    const bool kits = !m_state.scene.kits.empty() || e->shape == world::Shape::kit;
    std::vector<const char*> shapes = {"cube", "sphere", "plane", "mesh"};
    if (kits) shapes.push_back("kit");
    shapes.push_back("decal");
    shapes.push_back("cloth");
    const char* current = world::to_string(e->shape);
    int shape = 0;
    for (usize i = 0; i < shapes.size(); ++i) {
        if (std::string_view(shapes[i]) == current) shape = static_cast<int>(i);
    }
    if (ImGui::Combo("Shape", &shape, shapes.data(), static_cast<int>(shapes.size()))) {
        const std::string_view chosen = shapes[static_cast<usize>(shape)];
        if (chosen == "kit" && m_state.scene.find_kit(e->kit) == nullptr) {
            if (!m_state.scene.kits.empty()) {
                run("set_entity", {{"id", id}, {"kit", m_state.scene.kits.front().name}}, edit_group());
            }
        } else {
            set("shape", std::string(chosen));
        }
    }
    {
        // How it collides: fixed, falls (while simulating), or not at all.
        const char* bodies[] = {"fixed", "dynamic", "none"};
        int body = static_cast<int>(e->body);
        if (ImGui::Combo("Body", &body, bodies, IM_ARRAYSIZE(bodies))) {
            set("body", std::string(bodies[body]));
        }
    }
    if (e->shape == world::Shape::kit) {
        if (ImGui::BeginCombo("Kit", e->kit.c_str())) {
            for (const world::Kit& kit : m_state.scene.kits) {
                if (ImGui::Selectable(kit.name.c_str(), kit.name == e->kit)) {
                    set("kit", kit.name);
                }
            }
            ImGui::EndCombo();
        }
        if (const world::Kit* kit = m_state.scene.find_kit(e->kit)) {
            ImGui::TextDisabled("%zu pieces (their own materials)", kit->pieces.size());
        }
    }
    if (e->shape == world::Shape::decal) {
        int mask = static_cast<int>(e->decal.shape);
        const char* const masks[] = {"disc", "rectangle", "ring", "splat", "cracks"};
        if (ImGui::Combo("Stamp", &mask, masks, IM_ARRAYSIZE(masks))) {
            set("decal", {{"shape", masks[mask]}});
        }
        float opacity = e->decal.opacity;
        if (ImGui::SliderFloat("Opacity", &opacity, 0.0f, 1.0f)) {
            set("decal", {{"opacity", std::clamp(opacity, 0.0f, 1.0f)}});
        }
        float softness = e->decal.softness;
        if (ImGui::SliderFloat("Softness", &softness, 0.0f, 1.0f)) {
            set("decal", {{"softness", std::clamp(softness, 0.0f, 1.0f)}});
        }
        u32 seed = e->decal.seed;
        const u32 step = 1;
        if (ImGui::InputScalar("Seed", ImGuiDataType_U32, &seed, &step)) {
            set("decal", {{"seed", seed}});
        }
    }
    if (e->shape == world::Shape::cloth) {
        float width = e->cloth.width;
        if (ImGui::DragFloat("Width", &width, 0.01f, 0.05f, 20.0f, "%.2f m")) {
            set("cloth", {{"width", std::clamp(width, 0.05f, 20.0f)}});
        }
        float height = e->cloth.height;
        if (ImGui::DragFloat("Height", &height, 0.01f, 0.05f, 20.0f, "%.2f m")) {
            set("cloth", {{"height", std::clamp(height, 0.05f, 20.0f)}});
        }
        int pin = static_cast<int>(e->cloth.pin);
        const char* const pins[] = {"top", "corners", "left"};
        if (ImGui::Combo("Held by", &pin, pins, IM_ARRAYSIZE(pins))) {
            set("cloth", {{"pin", pins[pin]}});
        }
        float weight = e->cloth.weight;
        if (ImGui::SliderFloat("Weight", &weight, 0.02f, 5.0f, "%.2f kg/m2", ImGuiSliderFlags_Logarithmic)) {
            set("cloth", {{"weight", std::clamp(weight, 0.02f, 5.0f)}});
        }
        float stiffness = e->cloth.stiffness;
        if (ImGui::SliderFloat("Stiffness", &stiffness, 0.0f, 1.0f)) {
            set("cloth", {{"stiffness", std::clamp(stiffness, 0.0f, 1.0f)}});
        }
    }
    if (sim::can_swing(e->shape)) {
        bool swings = e->swing.on;
        if (ImGui::Checkbox("Swings", &swings)) {
            set("swing", {{"on", swings}});
        }
        if (e->swing.on) {
            float length = e->swing.length;
            if (ImGui::DragFloat("Hangs below", &length, 0.01f, 0.02f, 20.0f, "%.2f m")) {
                set("swing", {{"length", std::clamp(length, 0.02f, 20.0f)}});
            }
            int hinge = static_cast<int>(e->swing.hinge);
            const char* const hinges[] = {"free", "x"};
            if (ImGui::Combo("Hinge", &hinge, hinges, IM_ARRAYSIZE(hinges))) {
                set("swing", {{"hinge", hinges[hinge]}});
            }
            float weight = e->swing.weight;
            if (ImGui::SliderFloat("Swing weight", &weight, 0.05f, 1000.0f, "%.2f kg", ImGuiSliderFlags_Logarithmic)) {
                set("swing", {{"weight", std::clamp(weight, 0.05f, 1000.0f)}});
            }
            float limit = e->swing.limit;
            if (ImGui::SliderFloat("Swing limit", &limit, 5.0f, 175.0f, "%.0f deg")) {
                set("swing", {{"limit", std::clamp(limit, 5.0f, 175.0f)}});
            }
        }
    }
    if (e->shape == world::Shape::mesh || !e->mesh.empty()) {
        ImGui::TextWrapped("Mesh: %s", e->mesh.empty() ? "(none)" : e->mesh.c_str());
        if (m_state.meshes != nullptr && !e->mesh.empty()) {
            if (const MeshLibrary::Mesh* m = m_state.meshes->find(e->mesh)) {
                ImGui::TextDisabled("%u triangles, %u clusters, %u levels", m->triangles, m->clusters,
                                    m->levels);
            } else {
                ImGui::TextDisabled("not loaded (see the log)");
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Reload")) {
                run("import_mesh", {{"path", e->mesh}, {"reload", true}});
            }
        }
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

void EditorPanels::draw_weather() {
    ImGui::SeparatorText("Weather");
    const std::optional<world::Weather>& w = m_state.scene.weather;
    // A preset blends in over a few seconds; the sliders change it at once.
    if (ImGui::BeginCombo("Preset", "choose...")) {
        for (const std::string_view name : world::weather_preset_names()) {
            if (ImGui::Selectable(std::string(name).c_str())) {
                run("set_weather", {{"preset", std::string(name)}, {"over_seconds", 5.0}}, 0);
            }
        }
        ImGui::EndCombo();
    }
    if (!w) {
        ImGui::TextDisabled("No weather: the flat sky colours above.");
        return;
    }
    const auto set = [this](const char* field, float value) {
        ai::Json args = {{field, value}};
        // During a preset's blend, a slider keeps blending (over what is left).
        if (m_state.weather != nullptr && m_state.weather->remaining() > 0.0) {
            args["over_seconds"] = m_state.weather->remaining();
        }
        run("set_weather", args, edit_group());
    };
    const auto slider = [&set](const char* label, const char* field, float value, float lo, float hi,
                               const char* format = "%.2f", ImGuiSliderFlags flags = 0) {
        if (ImGui::SliderFloat(label, &value, lo, hi, format, flags)) set(field, std::clamp(value, lo, hi));
    };
    slider("Cloud cover", "cloud_cover", w->cloud_cover, 0.0f, 1.0f);
    slider("Cloud density", "cloud_density", w->cloud_density, 0.0f, 1.0f);
    slider("Cloud base", "cloud_base", w->cloud_base, 100.0f, 10000.0f, "%.0f m");
    slider("Cloud thickness", "cloud_thickness", w->cloud_thickness, 100.0f, 10000.0f, "%.0f m");
    slider("Cloud size", "cloud_scale", w->cloud_scale, 0.25f, 4.0f);
    slider("Wind direction", "wind_direction_degrees", w->wind_direction_degrees, -360.0f, 360.0f, "%.0f deg");
    slider("Wind speed", "wind_speed", w->wind_speed, 0.0f, 200.0f, "%.1f m/s", ImGuiSliderFlags_Logarithmic);
    slider("Fog", "fog_density", w->fog_density, 0.0f, 1.0f);
    slider("Fog height", "fog_height", w->fog_height, 1.0f, 10000.0f, "%.0f m", ImGuiSliderFlags_Logarithmic);
    slider("Haze", "haze", w->haze, 0.0f, 10.0f);
    slider("Rain / snow", "precipitation", w->precipitation, 0.0f, 1.0f);
    slider("Temperature", "temperature", w->temperature, -50.0f, 50.0f, "%.0f C");
    // The ground's start values: setting one puts the ground there now.
    slider("Ground wet", "wetness", w->wetness, 0.0f, 1.0f);
    slider("Snow lying", "snow_cover", w->snow_cover, 0.0f, 1.0f);
    if (m_state.weather != nullptr) {
        const world::Ground g = m_state.weather->ground();
        const world::Falling f = world::falling(m_state.weather->shown() ? *m_state.weather->shown() : *w);
        ImGui::TextDisabled("Now: ground %.0f%% wet, %.0f%% snow; falling: rain %.2f, snow %.2f", g.wetness * 100.0f,
                            g.snow_cover * 100.0f, f.rain, f.snow);
    }
    if (ImGui::Button("Clear weather")) {
        run("clear_weather");
    }
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
        const auto undo = m_commands.undo_names();
        const auto redo = m_commands.redo_names();
        if (undo.empty() && redo.empty()) {
            ImGui::TextDisabled("Nothing to undo yet.");
        }
        for (const auto& name : undo) {
            ImGui::BulletText("%s", name.c_str());
        }
        for (auto it = redo.rbegin(); it != redo.rend(); ++it) {
            ImGui::TextDisabled("  (undone) %s", it->c_str());
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
        // GPU timestamps of the newest finished frame (a frame or two back).
        ImGui::Text("GPU time: %.2f ms", static_cast<double>(m_state.device->gpu_frame_ms()));
        if (ImGui::BeginTable("gpu passes", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) {
            for (const rhi::GpuTiming& pass : editor::gpu_passes(*m_state.device)) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(pass.name.c_str());
                ImGui::TableNextColumn();
                ImGui::Text("%.3f ms", static_cast<double>(pass.ms));
            }
            ImGui::EndTable();
        }
        const render::RenderStats& stats = m_state.scene_stats;
        ImGui::Text("Scene: %u instances in %u draws (+%u full-screen)", stats.instances, stats.draws,
                    stats.post_draws);
        ImGui::Text("Clusters: %u, triangles: %u", stats.clusters, stats.triangles);
        if (stats.from_gpu) {
            ImGui::Text("Occluded: %u instances, %u clusters", stats.occluded_instances,
                        stats.occluded_clusters);
            if (stats.overflow) {
                ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.3f, 1.0f),
                                   "GPU lists full: some geometry dropped");
            }
        }
        ImGui::Text("Geometry pool: %.0f / %.0f MiB, %u of %u groups",
                    static_cast<double>(stats.pool_used_bytes) / (1024.0 * 1024.0),
                    static_cast<double>(stats.pool_bytes) / (1024.0 * 1024.0), stats.resident_groups,
                    stats.total_groups);
        if (stats.waiting > 0 || stats.streamed_in > 0) {
            ImGui::Text("Streaming: %u loaded, %u waiting", stats.streamed_in, stats.waiting);
        }
        ImGui::Text("Voxel world: %u of %u bricks, %u waiting (%.2f ms CPU)", stats.voxel_bricks,
                    stats.voxel_capacity, stats.voxel_pending, static_cast<double>(stats.voxel_cpu_ms));
        if (stats.voxel_overflow) {
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.3f, 1.0f), "Voxel brick pool full: %u bricks missing",
                               stats.voxel_missing);
        }
        if (stats.far_field) {
            ImGui::Text("Far field: %u instances left to the voxels", stats.far_culled);
        }
        if (stats.shadows) {
            ImGui::Text("Shadow pages: %u of %u, %u drawn, %u waiting", stats.shadow_pages, stats.shadow_capacity,
                        stats.shadow_drawn, stats.shadow_waiting);
            if (stats.shadow_missing > 0) {
                ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.3f, 1.0f), "Shadow page pool full: %u pages missing",
                                   stats.shadow_missing);
            }
        }
        if (stats.gi) {
            ImGui::Text("GI: %u bricks lit, %u waiting", stats.gi_lit, stats.gi_waiting);
        }
        if (stats.reflections || stats.screen_reflections) {
            ImGui::Text("Reflections: %s%s", stats.reflections ? "voxel cones" : "",
                        stats.screen_reflections ? (stats.reflections ? " + screen" : "screen") : "");
        }
        ImGui::Text("Materials: %u shared, decals: %u in view", stats.materials, stats.decals);
        if (stats.decal_overflow) {
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.3f, 1.0f), "Too many decals on screen: some not drawn");
        }
        ImGui::Text("UI draw calls: %u", frame.ui_draw_calls);
        ImGui::Text("Window: %u x %u pixels", m_state.window->pixel_width(),
                    m_state.window->pixel_height());
        ImGui::SeparatorText("Geometry");
        const render::RenderSettings& r = m_state.render_settings;
        int view = static_cast<int>(r.debug_view);
        const char* const views[] = {"lit", "clusters", "lod_levels", "triangles", "voxels", "lighting"};
        const char* const labels[] = {"Lit", "Clusters", "LOD levels", "Triangles", "Voxels", "Lighting only"};
        if (ImGui::Combo("View", &view, labels, IM_ARRAYSIZE(labels))) {
            run("set_render_settings", {{"debug_view", views[view]}});
        }
        float error = r.lod_error_pixels;
        if (ImGui::SliderFloat("LOD error (px)", &error, 0.1f, 16.0f, "%.2f", ImGuiSliderFlags_Logarithmic)) {
            run("set_render_settings", {{"lod_error_pixels", std::clamp(error, 0.1f, 64.0f)}});
        }
        bool gpu = r.gpu_culling;
        if (ImGui::Checkbox("GPU culling", &gpu)) {
            run("set_render_settings", {{"gpu_culling", gpu}});
        }
        ImGui::BeginDisabled(!r.gpu_culling);
        bool occlusion = r.occlusion_culling;
        if (ImGui::Checkbox("Occlusion culling", &occlusion)) {
            run("set_render_settings", {{"occlusion_culling", occlusion}});
        }
        ImGui::EndDisabled();
        ImGui::BeginDisabled(!adapter.mesh_shaders);
        bool mesh = r.mesh_shaders;
        if (ImGui::Checkbox("Mesh shaders", &mesh)) {
            run("set_render_settings", {{"mesh_shaders", mesh}});
        }
        ImGui::EndDisabled();
        bool aa = r.antialiasing;
        if (ImGui::Checkbox("Anti-aliasing (SMAA)", &aa)) {
            run("set_render_settings", {{"antialiasing", aa}});
        }
        bool specular = r.specular_antialiasing;
        if (ImGui::Checkbox("Specular anti-aliasing", &specular)) {
            run("set_render_settings", {{"specular_antialiasing", specular}});
        }
        bool voxels = r.voxel_world;
        if (ImGui::Checkbox("Voxel world", &voxels)) {
            run("set_render_settings", {{"voxel_world", voxels}});
        }
        bool far = r.voxel_far_field;
        if (ImGui::Checkbox("Voxel far field", &far)) {
            run("set_render_settings", {{"voxel_far_field", far}});
        }
        float far_distance = r.far_field_distance;
        if (ImGui::DragFloat("Far field from (m)", &far_distance, 10.0f, 0.0f, 100000.0f, "%.0f")) {
            run("set_render_settings", {{"far_field_distance", far_distance}});
        }
        ImGui::BeginDisabled(!r.gpu_culling);
        bool shadows = r.shadows;
        if (ImGui::Checkbox("Sun shadows", &shadows)) {
            run("set_render_settings", {{"shadows", shadows}});
        }
        int pages = static_cast<int>(r.shadow_pages_per_frame);
        if (ImGui::SliderInt("Shadow pages a frame", &pages, 1, 512, "%d", ImGuiSliderFlags_Logarithmic)) {
            run("set_render_settings", {{"shadow_pages_per_frame", std::clamp(pages, 1, 4096)}});
        }
        ImGui::EndDisabled();
        ImGui::BeginDisabled(!r.voxel_world);
        bool gi = r.gi;
        if (ImGui::Checkbox("Global illumination", &gi)) {
            run("set_render_settings", {{"gi", gi}});
        }
        int gi_bricks = static_cast<int>(r.gi_bricks_per_frame);
        if (ImGui::SliderInt("GI bricks a frame", &gi_bricks, 1, 4096, "%d", ImGuiSliderFlags_Logarithmic)) {
            run("set_render_settings", {{"gi_bricks_per_frame", std::clamp(gi_bricks, 1, 4096)}});
        }
        ImGui::EndDisabled();
        bool reflections = r.reflections;
        if (ImGui::Checkbox("Reflections", &reflections)) {
            run("set_render_settings", {{"reflections", reflections}});
        }
        ImGui::BeginDisabled(!r.reflections);
        bool ssr = r.screen_space_reflections;
        if (ImGui::Checkbox("Screen-space reflections", &ssr)) {
            run("set_render_settings", {{"screen_space_reflections", ssr}});
        }
        ImGui::EndDisabled();
        bool ao = r.ambient_occlusion;
        if (ImGui::Checkbox("Ambient occlusion", &ao)) {
            run("set_render_settings", {{"ambient_occlusion", ao}});
        }
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
