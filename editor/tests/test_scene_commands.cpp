// The scene commands, run the way MCP and scripts run them.
#include "../src/editor_commands.h"
#include "../src/mesh_library.h"

#include "giant_test.h"

#include "giant/geometry/mesh.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>

using namespace giant;
using namespace giant::editor;
using ai::Json;

namespace {
struct Fixture {
    ai::CommandRegistry commands;
    MeshLibrary meshes{nullptr}; // imports and measures, no GPU
    EditorState state;
    Fixture() {
        state.meshes = &meshes;
        register_scene_commands(commands, state);
    }
    ai::CommandResult run(const char* name, const Json& args = Json::object(), bool record = true) {
        ai::ExecuteOptions options;
        options.record_history = record;
        return commands.execute(name, args, options);
    }
};
// Writes a mesh as a GLB (positions, normals, 32-bit indices).
void write_glb(const std::filesystem::path& path, const geometry::TriangleMesh& mesh) {
    std::vector<u8> bin(mesh.vertices.size() * sizeof(geometry::Vertex) + mesh.indices.size() * 4);
    std::memcpy(bin.data(), mesh.vertices.data(), mesh.vertices.size() * sizeof(geometry::Vertex));
    std::memcpy(bin.data() + mesh.vertices.size() * sizeof(geometry::Vertex), mesh.indices.data(),
                mesh.indices.size() * 4);
    const usize vbytes = mesh.vertices.size() * sizeof(geometry::Vertex);
    std::string json = R"({"asset":{"version":"2.0"},"scenes":[{"nodes":[0]}],"nodes":[{"mesh":0}],)"
                       R"("meshes":[{"primitives":[{"attributes":{"POSITION":0,"NORMAL":1},"indices":2}]}],)"
                       R"("buffers":[{"byteLength":)" + std::to_string(bin.size()) + "}]," +
                       R"("bufferViews":[{"buffer":0,"byteLength":)" + std::to_string(vbytes) +
                       R"(,"byteStride":)" + std::to_string(sizeof(geometry::Vertex)) + R"(},{"buffer":0,"byteOffset":)" + std::to_string(vbytes) +
                       R"(,"byteLength":)" + std::to_string(mesh.indices.size() * 4) + "}]," +
                       R"("accessors":[{"bufferView":0,"componentType":5126,"count":)" +
                       std::to_string(mesh.vertices.size()) +
                       R"(,"type":"VEC3","min":[-1,-1,-1],"max":[1,1,1]},{"bufferView":0,"byteOffset":12,"componentType":5126,"count":)" +
                       std::to_string(mesh.vertices.size()) + R"(,"type":"VEC3"},{"bufferView":1,"componentType":5125,"count":)" +
                       std::to_string(mesh.indices.size()) + R"(,"type":"SCALAR"}]})";
    while (json.size() % 4 != 0) json += ' ';
    while (bin.size() % 4 != 0) bin.push_back(0);
    std::vector<u8> out;
    const auto u32le = [&out](u32 v) {
        for (int i = 0; i < 4; ++i) out.push_back(static_cast<u8>(v >> (8 * i)));
    };
    u32le(0x46546C67);
    u32le(2);
    u32le(static_cast<u32>(12 + 8 + json.size() + 8 + bin.size()));
    u32le(static_cast<u32>(json.size()));
    u32le(0x4E4F534A);
    out.insert(out.end(), json.begin(), json.end());
    u32le(static_cast<u32>(bin.size()));
    u32le(0x004E4942);
    out.insert(out.end(), bin.begin(), bin.end());
    std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(out.data()),
                                                static_cast<std::streamsize>(out.size()));
}

std::string temp_scene(const char* name) {
    return (std::filesystem::temp_directory_path() / name).string();
}
} // namespace

