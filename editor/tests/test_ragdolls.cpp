// Ragdolls: a skinned character gone limp with ragdoll_entity while
// simulating falls as bodies joined at its joints, which pose it from then
// on; elbows and knees fold one way; a walker goes limp where it walks;
// letting go gives it back to its clips.
#include "../src/animation_state.h"
#include "../src/editor_commands.h"
#include "../src/hit_targets.h"
#include "../src/mesh_library.h"
#include "../src/nav_state.h"
#include "../src/physics_state.h"
#include "../src/pose_math.h"
#include "../src/ragdoll_state.h"
#include "../src/scene_render.h"
#include "../src/walker_state.h"

#include "gltf_test_files.h"
#include "giant_test.h"

#include "giant/anim/skeleton.h"
#include "giant/sim/hit_shapes.h"
#include "giant/world/scene.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace giant;
using namespace giant::editor;
using ai::Json;
using geometry::testing::k_person;
using geometry::testing::k_person_joints;

namespace {

std::string person_file() {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "giant_ragdoll_tests";
    std::filesystem::create_directories(dir);
    const std::filesystem::path path = dir / "person.glb";
    const std::vector<u8> bytes = geometry::testing::skinned_person();
    std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()),
                                                static_cast<std::streamsize>(bytes.size()));
    return path.generic_string();
}

struct Fixture {
    HitTargets hits; // before the physics, which reads its shapes
    PhysicsState physics;
    NavState nav{nullptr, nullptr, nullptr};
    WalkerState walkers{physics, nav};
    MeshLibrary meshes{nullptr};
    AnimationState animation{nullptr};
    RagdollState ragdolls{physics, animation, &walkers};
    ai::CommandRegistry commands;
    EditorState state;
    std::string file = person_file();
    Fixture() {
        state.physics = &physics;
        state.nav = &nav;
        state.walkers = &walkers;
        state.meshes = &meshes;
        state.animation = &animation;
        state.hits = &hits;
        state.ragdolls = &ragdolls;
        physics.set_hit_targets(&hits);
        register_scene_commands(commands, state);
        register_physics_commands(commands, state);
        register_animation_commands(commands, state);
        register_nav_commands(commands, state);
        register_walker_commands(commands, state);
        register_ragdoll_commands(commands, state);
        connect_walkers(state);
        connect_ragdolls(state);
    }
    ai::CommandResult run(const char* name, const Json& args = Json::object()) {
        ai::ExecuteOptions options;
        options.record_history = true;
        return commands.execute(name, args, options);
    }
    world::EntityId create(const Json& args) {
        const ai::CommandResult r = run("create_entity", args);
        GIANT_CHECK(r.ok);
        return r.ok ? r.value["id"].get<world::EntityId>() : 0;
    }
    void floor() { create({{"shape", "plane"}, {"position", {0.0, 0.0, 0.0}}, {"scale", {60.0, 1.0, 60.0}}}); }
    // A person standing with its feet at (x, y, z), playing "idle".
    world::EntityId person(double x, double y, double z, const char* body = "fixed") {
        const world::EntityId id = create({{"shape", "mesh"}, {"mesh", file}, {"position", {x, y, z}}, {"body", body}});
        GIANT_CHECK(run("set_animation", {{"id", id}, {"clip", "idle"}}).ok);
        return id;
    }
    // A frame as main runs one.
    void frame(double seconds = 1.0 / 60.0) {
        const world::SceneChanges changed = state.scene.take_changes();
        walkers.before_step(state.scene, seconds);
        ragdolls.before_step(state.scene, meshes);
        physics.update(state.scene, changed, seconds);
        walkers.after_step();
        ragdolls.after_step();
        animation.update(state.scene, meshes, changed, seconds);
        hits.update(state.scene, meshes, animation);
    }
    void frames(double seconds) {
        for (int i = 0; i < static_cast<int>(seconds * 60.0 + 0.5); ++i) frame();
    }
    // Where a joint is in the world, as the character is drawn.
    DVec3 joint(world::EntityId id, usize j) const {
        const AnimationState::Animated* a = animation.find(id);
        const world::Entity* e = state.scene.find(id);
        GIANT_CHECK(a != nullptr && e != nullptr && j < a->model.size());
        if (a == nullptr || e == nullptr || j >= a->model.size()) return {};
        world::Transform at = e->transform;
        if (const auto p = walkers.poses().find(id); p != walkers.poses().end()) at = p->second;
        if (const auto p = ragdolls.poses().find(id); p != ragdolls.poses().end()) at = p->second;
        const Mat4& m = a->model[j];
        return pose::place(at, {m.at(0, 3), m.at(1, 3), m.at(2, 3)});
    }
};

