// Baked poses (crowds plan, CR1): a skinned file held in a pose as a small
// plain mesh, baked on a worker and handed to the renderer.
#include "../src/editor_commands.h"
#include "../src/mesh_library.h"
#include "../src/pose_bakes.h"

#include "giant_test.h"
#include "gltf_test_files.h"

#include "giant/anim/player.h"
#include "giant/rhi/rhi.h"

#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace giant;
using namespace giant::editor;
using ai::Json;

namespace {

std::string write(const char* name, const std::vector<u8>& bytes) {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "giant_pose_bake_tests";
    std::filesystem::create_directories(dir);
    const std::filesystem::path path = dir / name;
    std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()),
                                                static_cast<std::streamsize>(bytes.size()));
    return path.generic_string();
}

// The two-joint arm (clip "bend": the elbow a quarter turn over 1 s, its top swinging to x = -1).
std::string arm_file() { return write("arm.glb", geometry::testing::skinned_arm()); }
// The same in two materials: drawn in two parts.
std::string parts_file() {
    geometry::testing::SkinOptions o;
    o.materials = true;
    o.morphs = true;
    return write("parts.glb", geometry::testing::skinned_arm(o));
}
// A person of boxes: 144 triangles, more than one cluster holds.
std::string person_file() { return write("person.glb", geometry::testing::skinned_person()); }
std::string plain_file() { return write("quad.glb", geometry::testing::two_material_quad("")); }

struct Fixture {
    std::unique_ptr<rhi::Device> device;
    std::unique_ptr<render::Renderer> renderer;
    std::unique_ptr<MeshLibrary> meshes;
    std::unique_ptr<PoseBakes> poses;
    ai::CommandRegistry commands;
    EditorState state;
    explicit Fixture(bool gpu = false) {
        if (gpu) {
            rhi::DeviceDesc desc;
            desc.backend = rhi::Backend::null;
            device = rhi::create_device(desc);
            renderer = render::Renderer::create(*device);
        }
        meshes = std::make_unique<MeshLibrary>(renderer.get());
        poses = std::make_unique<PoseBakes>(renderer.get(), meshes.get());
        state.meshes = meshes.get();
        state.poses = poses.get();
        register_pose_commands(commands, state);
    }
    ~Fixture() {
        poses.reset(); // its meshes leave the renderer first
        meshes.reset();
    }
    ai::CommandResult run(const char* name, const Json& args = Json::object()) { return commands.execute(name, args, {}); }
};

} // namespace