GIANT_TEST("editor: undo skips changes made to a scene a script has since replaced") {
    Fixture f;
    const auto created = f.run("create_entity", {{"name", "a"}});
    GIANT_CHECK(created.ok);
    const u64 id = created.value["id"].get<u64>();
    const std::string path = temp_scene("giant_test_generation.scene.json");
    GIANT_CHECK(f.run("save_scene", {{"path", path}}).ok);
    GIANT_CHECK(f.run("set_entity", {{"id", id}, {"name", "b"}}).ok);

    // A script loads the saved file (same id, outside the history) and edits it.
    GIANT_CHECK(f.run("load_scene", {{"path", path}}, false).ok);
    GIANT_CHECK(f.run("set_entity", {{"id", id}, {"name", "c"}}, false).ok);

    // The recorded steps belong to the old scene: they must not touch this one.
    GIANT_CHECK(f.commands.undo()); // set_entity "b"
    GIANT_CHECK(f.state.scene.find(id) != nullptr && f.state.scene.find(id)->name == "c");
    GIANT_CHECK(f.commands.undo()); // save_scene records nothing; this is create_entity
    GIANT_CHECK(f.state.scene.find(id) != nullptr);
    std::filesystem::remove(path);
}

GIANT_TEST("editor: undo and redo across a recorded scene swap still apply") {
    Fixture f;
    const u64 id = f.run("create_entity", {{"name", "a"}}).value["id"].get<u64>();
    GIANT_CHECK(f.run("set_entity", {{"id", id}, {"name", "b"}}).ok);
    GIANT_CHECK(f.run("new_scene").ok);
    GIANT_CHECK(f.state.scene.entities().empty());
    GIANT_CHECK(f.commands.undo()); // new_scene: the old scene and its generation return
    GIANT_CHECK(f.commands.undo()); // set_entity
    GIANT_CHECK(f.state.scene.find(id)->name == "a");
    GIANT_CHECK(f.commands.redo());
    GIANT_CHECK(f.state.scene.find(id)->name == "b");
    GIANT_CHECK(f.commands.redo());
    GIANT_CHECK(f.state.scene.entities().empty());
    GIANT_CHECK(f.commands.undo());
    GIANT_CHECK(f.state.scene.find(id)->name == "b");
}

GIANT_TEST("editor: creating entities fails cleanly when every id is used") {
    Fixture f;
    f.state.scene.reserve_ids_below(world::k_max_entity_id); // one id left
    const auto batch = f.run("create_entities", {{"entities", Json::array({Json::object(), Json::object()})}});
    GIANT_CHECK(!batch.ok);
    GIANT_CHECK(batch.error.find("every entity id has been used") != std::string::npos);
    GIANT_CHECK(f.state.scene.entities().empty()); // all or nothing
    GIANT_CHECK(!f.commands.can_undo());

    // Ids are never reused, so the failed batch spent the last one.
    GIANT_CHECK(!f.run("create_entity").ok);
}

GIANT_TEST("editor: the last entity id can be used") {
    Fixture f;
    f.state.scene.reserve_ids_below(world::k_max_entity_id);
    const auto last = f.run("create_entity");
    GIANT_CHECK(last.ok && last.value["id"].get<u64>() == world::k_max_entity_id);
    GIANT_CHECK(!f.run("create_entity").ok);
    GIANT_CHECK(f.state.scene.entities().size() == 1);
}