// The person's skeleton as imported (its joints in the importer's order, not the file's).
const anim::Skeleton& bones(const Fixture& f) { return f.meshes.find_normalized(f.file)->skinned->skeleton; }

usize joint_named(const Fixture& f, const char* name) {
    const std::vector<std::string>& names = bones(f).names;
    const auto it = std::find(names.begin(), names.end(), name);
    GIANT_CHECK(it != names.end());
    return it != names.end() ? static_cast<usize>(it - names.begin()) : 0;
}

// How far a joint is from the one above it, at rest.
float rest_gap(const Fixture& f, usize j) {
    const std::vector<Mat4>& rest = f.meshes.find_normalized(f.file)->skinned->rest_model;
    const usize p = static_cast<usize>(bones(f).parents[j]);
    return length(Vec3{rest[j].at(0, 3) - rest[p].at(0, 3), rest[j].at(1, 3) - rest[p].at(1, 3),
                       rest[j].at(2, 3) - rest[p].at(2, 3)});
}

// The file's own record of a bone (where its box ends).
const geometry::testing::PersonJoint& person_joint(const char* name) {
    for (const geometry::testing::PersonJoint& p : k_person) {
        if (std::string(p.name) == name) return p;
    }
    GIANT_CHECK(false);
    return k_person[0];
}

// A joint's turn in the entity's space, on a direction (its model matrix's, which neither stretches nor shears here).
Vec3 turned(const Mat4& m, const Vec3& v) {
    return {m.at(0, 0) * v.x + m.at(0, 1) * v.y + m.at(0, 2) * v.z, m.at(1, 0) * v.x + m.at(1, 1) * v.y + m.at(1, 2) * v.z,
            m.at(2, 0) * v.x + m.at(2, 1) * v.y + m.at(2, 2) * v.z};
}

} // namespace

GIANT_TEST("ragdolls: bones are told apart by the usual names") {
    using K = RagdollState::Kind;
    const std::pair<const char*, K> names[] = {
        {"mixamorig:Hips", K::other},       {"mixamorig:Spine2", K::spine},      {"mixamorig:Neck", K::neck},
        {"mixamorig:Head", K::head},        {"mixamorig:LeftShoulder", K::collar}, {"mixamorig:LeftArm", K::upper_arm},
        {"mixamorig:LeftForeArm", K::forearm}, {"mixamorig:LeftHand", K::hand},  {"mixamorig:RightUpLeg", K::thigh},
        {"mixamorig:RightLeg", K::shin},    {"mixamorig:RightFoot", K::foot},    {"mixamorig:RightToeBase", K::foot},
        {"pelvis", K::other},               {"spine_03", K::spine},              {"clavicle_l", K::collar},
        {"upperarm_l", K::upper_arm},       {"lowerarm_r", K::forearm},          {"hand_r", K::hand},
        {"thigh_l", K::thigh},              {"calf_l", K::shin},                 {"foot_l", K::foot},
        {"upper_arm.L", K::upper_arm},      {"forearm.L", K::forearm},           {"shin.R", K::shin},
        {"Armature", K::other},             {"Chest", K::spine},                 {"tail_01", K::other},
    };
    for (const auto& [name, kind] : names) {
        if (RagdollState::kind_of(name) != kind) GIANT_CHECK(std::string(name).empty());
    }
    // Elbows and knees fold; the rest lean in cones.
    GIANT_CHECK(RagdollState::limits_of(K::forearm).fold > 90.0f && RagdollState::limits_of(K::shin).fold > 90.0f);
    for (const K k : {K::other, K::spine, K::neck, K::head, K::collar, K::upper_arm, K::hand, K::thigh, K::foot}) {
        GIANT_CHECK(RagdollState::limits_of(k).fold == 0.0f && RagdollState::limits_of(k).swing > 0.0f);
    }
}