GIANT_TEST("pose bakes: a pose is baked on a worker, the same one asked for again is the same; what cannot be is refused") {
    Fixture f;
    const std::string arm = arm_file();
    GIANT_CHECK(f.meshes->load(arm).mesh != nullptr);
    const MeshLibrary::Mesh* mesh = f.meshes->find(arm);

    const auto rest = f.run("bake_pose", {{"mesh", arm}, {"wait", true}});
    GIANT_CHECK(rest.ok && rest.value["pose"] == 0 && rest.value["state"] == "ready" && rest.value["clip"] == "");
    const auto bent = f.run("bake_pose", {{"mesh", arm}, {"clip", "bend"}, {"time", 1.0}});
    GIANT_CHECK(bent.ok && bent.value["pose"] == 1 && bent.value["state"] == "baking");
    GIANT_CHECK(f.poses->baking() == 1);
    f.poses->finish();
    GIANT_CHECK(f.poses->baking() == 0 && f.poses->baked() == 2);
    // The file was cut down once: the second pose, begun after the first was done, used the figure kept.
    GIANT_CHECK(f.poses->cuts() == 1);
    const PoseBakes::Pose a = f.poses->poses()[0]; // (copies: more are asked for below)
    const PoseBakes::Pose b = f.poses->poses()[1];
    GIANT_CHECK(a.state == PoseBakes::State::ready && b.state == PoseBakes::State::ready);
    // A small file is not cut: every triangle of it, in one mesh (none to hand over without a renderer).
    GIANT_CHECK(a.baked_triangles == mesh->triangles && a.source_triangles == mesh->triangles && a.error == 0.0f);
    GIANT_CHECK(a.meshes.size() == 1 && a.meshes[0] == render::k_no_mesh);
    // Bent, the arm's top has swung over to x = -1 and come down.
    GIANT_CHECK(b.center.x < a.center.x - 0.5f && b.center.y < a.center.y - 0.1f && a.radius > 0.5f && b.radius > 0.5f);

    // Asked for again: the same pose; so is any time past the clip's end, and the rest pose at any time.
    GIANT_CHECK(f.run("bake_pose", {{"mesh", arm}, {"clip", "bend"}, {"time", 1.0}}).value["pose"] == 1);
    GIANT_CHECK(f.run("bake_pose", {{"mesh", arm}, {"clip", "bend"}, {"time", 30.0}}).value["pose"] == 1);
    GIANT_CHECK(f.run("bake_pose", {{"mesh", arm}, {"time", 5.0}}).value["pose"] == 0);
    // Another time, or another size: another pose.
    GIANT_CHECK(f.run("bake_pose", {{"mesh", arm}, {"clip", "bend"}, {"time", 0.5}}).value["pose"] == 2);
    GIANT_CHECK(f.run("bake_pose", {{"mesh", arm}, {"clip", "bend"}, {"time", 0.5}, {"triangles", 64}}).value["pose"] == 3);
    const auto listed = f.run("get_baked_poses", {{"wait", true}});
    GIANT_CHECK(listed.ok && listed.value["poses"].size() == 4 && listed.value["baking"] == 0 && listed.value["baked"] == 4);
    GIANT_CHECK(f.poses->cuts() == 2 && listed.value["poses"][3]["seams_kept"] == true); // (one more: the other size)
    // Times in the same millisecond are one pose.
    GIANT_CHECK(f.run("bake_pose", {{"mesh", arm}, {"clip", "bend"}, {"time", 0.5004}}).value["pose"] == 2);
    GIANT_CHECK(listed.value["poses"][2]["time"] == 0.5 && listed.value["poses"][2]["parts"] == 1 &&
                listed.value["poses"][2]["baked_triangles"] == mesh->triangles);
    // Half way through the bend the top is half way over.
    const float half = f.poses->poses()[2].center.x;
    GIANT_CHECK(half < a.center.x - 0.05f && half > b.center.x);

    // Refused: a file not loaded, one not skinned, a clip it has not, numbers out of range.
    const std::string plain = plain_file();
    GIANT_CHECK(f.meshes->load(plain).mesh != nullptr);
    const auto unloaded = f.run("bake_pose", {{"mesh", "nowhere.glb"}});
    GIANT_CHECK(!unloaded.ok && unloaded.error.find("not loaded") != std::string::npos);
    const auto unskinned = f.run("bake_pose", {{"mesh", plain}});
    GIANT_CHECK(!unskinned.ok && unskinned.error.find("not a skinned mesh") != std::string::npos);
    const auto no_clip = f.run("bake_pose", {{"mesh", arm}, {"clip", "dance"}});
    GIANT_CHECK(!no_clip.ok && no_clip.error.find("no clip") != std::string::npos);
    GIANT_CHECK(!f.run("bake_pose", {{"mesh", arm}, {"triangles", 63}}).ok && !f.run("bake_pose", {{"mesh", arm}, {"triangles", 20001}}).ok);
    GIANT_CHECK(!f.run("bake_pose", {{"mesh", arm}, {"time", -1.0}}).ok && !f.run("bake_pose").ok);
    GIANT_CHECK(!f.run("bake_pose", {{"mesh", arm}, {"clip", "bend"}, {"time", 3600.5}}).ok);
    std::string error;
    GIANT_CHECK(f.poses->want(arm, "", 0.0f, 10, error) == PoseBakes::k_no_pose && !error.empty());
    GIANT_CHECK(f.poses->want(arm, "", std::nanf(""), 3000, error) == PoseBakes::k_no_pose);
    GIANT_CHECK(f.poses->poses().size() == 4); // none of those was kept

    // As many as are kept, and no more; cleared, indices begin again.
    for (usize i = f.poses->poses().size(); i < PoseBakes::k_max_poses; ++i) {
        GIANT_CHECK(f.poses->want(arm, "bend", 0.001f * static_cast<float>(i + 1), 3000, error) == i);
    }
    GIANT_CHECK(f.poses->want(arm, "bend", 0.9f, 3000, error) == PoseBakes::k_no_pose && error.find("clear_baked_poses") != std::string::npos);
    const auto cleared = f.run("clear_baked_poses");
    GIANT_CHECK(cleared.ok && cleared.value["cleared"] == PoseBakes::k_max_poses && f.poses->poses().empty());
    GIANT_CHECK(f.run("bake_pose", {{"mesh", arm}, {"wait", true}}).value["pose"] == 0);
}