GIANT_TEST("editor: imported meshes are drawn by entities that name them") {
    Fixture f;
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "giant_mesh_commands";
    std::filesystem::create_directories(dir);
    const std::string glb = (dir / "ball.glb").generic_string();
    write_glb(glb, geometry::make_sphere(64, 32));

    const auto imported = f.run("import_mesh", {{"path", glb}});
    GIANT_CHECK(imported.ok);
    if (!imported.ok) {
        std::fprintf(stderr, "    %s\n", imported.error.c_str());
        return;
    }
    GIANT_CHECK(imported.value["triangles"].get<u32>() == 64 * 32 * 2 - 2 * 64); // the pole rows are single triangles
    GIANT_CHECK(imported.value["clusters"].get<u32>() > 16 && imported.value["levels"].get<u32>() > 1);
    GIANT_CHECK(!f.commands.can_undo()); // an asset load, not a scene change
    GIANT_CHECK(f.run("list_meshes").value["meshes"].size() == 1);

    // Giving mesh implies the shape; the name comes from the file.
    const auto created = f.run("create_entity", {{"mesh", glb}});
    GIANT_CHECK(created.ok && created.value["shape"] == "mesh" && created.value["mesh"] == glb);
    GIANT_CHECK(created.value["name"].get<std::string>().rfind("ball", 0) == 0);
    const u64 id = created.value["id"].get<u64>();

    // A missing file is refused and changes nothing.
    const auto bad = f.run("set_entity", {{"id", id}, {"mesh", (dir / "missing.glb").generic_string()}});
    GIANT_CHECK(!bad.ok);
    GIANT_CHECK(f.state.scene.find(id)->mesh == glb);
    GIANT_CHECK(!f.run("create_entity", {{"shape", "mesh"}}).ok); // no file named
    const auto batch = f.run("create_entities",
                             {{"entities", Json::array({{{"shape", "cube"}}, {{"mesh", "nope.glb"}}})}});
    GIANT_CHECK(!batch.ok && batch.error.find("entities[1]") != std::string::npos);
    GIANT_CHECK(f.state.scene.size() == 1); // all or nothing

    // Saved by path; loading imports what is missing and reports failures.
    const std::string scene_path = (dir / "meshes.scene.json").generic_string();
    GIANT_CHECK(f.run("create_entity", {{"shape", "cube"}}).ok);
    GIANT_CHECK(f.run("set_entity", {{"id", id}, {"name", "kept"}}).ok);
    world::Entity broken;
    broken.shape = world::Shape::mesh;
    broken.mesh = "gone/away.glb";
    f.state.scene.add(broken);
    GIANT_CHECK(f.run("save_scene", {{"path", scene_path}}).ok);
    Fixture g;
    const auto loaded = g.run("load_scene", {{"path", scene_path}});
    GIANT_CHECK(loaded.ok && loaded.value["entities"] == 3);
    GIANT_CHECK(loaded.value.contains("mesh_errors") && loaded.value["mesh_errors"].size() == 1);
    GIANT_CHECK(g.meshes.find(glb) != nullptr);
    // The entity whose file is missing can still be moved, renamed and hidden.
    u64 broken_id = 0;
    for (const world::Entity& e : g.state.scene.entities()) {
        if (e.mesh == "gone/away.glb") broken_id = e.id;
    }
    GIANT_CHECK(broken_id != 0);
    GIANT_CHECK(g.run("set_entity", {{"id", broken_id}, {"position", {1, 2, 3}}, {"visible", false}}).ok);
    GIANT_CHECK(!g.run("set_entity", {{"id", broken_id}, {"shape", "mesh"}}).ok); // the file is still needed
    std::filesystem::remove_all(dir);
}