GIANT_TEST("ragdolls: a character goes limp as it stands, falls joined at its joints and comes to rest") {
    Fixture f;
    f.floor();
    const world::EntityId id = f.person(2.0, 0.3, 3.0);
    const world::EntityId cube = f.create({{"shape", "cube"}, {"position", {20.0, 0.5, 20.0}}});
    f.frame();
    // Not simulating: refused. Not a character, no such entity: refused.
    GIANT_CHECK(!f.run("ragdoll_entity", {{"id", id}}).ok);
    GIANT_CHECK(f.run("simulate", {{"on", true}}).ok);
    f.frames(0.2);
    GIANT_CHECK(!f.run("ragdoll_entity", {{"id", cube}}).ok && !f.run("ragdoll_entity", {{"id", 987654}}).ok);
    GIANT_CHECK(f.ragdolls.size() == 0 && f.animation.find(cube) == nullptr);
    GIANT_CHECK(!f.run("get_ragdoll", {{"id", id}}).ok && !f.run("stop_ragdoll", {{"id", id}}).ok);

    std::vector<DVec3> stood;
    for (usize j = 0; j < k_person_joints; ++j) stood.push_back(f.joint(id, j));
    GIANT_CHECK(std::fabs(stood[joint_named(f, "Head")].y - 1.8) < 1e-3);
    const usize bodies = f.physics.world().size();
    // (With a shove: let go standing straight and square on a level floor, it would balance on its shins.)
    const ai::CommandResult made = f.run("ragdoll_entity", {{"id", id}, {"velocity", {0.4, 0.0, 0.3}}});
    GIANT_CHECK(made.ok);
    if (!made.ok) return;
    // A body for each bone but the finger, which rides the forearm.
    GIANT_CHECK(made.value["bones"].get<usize>() == k_person_joints - 1 && made.value["joints"].get<usize>() == k_person_joints);
    GIANT_CHECK(f.physics.world().size() == bodies + k_person_joints - 1 && f.physics.world().ragdolls() == 1);
    GIANT_CHECK(made.value["mass"].get<float>() > 20.0f && made.value["mass"].get<float>() < 200.0f);
    const Json& list = made.value["bone_list"];
    GIANT_CHECK(list.size() == k_person_joints - 1 && list[0]["joint"] == "Hips" && list[0]["parent"].is_null());
    for (const Json& bone : list) {
        const std::string name = bone["joint"].get<std::string>();
        GIANT_CHECK(name != "LeftHandIndex1");
        const bool folds = name == "LeftForeArm" || name == "RightForeArm" || name == "LeftLeg" || name == "RightLeg";
        GIANT_CHECK(bone["hinge"].get<bool>() == folds);
        if (name == "LeftLeg") GIANT_CHECK(bone["kind"] == "shin" && bone["parent"] == "LeftUpLeg");
        if (name == "Head") GIANT_CHECK(bone["kind"] == "head" && bone["parent"] == "Spine");
        GIANT_CHECK(bone["mass"].get<float>() >= 0.02f * 0.99f * made.value["mass"].get<float>() / 11.0f);
    }
    GIANT_CHECK(!f.run("ragdoll_entity", {{"id", id}}).ok); // one already
    GIANT_CHECK(f.run("get_animation", {{"id", id}}).value["held"].get<bool>());
    // Nothing jumps as it goes limp: every joint where it stood.
    for (usize j = 0; j < k_person_joints; ++j) GIANT_CHECK(length(f.joint(id, j) - stood[j]) < 1e-4);
    GIANT_CHECK(f.ragdolls.take_redraw() == std::vector<world::EntityId>{id});

    // It falls: joints stay the lengths apart they were, the finger with its forearm.
    f.frames(0.25);
    GIANT_CHECK(f.joint(id, joint_named(f, "Hips")).y < stood[joint_named(f, "Hips")].y - 0.02); // dropping
    GIANT_CHECK(!f.ragdolls.take_moved().empty());
    double lowest = 1e9;
    for (int k = 0; k < 16; ++k) {
        f.frames(0.25);
        for (usize j = 0; j < k_person_joints; ++j) {
            const DVec3 at = f.joint(id, j);
            GIANT_CHECK(std::isfinite(at.x) && std::isfinite(at.y) && std::isfinite(at.z));
            lowest = std::min(lowest, at.y);
            if (bones(f).parents[j] >= 0) {
                const usize p = static_cast<usize>(bones(f).parents[j]);
                GIANT_CHECK(std::fabs(length(at - f.joint(id, p)) - static_cast<double>(rest_gap(f, j))) < 0.04);
            }
        }
    }
    GIANT_CHECK(lowest > -0.05); // on the floor, not through it
    // Down: its head near the ground, lying about where it stood; the scene keeps where it was put.
    const DVec3 head = f.joint(id, joint_named(f, "Head"));
    GIANT_CHECK(head.y < 0.5 && std::hypot(head.x - 2.0, head.z - 3.0) < 2.5);
    GIANT_CHECK(f.state.scene.find(id)->transform.position == (DVec3{2.0, 0.3, 3.0}));
    const auto drawn = f.ragdolls.poses().find(id);
    GIANT_CHECK(drawn != f.ragdolls.poses().end() && drawn->second.position.y < 0.3 - 0.4);
    // At rest: asleep, and no more poses handed on.
    for (int k = 0; k < 40 && !f.run("get_ragdoll", {{"id", id}}).value["ragdoll"]["asleep"].get<bool>(); ++k) f.frames(0.25);
    const Json got = f.run("get_ragdoll", {{"id", id}}).value;
    GIANT_CHECK(got["ragdolls"] == 1 && got["asleep"] == 1 && got["bodies"] == k_person_joints - 1);
    GIANT_CHECK(got["ragdoll"]["asleep"].get<bool>() && !got["ragdoll"].contains("bone_list"));
    f.frames(0.1);
    const u64 poses = f.animation.poses();
    f.ragdolls.take_moved();
    f.frames(0.5);
    GIANT_CHECK(f.animation.poses() == poses && f.ragdolls.take_moved().empty());
    GIANT_CHECK(f.animation.posed_last() == 0); // (its clip's time moving underneath works out no pose)
}