GIANT_TEST("pose bakes: a figure is cut down to the triangles asked for, from the file's original triangles alone") {
    Fixture f;
    const std::string person = person_file();
    GIANT_CHECK(f.meshes->load(person).mesh != nullptr);
    const MeshLibrary::Mesh* mesh = f.meshes->find(person);
    GIANT_CHECK(mesh != nullptr && mesh->skinned != nullptr && mesh->triangles == 144);
    if (mesh == nullptr || mesh->skinned == nullptr) return;
    // (Its clusters hold more than its original triangles: the coarser levels too.)
    GIANT_CHECK(mesh->skinned->clusters.mesh.indices.size() / 3 > mesh->triangles);
    std::string error;
    const u32 whole = f.poses->want(person, "", 0.0f, 3000, error);
    const u32 small = f.poses->want(person, "", 0.0f, 64, error);
    GIANT_CHECK(whole == 0 && small == 1);
    f.poses->finish();
    const PoseBakes::Pose& w = f.poses->poses()[0];
    const PoseBakes::Pose& s = f.poses->poses()[1];
    GIANT_CHECK(w.state == PoseBakes::State::ready && s.state == PoseBakes::State::ready);
    GIANT_CHECK(w.baked_triangles == 144 && w.error == 0.0f);
    GIANT_CHECK(s.baked_triangles > 0 && s.baked_triangles <= 80 && s.error > 0.0f && s.error < 0.2f);
    // Cut or not, it stands where it stood: a person 1.7 m tall, arms out.
    for (const PoseBakes::Pose* p : {&w, &s}) {
        GIANT_CHECK(std::abs(p->center.x) < 0.1f && p->center.y > 0.6f && p->center.y < 1.1f && p->radius > 0.8f &&
                    p->radius < 1.5f);
    }
}