GIANT_TEST("editor: render settings are view state") {
    Fixture f;
    const auto set = f.run("set_render_settings", {{"lod_error_pixels", 4.0}, {"debug_view", "lod_levels"}});
    GIANT_CHECK(set.ok && set.value["debug_view"] == "lod_levels");
    GIANT_CHECK(f.state.render_settings.debug_view == render::DebugView::lod_levels);
    GIANT_CHECK(f.state.render_settings.lod_error_pixels == 4.0f);
    GIANT_CHECK(!f.commands.can_undo());
    GIANT_CHECK(!f.run("set_render_settings", {{"lod_error_pixels", 0.0}}).ok); // below the minimum
    GIANT_CHECK(!f.run("set_render_settings", {{"debug_view", "wireframe"}}).ok);
    GIANT_CHECK(f.run("get_render_settings").value["lod_error_pixels"] == 4.0);
    const auto culling = f.run("set_render_settings", {{"gpu_culling", false},
                                                       {"occlusion_culling", false},
                                                       {"mesh_shaders", false}});
    GIANT_CHECK(culling.ok && culling.value["gpu_culling"] == false);
    GIANT_CHECK(!f.state.render_settings.gpu_culling && !f.state.render_settings.occlusion_culling &&
                !f.state.render_settings.mesh_shaders);
    GIANT_CHECK(!f.run("set_render_settings", {{"gpu_culling", "yes"}}).ok);
    // The physical sky: on by default, off and on again, its distance in range.
    GIANT_CHECK(f.state.render_settings.physical_sky && f.run("get_render_settings").value["physical_sky"] == true);
    const auto sky = f.run("set_render_settings", {{"physical_sky", false}, {"aerial_distance", 20000}});
    GIANT_CHECK(sky.ok && sky.value["physical_sky"] == false && sky.value["aerial_distance"] == 20000.0);
    GIANT_CHECK(!f.state.render_settings.physical_sky && f.state.render_settings.aerial_distance == 20000.0f);
    GIANT_CHECK(!f.run("set_render_settings", {{"aerial_distance", 500}}).ok);
    GIANT_CHECK(f.run("set_render_settings", {{"physical_sky", true}}).ok && f.state.render_settings.physical_sky);
    // Exposure, bloom and the tone mapper.
    const auto shown = f.run("get_render_settings");
    GIANT_CHECK(shown.value["auto_exposure"] == true && shown.value["bloom"] == true &&
                shown.value["tone_mapper"] == "pbr_neutral");
    GIANT_CHECK(shown.value["last_frame"].contains("exposure"));
    const auto eye = f.run("set_render_settings", {{"auto_exposure", false},
                                                   {"exposure_compensation", 1.5},
                                                   {"exposure_min", -4},
                                                   {"exposure_max", 6},
                                                   {"exposure_speed_up", 5},
                                                   {"exposure_speed_down", 2},
                                                   {"bloom", false},
                                                   {"bloom_strength", 0.1},
                                                   {"tone_mapper", "agx"}});
    GIANT_CHECK(eye.ok && eye.value["tone_mapper"] == "agx" && eye.value["auto_exposure"] == false);
    const render::RenderSettings& rs = f.state.render_settings;
    GIANT_CHECK(!rs.auto_exposure && rs.exposure_compensation == 1.5f && rs.exposure_min == -4.0f &&
                rs.exposure_max == 6.0f && rs.exposure_speed_up == 5.0f && rs.exposure_speed_down == 2.0f &&
                !rs.bloom && std::abs(rs.bloom_strength - 0.1f) < 1e-6f && rs.tone_mapper == render::ToneMapper::agx);
    GIANT_CHECK(!f.run("set_render_settings", {{"tone_mapper", "aces"}}).ok);
    GIANT_CHECK(!f.run("set_render_settings", {{"bloom_strength", 2}}).ok);
    GIANT_CHECK(!f.run("set_render_settings", {{"exposure_compensation", 11}}).ok);
    GIANT_CHECK(f.run("set_render_settings", {{"tone_mapper", "pbr_neutral"}, {"auto_exposure", true}, {"bloom", true}}).ok);
    GIANT_CHECK(rs.tone_mapper == render::ToneMapper::pbr_neutral && rs.auto_exposure && rs.bloom);
    GIANT_CHECK(f.state.render_settings.antialiasing && f.state.render_settings.specular_antialiasing);
    const auto aa = f.run("set_render_settings", {{"antialiasing", false}, {"specular_antialiasing", false}});
    GIANT_CHECK(aa.ok && aa.value["antialiasing"] == false && aa.value["specular_antialiasing"] == false);
    GIANT_CHECK(!f.state.render_settings.antialiasing && !f.state.render_settings.specular_antialiasing);
    GIANT_CHECK(f.run("get_render_settings").value["last_frame"].contains("post_draws"));
    GIANT_CHECK(f.run("get_render_settings").value["last_frame"].contains("overflow"));
    GIANT_CHECK(f.run("get_render_settings").value["last_frame"]["streaming"].contains("pool_bytes"));
}