GIANT_TEST("ragdolls: thrown about, elbows fold forward and knees back, never the other way") {
    Fixture f;
    f.floor();
    GIANT_CHECK(f.run("simulate", {{"on", true}}).ok);
    // Three thrown different ways, tumbling over the floor.
    const Vec3 thrown[] = {{3.0f, 4.0f, -2.0f}, {-2.0f, 5.0f, 3.0f}, {0.5f, 2.0f, 5.0f}};
    std::vector<world::EntityId> ids;
    for (int i = 0; i < 3; ++i) ids.push_back(f.person(6.0 * i, 1.0, 0.0));
    f.frames(0.1);
    for (int i = 0; i < 3; ++i) {
        const Vec3& v = thrown[i];
        GIANT_CHECK(f.run("ragdoll_entity", {{"id", ids[static_cast<usize>(i)]}, {"velocity", {v.x, v.y, v.z}}}).ok);
    }
    // Each limb's fold: the turn of its lower bone against its upper, about the axis an elbow or a
    // knee turns on (square to the limb and to the way it folds, carried by the upper bone), in
    // degrees: over 0 the way it should fold (an elbow forward, a knee back), under 0 the other way.
    struct Limb {
        const char* top;
        const char* middle;
        float ahead;
    };
    const Limb limbs[] = {{"LeftArm", "LeftForeArm", 1.0f}, {"RightArm", "RightForeArm", 1.0f},
                          {"LeftUpLeg", "LeftLeg", -1.0f}, {"RightUpLeg", "RightLeg", -1.0f}};
    float least = 0.0f, most = 0.0f, aside = 0.0f;
    for (int step = 0; step < 300; ++step) {
        f.frame();
        for (const world::EntityId id : ids) {
            const AnimationState::Animated* a = f.animation.find(id);
            for (const Limb& limb : limbs) {
                const geometry::testing::PersonJoint& bone = person_joint(limb.middle);
                const Vec3 along = normalize_or_zero(Vec3{bone.to[0] - bone.at[0], bone.to[1] - bone.at[1], bone.to[2] - bone.at[2]});
                const Vec3 axis = normalize_or_zero(cross(along, Vec3{0.0f, 0.0f, limb.ahead}));
                // (At rest no joint is turned: the limb lies along `along` in both bones' spaces.)
                const Vec3 upper = turned(a->model[joint_named(f, limb.top)], along);
                const Vec3 lower = turned(a->model[joint_named(f, limb.middle)], along);
                const Vec3 hinge = turned(a->model[joint_named(f, limb.top)], axis);
                const float angle = std::atan2(dot(cross(upper, lower), hinge), dot(upper, lower)) * 57.29578f;
                least = std::min(least, angle);
                most = std::max(most, angle);
                // Out of the plane it folds in: the lower bone's lean along the hinge's axis.
                aside = std::max(aside, std::fabs(std::asin(std::clamp(dot(lower, hinge), -1.0f, 1.0f))) * 57.29578f);
            }
        }
    }
    GIANT_CHECK(most > 25.0f);  // limbs did fold
    GIANT_CHECK(most < 155.0f); // not past where an elbow or a knee stops (140 degrees, and a joint's give)
    // Never backwards, nor sideways. (A joint gives some degrees while the body's weight lands on a
    // limb at speed: measured 14 back and 15 aside at worst here, 3 and 9 let fall gently; folding
    // the wrong way would show as far as it folds the right way.)
    GIANT_CHECK(least > -25.0f);
    GIANT_CHECK(aside < 25.0f);
}