GIANT_TEST("pose bakes: the meshes go to the renderer a few a frame, part by part; a file imported again is baked again") {
    Fixture f(true);
    if (f.renderer == nullptr) return;
    const std::string parts = parts_file();
    GIANT_CHECK(f.meshes->load(parts).mesh != nullptr);
    const MeshLibrary::Mesh* mesh = f.meshes->find(parts);
    GIANT_CHECK(mesh != nullptr && mesh->parts.size() == 2);
    if (mesh == nullptr || mesh->parts.size() != 2) return;
    std::string error;
    GIANT_CHECK(f.poses->want(parts, "bend", 1.0f, 3000, error) == 0);
    // One mesh a frame: baked, the first part is handed over, then the second; ready only with both.
    f.poses->uploads_a_frame = 1;
    for (int i = 0; i < 5000 && f.poses->poses()[0].meshes.empty(); ++i) {
        f.poses->update();
        if (f.poses->poses()[0].meshes.empty()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    GIANT_CHECK(f.poses->poses()[0].meshes.size() == 1 && f.poses->poses()[0].state == PoseBakes::State::baking);
    f.poses->update();
    const PoseBakes::Pose& pose = f.poses->poses()[0];
    GIANT_CHECK(pose.meshes.size() == 2 && pose.state == PoseBakes::State::ready && f.poses->uploads_a_frame == 1);
    const std::vector<render::MeshId> first = pose.meshes;
    for (const render::MeshId id : first) {
        GIANT_CHECK(id != render::k_no_mesh && f.renderer->mesh(id) != nullptr && !f.renderer->skinned(id));
    }
    // Each part's triangles are its own: the sleeve's, and the sword's one.
    GIANT_CHECK(f.renderer->mesh(first[0])->source_triangles == mesh->parts[0].triangles);
    GIANT_CHECK(f.renderer->mesh(first[1])->source_triangles == 1 && pose.baked_triangles == mesh->triangles);
    const auto listed = f.run("get_baked_poses");
    GIANT_CHECK(listed.ok && listed.value["poses"][0]["parts"] == 2 && listed.value["poses"][0]["state"] == "ready");

    // Imported again: baked again from the new file (cut down again), the old meshes gone meanwhile.
    const u64 baked = f.poses->baked(), cuts = f.poses->cuts();
    GIANT_CHECK(f.meshes->reload(parts).mesh != nullptr);
    // (Asked for before a frame has passed, it is already the new file's.)
    GIANT_CHECK(f.poses->want(parts, "bend", 1.0f, 3000, error) == 0);
    GIANT_CHECK(f.poses->poses()[0].state == PoseBakes::State::baking && f.poses->poses()[0].meshes.empty());
    for (const render::MeshId id : first) GIANT_CHECK(f.renderer->mesh(id) == nullptr);
    f.poses->finish();
    GIANT_CHECK(f.poses->poses()[0].state == PoseBakes::State::ready && f.poses->poses()[0].meshes.size() == 2 &&
                f.poses->uploads_a_frame == 1);
    GIANT_CHECK(f.poses->baked() == baked + 1 && f.poses->cuts() == cuts + 1);
    for (const render::MeshId id : f.poses->poses()[0].meshes) GIANT_CHECK(f.renderer->mesh(id) != nullptr);

    // Imported again without its clip: the pose fails and says why (asked for again, it fails again); with the clip back it is baked again.
    write("parts.glb", geometry::testing::skinned_person());
    GIANT_CHECK(f.meshes->reload(parts).mesh != nullptr);
    f.poses->update();
    const bool lost = f.meshes->find(parts)->skinned == nullptr || f.meshes->find(parts)->skinned->find_clip("bend") == anim::k_no_clip;
    GIANT_CHECK(lost);
    if (lost) {
        GIANT_CHECK(f.poses->poses()[0].state == PoseBakes::State::failed && !f.poses->poses()[0].problem.empty() &&
                    f.poses->poses()[0].meshes.empty());
        GIANT_CHECK(f.poses->want(parts, "bend", 1.0f, 3000, error) == PoseBakes::k_no_pose && f.poses->poses().size() == 1);
        parts_file();
        GIANT_CHECK(f.meshes->reload(parts).mesh != nullptr);
        // Two meshes a frame, but no triangles to spare after a frame's first: still one a frame.
        f.poses->uploads_a_frame = 2;
        f.poses->triangles_a_frame = 0;
        for (int i = 0; i < 5000 && f.poses->poses()[0].meshes.empty(); ++i) {
            f.poses->update();
            if (f.poses->poses()[0].meshes.empty()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        GIANT_CHECK(f.poses->poses()[0].meshes.size() == 1 && f.poses->poses()[0].state == PoseBakes::State::baking);
        f.poses->update();
        GIANT_CHECK(f.poses->poses()[0].state == PoseBakes::State::ready && f.poses->poses()[0].meshes.size() == 2);
    }

    // Cleared, or gone, with bakes still running: they end on their own, nothing of theirs is kept.
    GIANT_CHECK(f.poses->want(parts, "", 0.0f, 3000, error) != PoseBakes::k_no_pose);
    GIANT_CHECK(f.poses->want(parts, "", 0.0f, 64, error) != PoseBakes::k_no_pose);
    f.poses->clear();
    GIANT_CHECK(f.poses->poses().empty() && f.poses->baking() == 0);
    f.poses->finish();
    GIANT_CHECK(f.poses->poses().empty());
    GIANT_CHECK(f.poses->want(parts, "bend", 1.0f, 3000, error) == 0);
    f.poses->finish();

    // Cleared: out of the renderer.
    const std::vector<render::MeshId> second = f.poses->poses()[0].meshes;
    f.poses->clear();
    for (const render::MeshId id : second) GIANT_CHECK(f.renderer->mesh(id) == nullptr);
    GIANT_CHECK(f.poses->poses().empty() && f.poses->baking() == 0);
}