GIANT_TEST("editor: frame phases name the slow frames' parts") {
    FrameTimes t;
    const auto at = [](FramePhase p) { return static_cast<usize>(p); };
    // 300 frames (the first 60 roll out): steady 10 ms of render, one of
    // 40 ms where the scatter took 30, the renderer's voxels a part of render.
    for (usize i = 0; i < 300; ++i) {
        FramePhaseMs phases{};
        phases[at(FramePhase::render)] = 10.0f;
        phases[at(FramePhase::count) + static_cast<usize>(render::CpuSection::voxels)] = 4.0f;
        phases[at(FramePhase::mcp)] = 0.01f; // under 0.05 ms: left out
        float ms = 10.01f;
        if (i == 290) {
            phases[at(FramePhase::scatter)] = 30.0f;
            ms = 40.01f;
        }
        // The device: one resource made a frame; frames 288 and 289 made
        // memory, and 290 waited on the GPU.
        FrameDevice device{1, i == 288 || i == 289 ? 5u : 0u, 1, i == 290 ? 28.0f : 0.5f};
        t.add(ms, phases, i == 290 ? 39.0f : 9.0f, device);
    }
    usize slot = 0;
    GIANT_CHECK(t.slot_of(299, slot) && slot == 299 % FrameTimes::k_frames);
    GIANT_CHECK(t.slot_of(60, slot) && !t.slot_of(59, slot) && !t.slot_of(300, slot));
    // All kept frames: from 60, the oldest kept, whatever was asked.
    const ai::Json all = frame_phases(t, 0, 25.0f);
    GIANT_CHECK(all["since"] == 60 && all["frames"] == 240);
    GIANT_CHECK(all["typical"]["render"] == 10.0 && all["typical"]["render/voxels"] == 4.0);
    GIANT_CHECK(all["typical"]["scatter"] == 0.0 && all["typical_cpu_ms"] == 9.0);
    GIANT_CHECK(all["slow"].size() == 1);
    const ai::Json& slow = all["slow"][0];
    GIANT_CHECK(slow["frame"] == 290 && slow["ms"] == 40.01 && slow["cpu_ms"] == 39.0);
    GIANT_CHECK(slow["phases"]["scatter"] == 30.0 && slow["phases"]["render/voxels"] == 4.0);
    GIANT_CHECK(!slow["phases"].contains("mcp") && !slow["phases"].contains("ui"));
    GIANT_CHECK(slow["device"]["wait_ms"] == 28.0 && slow["device"]["allocations"] == 0);
    GIANT_CHECK(slow["device_before"]["allocations"] == 10 && slow["device_before"]["created"] == 2);
    GIANT_CHECK(all["typical_device"]["created"] == 1 && all["typical_device"]["allocations"] == 0 &&
                all["typical_device"]["wait_ms"] == 0.5);
    // Since frame 295: five frames, none slow; past the newest: none.
    const ai::Json recent = frame_phases(t, 295, 25.0f);
    GIANT_CHECK(recent["since"] == 295 && recent["frames"] == 5 && recent["slow"].empty());
    const ai::Json none = frame_phases(t, 1000, 25.0f);
    GIANT_CHECK(none["frames"] == 0 && none["slow"].empty() && none["typical"]["render"] == 0.0);
    // Every frame in full when asked for all over 0 ms.
    GIANT_CHECK(frame_phases(t, 290, 0.0f)["slow"].size() == 10);
    GIANT_CHECK(frame_phase_name(at(FramePhase::count)) == "render/cells");
    GIANT_CHECK(frame_phase_name(at(FramePhase::tail)) == "tail");
}

GIANT_TEST("editor: mesh paths have one spelling") {
    GIANT_CHECK(MeshLibrary::normalize("assets/./rocks/../rock.glb") == "assets/rock.glb");
    GIANT_CHECK(MeshLibrary::normalize("assets\\rock.glb") == "assets/rock.glb");
}

GIANT_TEST("editor: frame times report consistency, not just the average") {
    FrameTimes t;
    GIANT_CHECK(t.average() == 0.0f && t.percentile(0.99f) == 0.0f && t.hitches() == 0);
    for (int i = 0; i < 300; ++i) {
        t.add(i == 250 ? 50.0f : 16.0f); // one stutter among steady frames
    }
    GIANT_CHECK(t.size() == FrameTimes::k_frames);
    GIANT_CHECK(t.percentile(0.5f) == 16.0f && t.worst() == 50.0f);
    GIANT_CHECK(t.hitches() == 1);
    GIANT_CHECK(t.percentile(0.99f) == 16.0f); // one frame in 240 is under 1%
    t.add(40.0f);
    t.add(40.0f);
    t.add(40.0f);
    GIANT_CHECK(t.percentile(0.99f) == 40.0f && t.hitches() == 4);
    const std::vector<float> order = t.in_order();
    GIANT_CHECK(order.size() == FrameTimes::k_frames && order.back() == 40.0f);
    GIANT_CHECK(order[order.size() - 4] == 16.0f); // the oldest of the last four is steady
    FrameTimes few;
    few.add(16.7f);
    few.add(33.4f); // one missed refresh at 60 Hz
    few.add(16.7f);
    GIANT_CHECK(few.hitches() == 1 && few.in_order().front() == 16.7f);
    FrameTimes fast; // 144 Hz: 7 ms frames, a 7.5 ms one is noise, not a hitch
    for (int i = 0; i < 10; ++i) fast.add(i == 5 ? 7.5f : 7.0f);
    GIANT_CHECK(fast.hitches() == 0);
}