GIANT_TEST("ragdolls: a walker goes limp where it walks, moving as it was, and walks no more") {
    Fixture f;
    f.floor();
    const world::EntityId id = f.person(-10.0, 0.0, 0.0, "none");
    GIANT_CHECK(f.run("simulate", {{"on", true}}).ok);
    f.frames(0.5);
    const ai::CommandResult sent = f.run("walk_to", {{"id", id}, {"to", {10.0, 0.0, 0.0}}, {"speed", 3.0}});
    GIANT_CHECK(sent.ok);
    if (!sent.ok) return;
    for (int k = 0; k < 600 && f.run("get_walker", {{"id", id}}).value["walker"]["phase"] != "walking"; ++k) f.frame();
    f.frames(1.5);
    const auto walked = f.walkers.poses().find(id);
    GIANT_CHECK(walked != f.walkers.poses().end());
    if (walked == f.walkers.poses().end()) return;
    const double x = walked->second.position.x;
    GIANT_CHECK(x > -8.0 && x < 0.0); // well on its way
    const DVec3 head = f.joint(id, joint_named(f, "Head"));
    const ai::CommandResult made = f.run("ragdoll_entity", {{"id", id}});
    GIANT_CHECK(made.ok);
    if (!made.ok) return;
    GIANT_CHECK(length(f.joint(id, joint_named(f, "Head")) - head) < 1e-3); // where it walked, not where the scene has it
    GIANT_CHECK(f.walkers.find(id) == nullptr && !f.run("get_walker", {{"id", id}}).ok);
    // Nor is it sent walking while it lies; one made to (a game's walkers put back) is stopped at the next step.
    GIANT_CHECK(!f.run("walk_to", {{"id", id}, {"to", {10.0, 0.0, 0.0}}}).ok && f.walkers.find(id) == nullptr);
    GIANT_CHECK(f.walkers.walk_to(f.state.scene, id, {10.0, 0.0, 0.0}, 1.4f).empty() && f.walkers.find(id) != nullptr);
    f.frame();
    GIANT_CHECK(f.walkers.find(id) == nullptr && f.ragdolls.find(id) != nullptr);
    GIANT_CHECK(made.value["velocity"][0].get<float>() > 2.0f); // carried on at its walk
    f.frames(0.5);
    GIANT_CHECK(f.ragdolls.poses().at(id).position.x > x + 0.5);
    // A velocity given is the one it gets.
    const world::EntityId other = f.person(0.0, 0.0, 12.0);
    f.frame();
    const ai::CommandResult thrown = f.run("ragdoll_entity", {{"id", other}, {"velocity", {0.0, 6.0, 0.0}}});
    GIANT_CHECK(thrown.ok && std::fabs(thrown.value["velocity"][1].get<float>() - 6.0f) < 1e-3f);
    f.frames(0.3);
    GIANT_CHECK(f.joint(other, joint_named(f, "Hips")).y > 2.0);
}

GIANT_TEST("ragdolls: let go by command, with its entity, and all when the simulation stops or the scene goes") {
    Fixture f;
    f.floor();
    const world::EntityId a = f.person(0.0, 0.5, 0.0), b = f.person(5.0, 0.5, 0.0), c = f.person(10.0, 0.5, 0.0);
    // (One with nothing of its own to play: animated only while it is a ragdoll.)
    const world::EntityId d = f.create({{"shape", "mesh"}, {"mesh", f.file}, {"position", {15.0, 0.5, 0.0}}});
    GIANT_CHECK(f.run("simulate", {{"on", true}}).ok);
    f.frames(0.2);
    const usize bodies = f.physics.world().size();
    GIANT_CHECK(f.animation.find(d) == nullptr);
    for (const world::EntityId id : {a, b, c, d}) GIANT_CHECK(f.run("ragdoll_entity", {{"id", id}}).ok);
    GIANT_CHECK(f.animation.find(d) != nullptr && f.ragdolls.size() == 4);
    f.frames(1.0);
    f.ragdolls.take_redraw();
    // By command: its bodies gone, drawn where the scene has it, its clip posing it again.
    GIANT_CHECK(f.run("stop_ragdoll", {{"id", a}}).ok && !f.run("stop_ragdoll", {{"id", a}}).ok);
    GIANT_CHECK(f.ragdolls.find(a) == nullptr && !f.ragdolls.poses().contains(a));
    GIANT_CHECK(f.ragdolls.take_redraw() == std::vector<world::EntityId>{a});
    GIANT_CHECK(f.physics.world().ragdolls() == 3 && f.physics.world().size() == bodies + 3 * (k_person_joints - 1));
    f.frame();
    GIANT_CHECK(!f.run("get_animation", {{"id", a}}).value["held"].get<bool>());
    GIANT_CHECK(std::fabs(f.joint(a, joint_named(f, "Head")).y - 2.0) < 1e-3); // standing as placed
    // One with nothing to play stands at rest again, animated no more.
    GIANT_CHECK(f.run("stop_ragdoll", {{"id", d}}).ok);
    GIANT_CHECK(f.animation.find(d) == nullptr);
    // Hidden: let go at the next step. Removed: likewise.
    GIANT_CHECK(f.run("set_entity", {{"id", b}, {"visible", false}}).ok);
    f.frame();
    GIANT_CHECK(f.ragdolls.find(b) == nullptr && f.physics.world().ragdolls() == 1);
    GIANT_CHECK(f.run("delete_entities", {{"ids", Json::array({c})}}).ok);
    f.frame();
    GIANT_CHECK(f.ragdolls.size() == 0 && f.physics.world().ragdolls() == 0 && f.physics.world().size() == bodies);
    GIANT_CHECK(f.ragdolls.poses().empty());
    // Stopping the simulation lets every one go. (A clip played on it meanwhile plays underneath.)
    GIANT_CHECK(f.run("ragdoll_entity", {{"id", a}}).ok);
    GIANT_CHECK(f.run("play_animation", {{"id", a}, {"clip", "raise"}}).ok && f.animation.holds_pose(a));
    f.frames(0.5);
    GIANT_CHECK(f.ragdolls.find(a) != nullptr && f.joint(a, joint_named(f, "Hips")).y < 1.4);
    // (Stopped and started again within a frame: another run, let go all the same.)
    GIANT_CHECK(f.run("simulate", {{"on", false}}).ok && f.run("simulate", {{"on", true}}).ok);
    f.frame();
    GIANT_CHECK(f.ragdolls.size() == 0 && f.physics.world().ragdolls() == 0);
    GIANT_CHECK(f.run("ragdoll_entity", {{"id", a}}).ok);
    f.frames(0.5);
    GIANT_CHECK(f.run("simulate", {{"on", false}}).ok);
    f.frame();
    GIANT_CHECK(f.ragdolls.size() == 0 && f.physics.world().ragdolls() == 0 && !f.animation.holds_pose(a));
    GIANT_CHECK(std::fabs(f.joint(a, joint_named(f, "Head")).y - 2.0) < 1e-3);
    // A new scene: the world made afresh with the ragdolls in it, theirs forgotten.
    GIANT_CHECK(f.run("simulate", {{"on", true}}).ok);
    f.frame();
    GIANT_CHECK(f.run("ragdoll_entity", {{"id", a}}).ok);
    f.frames(0.2);
    const u64 clears = f.physics.world().clears();
    GIANT_CHECK(f.run("new_scene").ok);
    // (As main: the physics notices the scene first, and clears its world whole with the bones in it.)
    f.physics.flush(f.state.scene);
    GIANT_CHECK(f.physics.world().clears() == clears + 1 && f.physics.world().ragdolls() == 0 && f.physics.world().size() == 0);
    GIANT_CHECK(f.ragdolls.size() == 1); // not yet looked
    f.ragdolls.after_step();             // (a step in between touches nothing gone)
    // (Asked before the next frame: none is one any more.)
    const ai::CommandResult none = f.run("get_ragdoll");
    GIANT_CHECK(none.ok && none.value["ragdolls"] == 0);
    GIANT_CHECK(!f.run("get_ragdoll", {{"id", a}}).ok && !f.run("stop_ragdoll", {{"id", a}}).ok);
    f.frame();
    GIANT_CHECK(f.ragdolls.size() == 0 && f.ragdolls.poses().empty() && f.physics.world().size() == 0);
    // And they work on in the new scene.
    f.floor();
    const world::EntityId again = f.person(0.0, 0.5, 0.0);
    GIANT_CHECK(f.run("simulate", {{"on", true}}).ok);
    f.frames(0.2);
    GIANT_CHECK(f.run("ragdoll_entity", {{"id", again}}).ok);
    f.frames(0.5);
    GIANT_CHECK(f.joint(again, joint_named(f, "Hips")).y < 1.4);
}

GIANT_TEST("ragdolls: bullets meet a ragdoll's bones, not its hit capsules, and push them") {
    Fixture f;
    f.floor();
    const world::EntityId id = f.person(0.0, 0.0, 0.0);
    GIANT_CHECK(f.run("simulate", {{"on", true}}).ok);
    f.frames(0.2);
    GIANT_CHECK(f.hits.shapes().find(id) != nullptr);
    // Standing: a shot at its chest meets its capsule.
    const Json shot = {{"origin", {0.0, 1.3, -5.0}}, {"direction", {0.0, 0.0, 1.0}}, {"resolve", true}};
    ai::CommandResult fired = f.run("fire", shot);
    GIANT_CHECK(fired.ok && !fired.value["hits"].empty() && fired.value["hits"][0]["entity"] == id);
    GIANT_CHECK(fired.value["hits"][0]["bone"] == "Spine");
    GIANT_CHECK(f.run("ragdoll_entity", {{"id", id}}).ok);
    GIANT_CHECK(f.hits.shapes().find(id) == nullptr); // its bones' bodies are met now, at once
    // (A ray names the entity by its bones too.)
    const ai::CommandResult ray = f.run("raycast", {{"origin", {0.0, 1.3, -5.0}}, {"direction", {0.0, 0.0, 1.0}}});
    GIANT_CHECK(ray.ok && ray.value["hit"].get<bool>() && ray.value["entity"] == id);
    const float before = f.run("get_ragdoll", {{"id", id}, {"bones", true}}).value["ragdoll"]["velocity"][2].get<float>();
    // (A heavy slug that stops in it: all its momentum goes to the body.)
    Json slug = shot;
    slug["mass"] = 0.05;
    slug["penetration"] = 0.05;
    fired = f.run("fire", slug);
    GIANT_CHECK(fired.ok && !fired.value["hits"].empty());
    if (!fired.ok || fired.value["hits"].empty()) return;
    const Json& hit = fired.value["hits"][0];
    GIANT_CHECK(hit["entity"] == id && hit["character"].get<bool>() && hit["surface"] == "flesh");
    // Pushed along the shot.
    f.frame();
    const RagdollState::Ragdoll& r = *f.ragdolls.find(id);
    const sim::BodyId spine = f.physics.world().ragdoll_bodies(r.id)[r.bone_of[joint_named(f, "Spine")]];
    GIANT_CHECK(f.physics.world().state(spine)->linear_velocity.z > before + 0.1f);
    // Let go once fallen: its capsules are met again, standing where the scene has it (not as it lay).
    f.frames(1.5);
    GIANT_CHECK(f.joint(id, joint_named(f, "Head")).y < 1.5);
    GIANT_CHECK(f.run("stop_ragdoll", {{"id", id}}).ok);
    const sim::HitCharacter* stands = f.hits.shapes().find(id);
    GIANT_CHECK(stands != nullptr);
    double top = 0.0;
    if (stands != nullptr) {
        for (const sim::HitCapsule& c : stands->capsules) top = std::max({top, c.a.y, c.b.y});
    }
    GIANT_CHECK(top > 1.5);
}

GIANT_TEST("ragdolls: a scaled, turned character keeps its shape as it goes limp; one of no size is refused") {
    Fixture f;
    f.floor();
    const world::EntityId id = f.create({{"shape", "mesh"}, {"mesh", f.file}, {"position", {3.0, 0.2, -4.0}},
                                         {"rotation", {{"yaw", 70.0}}}, {"scale", {2.0, 2.0, 2.0}}});
    GIANT_CHECK(f.run("set_animation", {{"id", id}, {"clip", "raise"}}).ok); // (posed away from rest: an arm up)
    const world::EntityId flat = f.create({{"shape", "mesh"}, {"mesh", f.file}, {"position", {30.0, 0.2, 0.0}},
                                           {"scale", {1.0, 0.0005, 1.0}}});
    GIANT_CHECK(f.run("simulate", {{"on", true}}).ok);
    f.frames(0.2);
    std::vector<DVec3> stood;
    for (usize j = 0; j < k_person_joints; ++j) stood.push_back(f.joint(id, j));
    GIANT_CHECK(stood[joint_named(f, "LeftForeArm")].y > stood[joint_named(f, "LeftArm")].y + 0.4); // raised, twice the size
    const ai::CommandResult made = f.run("ragdoll_entity", {{"id", id}, {"bones", true}});
    GIANT_CHECK(!made.ok); // (no such argument)
    const ai::CommandResult limp = f.run("ragdoll_entity", {{"id", id}});
    GIANT_CHECK(limp.ok && limp.value["mass"].get<float>() > 8.0f * 20.0f); // eight times the weight
    for (usize j = 0; j < k_person_joints; ++j) GIANT_CHECK(length(f.joint(id, j) - stood[j]) < 1e-3);
    f.frames(1.0);
    for (usize j = 0; j < k_person_joints; ++j) {
        if (bones(f).parents[j] < 0) continue;
        const usize p = static_cast<usize>(bones(f).parents[j]);
        GIANT_CHECK(std::fabs(length(f.joint(id, j) - f.joint(id, p)) - 2.0 * static_cast<double>(rest_gap(f, j))) < 0.08);
    }
    GIANT_CHECK(!f.run("ragdoll_entity", {{"id", flat}}).ok && !f.animation.holds_pose(flat) && f.ragdolls.size() == 1);
}

GIANT_TEST("ragdolls: step_physics moves them too, and the scene draws them where they lie, as moving") {
    Fixture f;
    f.floor();
    const world::EntityId id = f.person(0.0, 1.0, 0.0);
    GIANT_CHECK(f.run("simulate", {{"on", true}}).ok);
    f.frame();
    GIANT_CHECK(f.run("ragdoll_entity", {{"id", id}}).ok);
    GIANT_CHECK(f.run("step_physics", {{"seconds", 1.0}}).ok);
    const auto at = f.ragdolls.poses().find(id);
    GIANT_CHECK(at != f.ragdolls.poses().end() && at->second.position.y < 0.5);
    // Its characters' light: its capsules where it is drawn.
    std::vector<render::Occluder> out;
    f.animation.follow_now(f.state.scene, f.meshes, {});
    character_occluders(f.state.scene, f.meshes, f.animation, nullptr, nullptr, nullptr, nullptr, DVec3{0.0, 1.0, 0.0}, out,
                        &f.ragdolls.poses());
    GIANT_CHECK(out.size() == k_person_joints);
    double top = -1e9;
    for (const render::Occluder& o : out) top = std::max({top, o.a.y, o.b.y});
    GIANT_CHECK(top < 1.9); // (standing as placed it would reach 2.7)
}
